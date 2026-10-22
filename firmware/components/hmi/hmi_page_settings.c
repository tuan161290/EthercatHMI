/*
 * Settings: one tile per settings page with the user level it needs (tiles instead of the
 * mockup's rows, user 2026-10-11: easier to hit on the touch screen). Built: Time
 * (hmi_page_time.c), Servo (hmi_page_servo.c), EtherCAT (hmi_page_ethercat.c), Network
 * (hmi_page_network.c), System (display settings and screen timeouts, here) and System info
 * (hmi_page_info.c). Recipe, User & password, Maintenance and Backup / restore have no mockup yet
 * and open a placeholder. Sub-pages keep Settings selected on the rail and show the
 * breadcrumb "Settings › Page" (docs/HMI_THEME.md section 1).
 */

#include <stdint.h>
#include <stdio.h>
#include "hmi_internal.h"

#define TILE_COLS     4
#define TILE_GAP      12
#define TILE_H        150
#define TILE_ICON     52
#define SLIDER_ROW_H  52
#define LABEL_W       96
#define VALUE_W       64
#define SIDE_W        320 /* screen timeouts and test pattern */
#define FIELD_LABEL_W 132
#define SWATCH_H      64
#define ACCENT_10_OPA 26 /* accent at 10 %: icon tile of a page that exists (mockup accentSoft) */
#define POLL_MS       500

typedef enum {
    SUB_LIST = -1,
    SUB_TIME = HMI_SET_TIME,
    SUB_SERVO = HMI_SET_SERVO,
    SUB_RECIPE = HMI_SET_RECIPE,
    SUB_USERS = HMI_SET_USERS,
    SUB_MAINTENANCE = HMI_SET_MAINTENANCE,
    SUB_ETHERCAT = HMI_SET_ETHERCAT,
    SUB_NETWORK = HMI_SET_NETWORK,
    SUB_SYSTEM = HMI_SET_SYSTEM,
    SUB_INFO = HMI_SET_INFO,
    SUB_BACKUP = HMI_SET_BACKUP,
    SUB_COUNT = HMI_SET_COUNT,
} sub_t;

static const struct {
    const char *title; /* tile */
    const char *crumb; /* breadcrumb */
    const char *caption; /* tile, two short lines at most */
    const char *level;
    hmi_icon_t icon;
} s_subs[SUB_COUNT] = {
    [SUB_TIME] = {"Time", "Time", "Timers, date and clock", "Engineer", HMI_ICON_CLOCK},
    [SUB_SERVO] = {"Servo", "Servo", "Speed, limits and homing per axis", "Engineer", HMI_ICON_GAUGE},
    [SUB_RECIPE] = {"Recipe", "Recipe", "Products and their parameters", "Operator", HMI_ICON_DOCUMENT},
    [SUB_USERS] = {"Users", "User & password", "Login and access levels", "Admin", HMI_ICON_USER},
    [SUB_MAINTENANCE] = {"Maintenance", "Maintenance", "Run hours and service", "Engineer", HMI_ICON_WRENCH},
    [SUB_ETHERCAT] = {"EtherCAT", "EtherCAT", "Bus scan, topology, timing", "Engineer", HMI_ICON_NETWORK},
    [SUB_NETWORK] = {"Network", "Network", "Wi-Fi and internet time", "Admin", HMI_ICON_WIFI},
    [SUB_SYSTEM] = {"Display", "Display", "Brightness, colours, screen off", "Admin", HMI_ICON_MONITOR},
    [SUB_INFO] = {"System info", "System info", "Memory, firmware, uptime", "Admin", HMI_ICON_INFO},
    [SUB_BACKUP] = {"Backup", "Backup / restore", "Export and import (SD card)", "Admin", HMI_ICON_DOWNLOAD},
};

typedef enum {
    CH_BRIGHTNESS,
    CH_RED,
    CH_GREEN,
    CH_BLUE,
    CH_COUNT,
} ch_t;

static lv_obj_t *s_root; /* the whole Settings page; NULL once the shell has cleared it */

/* System page only; cleared when it is deleted */
static struct {
    lv_obj_t *slider;
    lv_obj_t *value;
} s_ch[CH_COUNT];
static lv_obj_t *s_dim_val, *s_off_val; /* screen timeouts */
static lv_obj_t *s_unsaved;
static lv_timer_t *s_poll;

static void show(sub_t sub);

/* ---- Navigation: rows and the breadcrumb sit inside s_root, so switch after their event ---- */

static void show_async_cb(void *arg)
{
    if (s_root) {
        show((sub_t)(intptr_t)arg);
    }
}

static void go_cb(lv_event_t *e)
{
    lv_async_call(show_async_cb, lv_event_get_user_data(e));
}

static void root_deleted_cb(lv_event_t *e)
{
    (void)e;
    s_root = NULL;
}

/* ---- Pieces ---- */

static void level_pill(lv_obj_t *parent, const char *text)
{
    lv_obj_t *pill = hmi_box(parent, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_height(pill, 26);
    lv_obj_set_style_pad_hor(pill, 10, 0);
    lv_obj_set_style_radius(pill, 13, 0);
    lv_obj_set_style_bg_color(pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pill, HMI_STROKE_OPA, 0);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(pill, text, HMI_FS_SMALL, HMI_SEMIBOLD, HMI_TEXT_2);
}

static void divider_top(lv_obj_t *obj)
{
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_opa(obj, HMI_STROKE_OPA, 0);
}

/** "Settings › Page" in the page title row; "Settings" goes back to the list. */
static lv_obj_t *crumb_create(lv_obj_t *root, sub_t sub)
{
    lv_obj_t *row = hmi_box(root, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(row, LV_PCT(100), HMI_TOUCH_H);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back = hmi_box(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(back, HMI_TOUCH_H);
    lv_obj_set_style_pad_hor(back, 8, 0);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, HMI_TINT_OPA, LV_STATE_PRESSED);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(back, go_cb, LV_EVENT_CLICKED, (void *)(intptr_t)SUB_LIST);
    hmi_label(back, "Settings", HMI_TITLE_PX, HMI_SEMIBOLD, HMI_TEXT_2);

    hmi_icon(row, HMI_ICON_CHEVRON_RIGHT, 24, HMI_TEXT_2);
    hmi_label(row, s_subs[sub].crumb, HMI_TITLE_PX, HMI_SEMIBOLD, HMI_TEXT);
    return row;
}

/* ---- Tiles ---- */

static bool built(sub_t sub)
{
    return sub == SUB_TIME || sub == SUB_SERVO || sub == SUB_SYSTEM || sub == SUB_ETHERCAT || sub == SUB_NETWORK ||
           sub == SUB_INFO;
}

static void tile_create(lv_obj_t *grid, sub_t sub)
{
    const bool on = built(sub);
    lv_obj_t *t = hmi_box(grid, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(t, &hmi_st_card, 0);
    lv_obj_add_style(t, &hmi_st_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, (int32_t)sub % TILE_COLS, 1, LV_GRID_ALIGN_STRETCH,
                         (int32_t)sub / TILE_COLS, 1);
    lv_obj_add_event_cb(t, go_cb, LV_EVENT_CLICKED, (void *)(intptr_t)sub);

    lv_obj_t *top = hmi_box(t, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *icon = hmi_box(top, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(icon, TILE_ICON, TILE_ICON);
    lv_obj_set_style_radius(icon, 10, 0);
    lv_obj_set_style_bg_color(icon, on ? HMI_C(HMI_ACCENT) : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(icon, on ? ACCENT_10_OPA : HMI_STROKE_OPA, 0);
    lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(icon, s_subs[sub].icon, 26, on ? HMI_ACCENT : HMI_TEXT_2);
    level_pill(top, s_subs[sub].level);

    lv_obj_t *title = hmi_label(t, s_subs[sub].title, HMI_FS_LARGE, HMI_SEMIBOLD, on ? HMI_TEXT : HMI_TEXT_2);
    lv_obj_set_style_pad_top(title, 6, 0);
    lv_obj_t *cap = hmi_label(t, s_subs[sub].caption, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(cap, LV_PCT(100));
}

static void list_create(lv_obj_t *root)
{
    static const int32_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    hmi_page_header(root, "Settings", "Saved at once, also while EtherCAT is in OP", HMI_TEXT_2);
    lv_obj_t *grid = hmi_box(root, -1, 0);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, cols, rows);
    lv_obj_set_style_pad_row(grid, TILE_GAP, 0);
    lv_obj_set_style_pad_column(grid, TILE_GAP, 0);
    for (int i = 0; i < SUB_COUNT; i++) {
        tile_create(grid, (sub_t)i);
    }
}

/* ---- System: display ---- */

static uint8_t *ch_field(hmi_display_t *d, ch_t ch)
{
    switch (ch) {
    case CH_RED:
        return &d->red;
    case CH_GREEN:
        return &d->green;
    case CH_BLUE:
        return &d->blue;
    case CH_BRIGHTNESS:
    default:
        return &d->brightness;
    }
}

static void ch_show(ch_t ch, int v)
{
    if (lv_slider_get_value(s_ch[ch].slider) != v) {
        lv_slider_set_value(s_ch[ch].slider, v, LV_ANIM_OFF);
    }
    lv_label_set_text_fmt(s_ch[ch].value, "%d %%", v);
}

static void unsaved_update(void)
{
    if (hmi_display_save_pending()) {
        lv_obj_remove_flag(s_unsaved, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_unsaved, LV_OBJ_FLAG_HIDDEN);
    }
}

static void slider_cb(lv_event_t *e)
{
    const ch_t ch = (ch_t)(uintptr_t)lv_event_get_user_data(e);
    const int v = lv_slider_get_value(s_ch[ch].slider);
    lv_label_set_text_fmt(s_ch[ch].value, "%d %%", v);

    hmi_display_t d;
    hmi_display_get(&d);
    *ch_field(&d, ch) = (uint8_t)v;
    /* Live while dragging; written to NVS on release (only if it changed). */
    (void)hmi_display_set(&d, lv_event_get_code(e) == LV_EVENT_RELEASED);
    unsaved_update();
}

/* Brightness and colour balance only; the screen timeouts have their own card. */
static void reset_cb(lv_event_t *e)
{
    (void)e;
    hmi_display_t d, cur;
    hmi_display_get(&cur);
    hmi_display_defaults(&d);
    d.dim_s = cur.dim_s;
    d.off_s = cur.off_s;
    (void)hmi_display_set(&d, true);
    for (int ch = 0; ch < CH_COUNT; ch++) {
        ch_show((ch_t)ch, *ch_field(&d, (ch_t)ch));
    }
    unsaved_update();
}

/* ---- System: screen timeouts ---- */

static const uint16_t s_timeouts[] = {0, 30, 60, 120, 300, 600, 900, 1800, 3600}; /* s, 0 = never */
#define N_TIMEOUTS ((int)(sizeof(s_timeouts) / sizeof(s_timeouts[0])))
static const char s_timeout_opts[] = "Never\n30 s\n1 min\n2 min\n5 min\n10 min\n15 min\n30 min\n1 hour";

static void timeout_text(uint16_t s, char *out, size_t len)
{
    if (s == 0) {
        snprintf(out, len, "Never");
    } else if (s % 3600 == 0) {
        snprintf(out, len, "%u hour", s / 3600u);
    } else if (s % 60 == 0) {
        snprintf(out, len, "%u min", s / 60u);
    } else {
        snprintf(out, len, "%u s", s);
    }
}

/* Option to preselect: the set value, or the next longer one (the console takes any value). */
static int timeout_index(uint16_t s)
{
    for (int i = 0; i < N_TIMEOUTS; i++) {
        if (s_timeouts[i] >= s) {
            return i;
        }
    }
    return N_TIMEOUTS - 1;
}

static void timeouts_show(const hmi_display_t *d)
{
    char t[16];
    timeout_text(d->dim_s, t, sizeof(t));
    hmi_set_text(s_dim_val, t);
    timeout_text(d->off_s, t, sizeof(t));
    hmi_set_text(s_off_val, t);
}

/* `user` != NULL: Turn off after; else Dim after. */
static void timeout_set_cb(int index, void *user)
{
    if (index < 0 || index >= N_TIMEOUTS) {
        return;
    }
    hmi_display_t d;
    hmi_display_get(&d);
    if (user) {
        d.off_s = s_timeouts[index];
    } else {
        d.dim_s = s_timeouts[index];
    }
    (void)hmi_display_set(&d, true);
    if (s_dim_val) {
        timeouts_show(&d);
        unsaved_update();
    }
}

static void timeout_cb(lv_event_t *e)
{
    void *off = lv_event_get_user_data(e);
    hmi_display_t d;
    hmi_display_get(&d);
    hmi_choice(off ? "Turn off after" : "Dim after", s_timeout_opts, timeout_index(off ? d.off_s : d.dim_s),
               timeout_set_cb, off);
}

/* Name and a combo (as Time zone on the Time page). @return the value label */
static lv_obj_t *timeout_field(lv_obj_t *card, const char *name, bool off)
{
    lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(r, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), FIELD_LABEL_W);
    lv_obj_t *b = hmi_control(r, 0, 44);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_t *v = hmi_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    hmi_icon(b, HMI_ICON_CHEVRON_DOWN, 16, HMI_TEXT_2);
    lv_obj_add_event_cb(b, timeout_cb, LV_EVENT_CLICKED, off ? (void *)1 : NULL);
    return v;
}

static void screen_card(lv_obj_t *side)
{
    lv_obj_t *card = hmi_card(side, 10, 16, 12);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_t *head = hmi_box(card, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_width(head, LV_PCT(100));
    hmi_label(head, "Screen", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_t *note = hmi_label(head, "Counted from the last touch. A touch on a dimmed or dark screen only lights it.",
                               HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(note, LV_PCT(100));
    s_dim_val = timeout_field(card, "Dim after", false);
    s_off_val = timeout_field(card, "Turn off after", true);
}

/* Follows changes made from the console ('display' command) and the deferred save. */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    hmi_display_t d;
    hmi_display_get(&d);
    for (int ch = 0; ch < CH_COUNT; ch++) {
        if (!lv_obj_has_state(s_ch[ch].slider, LV_STATE_PRESSED)) {
            ch_show((ch_t)ch, *ch_field(&d, (ch_t)ch));
        }
    }
    timeouts_show(&d);
    unsaved_update();
}

static void system_deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    s_unsaved = NULL;
    s_dim_val = NULL;
    s_off_val = NULL;
    for (int ch = 0; ch < CH_COUNT; ch++) {
        s_ch[ch].slider = NULL;
        s_ch[ch].value = NULL;
    }
}

/* Fluent slider: 4 px rail, coloured fill, white 20 px thumb with a coloured centre. */
static void slider_row(lv_obj_t *card, ch_t ch, const char *name, uint32_t color, int min)
{
    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_size(row, LV_PCT(100), SLIDER_ROW_H);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    divider_top(row);

    lv_obj_t *label = hmi_label(row, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_width(label, LABEL_W);

    lv_obj_t *s = lv_slider_create(row);
    lv_obj_remove_style_all(s);
    lv_obj_set_height(s, 6);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_ext_click_area(s, 24);
    lv_slider_set_range(s, min, 100);
    lv_obj_set_style_radius(s, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, HMI_C(HMI_RING), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, HMI_C(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s, 11, LV_PART_KNOB); /* 6 px rail + 2 x 11 = 28 px thumb */
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, HMI_C(color), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 6, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, HMI_C(HMI_WHITE), LV_PART_KNOB);
    lv_obj_set_style_border_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_outline_width(s, 1, LV_PART_KNOB);
    lv_obj_set_style_outline_color(s, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_outline_opa(s, LV_OPA_20, LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 7, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)ch);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)ch);

    lv_obj_t *value = hmi_label(row, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_width(value, VALUE_W);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    s_ch[ch].slider = s;
    s_ch[ch].value = value;
}

static void display_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 10, 16, 0);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);

    lv_obj_t *title = hmi_label(card, "Display", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_style_pad_bottom(title, 6, 0);
    slider_row(card, CH_BRIGHTNESS, "Brightness", HMI_ACCENT, HMI_BRIGHTNESS_MIN);

    lv_obj_t *sec = hmi_box(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_top(sec, 14, 0);
    lv_obj_set_style_pad_bottom(sec, 6, 0);
    hmi_label(sec, "Colour balance", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(sec, "Lower a channel to take a colour tint out of white.", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    slider_row(card, CH_RED, "Red", HMI_CRITICAL, HMI_GAIN_MIN);
    slider_row(card, CH_GREEN, "Green", HMI_SUCCESS, HMI_GAIN_MIN);
    slider_row(card, CH_BLUE, "Blue", 0x005FB8, HMI_GAIN_MIN);

    hmi_spacer(card);
    lv_obj_t *reset = hmi_control(card, LV_SIZE_CONTENT, HMI_TOUCH_H);
    lv_obj_set_style_pad_hor(reset, 14, 0);
    lv_obj_add_event_cb(reset, reset_cb, LV_EVENT_CLICKED, NULL);
    hmi_icon(reset, HMI_ICON_RESET, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(reset, "Reset to default", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
}

static void swatch_row(lv_obj_t *card, const uint32_t *colors, int n)
{
    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(row, LV_PCT(100), SWATCH_H);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_border_opa(row, HMI_STROKE_OPA, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t *s = hmi_box(row, -1, 0);
        lv_obj_set_height(s, LV_PCT(100));
        lv_obj_set_flex_grow(s, 1);
        lv_obj_set_style_bg_color(s, HMI_C(colors[i]), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    }
}

static void pattern_card(lv_obj_t *side)
{
    static const uint32_t greys[] = {0xFFFFFF, 0xDADADA, 0xB6B6B6, 0x919191, 0x6D6D6D, 0x484848, 0x242424, 0x000000};
    static const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0x00FFFF, 0xFF00FF, 0xFFFF00};

    lv_obj_t *card = hmi_card(side, 10, 16, 12);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    hmi_label(card, "Test pattern", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    swatch_row(card, greys, sizeof(greys) / sizeof(greys[0]));
    swatch_row(card, colors, sizeof(colors) / sizeof(colors[0]));
    lv_obj_t *note = hmi_label(card, "White and the greys should look neutral, without a colour tint.", HMI_FS_SMALL, HMI_REGULAR,
                               HMI_TEXT_2);
    lv_obj_set_width(note, LV_PCT(100));
}

static void system_create(lv_obj_t *root)
{
    lv_obj_t *head = crumb_create(root, SUB_SYSTEM);
    hmi_spacer(head);
    s_unsaved = hmi_box(head, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_height(s_unsaved, 24);
    lv_obj_set_style_pad_hor(s_unsaved, 10, 0);
    lv_obj_set_style_radius(s_unsaved, 12, 0);
    lv_obj_set_style_bg_color(s_unsaved, HMI_C(HMI_CAUTION_BG), 0);
    lv_obj_set_style_bg_opa(s_unsaved, LV_OPA_COVER, 0);
    lv_obj_set_flex_align(s_unsaved, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *dot = hmi_box(s_unsaved, -1, 0);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, HMI_C(HMI_CAUTION), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    hmi_label(s_unsaved, "Not saved yet: EtherCAT is in OP", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_CAUTION);

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_event_cb(body, system_deleted_cb, LV_EVENT_DELETE, NULL);
    display_card(body);
    lv_obj_t *side = hmi_box(body, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(side, SIDE_W, LV_PCT(100));
    screen_card(side);
    pattern_card(side);

    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    poll_cb(NULL);
}

/* ---- Public ---- */

static void show(sub_t sub)
{
    lv_obj_clean(s_root);
    if (sub == SUB_LIST) {
        list_create(s_root);
    } else if (sub == SUB_SYSTEM) {
        system_create(s_root);
    } else if (sub == SUB_ETHERCAT) {
        hmi_ethercat_create(s_root, crumb_create(s_root, sub));
    } else if (sub == SUB_TIME) {
        hmi_time_create(s_root, crumb_create(s_root, sub));
    } else if (sub == SUB_SERVO) {
        hmi_servo_create(s_root, crumb_create(s_root, sub));
    } else if (sub == SUB_NETWORK) {
        hmi_network_create(s_root, crumb_create(s_root, sub));
    } else if (sub == SUB_INFO) {
        hmi_info_create(s_root, crumb_create(s_root, sub));
    } else {
        crumb_create(s_root, sub);
        hmi_todo_card(s_root);
    }
}

/* After the event that asked for it: the shell clears the page the caller lives in. */
static void open_async(void *arg)
{
    hmi_shell_show(HMI_PAGE_SETTINGS);
    if (s_root) {
        show((sub_t)(intptr_t)arg);
    }
}

void hmi_settings_open(hmi_settings_sub_t sub)
{
    if (sub >= 0 && sub < HMI_SET_COUNT) {
        lv_async_call(open_async, (void *)(intptr_t)sub);
    }
}

void hmi_page_settings_create(lv_obj_t *page)
{
    s_root = hmi_box(page, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(s_root, root_deleted_cb, LV_EVENT_DELETE, NULL);
    show(SUB_LIST);
}
