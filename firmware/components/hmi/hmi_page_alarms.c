/*
 * Alarms (mockup Alarm.dc.html): the current alarm list or the history on the left, the details
 * of the selected alarm (cause, remedy) on the right.
 *
 * Alarms come from machine_alarm.h (raised by main/app_alarms.c). Reset sends a fault reset to
 * every axis that is in fault or was stopped by an error; the alarms then clear by themselves
 * once their condition is gone. Export to SD is not available yet (no SD card driver).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "ecat_axis.h"
#include "hmi_internal.h"
#include "machine_alarm.h"

#define POLL_MS       250
#define ROW_H         60
#define DETAIL_W      290
#define GAP           10 /* between the columns of a row and of the column header */
#define TILE          36
#define COL_TIME_W    92 /* "10-10 18:07" at 15 px */
#define COL_STATUS_W  128 /* "Acknowledged" tag */
#define ACK_W         56  /* tick button */
#define ACCENT_10_OPA 26
#define MAX_ROWS      (MACHINE_ALARM_ACTIVE_MAX + MACHINE_ALARM_HISTORY_MAX)
#define MAX_SHOWN     60 /* rows built at once: each is about a dozen objects */

static bool s_history;   /* tab: kept while the firmware runs */
static uint32_t s_sel_id;
static machine_alarm_t *s_rows; /* MAX_ROWS entries in PSRAM-backed LVGL heap, allocated on first use */
static int s_n;
static EXT_RAM_BSS_ATTR ecat_axes_status_t s_ax;

static struct {
    lv_obj_t *root;
    lv_obj_t *list;
    lv_obj_t *detail;
    lv_obj_t *active_pill_label;
    lv_timer_t *poll;
    uint32_t gen;
} s_a;

static void build(void);

/* ---- Helpers ---- */

static void stamp_text(int64_t t, bool with_date, char *out, size_t len)
{
    if (t < 0) {
        snprintf(out, len, "+%lld s", (long long)-t); /* clock not set: seconds since start-up */
        return;
    }
    const time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, len, with_date ? "%m-%d %H:%M" : "%H:%M:%S", &tm);
}

static void status_style(const machine_alarm_t *a, bool history, const char **text, uint32_t *bg, uint32_t *fg)
{
    if (history) {
        *text = "Cleared";
        *bg = HMI_WHITE;
        *fg = HMI_TEXT_2;
        return;
    }
    switch (machine_alarm_state(a)) {
    case MACHINE_ALARM_ST_ACTIVE:
        *text = "Active";
        *bg = HMI_CRITICAL_BG;
        *fg = HMI_CRITICAL;
        break;
    case MACHINE_ALARM_ST_ACK:
        *text = "Acknowledged";
        *bg = HMI_CAUTION_BG;
        *fg = HMI_CAUTION;
        break;
    default:
        *text = "Cleared";
        *bg = HMI_WHITE;
        *fg = HMI_TEXT_2;
        break;
    }
}

static lv_obj_t *sev_tile(lv_obj_t *parent, uint8_t sev, int32_t size)
{
    const bool err = sev == MACHINE_ALARM_ERROR;
    lv_obj_t *tile = hmi_box(parent, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(tile, size, size);
    lv_obj_set_style_radius(tile, 6, 0);
    lv_obj_set_style_bg_color(tile, HMI_C(err ? HMI_CRITICAL_BG : HMI_CAUTION_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(tile, err ? HMI_ICON_ERROR : HMI_ICON_WARNING, size >= 40 ? 22 : 20, HMI_TEXT);
    return tile;
}

static const machine_alarm_t *selected(void)
{
    for (int i = 0; i < s_n; i++) {
        if (s_rows[i].id == s_sel_id) {
            return &s_rows[i];
        }
    }
    return s_n ? &s_rows[0] : NULL;
}

static bool row_is_history(const machine_alarm_t *a)
{
    return s_history && a->ended != 0;
}

/* The History tab has no Acknowledge column: its text gets the room (acknowledge from the Active
 * tab or the details panel). */
static bool ack_column(void)
{
    return !s_history;
}

/* One line, "..." when too long, so a row never grows or clips (DOTS alone wraps while the height
 * follows the text). The details panel shows the whole text. */
static lv_obj_t *line_label(lv_obj_t *parent, const char *text, int px, hmi_weight_t wt, uint32_t color)
{
    lv_obj_t *l = hmi_label(parent, text, px, wt, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(hmi_font(px, wt)));
    return l;
}

/* ---- Actions ---- */

static void ack_cb(lv_event_t *e)
{
    machine_alarm_ack((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void ack_all_cb(lv_event_t *e)
{
    (void)e;
    machine_alarm_ack_all();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    ecat_axes_read(&s_ax);
    for (uint8_t i = 0; s_ax.in_op && i < s_ax.n_axes; i++) {
        const ecat_axis_status_t *a = &s_ax.axis[i];
        const bool fault = a->cia_state == ECAT_CIA_FAULT || a->cia_state == ECAT_CIA_FAULT_REACTION;
        if (a->available && (fault || a->error != ECAT_AXERR_NONE)) {
            const ecat_axis_cmd_t c = {.axis = i, .code = ECAT_AXIS_RESET};
            (void)ecat_axis_command(&c);
        }
    }
}

static void select_async(void *arg)
{
    if (s_a.root) {
        s_sel_id = (uint32_t)(uintptr_t)arg;
        build();
    }
}

static void row_clicked_cb(lv_event_t *e)
{
    lv_async_call(select_async, lv_event_get_user_data(e)); /* the row is deleted by the rebuild */
}

static void tab_async(void *arg)
{
    if (s_a.root) {
        s_history = arg != NULL;
        s_sel_id = 0;
        build();
    }
}

static void tab_cb(lv_event_t *e)
{
    lv_async_call(tab_async, lv_event_get_user_data(e));
}

static void export_cb(lv_event_t *e)
{
    (void)e;
    hmi_confirm("Export not available", "There is no SD card driver in the firmware yet.", "OK", false, NULL, NULL);
}

/* ---- Layout ---- */

static void header(int n_active)
{
    lv_obj_t *row = hmi_page_header(s_a.root, "Alarms", NULL, HMI_TEXT_2);
    lv_obj_t *pills = hmi_box(row, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_style_pad_left(pills, 4, 0);
    char t[24];
    snprintf(t, sizeof(t), "Active (%d)", n_active);
    lv_obj_t *p = hmi_pill(pills, t, !s_history, true);
    lv_obj_add_event_cb(p, tab_cb, LV_EVENT_CLICKED, NULL);
    p = hmi_pill(pills, "History", s_history, true);
    lv_obj_add_event_cb(p, tab_cb, LV_EVENT_CLICKED, (void *)1);
    hmi_spacer(row);

    lv_obj_t *b = hmi_control(row, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(b, 16, 0);
    hmi_icon(b, HMI_ICON_CHECK, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(b, "Acknowledge all", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(b, ack_all_cb, LV_EVENT_CLICKED, NULL);

    b = hmi_filled_button(row, 44, HMI_ACCENT, 4);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 16, 0);
    hmi_icon(b, HMI_ICON_RESET, HMI_ICON_BTN, HMI_WHITE);
    hmi_label(b, "Reset", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(b, reset_cb, LV_EVENT_CLICKED, NULL);

    b = hmi_control(row, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(b, 16, 0);
    hmi_label(b, "Export", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_OFF);
    lv_obj_add_event_cb(b, export_cb, LV_EVENT_CLICKED, NULL);
}

static void list_row(const machine_alarm_t *a, bool sel)
{
    const bool hist = row_is_history(a);
    lv_obj_t *row = hmi_box(s_a.list, LV_FLEX_FLOW_ROW, GAP);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_border_opa(row, HMI_STROKE_OPA, 0);
    /* Selected: accent at 10 % on white, opaque. Translucent, it showed the darkened background
     * through and looked grey. */
    lv_obj_set_style_bg_color(
        row, sel ? lv_color_mix(HMI_C(HMI_ACCENT), HMI_C(HMI_WHITE), ACCENT_10_OPA) : HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (sel) {
        lv_obj_t *bar = hmi_box(row, -1, 0);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(bar, 3, 16);
        lv_obj_set_pos(bar, -11, (ROW_H - 16) / 2 - 1);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, HMI_C(HMI_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }

    lv_obj_t *pick = hmi_box(row, LV_FLEX_FLOW_ROW, GAP);
    lv_obj_add_flag(pick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(pick, LV_PCT(100));
    lv_obj_set_flex_grow(pick, 1);
    lv_obj_set_flex_align(pick, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(pick, row_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)a->id);
    sev_tile(pick, a->sev, TILE);

    lv_obj_t *text = hmi_box(pick, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_width(line_label(text, a->title, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT), LV_PCT(100));
    char t[96];
    snprintf(t, sizeof(t), "%s \xC2\xB7 %s", a->code, a->source);
    lv_obj_set_width(line_label(text, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), LV_PCT(100));

    stamp_text(s_history ? (hist ? a->ended : a->raised) : a->raised, s_history, t, sizeof(t));
    lv_obj_set_width(line_label(pick, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), COL_TIME_W);

    const char *st;
    uint32_t bg, fg;
    status_style(a, hist, &st, &bg, &fg);
    lv_obj_t *cell = hmi_box(pick, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(cell, COL_STATUS_W);
    hmi_tag_set(hmi_tag(cell, false), st, bg, fg);

    if (!ack_column()) {
        return;
    }
    if (!hist && !a->acked) {
        lv_obj_t *ack = hmi_control(row, ACK_W, 46);
        hmi_icon(ack, HMI_ICON_CHECK, 24, HMI_ACCENT);
        lv_obj_add_event_cb(ack, ack_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)a->id);
    } else {
        lv_obj_set_width(hmi_box(row, -1, 0), ACK_W); /* nothing to acknowledge: keeps the columns aligned */
    }
}

static void kv(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *r = hmi_box(parent, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_t *k = hmi_label(r, key, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(k, 56);
    lv_obj_t *v = hmi_label(r, value, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(v, 1);
}

static void detail(const machine_alarm_t *a)
{
    lv_obj_t *c = hmi_card(s_a.detail, 14, 16, 12);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
    if (a == NULL) {
        hmi_label(c, s_history ? "No alarms in the history since start-up." : "No active alarms.", HMI_FS_BODY, HMI_REGULAR,
                  HMI_TEXT_2);
        return;
    }
    const bool hist = row_is_history(a);
    const bool err = a->sev == MACHINE_ALARM_ERROR;

    lv_obj_t *top = hmi_box(c, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sev_tile(top, a->sev, 40);
    lv_obj_t *col = hmi_box(top, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_t *code = hmi_box(col, LV_FLEX_FLOW_ROW, 0);
    char t[96];
    snprintf(t, sizeof(t), "%s \xC2\xB7 ", a->code);
    hmi_label(code, t, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(code, err ? "Error" : "Warning", HMI_FS_BODY, HMI_SEMIBOLD, err ? HMI_CRITICAL : HMI_CAUTION);
    lv_obj_t *title = hmi_label(col, a->title, HMI_FS_LARGE, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t *info = hmi_box(c, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(info, LV_PCT(100));
    stamp_text(a->raised, true, t, sizeof(t));
    kv(info, "Time", t);
    if (hist) {
        stamp_text(a->ended, true, t, sizeof(t));
        kv(info, "Ended", t);
    }
    kv(info, "Source", a->source);
    lv_obj_t *sr = hmi_box(info, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(sr, "Status", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), 56);
    const char *st;
    uint32_t bg, fg;
    status_style(a, hist, &st, &bg, &fg);
    hmi_tag_set(hmi_tag(sr, false), st, bg, fg);

    lv_obj_t *cause = hmi_box(c, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(cause, LV_PCT(100));
    lv_obj_set_style_pad_top(cause, 12, 0);
    hmi_divider_top(cause);
    hmi_label(cause, "Cause", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(hmi_label(cause, a->cause, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), LV_PCT(100));
    lv_obj_t *rem = hmi_box(c, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(rem, LV_PCT(100));
    hmi_label(rem, "Remedy", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(hmi_label(rem, a->remedy, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), LV_PCT(100));

    hmi_spacer(c);
    const bool can_ack = !hist && !a->acked;
    lv_obj_t *b;
    if (can_ack) {
        b = hmi_filled_button(c, HMI_TOUCH_H, HMI_ACCENT, 4);
        hmi_label(b, "Acknowledge", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
        lv_obj_add_event_cb(b, ack_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)a->id);
    } else {
        b = hmi_box(c, LV_FLEX_FLOW_ROW, 0);
        lv_obj_set_size(b, LV_PCT(100), 44);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(b, HMI_STROKE_OPA, 0);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_label(b, "Acknowledge", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT_OFF);
    }
}

static void build(void)
{
    lv_obj_t *root = s_a.root;
    lv_timer_t *poll = s_a.poll;
    const int32_t scroll = s_a.list ? lv_obj_get_scroll_y(s_a.list) : 0;
    memset(&s_a, 0, sizeof(s_a));
    s_a.root = root;
    s_a.poll = poll;
    lv_obj_clean(root);

    if (s_rows == NULL) {
        s_rows = lv_malloc(sizeof(machine_alarm_t) * MAX_ROWS);
        LV_ASSERT_MALLOC(s_rows);
    }
    s_a.gen = machine_alarm_gen();
    const int n_active = machine_alarm_list(s_rows, MAX_ROWS);
    s_n = n_active;
    if (s_history) {
        s_n += machine_alarm_history(s_rows + s_n, MAX_ROWS - s_n);
    }

    header(n_active);

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);

    lv_obj_t *left = hmi_box(body, LV_FLEX_FLOW_COLUMN, 6);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 1);
    lv_obj_t *h = hmi_box(left, LV_FLEX_FLOW_ROW, GAP);
    lv_obj_set_size(h, LV_PCT(100), 16);
    lv_obj_set_style_pad_left(h, 11, 0); /* row padding + border */
    lv_obj_set_style_pad_right(h, 9 + (ack_column() ? GAP + ACK_W : 0), 0);
    lv_obj_t *l = hmi_label(h, "Alarm", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_width(hmi_label(h, "Time", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), COL_TIME_W);
    lv_obj_set_width(hmi_label(h, "Status", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), COL_STATUS_W);

    s_a.list = hmi_box(left, LV_FLEX_FLOW_COLUMN, 6);
    hmi_scroll_area(s_a.list);
    lv_obj_set_width(s_a.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_a.list, 1);

    const machine_alarm_t *sel = selected();
    if (sel) {
        s_sel_id = sel->id;
    }
    for (int i = 0; i < s_n && i < MAX_SHOWN; i++) {
        list_row(&s_rows[i], sel == &s_rows[i]);
    }
    if (s_n > MAX_SHOWN) {
        char t[64];
        snprintf(t, sizeof(t), "%d older entries are not shown.", s_n - MAX_SHOWN);
        hmi_label(s_a.list, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    }
    if (s_n == 0) {
        hmi_info_card(s_a.list, s_history ? "No history" : "No active alarms",
                      s_history ? "Alarms that ended since start-up appear here (the history is not kept over a reset)."
                                : "Everything is running as expected.",
                      NULL, NULL);
    }

    s_a.detail = hmi_box(body, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_size(s_a.detail, DETAIL_W, LV_PCT(100));
    detail(sel);
    if (scroll > 0) {
        lv_obj_update_layout(s_a.list);
        lv_obj_scroll_to_y(s_a.list, scroll, LV_ANIM_OFF); /* a rebuild keeps the list where it was */
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (machine_alarm_gen() != s_a.gen) {
        build();
    }
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_a.poll) {
        lv_timer_delete(s_a.poll);
    }
    memset(&s_a, 0, sizeof(s_a));
}

void hmi_page_alarms_create(lv_obj_t *page)
{
    memset(&s_a, 0, sizeof(s_a));
    s_a.root = hmi_box(page, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(s_a.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(s_a.root, deleted_cb, LV_EVENT_DELETE, NULL);
    build();
    s_a.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
