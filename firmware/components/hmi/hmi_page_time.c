/*
 * Settings > Time (mockup TimeSetting.dc.html): sequence timers T01..T08 on the left, date,
 * time and time zone on the right.
 *
 * Timers are edited with the keypad and stored with Save (machine_params.h; while EtherCAT is in
 * OP the write waits until it leaves OP). The clock is set at once through the date/time picker:
 * system clock and RX8025T RTC (machine_clock.h). The Internet time row switches NTP over Wi-Fi
 * on or off (components/net); Wi-Fi and the server are set in Settings > Network.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "hmi_internal.h"
#include "machine_clock.h"
#include "machine_params.h"
#include "machine_store.h"
#include "net.h"

#define POLL_MS     500
#define RIGHT_W     340
#define VALUE_W     132
#define TZ_STEP_MIN 15
#define TZ_COUNT    ((MACHINE_TZ_MAX_MINUTES - MACHINE_TZ_MIN_MINUTES) / TZ_STEP_MIN + 1)

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *unsaved;
    lv_obj_t *box[MACHINE_TIMER_COUNT];
    lv_obj_t *val[MACHINE_TIMER_COUNT];
    lv_obj_t *big_time;
    lv_obj_t *date_val, *time_val, *tz_val;
    lv_obj_t *ntp_val, *ntp_sw;
    lv_obj_t *message;
    lv_timer_t *poll;
    uint32_t ms[MACHINE_TIMER_COUNT];
    bool changed[MACHINE_TIMER_COUNT];
    bool dirty;
} s_tm;

/* ---- Timers ---- */

static void box_style(int i)
{
    hmi_field_style(s_tm.box[i], s_tm.val[i], s_tm.changed[i] ? HMI_FIELD_CHANGED : HMI_FIELD_EDIT);
}

static void value_show(int i)
{
    char t[16];
    snprintf(t, sizeof(t), "%.2f", s_tm.ms[i] / 1000.0);
    hmi_set_text(s_tm.val[i], t);
    box_style(i);
}

static void unsaved_show(void)
{
    if (s_tm.dirty) {
        hmi_tag_set(s_tm.unsaved, "Unsaved changes", HMI_CAUTION_BG, HMI_CAUTION);
        hmi_show(s_tm.unsaved, true);
    } else if (machine_store_key_pending("timers")) {
        hmi_tag_set(s_tm.unsaved, "Not saved yet: EtherCAT is in OP", HMI_CAUTION_BG, HMI_CAUTION);
        hmi_show(s_tm.unsaved, true);
    } else {
        hmi_show(s_tm.unsaved, false);
    }
}

static void timer_set_cb(double value, void *user)
{
    const int i = (int)(intptr_t)user;
    if (s_tm.poll == NULL) {
        return; /* the page was left while the keypad was open */
    }
    const uint32_t ms = (uint32_t)(value * 1000.0 + 0.5);
    if (ms != s_tm.ms[i]) {
        s_tm.ms[i] = ms;
        s_tm.changed[i] = true;
        s_tm.dirty = true;
    }
    value_show(i);
    unsaved_show();
}

static void timer_cb(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const machine_timer_def_t *d = machine_timer_def(i);
    char title[48];
    snprintf(title, sizeof(title), "T%02d \xC2\xB7 %s", i + 1, d->name);
    hmi_keypad(title, "s", s_tm.ms[i] / 1000.0, d->min_ms / 1000.0, d->max_ms / 1000.0, 2, timer_set_cb,
               (void *)(intptr_t)i);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (machine_timers_set(s_tm.ms) != ESP_OK) {
        hmi_set_text(s_tm.message, "Saving the timers failed: see the console log.");
        return;
    }
    s_tm.dirty = false;
    for (int i = 0; i < MACHINE_TIMER_COUNT; i++) {
        s_tm.changed[i] = false;
        value_show(i);
    }
    unsaved_show();
}

static void timers_card(lv_obj_t *body)
{
    lv_obj_t *c = hmi_card(body, 10, 16, 6);
    lv_obj_set_style_pad_bottom(c, 4, 0);
    lv_obj_set_height(c, LV_PCT(100));
    lv_obj_set_flex_grow(c, 1);
    lv_obj_t *title = hmi_label(c, "Sequence timers", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_height(title, 24);

    lv_obj_t *rows = hmi_box(c, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(rows, LV_PCT(100));
    lv_obj_set_flex_grow(rows, 1);
    for (int i = 0; i < MACHINE_TIMER_COUNT; i++) {
        const machine_timer_def_t *d = machine_timer_def(i);
        lv_obj_t *r = hmi_box(rows, LV_FLEX_FLOW_ROW, 12);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_flex_grow(r, 1);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_divider_top(r);

        char t[48];
        snprintf(t, sizeof(t), "T%02d", i + 1);
        lv_obj_set_width(hmi_label(r, t, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT_2), 40);
        lv_obj_t *col = hmi_box(r, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_flex_grow(col, 1);
        hmi_label(col, d->name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
        snprintf(t, sizeof(t), "Range %.2f \xE2\x80\x93 %.2f s", d->min_ms / 1000.0, d->max_ms / 1000.0);
        hmi_label(col, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);

        s_tm.box[i] = hmi_control(r, VALUE_W, 40);
        lv_obj_set_flex_align(s_tm.box[i], LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(s_tm.box[i], 12, 0);
        lv_obj_set_style_pad_column(s_tm.box[i], 6, 0);
        s_tm.val[i] = hmi_label(s_tm.box[i], "", HMI_FS_LARGE, HMI_REGULAR, HMI_TEXT);
        hmi_label(s_tm.box[i], "s", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_add_event_cb(s_tm.box[i], timer_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        value_show(i);
    }
}

/* ---- Clock ---- */

static void clock_update(void)
{
    char t[24], d[24];
    if (machine_clock_valid()) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(t, sizeof(t), "%H:%M:%S", &tm);
        strftime(d, sizeof(d), "%Y-%m-%d", &tm);
    } else {
        strlcpy(t, "--:--:--", sizeof(t));
        strlcpy(d, "Not set", sizeof(d));
    }
    hmi_set_text(s_tm.big_time, t);
    hmi_set_text(s_tm.time_val, t);
    hmi_set_text(s_tm.date_val, d);
    machine_clock_tz_text(machine_clock_tz_minutes(), t, sizeof(t));
    hmi_set_text(s_tm.tz_val, t);
}

static void clock_set_cb(const struct tm *local, void *user)
{
    (void)user;
    if (s_tm.poll == NULL) {
        return;
    }
    const esp_err_t err = machine_clock_set_local(local);
    if (err == ESP_OK) {
        hmi_set_text(s_tm.message, "Clock set. The RTC keeps it while the power is off.");
    } else if (err == ESP_ERR_NOT_FOUND) {
        hmi_set_text(s_tm.message, "Clock set, but there is no RTC: it is lost at the next reset.");
    } else {
        hmi_set_text(s_tm.message, "Setting the clock failed: see the console log.");
    }
    clock_update();
}

static void set_clock_cb(lv_event_t *e)
{
    (void)e;
    struct tm tm;
    const time_t now = time(NULL);
    localtime_r(&now, &tm);
    hmi_datetime("Set date and time", &tm, clock_set_cb, NULL);
}

static void tz_set_cb(int index, void *user)
{
    (void)user;
    if (s_tm.poll == NULL) {
        return;
    }
    const int minutes = MACHINE_TZ_MIN_MINUTES + index * TZ_STEP_MIN;
    if (machine_clock_set_tz(minutes) != ESP_OK) {
        hmi_set_text(s_tm.message, "Saving the time zone failed: see the console log.");
    } else if (machine_store_key_pending("tz")) {
        hmi_set_text(s_tm.message, "Time zone applied; it is saved when EtherCAT leaves OP.");
    } else {
        hmi_set_text(s_tm.message, "Time zone saved.");
    }
    clock_update();
}

static void tz_cb(lv_event_t *e)
{
    (void)e;
    static char opts[TZ_COUNT * 10 + 1];
    if (opts[0] == '\0') {
        char *w = opts;
        for (int i = 0; i < TZ_COUNT; i++) {
            char t[12];
            machine_clock_tz_text(MACHINE_TZ_MIN_MINUTES + i * TZ_STEP_MIN, t, sizeof(t));
            w += sprintf(w, "%s%s", t, i + 1 < TZ_COUNT ? "\n" : "");
        }
    }
    const int sel = (machine_clock_tz_minutes() - MACHINE_TZ_MIN_MINUTES) / TZ_STEP_MIN;
    hmi_choice("Time zone", opts, sel, tz_set_cb, NULL);
}

/* Internet time (Wi-Fi and the server are set in Settings > Network). */
static void ntp_show(void)
{
    net_status_t st;
    net_status(&st);
    hmi_switch_set(s_tm.ntp_sw, st.ntp_on);
    char t[32];
    if (!st.ntp_on) {
        snprintf(t, sizeof(t), "Off");
    } else if (st.last_sync) {
        struct tm l;
        localtime_r(&st.last_sync, &l);
        strftime(t, sizeof(t), "Synced %H:%M", &l);
    } else {
        snprintf(t, sizeof(t), st.state == NET_WIFI_CONNECTED ? "Waiting" : "Needs Wi-Fi");
    }
    hmi_set_text(s_tm.ntp_val, t);
}

static void ntp_toggle_cb(lv_event_t *e)
{
    (void)e;
    net_status_t st;
    net_status(&st);
    (void)net_set_ntp(!st.ntp_on, NULL);
    hmi_switch_set(s_tm.ntp_sw, !st.ntp_on);
    hmi_set_text(s_tm.message, st.ntp_on ? "Internet time off." : "Internet time on: Wi-Fi and the server are set in "
                                                                  "Settings \xE2\x80\xBA Network.");
}

static lv_obj_t *field(lv_obj_t *grid, const char *name, lv_event_cb_t cb, bool chevron)
{
    lv_obj_t *r = hmi_box(grid, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(r, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), 96);
    lv_obj_t *b = hmi_control(r, 0, 44);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_t *v = hmi_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    if (chevron) {
        hmi_icon(b, HMI_ICON_CHEVRON_DOWN, 16, HMI_TEXT_2);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return v;
}

static void clock_card(lv_obj_t *body)
{
    lv_obj_t *c = hmi_card(body, 10, 16, 6);
    lv_obj_set_style_pad_bottom(c, 12, 0);
    lv_obj_set_size(c, RIGHT_W, LV_PCT(100));
    lv_obj_set_height(hmi_label(c, "Date & time", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT), 24);
    s_tm.big_time = hmi_label(c, "", HMI_FS_BIG, HMI_SEMIBOLD, HMI_TEXT);

    lv_obj_t *grid = hmi_box(c, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_width(grid, LV_PCT(100));
    s_tm.date_val = field(grid, "Date", set_clock_cb, false);
    s_tm.time_val = field(grid, "Time", set_clock_cb, false);
    s_tm.tz_val = field(grid, "Time zone", tz_cb, true);

    lv_obj_t *rows = hmi_box(c, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(rows, LV_PCT(100));
    lv_obj_t *r = hmi_box(rows, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(r, LV_PCT(100), 40);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_divider_top(r);
    hmi_label(r, "Clock source", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    hmi_label(r, machine_clock_rtc_present() ? "On-board RTC" : "RTC not found", HMI_FS_BODY, HMI_REGULAR,
              machine_clock_rtc_present() ? HMI_TEXT_2 : HMI_CRITICAL);

    r = hmi_box(rows, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), 44);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, ntp_toggle_cb, LV_EVENT_CLICKED, NULL);
    hmi_divider_top(r);
    lv_obj_t *l = hmi_label(r, "Internet time", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(l, 1);
    s_tm.ntp_val = hmi_label(r, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    net_status_t st;
    net_status(&st);
    s_tm.ntp_sw = hmi_switch(r, st.ntp_on);

    s_tm.message = hmi_label(c, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_tm.message, LV_PCT(100));
    hmi_spacer(c);
    lv_obj_t *b = hmi_control(c, LV_PCT(100), HMI_TOUCH_H);
    hmi_icon(b, HMI_ICON_CLOCK, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(b, "Set clock", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(b, set_clock_cb, LV_EVENT_CLICKED, NULL);
    clock_update();
}

/* ---- Page ---- */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    clock_update();
    ntp_show();
    unsaved_show(); /* a save waiting for OP to end is written by machine_store */
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_tm.poll) {
        lv_timer_delete(s_tm.poll);
    }
    memset(&s_tm, 0, sizeof(s_tm));
}

void hmi_time_create(lv_obj_t *root, lv_obj_t *head)
{
    memset(&s_tm, 0, sizeof(s_tm));
    for (int i = 0; i < MACHINE_TIMER_COUNT; i++) {
        s_tm.ms[i] = machine_timer_ms(i);
    }

    hmi_spacer(head);
    s_tm.unsaved = hmi_unsaved_tag(head, "Unsaved changes");
    lv_obj_t *save = hmi_filled_button(head, 44, HMI_ACCENT, 4);
    lv_obj_set_width(save, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(save, 20, 0);
    hmi_icon(save, HMI_ICON_SAVE, HMI_ICON_BTN, HMI_WHITE);
    hmi_label(save, "Save", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_event_cb(body, deleted_cb, LV_EVENT_DELETE, NULL);
    timers_card(body);
    clock_card(body);
    unsaved_show();
    s_tm.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
