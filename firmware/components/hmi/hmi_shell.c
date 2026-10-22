/*
 * Shell of every page, as in the mockup (docs/HMI_THEME.md section 1):
 *   title bar 1024x56: app tile, machine name, status chips (mode, state, EtherCAT, CPU), time over
 *   date, user; navigation rail 96 px: Main, Manual, IO monitor, Alarms, and Settings at the
 *   bottom; page area 928x544, all on one solid grey background. Sizes for touch (2026-10-11).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_lvgl_port.h"
#include "hmi_internal.h"
#include "machine_alarm.h"
#include "machine_clock.h"

#define CHIP_H        36
#define NAV_ITEM_W    84
#define NAV_ITEM_H    76
#define NAV_LABEL_PX  HMI_FS_SMALL
/* CPU chip dot: amber from this load of the busier core, red from the next (%) */
#define CPU_CAUTION_PCT  70
#define CPU_CRITICAL_PCT 90

typedef enum {
    CHIP_NEUTRAL,
    CHIP_SUCCESS,
    CHIP_CAUTION,
    CHIP_CRITICAL,
} chip_kind_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *icon;   /* icon or dot */
    lv_obj_t *label;
} chip_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *indicator;
    lv_obj_t *icon;
    lv_obj_t *label;
} nav_item_t;

static const struct {
    const char *name;
    hmi_icon_t icon;
} s_pages[HMI_PAGE_COUNT] = {
    [HMI_PAGE_MAIN] = {"Main", HMI_ICON_HOME},
    [HMI_PAGE_MANUAL] = {"Manual", HMI_ICON_MOVE},
    [HMI_PAGE_IO] = {"IO monitor", HMI_ICON_CHIP},
    [HMI_PAGE_ALARMS] = {"Alarms", HMI_ICON_BELL},
    [HMI_PAGE_SETTINGS] = {"Settings", HMI_ICON_GEAR},
};

static nav_item_t s_nav[HMI_PAGE_COUNT];
static lv_obj_t *s_badge, *s_badge_label; /* alarm count on the Alarms item */
static lv_obj_t *s_page_area;
static hmi_page_t s_page = HMI_PAGE_COUNT;
static chip_t s_mode_chip, s_state_chip, s_bus_chip, s_cpu_chip;
static lv_obj_t *s_time, *s_date;
static bool s_auto, s_running;

/* ---- Chips ---- */

static void chip_style(chip_t *c, chip_kind_t kind)
{
    static const struct {
        uint32_t bg, fg;
        lv_opa_t stroke_opa;
    } k[] = {
        [CHIP_NEUTRAL] = {HMI_WHITE, HMI_TEXT, HMI_STROKE_OPA},
        [CHIP_SUCCESS] = {HMI_SUCCESS_BG, HMI_SUCCESS, LV_OPA_20},
        [CHIP_CAUTION] = {HMI_CAUTION_BG, HMI_CAUTION, LV_OPA_20},
        [CHIP_CRITICAL] = {HMI_CRITICAL_BG, HMI_CRITICAL, LV_OPA_20},
    };
    lv_obj_set_style_bg_color(c->obj, HMI_C(k[kind].bg), 0);
    lv_obj_set_style_border_color(c->obj, kind == CHIP_NEUTRAL ? lv_color_black() : HMI_C(k[kind].fg), 0);
    lv_obj_set_style_border_opa(c->obj, k[kind].stroke_opa, 0);
    lv_obj_set_style_text_color(c->label, HMI_C(k[kind].fg), 0);
}

static void dot_color(lv_obj_t *dot, uint32_t color)
{
    lv_obj_set_style_bg_color(dot, HMI_C(color), 0);
}

static void chip_create(chip_t *c, lv_obj_t *parent, bool with_icon)
{
    c->obj = hmi_box(parent, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_height(c->obj, CHIP_H);
    lv_obj_set_style_pad_hor(c->obj, 14, 0);
    lv_obj_set_style_radius(c->obj, CHIP_H / 2, 0);
    lv_obj_set_style_bg_opa(c->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c->obj, 1, 0);
    lv_obj_set_flex_align(c->obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (with_icon) {
        c->icon = hmi_icon(c->obj, HMI_ICON_SYNC, 18, HMI_TEXT);
    } else {
        c->icon = hmi_box(c->obj, -1, 0);
        lv_obj_set_size(c->icon, 10, 10);
        lv_obj_set_style_radius(c->icon, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(c->icon, LV_OPA_COVER, 0);
    }
    c->label = hmi_label(c->obj, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT);
    chip_style(c, CHIP_NEUTRAL);
}

static void update_machine_chips(void)
{
    lv_label_set_text(s_mode_chip.label, s_auto ? "Auto" : "Manual");
    chip_style(&s_mode_chip, s_auto ? CHIP_NEUTRAL : CHIP_CAUTION);
    hmi_icon_set_color(s_mode_chip.icon, s_auto ? HMI_TEXT : HMI_CAUTION);

    lv_label_set_text(s_state_chip.label, s_running ? "Running" : "Stopped");
    chip_style(&s_state_chip, s_running ? CHIP_SUCCESS : CHIP_NEUTRAL);
    dot_color(s_state_chip.icon, s_running ? HMI_SUCCESS : HMI_TEXT);
}

static void update_bus_chip(void)
{
    hmi_bus_status_t st = {.state = HMI_BUS_STARTING};
    (void)hmi_bus_status_read(&st);

    char text[40];
    uint32_t dot = HMI_RING;
    switch (st.state) {
    case HMI_BUS_NO_LINK:
        snprintf(text, sizeof(text), "EtherCAT no link");
        dot = HMI_CRITICAL;
        break;
    case HMI_BUS_HELD:
        snprintf(text, sizeof(text), "EtherCAT PRE-OP");
        dot = HMI_CRITICAL;
        break;
    case HMI_BUS_OP:
    case HMI_BUS_OP_FAULT:
        if (st.cycle_us % 100 == 0) {
            snprintf(text, sizeof(text), "EtherCAT OP \xC2\xB7 %u.%u ms", (unsigned)(st.cycle_us / 1000),
                     (unsigned)(st.cycle_us % 1000 / 100));
        } else {
            snprintf(text, sizeof(text), "EtherCAT OP \xC2\xB7 %u \xC2\xB5s", (unsigned)st.cycle_us);
        }
        dot = st.state == HMI_BUS_OP ? HMI_SUCCESS : HMI_CAUTION;
        break;
    case HMI_BUS_STARTING:
    default:
        snprintf(text, sizeof(text), "EtherCAT starting");
        break;
    }
    if (strcmp(lv_label_get_text(s_bus_chip.label), text) != 0) {
        lv_label_set_text(s_bus_chip.label, text);
    }
    dot_color(s_bus_chip.icon, dot);
}

/* Load of core 0 (HMI, Wi-Fi, SOEM mailbox) and core 1 (EtherCAT cycle) over the last second. */
static void update_cpu_chip(void)
{
    hmi_cpu_t cpu;
    char text[32];
    uint32_t dot = HMI_RING;
    if (!hmi_cpu_get(&cpu)) {
        snprintf(text, sizeof(text), "CPU \xE2\x80\x94");
    } else {
        snprintf(text, sizeof(text), "CPU %u \xC2\xB7 %u %%", cpu.load[0], cpu.load[1]);
        const unsigned worst = cpu.load[0] > cpu.load[1] ? cpu.load[0] : cpu.load[1];
        dot = worst >= CPU_CRITICAL_PCT ? HMI_CRITICAL : worst >= CPU_CAUTION_PCT ? HMI_CAUTION : HMI_SUCCESS;
    }
    if (strcmp(lv_label_get_text(s_cpu_chip.label), text) != 0) {
        lv_label_set_text(s_cpu_chip.label, text);
    }
    dot_color(s_cpu_chip.icon, dot);
}

static void update_clock(void)
{
    char t[16], d[24];
    time_t now = time(NULL);
    if (!machine_clock_valid()) {
        snprintf(t, sizeof(t), "--:--");
        snprintf(d, sizeof(d), "Clock not set");
    } else {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(t, sizeof(t), "%H:%M:%S", &tm);
        strftime(d, sizeof(d), "%Y-%m-%d", &tm);
    }
    if (strcmp(lv_label_get_text(s_time), t) != 0) {
        lv_label_set_text(s_time, t);
    }
    if (strcmp(lv_label_get_text(s_date), d) != 0) {
        lv_label_set_text(s_date, d);
    }
}

/* Number of alarms in the current list; red with errors, amber with warnings only. */
static void update_badge(void)
{
    int total, errors;
    machine_alarm_counts(&total, &errors, NULL);
    if (total == 0) {
        hmi_show(s_badge, false);
        return;
    }
    char t[8];
    snprintf(t, sizeof(t), total > 99 ? "99+" : "%d", total);
    hmi_set_text(s_badge_label, t);
    lv_obj_set_style_bg_color(s_badge, HMI_C(errors ? HMI_CRITICAL : HMI_CAUTION), 0);
    hmi_show(s_badge, true);
}

static void shell_timer_cb(lv_timer_t *t)
{
    update_clock();
    update_bus_chip();
    update_cpu_chip();
    update_badge();
}

/* ---- Title bar ---- */

static void title_bar_create(lv_obj_t *scr)
{
    lv_obj_t *bar = hmi_box(scr, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(bar, BOARD_LCD_H_RES, HMI_TITLE_H);
    lv_obj_set_style_pad_left(bar, 30, 0);
    lv_obj_set_style_pad_right(bar, 12, 0);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *tile = hmi_box(bar, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(tile, 30, 30);
    lv_obj_set_style_radius(tile, 6, 0);
    lv_obj_set_style_bg_color(tile, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(tile, HMI_ICON_APP, 18, HMI_WHITE);
    hmi_label(bar, CONFIG_HMI_MACHINE_NAME, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);

    lv_obj_t *chips = hmi_box(bar, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_grow(chips, 1);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    chip_create(&s_mode_chip, chips, true);
    chip_create(&s_state_chip, chips, false);
    chip_create(&s_bus_chip, chips, false);
    chip_create(&s_cpu_chip, chips, false);

    lv_obj_t *clock = hmi_box(bar, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    s_time = hmi_label(clock, "", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    s_date = hmi_label(clock, "", HMI_FS_TINY, HMI_REGULAR, HMI_TEXT_2);

    /* User level. There is no login yet, so this only shows the level of the mockup. */
    lv_obj_t *user = hmi_box(bar, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(user, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(user, 8, 0);
    lv_obj_t *avatar = hmi_box(user, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(avatar, 38, 38);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(avatar, 13, 0);
    lv_obj_set_style_border_width(avatar, 1, 0);
    lv_obj_set_style_border_color(avatar, lv_color_black(), 0);
    lv_obj_set_style_border_opa(avatar, 20, 0);
    lv_obj_set_flex_align(avatar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(avatar, "EN", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(user, "Engineer", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT);
}

/* ---- Navigation rail ---- */

static void nav_clicked_cb(lv_event_t *e)
{
    hmi_shell_show((hmi_page_t)(uintptr_t)lv_event_get_user_data(e));
}

static void nav_item_create(nav_item_t *it, lv_obj_t *rail, hmi_page_t page)
{
    it->obj = hmi_box(rail, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_add_flag(it->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(it->obj, NAV_ITEM_W, NAV_ITEM_H);
    lv_obj_set_style_radius(it->obj, 6, 0);
    lv_obj_set_style_bg_color(it->obj, HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_bg_opa(it->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(it->obj, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it->obj, HMI_TINT_OPA, LV_STATE_PRESSED);
    lv_obj_set_flex_align(it->obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(it->obj, nav_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

    it->indicator = hmi_box(it->obj, -1, 0);
    lv_obj_add_flag(it->indicator, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(it->indicator, 4, 24);
    lv_obj_set_pos(it->indicator, 0, (NAV_ITEM_H - 24) / 2);
    lv_obj_set_style_radius(it->indicator, 2, 0);
    lv_obj_set_style_bg_color(it->indicator, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_bg_opa(it->indicator, LV_OPA_COVER, 0);

    it->icon = hmi_icon(it->obj, s_pages[page].icon, 28, HMI_TEXT);
    it->label = hmi_label(it->obj, s_pages[page].name, NAV_LABEL_PX, HMI_REGULAR, HMI_TEXT_2);

    if (page == HMI_PAGE_ALARMS) {
        s_badge = hmi_box(it->obj, LV_FLEX_FLOW_ROW, 0);
        lv_obj_add_flag(s_badge, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_badge, NAV_ITEM_W / 2 + 6, 6);
        lv_obj_set_height(s_badge, 22);
        lv_obj_set_style_min_width(s_badge, 22, 0);
        lv_obj_set_style_pad_hor(s_badge, 5, 0);
        lv_obj_set_style_radius(s_badge, 11, 0);
        lv_obj_set_style_bg_color(s_badge, HMI_C(HMI_CRITICAL), 0);
        lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
        lv_obj_set_flex_align(s_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        s_badge_label = hmi_label(s_badge, "", HMI_FS_TINY, HMI_SEMIBOLD, HMI_WHITE);
    }
}

static void nav_select(hmi_page_t page)
{
    for (int i = 0; i < HMI_PAGE_COUNT; i++) {
        nav_item_t *it = &s_nav[i];
        bool sel = i == (int)page;
        lv_obj_set_style_bg_opa(it->obj, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel) {
            lv_obj_remove_flag(it->indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(it->indicator, LV_OBJ_FLAG_HIDDEN);
        }
        hmi_icon_set_color(it->icon, sel ? HMI_ACCENT : HMI_TEXT);
        lv_obj_set_style_text_font(it->label, hmi_font(NAV_LABEL_PX, sel ? HMI_SEMIBOLD : HMI_REGULAR), 0);
        lv_obj_set_style_text_color(it->label, HMI_C(sel ? HMI_TEXT : HMI_TEXT_2), 0);
    }
}

static void rail_create(lv_obj_t *scr)
{
    lv_obj_t *rail = hmi_box(scr, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_pos(rail, 0, HMI_TITLE_H);
    lv_obj_set_size(rail, HMI_RAIL_W, BOARD_LCD_V_RES - HMI_TITLE_H);
    lv_obj_set_style_pad_top(rail, 6, 0);
    lv_obj_set_style_pad_bottom(rail, 8, 0);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = HMI_PAGE_MAIN; i < HMI_PAGE_SETTINGS; i++) {
        nav_item_create(&s_nav[i], rail, (hmi_page_t)i);
    }
    hmi_spacer(rail);
    nav_item_create(&s_nav[HMI_PAGE_SETTINGS], rail, HMI_PAGE_SETTINGS);
}

/* ---- Public ---- */

lv_obj_t *hmi_page_header(lv_obj_t *page, const char *title, const char *caption, uint32_t caption_color)
{
    lv_obj_t *row = hmi_box(page, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(row, title, HMI_TITLE_PX, HMI_SEMIBOLD, HMI_TEXT);
    if (caption) {
        lv_obj_t *c = hmi_label(row, caption, HMI_FS_BODY, HMI_REGULAR, caption_color);
        lv_obj_set_style_pad_top(c, 6, 0);
    }
    return row;
}

void hmi_shell_set_machine(bool auto_mode, bool running)
{
    s_auto = auto_mode;
    s_running = running;
    update_machine_chips();
}

bool hmi_shell_auto_mode(void)
{
    return s_auto;
}

bool hmi_shell_running(void)
{
    return s_running;
}

void hmi_shell_show(hmi_page_t page)
{
    /* Selecting Settings again leaves its sub-page for the list, as in NavigationView. */
    if (page >= HMI_PAGE_COUNT || (page == s_page && page != HMI_PAGE_SETTINGS)) {
        return;
    }
    s_page = page;
    nav_select(page);
    lv_obj_clean(s_page_area);
    if (page == HMI_PAGE_MAIN) {
        hmi_page_main_create(s_page_area);
    } else if (page == HMI_PAGE_MANUAL) {
        hmi_page_manual_create(s_page_area);
    } else if (page == HMI_PAGE_IO) {
        hmi_page_io_create(s_page_area);
    } else if (page == HMI_PAGE_ALARMS) {
        hmi_page_alarms_create(s_page_area);
    } else {
        hmi_page_settings_create(s_page_area);
    }
}

/* ---- Page by name (console 'page' command, bench tests without touching the screen) ---- */

static const struct {
    const char *name;
    hmi_page_t page;
    int sub; /* hmi_settings_sub_t, or -1; for Manual: hmi_manual_view_t */
} s_page_names[] = {
    {"main", HMI_PAGE_MAIN, -1},
    {"manual", HMI_PAGE_MANUAL, HMI_MANUAL_JOG},
    {"jog", HMI_PAGE_MANUAL, HMI_MANUAL_JOG},
    {"teach", HMI_PAGE_MANUAL, HMI_MANUAL_TEACH},
    {"interp", HMI_PAGE_MANUAL, HMI_MANUAL_INTERP},
    {"io", HMI_PAGE_IO, -1},
    {"alarms", HMI_PAGE_ALARMS, -1},
    {"settings", HMI_PAGE_SETTINGS, -1},
    {"time", HMI_PAGE_SETTINGS, HMI_SET_TIME},
    {"servo", HMI_PAGE_SETTINGS, HMI_SET_SERVO},
    {"ethercat", HMI_PAGE_SETTINGS, HMI_SET_ETHERCAT},
    {"system", HMI_PAGE_SETTINGS, HMI_SET_SYSTEM},
    {"network", HMI_PAGE_SETTINGS, HMI_SET_NETWORK},
    {"info", HMI_PAGE_SETTINGS, HMI_SET_INFO},
};

bool hmi_open_page(const char *name)
{
    size_t i = 0;
    while (i < sizeof(s_page_names) / sizeof(s_page_names[0]) && strcmp(s_page_names[i].name, name) != 0) {
        i++;
    }
    if (i == sizeof(s_page_names) / sizeof(s_page_names[0]) || s_page_area == NULL || !lvgl_port_lock(1000)) {
        return false;
    }
    if (s_page_names[i].page == HMI_PAGE_MANUAL) {
        hmi_manual_open((hmi_manual_view_t)s_page_names[i].sub);
    } else if (s_page_names[i].page == HMI_PAGE_SETTINGS && s_page_names[i].sub >= 0) {
        hmi_settings_open((hmi_settings_sub_t)s_page_names[i].sub);
    } else {
        hmi_shell_show(s_page_names[i].page);
    }
    lvgl_port_unlock();
    return true;
}

void hmi_shell_create(void)
{
    /* Bottom layer: one solid grey under title bar, rail and page (user, 2026-10-10: no gradient,
     * no dithering). Cards are white on it, controls sit on the cards. */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, HMI_C(HMI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    title_bar_create(scr);
    rail_create(scr);

    /* Page area: only a padding box on the background. */
    s_page_area = hmi_box(scr, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_pos(s_page_area, HMI_RAIL_W, HMI_TITLE_H);
    lv_obj_set_size(s_page_area, BOARD_LCD_H_RES - HMI_RAIL_W, BOARD_LCD_V_RES - HMI_TITLE_H);
    lv_obj_set_style_pad_top(s_page_area, 10, 0);
    lv_obj_set_style_pad_right(s_page_area, 16, 0);
    lv_obj_set_style_pad_bottom(s_page_area, 14, 0);
    lv_obj_set_style_pad_left(s_page_area, 16, 0);

#if CONFIG_HMI_DEMO_DATA
    hmi_shell_set_machine(true, true);
#else
    hmi_shell_set_machine(false, false);
#endif
    update_clock();
    update_bus_chip();
    update_cpu_chip();
    update_badge();
    lv_timer_create(shell_timer_cb, 500, NULL);
    hmi_shell_show(HMI_PAGE_MAIN);
}
