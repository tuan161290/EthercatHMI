/*
 * HMI start-up (display, touch, LVGL port) and the EtherCAT status snapshot.
 *
 * LVGL runs in the esp_lvgl_port task pinned to core 0 with one software draw unit, so nothing
 * of the HMI runs on core 1 (CLAUDE.md, architecture rules). The draw buffer is a 48-line strip
 * in internal RAM; DMA2D copies it into the DPI frame buffer in PSRAM.
 */

#include <stdatomic.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hmi_internal.h"

static const char *TAG = "hmi";

#define DRAW_BUF_LINES         48
#define LVGL_TASK_STACK        12288
#define FIRST_FRAME_TIMEOUT_MS 2000

static SemaphoreHandle_t s_first_frame;
static bool (*s_bus_request)(hmi_bus_req_t req);

bool hmi_bus_request(hmi_bus_req_t req)
{
    return s_bus_request ? s_bus_request(req) : false;
}

/* ---- EtherCAT status: seqlock, one writer (the EtherCAT monitor task), reader = LVGL task ---- */

static atomic_uint s_seq;
static hmi_bus_status_t s_bus;
static atomic_bool s_bus_valid;

void hmi_publish_bus_status(const hmi_bus_status_t *status)
{
    unsigned seq = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, seq + 1, memory_order_relaxed); /* odd: write in progress */
    atomic_thread_fence(memory_order_release);
    s_bus = *status;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s_seq, seq + 2, memory_order_relaxed);
    atomic_store_explicit(&s_bus_valid, true, memory_order_release);
}

bool hmi_bus_status_read(hmi_bus_status_t *out)
{
    if (!atomic_load_explicit(&s_bus_valid, memory_order_acquire)) {
        return false;
    }
    unsigned before, after;
    do {
        before = atomic_load_explicit(&s_seq, memory_order_acquire);
        *out = s_bus;
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_seq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
    return true;
}

/* ---- Start-up ---- */

/* LVGL task, after a refresh that rendered something. */
static void first_frame_cb(lv_event_t *e)
{
    (void)e;
    xSemaphoreGive(s_first_frame);
}

esp_err_t hmi_start(const hmi_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "no config");
    s_bus_request = cfg->bus_request;

    board_lcd_t lcd;
    ESP_RETURN_ON_ERROR(board_lcd_init(cfg->panel, &lcd), TAG, "display");
    hmi_screenshot_init(lcd.panel);

    const lvgl_port_cfg_t port = {
        .task_priority = CONFIG_HMI_TASK_PRIORITY,
        .task_stack = LVGL_TASK_STACK,
        .task_affinity = 0, /* core 1 belongs to EtherCAT */
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd.io,
        .panel_handle = lcd.panel,
        .buffer_size = BOARD_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = false,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {.flags = {.avoid_tearing = false}};
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "LVGL display");

    esp_lcd_touch_handle_t touch = NULL;
    lv_indev_t *indev = NULL;
    esp_err_t err = board_touch_init(&touch);
    if (err == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = touch};
        indev = lvgl_port_add_touch(&touch_cfg);
        if (indev == NULL) {
            ESP_LOGE(TAG, "LVGL touch input failed");
        }
    } else {
        ESP_LOGE(TAG, "touch controller not found (%s): the screen will not react to touch", esp_err_to_name(err));
    }

    s_first_frame = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_first_frame, ESP_ERR_NO_MEM, TAG, "first-frame semaphore");

    lvgl_port_lock(0);
    err = hmi_display_init(disp, cfg); /* colour balance from NVS applies to the first frame too */
    if (err != ESP_OK) {
        lvgl_port_unlock();
        vSemaphoreDelete(s_first_frame);
        s_first_frame = NULL;
        return err;
    }
    hmi_theme_init();
    hmi_shell_create();
    hmi_touchlog_start(indev);
    hmi_touch_slide_cancel(indev);
    hmi_screen_start(indev);
    hmi_cpu_start(disp);
    /* The LVGL task renders the first frame. Rendering here with lv_refr_now() overflowed the
     * 4 KB app_main stack (software border masks in hmi_icon.c); the LVGL task has 12 KB. */
    lv_display_add_event_cb(disp, first_frame_cb, LV_EVENT_RENDER_READY, NULL);
    lvgl_port_unlock();

    /* Backlight on only once the first frame is in the frame buffer. */
    if (xSemaphoreTake(s_first_frame, pdMS_TO_TICKS(FIRST_FRAME_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "first frame not rendered within %d ms", FIRST_FRAME_TIMEOUT_MS);
    }
    lvgl_port_lock(0);
    lv_display_remove_event_cb_with_user_data(disp, first_frame_cb, NULL);
    err = hmi_display_light();
    lvgl_port_unlock();
    vSemaphoreDelete(s_first_frame);
    s_first_frame = NULL;
    ESP_RETURN_ON_ERROR(err, TAG, "backlight");
    ESP_LOGI(TAG, "HMI up: %s panel, free internal %u B, free PSRAM %u B", board_lcd_panel_name(cfg->panel),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}
