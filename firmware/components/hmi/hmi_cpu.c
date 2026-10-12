/*
 * CPU load of both cores and the LVGL frame rate, measured over one second (title bar chip,
 * console 'cpu').
 *
 * Load = 100 % minus the share of the second the core spent in its idle task, from the FreeRTOS
 * run-time counters (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, clocked by esp_timer in us). FreeRTOS
 * adds to a task's counter when the task is switched out, so a core that sits in its idle task
 * shows the idle time up to its last context switch only. A core that did not switch at all in the
 * second and is in its idle task counts as 0 %; otherwise the error is the time since that core's
 * last switch (in OP core 1 switches every cycle).
 *
 * The sampler is an esp_timer callback (esp_timer task, core 0), so it keeps running while the
 * screen is off and the LVGL timers are paused.
 */

#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hmi_internal.h"

#define SAMPLE_US 1000000

static const char *TAG = "hmi_cpu";

static esp_timer_handle_t s_timer;
static uint32_t s_last_us;
static configRUN_TIME_COUNTER_TYPE s_last_idle[2];
static atomic_uint s_frames;
/* load core 0 | load core 1 << 8 | fps << 16; 0 until the first sample */
static atomic_uint s_result;

static unsigned core_load(int core, uint32_t dt_us)
{
    const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);
    const uint32_t d_idle = (uint32_t)(idle - s_last_idle[core]);
    s_last_idle[core] = idle;
    if (d_idle == 0 && xTaskGetCurrentTaskHandleForCore(core) == xTaskGetIdleTaskHandleForCore(core)) {
        return 0;
    }
    const uint32_t busy = dt_us > d_idle ? dt_us - d_idle : 0;
    return (unsigned)(((uint64_t)busy * 100 + dt_us / 2) / dt_us);
}

static void sample_cb(void *arg)
{
    (void)arg;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const uint32_t dt = now - s_last_us;
    s_last_us = now;
    if (dt == 0) {
        return;
    }
    const unsigned l0 = core_load(0, dt), l1 = core_load(1, dt);
    const unsigned frames = atomic_exchange_explicit(&s_frames, 0, memory_order_relaxed);
    unsigned fps = (unsigned)(((uint64_t)frames * 1000000 + dt / 2) / dt);
    fps = fps > 0x7FFF ? 0x7FFF : fps;
    atomic_store_explicit(&s_result, (l0 > 100 ? 100 : l0) | (l1 > 100 ? 100 : l1) << 8 | fps << 16 | 1u << 31,
                          memory_order_relaxed);
}

/* LVGL task, after each refresh that rendered something. */
static void frame_cb(lv_event_t *e)
{
    (void)e;
    atomic_fetch_add_explicit(&s_frames, 1, memory_order_relaxed);
}

void hmi_cpu_start(lv_display_t *disp)
{
    lv_display_add_event_cb(disp, frame_cb, LV_EVENT_RENDER_READY, NULL);
    s_last_us = (uint32_t)esp_timer_get_time();
    for (int c = 0; c < 2; c++) {
        s_last_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    }
    const esp_timer_create_args_t args = {.callback = sample_cb, .name = "hmi_cpu"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK || esp_timer_start_periodic(s_timer, SAMPLE_US) != ESP_OK) {
        ESP_LOGE(TAG, "CPU load timer failed: the title bar shows no CPU load");
    }
}

bool hmi_cpu_get(hmi_cpu_t *out)
{
    const unsigned r = atomic_load_explicit(&s_result, memory_order_relaxed);
    if (!(r & 1u << 31)) {
        return false;
    }
    out->load[0] = r & 0xFF;
    out->load[1] = r >> 8 & 0xFF;
    out->fps = r >> 16 & 0x7FFF;
    return true;
}
