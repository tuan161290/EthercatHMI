/*
 * Manual > Jog (mockup Manual.dc.html): pick an axis, see its position and drive state, switch the
 * servo on/off, reset alarms, home, jog it (continuous or by steps) and move it to a position.
 *
 * Axes come from ecat_axes_read() (the drives of the accepted topology, bus in OP); commands go
 * through ecat_axis_command(). Continuous jog is hold-to-run: the command is repeated every
 * JOG_REFRESH_MS while the button is held, and the cyclic task stops the axis by itself if the
 * repetition stops (ECAT_JOG_DEADMAN_MS), e.g. when the panel freezes or a release is lost.
 *
 * Units (hmi_axis.c): the axis' Unit of Settings > Servo (mm and mm/s, or deg and deg/s) once its
 * Distance/Rev is set, else motor revolutions and rpm. Pulse/Rev comes from the drive (0x608F, 0x6091, 0x6092); if it reports
 * none, HMI_ASSUMED_UNITS_PER_REV is used and the page says so.
 * Speeds: Low 10 and Mid 60 rpm, High is the axis' "Jog speed (high)", all limited by its Max
 * speed; moves ramp with its Acc/Dec times (hmi_axis_cmd()).
 * The Teaching view lives in hmi_page_teach.c, Interpolation in hmi_page_interp.c (only with
 * CONFIG_ECAT_PATH); Actuators and Homing are not built yet.
 */

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "ecat_axis.h"
#include "hmi_internal.h"
#include "machine_params.h"

#define POLL_MS               50
#define JOG_REFRESH_MS        100
#define LEFT_W                270
#define RIGHT_W               230
#define ROW_H                 40
#define SEG_H                 48

static const double s_steps[] = {0.01, 0.1, 1.0, 10.0}; /* in the axis' position unit (mm or rev) */
static const char *const s_step_labels[] = {"0.01", "0.1", "1.0", "10"};
static const double s_speeds_rpm[] = {10.0, 60.0}; /* Low, Mid; High comes from the axis settings */
static const char *const s_speed_labels[] = {"Low", "Mid", "High"};
static const char *const s_mode_labels[] = {"Continuous", "Step"};

enum { MODE_CONTINUOUS, MODE_STEP };
enum { SPEED_LOW, SPEED_MID, SPEED_HIGH };

/* Operator choices: kept while the firmware runs, like the mockup. */
static hmi_manual_view_t s_view = HMI_MANUAL_JOG;
static int s_axis_sel;
static int s_mode = MODE_STEP;
static int s_step = 1;
static int s_speed = 1;
static double s_target; /* Move to, in the axis' position unit */

static struct {
    lv_obj_t *page;
    lv_obj_t *root;
    lv_timer_t *poll;
    lv_timer_t *jog_timer;
    int jog_dir;
    /* Layout signature: rebuild when the axes change */
    bool in_op;
    uint8_t n_axes;
    char names[ECAT_MAX_AXES][ECAT_TOPO_NAME_LEN];
    /* Widgets of the jog view */
    lv_obj_t *status_tag;
    lv_obj_t *axis_name; /* label of the axis combo */
    lv_obj_t *pos_title;
    lv_obj_t *pos_value;
    lv_obj_t *pos_unit;
    lv_obj_t *pos_raw;
    lv_obj_t *speed;
    lv_obj_t *state;
    lv_obj_t *drive_error;
    lv_obj_t *step_seg;
    lv_obj_t *step_name;
    lv_obj_t *speed_seg;
    lv_obj_t *lamp_on, *lamp_homed, *lamp_alarm;
    lv_obj_t *message;
    lv_obj_t *units_note;
    lv_obj_t *move_value;
    lv_obj_t *go_label;
    lv_obj_t *servo_label;
    lv_obj_t *plus_label, *minus_label;
    lv_obj_t *plus_btn, *minus_btn;
    lv_obj_t *hint;
} s_m;

static EXT_RAM_BSS_ATTR ecat_axes_status_t s_ax; /* snapshot, ~1 KB, PSRAM: not on the LVGL stack */

static void build(void);

/* ---- Helpers ---- */

#define set_text hmi_set_text

static const ecat_axis_status_t *cur(void)
{
    return (s_axis_sel >= 0 && s_axis_sel < s_ax.n_axes) ? &s_ax.axis[s_axis_sel] : NULL;
}

static void scale(const ecat_axis_status_t *a, hmi_scale_t *sc)
{
    hmi_axis_scale(a->name, a->units_per_rev, sc);
}

double hmi_manual_step(void)
{
    return s_steps[s_step];
}

static double speed_rpm(const char *axis_name, int which)
{
    machine_axis_cfg_t cfg;
    machine_axis_get(axis_name, &cfg);
    double v = which == SPEED_HIGH ? cfg.jog_high_rpm : s_speeds_rpm[which];
    if (v > cfg.jog_high_rpm) {
        v = cfg.jog_high_rpm;
    }
    return v < cfg.max_rpm ? v : cfg.max_rpm;
}

double hmi_manual_speed_rpm(const char *axis_name)
{
    return speed_rpm(axis_name, s_speed);
}

static void send(uint8_t code, int8_t dir, int32_t position, double rpm)
{
    const ecat_axis_status_t *a = cur();
    if (a && !hmi_axis_cmd(s_axis_sel, a->name, a->units_per_rev, code, dir, position, rpm)) {
        set_text(s_m.message, "Command not sent: queue full");
    }
}

/* ---- Jog ---- */

static void jog_send(void)
{
    const ecat_axis_status_t *a = cur();
    if (a && s_m.jog_dir) {
        send(ECAT_AXIS_JOG, (int8_t)s_m.jog_dir, 0, hmi_manual_speed_rpm(a->name));
    }
}

static void jog_timer_cb(lv_timer_t *t)
{
    (void)t;
    jog_send();
}

static void jog_stop(void)
{
    if (s_m.jog_timer) {
        lv_timer_delete(s_m.jog_timer);
        s_m.jog_timer = NULL;
    }
    if (s_m.jog_dir) {
        s_m.jog_dir = 0;
        send(ECAT_AXIS_JOG, 0, 0, 0);
    }
}

static void jog_event_cb(lv_event_t *e)
{
    const int dir = (int)(intptr_t)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    const ecat_axis_status_t *a = cur();
    if (a == NULL) {
        return;
    }
    if (s_mode == MODE_CONTINUOUS) {
        if (code == LV_EVENT_PRESSED) {
            jog_stop();
            s_m.jog_dir = dir;
            jog_send();
            s_m.jog_timer = lv_timer_create(jog_timer_cb, JOG_REFRESH_MS, NULL);
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            jog_stop();
        }
    } else if (code == LV_EVENT_CLICKED) {
        hmi_scale_t sc;
        scale(a, &sc);
        send(ECAT_AXIS_MOVE_REL, 0, hmi_from_pos(&sc, dir * s_steps[s_step]), hmi_manual_speed_rpm(a->name));
    }
}

/* ---- Buttons ---- */

static void servo_cb(lv_event_t *e)
{
    (void)e;
    const ecat_axis_status_t *a = cur();
    if (a) {
        send(a->enabled ? ECAT_AXIS_DISABLE : ECAT_AXIS_ENABLE, 0, 0, 0);
    }
}

static void home_cb(lv_event_t *e)
{
    (void)e;
    send(ECAT_AXIS_HOME, 0, 0, 0);
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    jog_stop();
    send(ECAT_AXIS_STOP, 0, 0, 0);
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    send(ECAT_AXIS_RESET, 0, 0, 0);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    const ecat_axis_status_t *a = cur();
    if (a == NULL) {
        return;
    }
    if (a->busy) {
        send(ECAT_AXIS_STOP, 0, 0, 0);
    } else {
        hmi_scale_t sc;
        scale(a, &sc);
        send(ECAT_AXIS_MOVE_ABS, 0, hmi_from_pos(&sc, s_target), hmi_manual_speed_rpm(a->name));
    }
}

static void target_show(void)
{
    const ecat_axis_status_t *a = cur();
    hmi_scale_t sc = {.pos = "rev"};
    if (a) {
        scale(a, &sc);
    }
    char t[32];
    snprintf(t, sizeof(t), "%.3f %s", s_target, sc.pos);
    set_text(s_m.move_value, t);
}

static void target_set_cb(double value, void *user)
{
    (void)user;
    s_target = value;
    target_show();
}

static void target_cb(lv_event_t *e)
{
    (void)e;
    const ecat_axis_status_t *a = cur();
    if (a == NULL) {
        return;
    }
    char title[40];
    snprintf(title, sizeof(title), "Move %s to", a->name);
    hmi_scale_t sc;
    scale(a, &sc);
    const double limit = hmi_pos_limit(&sc);
    hmi_keypad(title, sc.pos, s_target, -limit, limit, 3, target_set_cb, NULL);
}

static void open_ethercat_cb(lv_event_t *e)
{
    (void)e;
    hmi_settings_open_ethercat();
}

static void update(void);

static void axis_pick_cb(int index, void *user)
{
    (void)user;
    jog_stop();
    s_axis_sel = index;
    update();
}

/* Axis combo: the list of axes on a roller (any number of axes, big touch targets). */
static void axis_combo_cb(lv_event_t *e)
{
    (void)e;
    static char opts[ECAT_MAX_AXES * (ECAT_TOPO_NAME_LEN + 1)];
    opts[0] = '\0';
    for (uint8_t i = 0; i < s_m.n_axes; i++) {
        if (i) {
            strlcat(opts, "\n", sizeof(opts));
        }
        strlcat(opts, s_m.names[i], sizeof(opts));
    }
    hmi_choice("Axis", opts, s_axis_sel, axis_pick_cb, NULL);
}

static void mode_pick_cb(int index, void *user)
{
    (void)user;
    jog_stop();
    s_mode = index;
    hmi_segmented_set_enabled(s_m.step_seg, s_mode == MODE_STEP);
}

static void step_pick_cb(int index, void *user)
{
    (void)user;
    s_step = index;
}

static void speed_pick_cb(int index, void *user)
{
    (void)user;
    s_speed = index;
}

/* ---- Update ---- */

static void update(void)
{
    const ecat_axis_status_t *a = cur();
    if (a == NULL || s_m.pos_value == NULL) {
        return;
    }
    char t[96];
    hmi_scale_t sc;
    scale(a, &sc);

    snprintf(t, sizeof(t), "%u axes", s_ax.n_axes);
    hmi_tag_set(s_m.status_tag, s_ax.bus_ok ? t : "Bus errors", s_ax.bus_ok ? HMI_SUCCESS_BG : HMI_CRITICAL_BG,
                s_ax.bus_ok ? HMI_SUCCESS : HMI_CRITICAL);

    snprintf(t, sizeof(t), "%s \xC2\xB7 Actual position", a->name);
    set_text(s_m.pos_title, t);
    set_text(s_m.axis_name, a->name);
    snprintf(t, sizeof(t), "%.3f", hmi_to_pos(&sc, a->position));
    set_text(s_m.pos_value, t);
    set_text(s_m.pos_unit, sc.pos);
    snprintf(t, sizeof(t), "%ld", (long)a->position);
    set_text(s_m.pos_raw, t);
    const double rpm = (double)a->velocity * 60.0 / (double)hmi_upr(a->units_per_rev);
    snprintf(t, sizeof(t), "%.*f %s", sc.speed_decimals + 1, hmi_rpm_to_speed(&sc, rpm), sc.speed);
    set_text(s_m.speed, t);
    set_text(s_m.state, a->available ? ecat_cia_state_text(a->cia_state) : "Unavailable");
    if (a->error_code || a->vendor_error) {
        snprintf(t, sizeof(t), "0x%04X / 0x%04X", a->error_code, a->vendor_error);
    } else {
        strlcpy(t, "None", sizeof(t));
    }
    set_text(s_m.drive_error, t);
    set_text(s_m.units_note, a->units_per_rev ? "" : "Pulse/Rev unknown: 10000 assumed");
    for (int k = SPEED_LOW; k <= SPEED_HIGH; k++) {
        snprintf(t, sizeof(t), "%.*f %s", sc.speed_decimals, hmi_rpm_to_speed(&sc, speed_rpm(a->name, k)), sc.speed);
        hmi_segmented_set_text(s_m.speed_seg, k, t);
    }
    snprintf(t, sizeof(t), "Step %s", sc.pos);
    set_text(s_m.step_name, t);
    target_show();

    const bool alarm = a->cia_state == ECAT_CIA_FAULT || a->cia_state == ECAT_CIA_FAULT_REACTION;
    hmi_lamp_set(s_m.lamp_on, a->enabled, HMI_SUCCESS);
    hmi_lamp_set(s_m.lamp_homed, a->homed, HMI_SUCCESS);
    hmi_lamp_set(s_m.lamp_alarm, alarm, HMI_CRITICAL);

    if (a->error != ECAT_AXERR_NONE) {
        if (a->error == ECAT_AXERR_FAULT && a->error_code) {
            snprintf(t, sizeof(t), "%s 0x%04X", ecat_axis_error_text(a->error), a->error_code);
        } else {
            strlcpy(t, ecat_axis_error_text(a->error), sizeof(t));
        }
    } else if (a->job != ECAT_JOB_IDLE) {
        strlcpy(t, ecat_axis_job_text(a->job), sizeof(t));
    } else {
        strlcpy(t, a->enabled ? "Ready" : "Servo off", sizeof(t));
    }
    set_text(s_m.message, t);
    lv_obj_set_style_text_color(s_m.message, HMI_C(a->error != ECAT_AXERR_NONE ? HMI_CRITICAL : HMI_TEXT_2), 0);

    set_text(s_m.servo_label, a->enabled ? "Servo off" : "Servo on");
    set_text(s_m.go_label, a->busy ? "Stop" : "Go");

    snprintf(t, sizeof(t), "%s +", a->name);
    set_text(s_m.plus_label, t);
    snprintf(t, sizeof(t), "%s \xE2\x88\x92", a->name); /* minus sign */
    set_text(s_m.minus_label, t);
    const lv_opa_t opa = a->enabled ? LV_OPA_COVER : LV_OPA_40;
    lv_obj_set_style_opa(s_m.plus_btn, opa, 0);
    lv_obj_set_style_opa(s_m.minus_btn, opa, 0);
    if (s_mode == MODE_STEP) {
        snprintf(t, sizeof(t), "Tap = one step of %s %s", s_step_labels[s_step], sc.pos);
    } else {
        strlcpy(t, "Hold to move \xC2\xB7 release to stop", sizeof(t));
    }
    set_text(s_m.hint, t);
}

static bool layout_changed(void)
{
    if (s_ax.in_op != s_m.in_op || s_ax.n_axes != s_m.n_axes) {
        return true;
    }
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        if (strcmp(s_ax.axis[i].name, s_m.names[i]) != 0) {
            return true;
        }
    }
    return false;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    ecat_axes_read(&s_ax);
    if (layout_changed()) {
        build();
        return;
    }
    update();
}

/* ---- Layout ---- */

static lv_obj_t *value_row(lv_obj_t *card, const char *name)
{
    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_border_opa(row, HMI_STROKE_OPA, 0);
    hmi_label(row, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    return hmi_label(row, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
}

static lv_obj_t *card(lv_obj_t *parent, bool grow)
{
    lv_obj_t *c = hmi_card(parent, 12, 14, 8);
    lv_obj_set_width(c, LV_PCT(100));
    if (grow) {
        lv_obj_set_flex_grow(c, 1);
    }
    return c;
}

static lv_obj_t *column(lv_obj_t *body, int32_t w)
{
    lv_obj_t *col = hmi_box(body, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_height(col, LV_PCT(100));
    if (w > 0) {
        lv_obj_set_width(col, w);
    } else {
        lv_obj_set_flex_grow(col, 1);
    }
    return col;
}

static lv_obj_t *action_button(lv_obj_t *row, const char *text, hmi_icon_t icon, lv_event_cb_t cb)
{
    lv_obj_t *b = hmi_control(row, 0, HMI_TOUCH_H);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_pad_column(b, 8, 0);
    hmi_icon(b, icon, HMI_ICON_BTN, HMI_TEXT);
    lv_obj_t *l = hmi_label(b, text, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return l;
}

/* One jog setting on a line: name on the left, segmented control filling the rest. */
static lv_obj_t *setting_row(lv_obj_t *card, const char *name, const char *const *labels, int n, int sel,
                             hmi_seg_cb_t cb, lv_obj_t **name_label)
{
    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = hmi_label(row, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(l, 96);
    if (name_label) {
        *name_label = l;
    }
    lv_obj_t *box = hmi_box(row, -1, 0);
    lv_obj_set_flex_grow(box, 1);
    return hmi_segmented(box, labels, n, 0, SEG_H, sel, cb, NULL);
}

static lv_obj_t *jog_button(lv_obj_t *col, int dir, lv_obj_t **label)
{
    lv_obj_t *b = hmi_filled_button(col, 0, HMI_ACCENT, 8);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    *label = hmi_label(b, "", HMI_FS_BIG, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(b, jog_event_cb, LV_EVENT_PRESSED, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, jog_event_cb, LV_EVENT_RELEASED, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, jog_event_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, jog_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)dir);
    return b;
}

static void pill_cb(lv_event_t *e)
{
    hmi_manual_open((hmi_manual_view_t)(intptr_t)lv_event_get_user_data(e));
}

lv_obj_t *hmi_manual_header(lv_obj_t *root, hmi_manual_view_t view)
{
    lv_obj_t *row = hmi_page_header(root, "Manual", NULL, HMI_TEXT_2);
    lv_obj_t *pills = hmi_box(row, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_style_pad_left(pills, 4, 0);
    lv_obj_t *jog = hmi_pill(pills, "Jog", view == HMI_MANUAL_JOG, true);
    lv_obj_t *teach = hmi_pill(pills, "Teaching", view == HMI_MANUAL_TEACH, true);
#if CONFIG_ECAT_PATH
    lv_obj_t *interp = hmi_pill(pills, "Interpolation", view == HMI_MANUAL_INTERP, true);
    lv_obj_add_event_cb(interp, pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)HMI_MANUAL_INTERP);
#endif
    hmi_pill(pills, "Actuators", false, false);
    hmi_pill(pills, "Homing", false, false);
    lv_obj_add_event_cb(jog, pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)HMI_MANUAL_JOG);
    lv_obj_add_event_cb(teach, pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)HMI_MANUAL_TEACH);
    hmi_spacer(row);
    return row;
}

static void header(void)
{
    lv_obj_t *row = hmi_manual_header(s_m.root, HMI_MANUAL_JOG);
    s_m.status_tag = hmi_tag(row, true);
}

static void build_no_axes(void)
{
    hmi_tag_set(s_m.status_tag, s_ax.in_op ? "No drives" : "Not in OP", HMI_CAUTION_BG, HMI_CAUTION);
    hmi_info_card(s_m.root, "No axes to move",
                  s_ax.in_op ? "The accepted topology has no servo or stepper drive."
                             : "The EtherCAT bus is not in OP. Scan the bus and accept the topology in "
                               "Settings \xE2\x80\xBA EtherCAT.",
                  "Open EtherCAT settings", open_ethercat_cb);
}

static void build_jog(void)
{
    lv_obj_t *body = hmi_box(s_m.root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);

    /* Left: axis and position */
    lv_obj_t *left = column(body, LEFT_W);
    lv_obj_t *c = card(left, false);
    hmi_card_title(c, "Axis");
    lv_obj_t *combo = hmi_control(c, LV_PCT(100), 56);
    lv_obj_set_flex_align(combo, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(combo, 16, 0);
    s_m.axis_name = hmi_label(combo, s_ax.axis[s_axis_sel].name, HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    hmi_icon(combo, HMI_ICON_CHEVRON_DOWN, 22, HMI_TEXT_2);
    lv_obj_add_event_cb(combo, axis_combo_cb, LV_EVENT_CLICKED, NULL);

    c = card(left, true);
    s_m.pos_title = hmi_card_title(c, "");
    lv_obj_t *big = hmi_box(c, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(big, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_bottom(big, 6, 0);
    s_m.pos_value = hmi_label(big, "", HMI_FS_HUGE, HMI_SEMIBOLD, HMI_TEXT);
    s_m.pos_unit = hmi_label(big, "rev", HMI_FS_LARGE, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_style_pad_bottom(s_m.pos_unit, 8, 0);
    s_m.pos_raw = value_row(c, "Pulses");
    s_m.speed = value_row(c, "Speed");
    s_m.state = value_row(c, "State");
    s_m.drive_error = value_row(c, "Error");
    s_m.units_note = hmi_label(c, "", HMI_FS_SMALL, HMI_REGULAR, HMI_CAUTION);
    lv_obj_set_width(s_m.units_note, LV_PCT(100));

    /* Middle: the jog settings in one card, then status and commands. Sized for the 470 px body
     * so the command buttons stay visible (one card per setting pushed them out of the page). */
    lv_obj_t *mid = column(body, 0);
    c = card(mid, false);
    setting_row(c, "Jog mode", s_mode_labels, 2, s_mode, mode_pick_cb, NULL);
    s_m.step_seg = setting_row(c, "Step", s_step_labels, 4, s_step, step_pick_cb, &s_m.step_name);
    hmi_segmented_set_enabled(s_m.step_seg, s_mode == MODE_STEP);

    c = card(mid, true);
    lv_obj_t *lamps = hmi_box(c, LV_FLEX_FLOW_ROW, 20);
    lv_obj_set_flex_align(lamps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const lamp_names[] = {"Servo on", "Homed", "Alarm"};
    lv_obj_t **lamp_obj[] = {&s_m.lamp_on, &s_m.lamp_homed, &s_m.lamp_alarm};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = hmi_box(lamps, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        *lamp_obj[i] = hmi_lamp(l);
        hmi_label(l, lamp_names[i], HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    }
    s_m.message = hmi_label(c, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_m.message, LV_PCT(100));
    lv_label_set_long_mode(s_m.message, LV_LABEL_LONG_MODE_DOTS); /* one line: keeps the buttons on the page */
    lv_obj_set_height(s_m.message, lv_font_get_line_height(hmi_font(HMI_FS_BODY, HMI_REGULAR)));

    lv_obj_t *move = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(move, LV_PCT(100));
    lv_obj_set_flex_align(move, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(move, "Move to", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_t *field = hmi_control(move, 0, HMI_TOUCH_H);
    lv_obj_set_flex_grow(field, 1);
    lv_obj_set_flex_align(field, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(field, 12, 0);
    lv_obj_set_style_border_side(field, LV_BORDER_SIDE_FULL, 0);
    s_m.move_value = hmi_label(field, "", HMI_FS_LARGE, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(field, target_cb, LV_EVENT_CLICKED, NULL);
    target_set_cb(s_target, NULL);
    lv_obj_t *go = hmi_filled_button(move, HMI_TOUCH_H, HMI_ACCENT, 4);
    lv_obj_set_width(go, 96);
    s_m.go_label = hmi_label(go, "Go", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(go, go_cb, LV_EVENT_CLICKED, NULL);

    hmi_spacer(c);
    lv_obj_t *actions = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(actions, LV_PCT(100));
    s_m.servo_label = action_button(actions, "Servo on", HMI_ICON_PLAY, servo_cb);
    action_button(actions, "Home axis", HMI_ICON_HOME, home_cb);
    actions = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_t *stop = hmi_filled_button(actions, HMI_TOUCH_H, HMI_CRITICAL, 4);
    lv_obj_set_width(stop, 0);
    lv_obj_set_flex_grow(stop, 1);
    lv_obj_set_style_pad_column(stop, 8, 0);
    hmi_icon(stop, HMI_ICON_STOP, HMI_ICON_BTN, HMI_WHITE);
    hmi_label(stop, "Stop", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(stop, stop_cb, LV_EVENT_CLICKED, NULL);
    action_button(actions, "Alarm reset", HMI_ICON_RESET, reset_cb);

    /* Right: jog buttons with the speed under them */
    lv_obj_t *right = column(body, RIGHT_W);
    lv_obj_set_style_pad_row(right, 10, 0);
    s_m.plus_btn = jog_button(right, +1, &s_m.plus_label);
    s_m.minus_btn = jog_button(right, -1, &s_m.minus_label);
    s_m.speed_seg = hmi_segmented(right, s_speed_labels, 3, 0, SEG_H, s_speed, speed_pick_cb, NULL);
    s_m.hint = hmi_label(right, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_m.hint, LV_PCT(100));
    lv_obj_set_style_text_align(s_m.hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* (Re)build the page for the current axes. Runs from the shell or the poll timer, never from an
 * event of an object it deletes. */
static void build(void)
{
    jog_stop();
    lv_obj_t *page = s_m.page;
    lv_obj_t *root = s_m.root;
    lv_timer_t *poll = s_m.poll;
    memset(&s_m, 0, sizeof(s_m));
    s_m.page = page;
    s_m.root = root;
    s_m.poll = poll;
    lv_obj_clean(s_m.root);

    s_m.in_op = s_ax.in_op;
    s_m.n_axes = s_ax.n_axes;
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        strlcpy(s_m.names[i], s_ax.axis[i].name, sizeof(s_m.names[i]));
    }
    if (s_axis_sel >= s_ax.n_axes) {
        s_axis_sel = 0;
    }

    header();
    if (!s_ax.in_op || s_ax.n_axes == 0) {
        build_no_axes();
    } else {
        build_jog();
        update();
    }
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    jog_stop(); /* leaving the page while a jog button is held */
    if (s_m.poll) {
        lv_timer_delete(s_m.poll);
    }
    memset(&s_m, 0, sizeof(s_m));
}

static lv_obj_t *s_page; /* the shell's page area while Manual is shown */

static void page_deleted_cb(lv_event_t *e)
{
    (void)e;
    s_page = NULL;
}

static void create_view(lv_obj_t *page)
{
    lv_obj_t *root = hmi_box(page, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(root, page_deleted_cb, LV_EVENT_DELETE, NULL);
    s_page = page;
    if (s_view == HMI_MANUAL_TEACH) {
        hmi_teach_create(root);
        return;
    }
#if CONFIG_ECAT_PATH
    if (s_view == HMI_MANUAL_INTERP) {
        hmi_interp_create(root);
        return;
    }
#endif
    memset(&s_m, 0, sizeof(s_m));
    s_m.page = page;
    s_m.root = hmi_box(root, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(s_m.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(s_m.root, deleted_cb, LV_EVENT_DELETE, NULL);
    ecat_axes_read(&s_ax);
    build();
    s_m.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}

void hmi_page_manual_create(lv_obj_t *page)
{
    create_view(page);
}

/* After the event that asked for it: the old view (and the pill that was tapped) is deleted. */
static void open_async(void *arg)
{
    s_view = (hmi_manual_view_t)(intptr_t)arg;
    if (s_page) {
        lv_obj_t *page = s_page;
        lv_obj_clean(page);
        create_view(page);
    } else {
        hmi_shell_show(HMI_PAGE_MANUAL);
    }
}

void hmi_manual_open(hmi_manual_view_t view)
{
    lv_async_call(open_async, (void *)(intptr_t)view);
}
