/*
 * HMI simulator: stand-ins for everything the HMI pages use outside components/hmi, with
 * plausible machine data (the bench bus of 2026-10-11: 7x L5N, 2x CL3B-EC507, one R1EC with
 * 2x SC-3200-1 + SC-0032-N-1, Wi-Fi on "OpenWrt").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "board_display.h"
#include "lvgl.h"

/* ---- C runtime gaps ---- */

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t n = strlen(src);
    if (size) {
        const size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    const size_t d = strnlen(dst, size);
    return d == size ? size + strlen(src) : d + strlcpy(dst + d, src, size - d);
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    return gmtime_s(out, t) == 0 ? out : NULL;
}

/* ---- ESP-IDF ---- */

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "ESP_ERR";
    }
}

int64_t esp_timer_get_time(void)
{
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER now;
    if (f.QuadPart == 0) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    QueryPerformanceCounter(&now);
    return (now.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart;
}

/* Values like the board's after start-up (2026-10-11). */
size_t heap_caps_get_free_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 30436648 : 152000;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 30300000 : 141000;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 29000000 : 98304;
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 32 * 1024 * 1024 : 445000;
}

/* LVGL heap (CONFIG_LV_USE_CUSTOM_MALLOC; hmi_lv_mem.c on the board) */
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}
void *lv_malloc_core(size_t size)
{
    return malloc(size);
}
void *lv_realloc_core(void *p, size_t size)
{
    return realloc(p, size);
}
void lv_free_core(void *p)
{
    free(p);
}
void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    memset(mon, 0, sizeof(*mon));
}
lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

/* ---- System information (the board's values, 2026-10-11) ---- */

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_psram.h"

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = {"1", "ehmi_general", __TIME__, __DATE__, "v5.5.4"};
    return &d;
}

void esp_chip_info(esp_chip_info_t *out)
{
    *out = (esp_chip_info_t){.revision = 103, .cores = 2};
}

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    *out_size = 16u << 20;
    return ESP_OK;
}

const char *esp_get_idf_version(void)
{
    return "v5.5.4";
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    static const uint8_t m[6] = {0xe8, 0xf6, 0x0a, 0xe7, 0x3a, 0x3e};
    memcpy(mac, m, sizeof(m));
    return ESP_OK;
}

size_t esp_psram_get_size(void)
{
    return 32u << 20;
}

/* ---- Board ---- */

const char *board_lcd_panel_name(board_lcd_panel_t panel)
{
    return panel == BOARD_LCD_PANEL_OLD ? "Old" : "New";
}

esp_err_t board_lcd_backlight_set(int percent)
{
    (void)percent;
    return ESP_OK;
}
