/*
 * Screenshot for bench diagnosis: the console command 'screenshot' prints the DPI frame buffer,
 * i.e. exactly what the panel shows (dialogs and the colour balance included), as text lines that
 * tools/screenshot.py turns into a PNG. Lets the HMI be checked without a camera.
 *
 * The frame is compressed with PackBits on RGB565 pixels (the flat Fluent theme packs about
 * 5-10x), then printed in base64 lines. Each line is one printf, so log lines of other tasks
 * fall between lines, never inside one; the tool ignores them.
 *
 *   @SNAP begin <width> <height> rgb565 packbits <bytes>
 *   @S <offset hex> <base64 of up to 48 bytes>
 *   @SNAP end <bytes> <adler32 hex>
 *
 * PackBits: control byte c < 128: c + 1 literal pixels follow; c >= 128: the next pixel repeats
 * c - 126 times (2..129). Pixels are little endian, as in the frame buffer.
 */

#include <stdio.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lvgl_port.h"
#include "hmi_internal.h"

#define LINE_BYTES 48 /* 64 base64 characters */

static esp_lcd_panel_handle_t s_panel;

void hmi_screenshot_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
}

static size_t pack(const uint16_t *px, size_t n, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 129 && px[i + run] == px[i]) {
            run++;
        }
        if (run >= 2) {
            out[o++] = (uint8_t)(run + 126);
            out[o++] = (uint8_t)px[i];
            out[o++] = (uint8_t)(px[i] >> 8);
            i += run;
            continue;
        }
        /* literals up to the next pair of equal pixels */
        size_t lit = 1;
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && px[i + lit] == px[i + lit + 1])) {
            lit++;
        }
        out[o++] = (uint8_t)(lit - 1);
        for (size_t k = 0; k < lit; k++) {
            out[o++] = (uint8_t)px[i + k];
            out[o++] = (uint8_t)(px[i + k] >> 8);
        }
        i += lit;
    }
    return o;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return b << 16 | a;
}

static void base64(const uint8_t *in, size_t n, char *out)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        *out++ = abc[v >> 18];
        *out++ = abc[(v >> 12) & 63];
        *out++ = abc[(v >> 6) & 63];
        *out++ = abc[v & 63];
    }
    if (i < n) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
        *out++ = abc[v >> 18];
        *out++ = abc[(v >> 12) & 63];
        *out++ = i + 1 < n ? abc[(v >> 6) & 63] : '=';
        *out++ = '=';
    }
    *out = '\0';
}

void hmi_screenshot_print(void)
{
    void *fb = NULL;
    if (s_panel == NULL || esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || fb == NULL) {
        printf("No frame buffer (display not started).\n");
        return;
    }
    const size_t n = (size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES;
    uint8_t *buf = heap_caps_malloc(n * 2 + n / 128 + 16, MALLOC_CAP_SPIRAM); /* worst case: all literals */
    if (buf == NULL) {
        printf("Out of PSRAM for the screenshot.\n");
        return;
    }

    /* The LVGL lock keeps the frame still; DMA2D wrote it, so drop stale cache lines first. */
    lvgl_port_lock(0);
    const esp_err_t sync = esp_cache_msync(fb, n * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    const size_t len = pack(fb, n, buf);
    lvgl_port_unlock();
    if (sync != ESP_OK) {
        printf("Warning: cache sync failed (%s); parts of the picture may be old.\n", esp_err_to_name(sync));
    }

    printf("@SNAP begin %d %d rgb565 packbits %u\n", BOARD_LCD_H_RES, BOARD_LCD_V_RES, (unsigned)len);
    char line[LINE_BYTES / 3 * 4 + 1];
    for (size_t off = 0; off < len; off += LINE_BYTES) {
        base64(buf + off, len - off < LINE_BYTES ? len - off : LINE_BYTES, line);
        printf("@S %x %s\n", (unsigned)off, line);
    }
    printf("@SNAP end %u %08lx\n", (unsigned)len, (unsigned long)adler32(buf, len));
    heap_caps_free(buf);
}
