/*
 * Main page (mockup artboard "MAIN — Auto"): machine state, production figures, axes,
 * mode, Start/Stop, latest alarm.
 *
 * There is no machine logic yet. With CONFIG_HMI_DEMO_DATA the page shows the mockup's values and
 * the mode/Start/Stop controls only change what is displayed; nothing is sent to the machine.
 * Without it every value reads "—".
 */

#include <stdio.h>
#include "hmi_internal.h"

#if CONFIG_HMI_DEMO_DATA
#define DEMO(value, none) (value)
#else
#define DEMO(value, none) (none)
#endif
#define NONE "\xE2\x80\x94" /* em dash */

typedef struct {
    lv_obj_t *state_disc;
    lv_obj_t *state_play;
    lv_obj_t *state_stop;
    lv_obj_t *state_label;
    lv_obj_t *step_label;
    lv_obj_t *cycle_label;
    lv_obj_t *progress;
    lv_obj_t *seg_auto;
    lv_obj_t *seg_manual;
} main_page_t;

static main_page_t s_ui;

/* ---- State-dependent parts ---- */

static void segment_style(lv_obj_t *seg, bool selected)
{
    lv_obj_t *label = lv_obj_get_child(seg, 0);
    lv_obj_set_style_bg_opa(seg, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(seg, selected ? HMI_STROKE_OPA : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(label, hmi_font(HMI_FS_BODY, selected ? HMI_SEMIBOLD : HMI_REGULAR), 0);
    lv_obj_set_style_text_color(label, HMI_C(selected ? HMI_ACCENT : HMI_TEXT_2), 0);
}

static void refresh(void)
{
    bool running = hmi_shell_running(), auto_mode = hmi_shell_auto_mode();

    lv_obj_set_style_bg_color(s_ui.state_disc, HMI_C(running ? HMI_SUCCESS_BG : 0xEFEFEF), 0);
    hmi_icon_set_color(s_ui.state_play, HMI_SUCCESS);
    hmi_icon_set_color(s_ui.state_stop, HMI_TEXT_2);
    if (running) {
        lv_obj_remove_flag(s_ui.state_play, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.state_stop, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.state_play, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.state_stop, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_ui.state_label, running ? "Running" : "Stopped");
    lv_label_set_text(s_ui.step_label, running ? DEMO("Step 4 of 12 \xC2\xB7 Pick part", NONE)
                                               : (auto_mode ? "Ready \xC2\xB7 press Start"
                                                            : "Manual mode \xC2\xB7 auto cycle disabled"));
    lv_label_set_text(s_ui.cycle_label, running ? DEMO("1.62 / 4.82 s", NONE) : NONE);
    lv_obj_set_width(s_ui.progress, running ? DEMO(LV_PCT(34), 0) : 0);

    segment_style(s_ui.seg_auto, auto_mode);
    segment_style(s_ui.seg_manual, !auto_mode);
}

static void set_machine(bool auto_mode, bool running)
{
    hmi_shell_set_machine(auto_mode, running);
    refresh();
}

static void on_auto(lv_event_t *e)
{
    set_machine(true, hmi_shell_running());
}

static void on_manual(lv_event_t *e)
{
    set_machine(false, false);
}

static void on_start(lv_event_t *e)
{
#if CONFIG_HMI_DEMO_DATA
    if (hmi_shell_auto_mode()) {
        set_machine(true, true);
    }
#endif
}

static void on_stop(lv_event_t *e)
{
    set_machine(hmi_shell_auto_mode(), false);
}

static void on_open_alarms(lv_event_t *e)
{
    hmi_shell_show(HMI_PAGE_ALARMS);
}

/* ---- Building blocks ---- */

static lv_obj_t *kpi_card(lv_obj_t *parent, const char *caption, const char *value, const char *sub, uint32_t color)
{
    lv_obj_t *c = hmi_card(parent, 10, 12, 2);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_width(c, 0); /* share the row equally, whatever the text */
    hmi_label(c, caption, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    hmi_label(c, value, HMI_TITLE_PX, HMI_SEMIBOLD, color);
    lv_obj_t *l = hmi_label(c, sub, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(hmi_font(HMI_FS_SMALL, HMI_REGULAR)));
    return c;
}

static void axis_row(lv_obj_t *grid, int index, const char *name, const char *pos, const char *load, bool ready)
{
    lv_obj_t *row = hmi_box(grid, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH, index / 4, 1, LV_GRID_ALIGN_STRETCH, index % 4, 1);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_border_opa(row, HMI_STROKE_OPA, 0);

    lv_obj_t *l = hmi_label(row, name, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_width(l, 44);
    l = hmi_label(row, pos, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    l = hmi_label(row, load, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(l, 42);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *status = hmi_box(row, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_width(status, 88);
    lv_obj_set_style_pad_left(status, 8, 0);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *dot = hmi_box(status, -1, 0);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    if (ready) {
        lv_obj_set_style_bg_color(dot, HMI_C(HMI_SUCCESS), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, HMI_C(HMI_RING), 0);
    }
    hmi_label(status, ready ? "Ready" : DEMO("Servo off", "No data"), HMI_FS_SMALL, HMI_REGULAR, ready ? HMI_SUCCESS : HMI_TEXT_2);
}

static void build_left(lv_obj_t *col)
{
    /* Machine state */
    lv_obj_t *card = hmi_card(col, 14, 16, 12);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.state_disc = hmi_box(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(s_ui.state_disc, 48, 48);
    lv_obj_set_style_radius(s_ui.state_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ui.state_disc, LV_OPA_COVER, 0);
    lv_obj_set_flex_align(s_ui.state_disc, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.state_play = hmi_icon(s_ui.state_disc, HMI_ICON_PLAY, 22, HMI_SUCCESS);
    s_ui.state_stop = hmi_icon(s_ui.state_disc, HMI_ICON_STOP, 20, HMI_TEXT_2);

    lv_obj_t *text = hmi_box(row, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_grow(text, 1);
    s_ui.state_label = hmi_label(text, "", HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    s_ui.step_label = hmi_label(text, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);

    lv_obj_t *cycle = hmi_box(row, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(cycle, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    hmi_label(cycle, "Cycle", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    s_ui.cycle_label = hmi_label(cycle, "", HMI_FS_LARGE, HMI_SEMIBOLD, HMI_TEXT);

    lv_obj_t *track = hmi_box(card, -1, 0);
    lv_obj_set_size(track, LV_PCT(100), 4);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_10, 0);
    s_ui.progress = hmi_box(track, -1, 0);
    lv_obj_set_size(s_ui.progress, 0, 4);
    lv_obj_set_style_radius(s_ui.progress, 2, 0);
    lv_obj_set_style_bg_color(s_ui.progress, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_ui.progress, LV_OPA_COVER, 0);

    /* Production figures */
    lv_obj_t *kpis = hmi_box(col, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(kpis, LV_PCT(100));
    kpi_card(kpis, "Output", DEMO("1 248", NONE), DEMO("62 % of 2 000", "Target " NONE), HMI_TEXT);
    kpi_card(kpis, "NG", DEMO("12", NONE), DEMO("Rate 0.96 %", "Rate " NONE), HMI_CAUTION);
    kpi_card(kpis, "Cycle time", DEMO("4.82 s", NONE), DEMO("Average 4.90 s", "Average " NONE), HMI_TEXT);
    kpi_card(kpis, "Run time", DEMO("06:41:12", NONE), DEMO("Shift start 07:50", "Shift start " NONE), HMI_TEXT);

    /* Axes */
    card = hmi_card(col, 10, 16, 6);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_bottom(card, 4, 0);
    row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(row, "Axes", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(row, "Actual position (mm) \xC2\xB7 load", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    hmi_spacer(row);
    hmi_label(row, DEMO("7 of 8 ready", "No data"), HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);

    static const int32_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_t *grid = hmi_box(card, -1, 0);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_grid_dsc_array(grid, cols, rows);
    lv_obj_set_style_pad_column(grid, 20, 0);
    lv_obj_set_style_pad_row(grid, 0, 0);

    static const struct {
        const char *name, *pos, *load;
        bool ready;
    } axes[8] = {
        {"AX1", "125.430", "32 %", true}, {"AX2", "40.250", "4 %", true},  {"AX3", "12.000", "18 %", true},
        {"AX4", "0.000", "1 %", true},    {"AX5", "350.000", "6 %", true}, {"AX6", "88.500", "9 %", true},
        {"AX7", "0.000", "0 %", false},   {"AX8", "210.750", "12 %", true},
    };
    for (int i = 0; i < 8; i++) {
        axis_row(grid, i, axes[i].name, DEMO(axes[i].pos, NONE), DEMO(axes[i].load, ""), DEMO(axes[i].ready, false));
    }
}

static lv_obj_t *segment(lv_obj_t *track, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *s = hmi_box(track, LV_FLEX_FLOW_ROW, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(s, HMI_TOUCH_H);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_radius(s, 4, 0);
    lv_obj_set_style_bg_color(s, HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_border_width(s, 1, 0);
    lv_obj_set_style_border_color(s, lv_color_black(), 0);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s, cb, LV_EVENT_CLICKED, NULL);
    hmi_label(s, text, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    return s;
}

static void build_right(lv_obj_t *col)
{
    /* Mode: segmented control */
    lv_obj_t *track = hmi_box(col, LV_FLEX_FLOW_ROW, 4);
    lv_obj_set_width(track, LV_PCT(100));
    lv_obj_set_style_pad_all(track, 3, 0);
    lv_obj_set_style_radius(track, 6, 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, HMI_TINT_OPA, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    lv_obj_set_style_border_opa(track, HMI_STROKE_OPA, 0);
    s_ui.seg_auto = segment(track, "Auto", on_auto);
    s_ui.seg_manual = segment(track, "Manual", on_manual);

    /* Start / Stop: fixed semantic colours, not the accent */
    lv_obj_t *b = hmi_filled_button(col, 84, HMI_SUCCESS, 8);
    lv_obj_add_event_cb(b, on_start, LV_EVENT_CLICKED, NULL);
    hmi_icon(b, HMI_ICON_PLAY, 26, HMI_WHITE);
    hmi_label(b, "Start", HMI_TITLE_PX, HMI_SEMIBOLD, HMI_WHITE);

    b = hmi_filled_button(col, 68, HMI_CRITICAL, 8);
    lv_obj_add_event_cb(b, on_stop, LV_EVENT_CLICKED, NULL);
    hmi_icon(b, HMI_ICON_STOP, 22, HMI_WHITE);
    hmi_label(b, "Stop", HMI_FS_HEAD, HMI_SEMIBOLD, HMI_WHITE);

    lv_obj_t *pair = hmi_box(col, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(pair, LV_PCT(100));
    b = hmi_control(pair, 0, 56);
    lv_obj_set_flex_grow(b, 1);
    hmi_icon(b, HMI_ICON_RESET, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(b, "Reset", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    b = hmi_control(pair, 0, 56);
    lv_obj_set_flex_grow(b, 1);
    hmi_icon(b, HMI_ICON_HOME, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(b, "Home all", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);

    /* Latest alarm (InfoBar style) */
    lv_obj_t *alarm = hmi_card(col, 12, 14, 4);
    lv_obj_set_width(alarm, LV_PCT(100));
    lv_obj_set_flex_grow(alarm, 1);
    lv_obj_add_flag(alarm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(alarm, &hmi_st_pressed_dim, LV_STATE_PRESSED);
    lv_obj_add_event_cb(alarm, on_open_alarms, LV_EVENT_CLICKED, NULL);
#if CONFIG_HMI_DEMO_DATA
    lv_obj_set_style_bg_color(alarm, HMI_C(HMI_CRITICAL_BG), 0);
    lv_obj_t *head = hmi_box(alarm, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(head, HMI_ICON_ERROR, 20, HMI_CRITICAL);
    hmi_label(head, "Latest alarm", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(alarm, "A101 \xC2\xB7 Servo AX1 drive fault", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    hmi_label(alarm, "14:31:58 \xC2\xB7 3 active alarms", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
#else
    lv_obj_t *head = hmi_box(alarm, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(head, HMI_ICON_INFO, 20, HMI_ACCENT);
    hmi_label(head, "Alarms", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(alarm, "No alarm source connected yet", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
#endif
    hmi_spacer(alarm);
    lv_obj_t *link = hmi_box(alarm, LV_FLEX_FLOW_ROW, 4);
    lv_obj_set_flex_align(link, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(link, "Open alarms", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_icon(link, HMI_ICON_CHEVRON_RIGHT, 18, HMI_TEXT);
}

void hmi_page_main_create(lv_obj_t *page)
{
#if CONFIG_HMI_DEMO_DATA
    lv_obj_t *header = hmi_page_header(page, "Main", "Auto operation \xC2\xB7 demo data", HMI_CAUTION);
#else
    lv_obj_t *header = hmi_page_header(page, "Main", "Auto operation", HMI_TEXT_2);
#endif
    hmi_spacer(header);
    lv_obj_t *recipe = hmi_control(header, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_left(recipe, 14, 0);
    lv_obj_set_style_pad_right(recipe, 12, 0);
    lv_obj_set_style_pad_column(recipe, 10, 0);
    hmi_label(recipe, "Recipe", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    hmi_label(recipe, DEMO("[PRODUCT A-01]", NONE), HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_icon(recipe, HMI_ICON_CHEVRON_DOWN, 16, HMI_TEXT);

    lv_obj_t *body = hmi_box(page, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);

    lv_obj_t *left = hmi_box(body, LV_FLEX_FLOW_COLUMN, 10);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 1);
    build_left(left);

    lv_obj_t *right = hmi_box(body, LV_FLEX_FLOW_COLUMN, 10);
    lv_obj_set_size(right, 256, LV_PCT(100));
    build_right(right);

    refresh();
}
