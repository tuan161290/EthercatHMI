/*
 * Display settings: backlight brightness, a per-channel colour balance and the screen timeouts.
 *
 * The colour balance is applied in software. Before LVGL hands a rendered strip (48 lines in
 * internal RAM, hmi.c) to the DSI driver, every RGB565 pixel goes through three small lookup
 * tables (LV_EVENT_FLUSH_START), so it covers everything on the screen. With all gains at 100 %
 * the pass is skipped. The JD9165 may have gamma registers for this, but its datasheet is not in
 * the repo (unverified).
 *
 * Screen timeouts: the backlight dims (CONFIG_HMI_DIM_LEVEL % of the brightness) and then switches
 * off after the set times without a touch. The touch that lights a dimmed or dark screen again is
 * not passed on to LVGL (it reads as released until the finger lifts), so it cannot press whatever
 * is under it. A new unacknowledged alarm lights it too. Without a touch controller the screen
 * never dims.
 *
 * While the screen is off the UI is suspended: every LVGL timer (page polls, title bar, touch log,
 * animations) is paused except the screen timer and the touch read timer. Nothing invalidates, so
 * LVGL renders nothing (its refresh timer pauses itself when idle) and the LVGL task only wakes for
 * touches (GT911 interrupt) and the screen timer. Lit again, the paused timers resume and catch up
 * at once, and the whole screen is redrawn.
 *
 * Kept in the machine store (machine_store.h, key "display"), which saves also while EtherCAT is
 * in OP. Earlier firmware kept them in NVS "hmi"/"display" (version 1; version 2 had a dithering
 * byte for a few hours on 2026-10-10): read once and moved.
 */

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "machine_alarm.h"
#include "machine_store.h"
#include "nvs.h"
#include "hmi_internal.h"

static const char *TAG = "hmi_display";

#define STORE_KEY      "display"
#define NVS_NS         "hmi" /* earlier firmware */
#define NVS_KEY        "display"
#define BLOB_VERSION   3 /* 2 was only ever in NVS */
#define BLOB_LEN       9 /* version, brightness, red, green, blue, dim_s, off_s (16 bit, little endian) */
#define BLOB_V1_LEN    5 /* version 1: version, brightness, red, green, blue */
#define BLOB_V2_LEN    6 /* version 2: version 1 + dithering */
#define SCREEN_POLL_MS 250
#define MAX_PAUSED     64 /* LVGL timers paused while the screen is off; more stay running */

static lv_display_t *s_disp;
static uint8_t s_default_brightness = CONFIG_HMI_BRIGHTNESS;
static hmi_display_t s_cur;   /* on the screen */
static hmi_display_t s_saved; /* stored (or the defaults when nothing is stored) */

static uint16_t s_lut_r[32], s_lut_g[64], s_lut_b[32];
static bool s_lut_on;

/* Screen timeouts; LVGL lock */
static hmi_screen_t s_screen = HMI_SCREEN_ON;
static bool s_lit;                    /* backlight on since the first frame (hmi_start) */
static uint32_t s_activity;           /* lv_tick of the last touch or wake */
static bool s_swallow;                /* the finger that lit the screen is still down */
static lv_indev_read_cb_t s_touch_read; /* the touch driver's (esp_lvgl_port) */
static lv_indev_t *s_indev;
static lv_timer_t *s_screen_timer;
static lv_timer_t *s_paused[MAX_PAUSED]; /* paused by ui_suspend() */
static int s_n_paused;
static uint32_t s_alarm_gen;
static int s_unacked;

static uint8_t clamp_pct(int v, int lo)
{
    return (uint8_t)(v < lo ? lo : v > 100 ? 100 : v);
}

static uint16_t clamp_timeout(unsigned s)
{
    return (uint16_t)(s > HMI_SCREEN_TIMEOUT_MAX_S ? HMI_SCREEN_TIMEOUT_MAX_S : s);
}

static void clamp_all(hmi_display_t *d)
{
    d->brightness = clamp_pct(d->brightness, HMI_BRIGHTNESS_MIN);
    d->red = clamp_pct(d->red, HMI_GAIN_MIN);
    d->green = clamp_pct(d->green, HMI_GAIN_MIN);
    d->blue = clamp_pct(d->blue, HMI_GAIN_MIN);
    d->dim_s = clamp_timeout(d->dim_s);
    d->off_s = clamp_timeout(d->off_s);
}

static bool same_colour(const hmi_display_t *a, const hmi_display_t *b)
{
    return a->red == b->red && a->green == b->green && a->blue == b->blue;
}

static bool same(const hmi_display_t *a, const hmi_display_t *b)
{
    return a->brightness == b->brightness && same_colour(a, b) && a->dim_s == b->dim_s && a->off_s == b->off_s;
}

/* Gains scale the RGB565 code values; each table entry is already shifted into place. */
static void lut_build(const hmi_display_t *d)
{
    for (int i = 0; i < 32; i++) {
        s_lut_r[i] = (uint16_t)(((i * d->red + 50) / 100) << 11);
        s_lut_b[i] = (uint16_t)((i * d->blue + 50) / 100);
    }
    for (int i = 0; i < 64; i++) {
        s_lut_g[i] = (uint16_t)(((i * d->green + 50) / 100) << 5);
    }
    s_lut_on = d->red != 100 || d->green != 100 || d->blue != 100;
}

/* LVGL task, just before a rendered strip is flushed: the active draw buffer holds exactly the
 * flushed area (partial render mode), so apply the colour balance to it in place. */
static void flush_start_cb(lv_event_t *e)
{
    if (!s_lut_on) {
        return;
    }
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(s_disp);
    const int32_t w = lv_area_get_width(a);
    const int32_t h = lv_area_get_height(a);
    uint8_t *row = buf->data;
    for (int32_t y = 0; y < h; y++, row += buf->header.stride) {
        uint16_t *p = (uint16_t *)row;
        for (int32_t x = 0; x < w; x++) {
            const uint16_t c = p[x];
            p[x] = s_lut_r[c >> 11] | s_lut_g[(c >> 5) & 0x3F] | s_lut_b[c & 0x1F];
        }
    }
}

/* ---- Storage ---- */

static esp_err_t store(const hmi_display_t *d)
{
    const uint8_t b[BLOB_LEN] = {BLOB_VERSION, d->brightness, d->red, d->green, d->blue,
                                 d->dim_s & 0xFF, d->dim_s >> 8, d->off_s & 0xFF, d->off_s >> 8};
    return machine_store_save(STORE_KEY, b, sizeof(b));
}

/* Brightness and colour balance, the same four bytes after the version in every layout. */
static void decode_v1(const uint8_t *raw, hmi_display_t *out)
{
    out->brightness = raw[1];
    out->red = raw[2];
    out->green = raw[3];
    out->blue = raw[4];
}

/* Settings of earlier firmware in NVS. @return true if there were any */
static bool load_nvs(hmi_display_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t raw[BLOB_V2_LEN];
    size_t len = sizeof(raw);
    const bool ok = nvs_get_blob(h, NVS_KEY, raw, &len) == ESP_OK &&
                    ((len == BLOB_V1_LEN && raw[0] == 1) || (len == BLOB_V2_LEN && raw[0] == 2));
    if (ok) {
        decode_v1(raw, out);
    }
    nvs_close(h);
    return ok;
}

static void load(hmi_display_t *out)
{
    hmi_display_defaults(out);
    uint8_t b[BLOB_LEN];
    if (machine_store_load(STORE_KEY, b, BLOB_LEN) == ESP_OK && b[0] == BLOB_VERSION) {
        decode_v1(b, out);
        out->dim_s = (uint16_t)(b[5] | b[6] << 8);
        out->off_s = (uint16_t)(b[7] | b[8] << 8);
    } else if (machine_store_load(STORE_KEY, b, BLOB_V1_LEN) == ESP_OK && b[0] == 1) {
        decode_v1(b, out); /* timeouts: the defaults */
    } else if (load_nvs(out)) {
        clamp_all(out);
        (void)store(out); /* moved into the machine store */
    }
    clamp_all(out);
}

/* Caller holds the LVGL lock. */
static void save(void)
{
    if (same(&s_cur, &s_saved)) {
        return;
    }
    esp_err_t err = store(&s_cur);
    if (err == ESP_OK) {
        s_saved = s_cur;
    } else {
        ESP_LOGE(TAG, "saving display settings failed: %s", esp_err_to_name(err));
    }
}

/* ---- Screen timeouts (LVGL lock) ---- */

static esp_err_t backlight_apply(void)
{
    if (!s_lit) {
        return ESP_OK; /* hmi_start switches it on after the first frame */
    }
    int pct = s_cur.brightness;
    if (s_screen == HMI_SCREEN_OFF) {
        pct = 0;
    } else if (s_screen == HMI_SCREEN_DIM) {
        pct = s_cur.brightness * CONFIG_HMI_DIM_LEVEL / 100;
        pct = pct < HMI_BRIGHTNESS_MIN ? HMI_BRIGHTNESS_MIN : pct;
    }
    return board_lcd_backlight_set(pct);
}

/* Screen off: pause every running LVGL timer but the screen timer and the touch read timer. */
static void ui_suspend(void)
{
    lv_timer_t *read_timer = s_indev ? lv_indev_get_read_timer(s_indev) : NULL;
    s_n_paused = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t != NULL && s_n_paused < MAX_PAUSED; t = lv_timer_get_next(t)) {
        if (t != s_screen_timer && t != read_timer && !lv_timer_get_paused(t)) {
            lv_timer_pause(t);
            s_paused[s_n_paused++] = t;
        }
    }
}

/* Resume what ui_suspend() paused. A timer deleted meanwhile (a page closed from the console) is no
 * longer in LVGL's list, so only listed ones are touched. Then redraw the whole screen. */
static void ui_resume(void)
{
    for (lv_timer_t *t = lv_timer_get_next(NULL); t != NULL; t = lv_timer_get_next(t)) {
        for (int i = 0; i < s_n_paused; i++) {
            if (s_paused[i] == t) {
                lv_timer_resume(t);
                break;
            }
        }
    }
    s_n_paused = 0;
    lv_obj_invalidate(lv_screen_active());
}

static esp_err_t screen_set(hmi_screen_t s)
{
    if (s != s_screen) {
        if (s == HMI_SCREEN_OFF) {
            ui_suspend();
            ESP_LOGI(TAG, "screen off, UI suspended (%d timers paused)", s_n_paused);
        } else {
            if (s_screen == HMI_SCREEN_OFF) {
                ui_resume();
            }
            ESP_LOGI(TAG, "screen %s", s == HMI_SCREEN_DIM ? "dimmed" : "on");
        }
        s_screen = s;
    }
    return backlight_apply();
}

/* A touch, a new alarm or the console: restart the countdown and light the screen. */
static void wake(void)
{
    s_activity = lv_tick_get();
    if (s_screen != HMI_SCREEN_ON) {
        (void)screen_set(HMI_SCREEN_ON);
    }
}

/* A new unacknowledged alarm lights the screen. Checked here, not in the title bar, because the
 * title bar's timer is paused while the screen is off. */
static void alarm_check(void)
{
    const uint32_t gen = machine_alarm_gen();
    if (gen == s_alarm_gen) {
        return;
    }
    s_alarm_gen = gen;
    int unacked;
    machine_alarm_counts(NULL, NULL, &unacked);
    if (unacked > s_unacked) {
        wake();
    }
    s_unacked = unacked;
}

/* LVGL task: dim, then switch off, after the timeouts. */
static void screen_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lit || s_swallow) {
        return;
    }
    alarm_check();
    const uint32_t idle_ms = lv_tick_elaps(s_activity);
    hmi_screen_t want = HMI_SCREEN_ON;
    if (s_cur.off_s && idle_ms >= s_cur.off_s * 1000u) {
        want = HMI_SCREEN_OFF;
    } else if (s_cur.dim_s && idle_ms >= s_cur.dim_s * 1000u) {
        want = HMI_SCREEN_DIM;
    }
    if (want != s_screen) {
        (void)screen_set(want);
    }
}

/* LVGL task, every touch read: a touch restarts the countdown. The touch that lights a dimmed or
 * dark screen reads as released until the finger lifts, so LVGL never sees it. */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_touch_read(indev, data);
    if (data->state != LV_INDEV_STATE_PRESSED) {
        s_swallow = false;
        return;
    }
    s_activity = lv_tick_get();
    if (s_screen != HMI_SCREEN_ON) {
        s_swallow = true;
        (void)screen_set(HMI_SCREEN_ON);
    }
    if (s_swallow) {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ---- Internal ---- */

esp_err_t hmi_display_init(lv_display_t *disp, const hmi_config_t *cfg)
{
    s_default_brightness = clamp_pct(cfg->brightness, HMI_BRIGHTNESS_MIN);
    load(&s_cur);
    s_saved = s_cur;
    lut_build(&s_cur);
    lv_display_add_event_cb(disp, flush_start_cb, LV_EVENT_FLUSH_START, NULL);
    s_disp = disp;
    ESP_LOGI(TAG, "brightness %u %%, colour balance R %u G %u B %u %%, dim after %u s, off after %u s (0 = never)",
             s_cur.brightness, s_cur.red, s_cur.green, s_cur.blue, s_cur.dim_s, s_cur.off_s);
    return ESP_OK;
}

void hmi_screen_start(lv_indev_t *indev)
{
    if (indev == NULL) {
        ESP_LOGW(TAG, "no touch input: the screen never dims or switches off");
        return;
    }
    s_indev = indev;
    s_touch_read = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, touch_read_cb);
    s_screen_timer = lv_timer_create(screen_timer_cb, SCREEN_POLL_MS, NULL);
}

esp_err_t hmi_display_light(void)
{
    s_lit = true;
    s_activity = lv_tick_get();
    return screen_set(HMI_SCREEN_ON);
}

/* ---- Public ---- */

void hmi_display_defaults(hmi_display_t *out)
{
    *out = (hmi_display_t){
        .brightness = s_default_brightness,
        .red = 100,
        .green = 100,
        .blue = 100,
        .dim_s = CONFIG_HMI_DIM_AFTER_S,
        .off_s = CONFIG_HMI_OFF_AFTER_S,
    };
}

void hmi_display_get(hmi_display_t *out)
{
    if (s_disp == NULL) {
        hmi_display_defaults(out);
        return;
    }
    lvgl_port_lock(0);
    *out = s_cur;
    lvgl_port_unlock();
}

esp_err_t hmi_display_set(const hmi_display_t *d, bool save_it)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "no settings");
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_INVALID_STATE, TAG, "HMI not started");
    hmi_display_t v = *d;
    clamp_all(&v);

    lvgl_port_lock(0);
    const bool colour_changed = !same_colour(&v, &s_cur);
    s_cur = v;
    s_activity = lv_tick_get();
    esp_err_t err = screen_set(HMI_SCREEN_ON);
    if (colour_changed) {
        lut_build(&v);
        lv_obj_invalidate(lv_screen_active()); /* the whole display goes through the new tables */
    }
    if (save_it) {
        save();
    }
    lvgl_port_unlock();
    return err;
}

bool hmi_display_save_pending(void)
{
    return machine_store_key_pending(STORE_KEY);
}

hmi_screen_t hmi_screen_get(void)
{
    return s_screen;
}

void hmi_screen_wake(void)
{
    if (s_disp == NULL) {
        return;
    }
    lvgl_port_lock(0);
    wake();
    lvgl_port_unlock();
}
