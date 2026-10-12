/*
 * Manual > Teaching (mockup Teaching.dc.html): teach points of an axis group, with the point's
 * and the current position side by side, Teach (with confirmation), Go to (hold to run) and step
 * jog buttons for the group's axes.
 *
 * Groups are the axes of the Manual page taken three at a time (AX1-AX3, AX4-AX6, ...) until
 * the machine defines its own groups. Points are stored per group in drive position units
 * (machine_params.h) and shown in each axis' unit: mm or deg once its Distance/Rev is set, else
 * motor revolutions (hmi_axis.c). There is no recipe system yet, so the points are not tied to a product.
 *
 * Go to: every axis of the group moves to the point with a PP move at the point's speed (% of
 * the axis' high jog speed); the axes are not interpolated. Releasing the button stops them.
 *
 * Delete removes the selected point; the points after it move up one number. Add, Teach, Delete
 * and the speed change the list in RAM; Save points stores the group (machine_store.h).
 */

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "ecat_axis.h"
#include "hmi_internal.h"
#include "machine_params.h"
#include "machine_store.h"

#define POLL_MS       100
#define RIGHT_W       300
#define ROW_H         52
#define COL_NO_W      52
#define COL_AXIS_W    92
#define COL_SPEED_W   68
#define ACCENT_10_OPA 26
#define POS_TOLERANCE 0.0005 /* displayed unit: closer than this shows as "at the point" */

/* Kept while the firmware runs: edits survive leaving the page (not a reset). */
static int s_group;
static int s_sel;
static bool s_dirty;
static bool s_loaded;
static EXT_RAM_BSS_ATTR machine_points_t s_pts;   /* ~1.1 KB, PSRAM */
static EXT_RAM_BSS_ATTR ecat_axes_status_t s_ax;  /* ~1 KB, PSRAM */

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *status_tag;
    lv_obj_t *group_label;
    lv_obj_t *unsaved;
    lv_obj_t *list;
    lv_obj_t *message;
    lv_obj_t *sel_no, *sel_name;
    lv_obj_t *pt_val[MACHINE_GROUP_AXES];
    lv_obj_t *cur_val[MACHINE_GROUP_AXES];
    lv_obj_t *speed_value;
    lv_obj_t *goto_btn;
    lv_obj_t *teach_btn;
    lv_obj_t *jog_caption;
    lv_timer_t *poll;
    bool going;
    /* layout signature */
    bool in_op;
    uint8_t n_axes;
    char names[ECAT_MAX_AXES][ECAT_TOPO_NAME_LEN];
    /* group axes: index into s_ax.axis[] */
    uint8_t n_grp;
    uint8_t idx[MACHINE_GROUP_AXES];
} s_t;

static void build(void);

/* ---- Helpers ---- */

static int n_groups(void)
{
    return (s_ax.n_axes + MACHINE_GROUP_AXES - 1) / MACHINE_GROUP_AXES;
}

static const ecat_axis_status_t *gax(int a)
{
    return a < s_t.n_grp ? &s_ax.axis[s_t.idx[a]] : NULL;
}

static void gscale(int a, hmi_scale_t *sc)
{
    hmi_axis_scale(gax(a)->name, gax(a)->units_per_rev, sc);
}

static void message(const char *text, bool error)
{
    hmi_set_text(s_t.message, text);
    lv_obj_set_style_text_color(s_t.message, HMI_C(error ? HMI_CRITICAL : HMI_TEXT_2), 0);
}

static void point_no(int i, char *out, size_t len)
{
    snprintf(out, len, "P%02u", (unsigned)(i + 1) % 100u); /* at most MACHINE_MAX_POINTS */
}

static void load_group(void)
{
    const char *names[MACHINE_GROUP_AXES];
    for (int a = 0; a < s_t.n_grp; a++) {
        names[a] = gax(a)->name;
    }
    machine_points_load(names, s_t.n_grp, &s_pts);
    s_loaded = true;
    s_dirty = false;
    if (s_sel >= s_pts.n_points) {
        s_sel = 0;
    }
}

/* The stored group must be the one on screen (same axes in the same order). */
static bool group_matches(void)
{
    if (!s_loaded || s_pts.n_axes != s_t.n_grp) {
        return false;
    }
    for (int a = 0; a < s_t.n_grp; a++) {
        if (strncmp(s_pts.axis[a], gax(a)->name, MACHINE_AXIS_NAME_LEN) != 0) {
            return false;
        }
    }
    return true;
}

static void go_stop(void)
{
    if (!s_t.going) {
        return;
    }
    s_t.going = false;
    for (int a = 0; a < s_t.n_grp; a++) {
        const ecat_axis_status_t *x = gax(a);
        (void)hmi_axis_cmd(s_t.idx[a], x->name, x->units_per_rev, ECAT_AXIS_STOP, 0, 0, 0);
    }
}

/* ---- Points list ---- */

static void row_style(lv_obj_t *row, bool sel)
{
    lv_obj_set_style_bg_color(row, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_bg_opa(row, sel ? ACCENT_10_OPA : LV_OPA_TRANSP, 0);
    hmi_show(lv_obj_get_child(row, 0), sel);
    lv_obj_t *no = lv_obj_get_child(row, 1);
    lv_obj_set_style_text_color(no, HMI_C(sel ? HMI_ACCENT : HMI_TEXT_2), 0);
    lv_obj_set_style_text_font(no, hmi_font(HMI_FS_BODY, sel ? HMI_SEMIBOLD : HMI_REGULAR), 0);
}

static void update_card(void);

static void row_clicked_cb(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == s_sel || i >= s_pts.n_points) {
        return;
    }
    go_stop();
    lv_obj_t *old = lv_obj_get_child(s_t.list, s_sel);
    if (old) {
        row_style(old, false);
    }
    s_sel = i;
    row_style(lv_obj_get_child(s_t.list, i), true);
    update_card();
}

static lv_obj_t *cell(lv_obj_t *row, const char *text, int32_t w, bool right, hmi_weight_t weight, uint32_t color)
{
    lv_obj_t *l = hmi_label(row, text, HMI_FS_BODY, weight, color);
    if (w > 0) {
        lv_obj_set_width(l, w);
    } else {
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    if (right) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    return l;
}

static void row_fill(lv_obj_t *row, int i)
{
    const machine_point_t *p = &s_pts.pt[i];
    char t[24];
    hmi_set_text(lv_obj_get_child(row, 2), p->name);
    for (int a = 0; a < s_t.n_grp; a++) {
        hmi_scale_t sc;
        gscale(a, &sc);
        snprintf(t, sizeof(t), "%.3f", hmi_to_pos(&sc, p->pos[a]));
        hmi_set_text(lv_obj_get_child(row, 3 + a), t);
    }
    snprintf(t, sizeof(t), "%u %%", p->speed_pct);
    hmi_set_text(lv_obj_get_child(row, 3 + s_t.n_grp), t);
}

static void list_fill(void)
{
    lv_obj_clean(s_t.list);
    if (s_pts.n_points == 0) {
        lv_obj_t *l = hmi_label(s_t.list, "No points. Add point stores the current position of the group's axes.", HMI_FS_SMALL,
                                HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_pad_all(l, 12, 0);
        return;
    }
    for (int i = 0; i < s_pts.n_points; i++) {
        lv_obj_t *row = hmi_box(s_t.list, LV_FLEX_FLOW_ROW, 8);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_divider_top(row);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *bar = hmi_box(row, -1, 0);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(bar, 3, 16);
        lv_obj_set_pos(bar, -12, (ROW_H - 16) / 2 - 1);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, HMI_C(HMI_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        char no[8];
        point_no(i, no, sizeof(no));
        cell(row, no, COL_NO_W, false, HMI_REGULAR, HMI_TEXT_2);
        cell(row, "", 0, false, HMI_SEMIBOLD, HMI_TEXT);
        for (int a = 0; a < s_t.n_grp; a++) {
            cell(row, "", COL_AXIS_W, true, HMI_REGULAR, HMI_TEXT);
        }
        cell(row, "", COL_SPEED_W, true, HMI_REGULAR, HMI_TEXT);
        row_fill(row, i);
        row_style(row, i == s_sel);
    }
}

/* ---- Point card ---- */

static void update_card(void)
{
    if (s_t.sel_no == NULL) {
        return;
    }
    if (s_sel >= s_pts.n_points) {
        hmi_set_text(s_t.sel_no, "\xE2\x80\x94");
        hmi_set_text(s_t.sel_name, "No points");
        for (int a = 0; a < s_t.n_grp; a++) {
            hmi_set_text(s_t.pt_val[a], "\xE2\x80\x94");
            hmi_set_text(s_t.cur_val[a], "");
        }
        hmi_set_text(s_t.speed_value, "\xE2\x80\x94");
        return;
    }
    const machine_point_t *p = &s_pts.pt[s_sel];
    char t[32];
    point_no(s_sel, t, sizeof(t));
    hmi_set_text(s_t.sel_no, t);
    hmi_set_text(s_t.sel_name, p->name);
    for (int a = 0; a < s_t.n_grp; a++) {
        const ecat_axis_status_t *x = gax(a);
        hmi_scale_t sc;
        gscale(a, &sc);
        const double pt = hmi_to_pos(&sc, p->pos[a]);
        const double now = hmi_to_pos(&sc, x->position);
        snprintf(t, sizeof(t), "%.3f", pt);
        hmi_set_text(s_t.pt_val[a], t);
        snprintf(t, sizeof(t), "%.3f", now);
        hmi_set_text(s_t.cur_val[a], t);
        const double d = now - pt;
        lv_obj_set_style_text_color(s_t.cur_val[a],
                                    HMI_C(d < POS_TOLERANCE && d > -POS_TOLERANCE ? HMI_SUCCESS : HMI_CAUTION), 0);
    }
    snprintf(t, sizeof(t), "%u %%", p->speed_pct);
    hmi_set_text(s_t.speed_value, t);
}

static void mark_dirty(void)
{
    s_dirty = true;
    hmi_tag_set(s_t.unsaved, "Unsaved", HMI_CAUTION_BG, HMI_CAUTION);
    hmi_show(s_t.unsaved, true);
}

static void teach_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (!ok || s_t.root == NULL || s_sel >= s_pts.n_points) {
        return;
    }
    ecat_axes_read(&s_ax);
    for (int a = 0; a < s_t.n_grp; a++) {
        s_pts.pt[s_sel].pos[a] = gax(a)->position;
    }
    mark_dirty();
    row_fill(lv_obj_get_child(s_t.list, s_sel), s_sel);
    update_card();
    message("Point taught. Save the points to keep them.", false);
}

static void teach_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel >= s_pts.n_points) {
        return;
    }
    char title[48], body[320], no[8];
    point_no(s_sel, no, sizeof(no));
    snprintf(title, sizeof(title), "Teach %s \xC2\xB7 %s?", no, s_pts.pt[s_sel].name);
    int n = snprintf(body, sizeof(body),
                     "The point is overwritten with the current position. Save the points afterwards to keep them.\n");
    for (int a = 0; a < s_t.n_grp && n > 0 && n < (int)sizeof(body); a++) {
        const ecat_axis_status_t *x = gax(a);
        hmi_scale_t sc;
        gscale(a, &sc);
        n += snprintf(body + n, sizeof(body) - (size_t)n, "\n%s   %.3f %s, was %.3f", x->name,
                      hmi_to_pos(&sc, x->position), sc.pos, hmi_to_pos(&sc, s_pts.pt[s_sel].pos[a]));
    }
    hmi_confirm(title, body, "Teach", false, teach_confirm_cb, NULL);
}

static void goto_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        go_stop();
        return;
    }
    if (code != LV_EVENT_PRESSED || s_sel >= s_pts.n_points) {
        return;
    }
    for (int a = 0; a < s_t.n_grp; a++) {
        if (!gax(a)->enabled) {
            char t[64];
            snprintf(t, sizeof(t), "Switch the servo of %s on first (Jog view).", gax(a)->name);
            message(t, true);
            return;
        }
    }
    const machine_point_t *p = &s_pts.pt[s_sel];
    for (int a = 0; a < s_t.n_grp; a++) {
        const ecat_axis_status_t *x = gax(a);
        machine_axis_cfg_t cfg;
        machine_axis_get(x->name, &cfg);
        double rpm = cfg.jog_high_rpm * p->speed_pct / 100.0;
        rpm = rpm < 1.0 ? 1.0 : rpm;
        (void)hmi_axis_cmd(s_t.idx[a], x->name, x->units_per_rev, ECAT_AXIS_MOVE_ABS, 0, p->pos[a], rpm);
    }
    s_t.going = true;
    message("Moving to the point \xC2\xB7 release to stop", false);
}

static void delete_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (!ok || s_t.root == NULL || s_sel >= s_pts.n_points) {
        return;
    }
    go_stop();
    const int n = s_pts.n_points;
    memmove(&s_pts.pt[s_sel], &s_pts.pt[s_sel + 1], sizeof(s_pts.pt[0]) * (size_t)(n - s_sel - 1));
    memset(&s_pts.pt[n - 1], 0, sizeof(s_pts.pt[0]));
    s_pts.n_points = (uint8_t)(n - 1);
    if (s_sel >= s_pts.n_points && s_sel > 0) {
        s_sel--;
    }
    mark_dirty();
    list_fill();
    update_card();
    message("Point deleted. Save the points to keep the change.", false);
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel >= s_pts.n_points) {
        message("There is no point to delete.", false);
        return;
    }
    char title[48], no[8];
    point_no(s_sel, no, sizeof(no));
    snprintf(title, sizeof(title), "Delete %s \xC2\xB7 %s?", no, s_pts.pt[s_sel].name);
    hmi_confirm(title,
                "The point is removed from the group. The points after it move up one number. "
                "Save the points afterwards to keep the change.",
                "Delete", true, delete_confirm_cb, NULL);
}

static void speed_set_cb(double value, void *user)
{
    (void)user;
    if (s_t.root == NULL || s_sel >= s_pts.n_points) {
        return;
    }
    s_pts.pt[s_sel].speed_pct = (uint8_t)(value + 0.5);
    mark_dirty();
    row_fill(lv_obj_get_child(s_t.list, s_sel), s_sel);
    update_card();
}

static void speed_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < s_pts.n_points) {
        hmi_keypad("Go-to speed", "%", s_pts.pt[s_sel].speed_pct, 1, 100, 0, speed_set_cb, NULL);
    }
}

/* ---- Header actions ---- */

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (s_pts.n_points >= MACHINE_MAX_POINTS) {
        message("The group already has the most points (32).", true);
        return;
    }
    const int i = s_pts.n_points++;
    machine_point_t *p = &s_pts.pt[i];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "POINT_%d", i + 1);
    p->speed_pct = 50;
    for (int a = 0; a < s_t.n_grp; a++) {
        p->pos[a] = gax(a)->position;
    }
    s_sel = i;
    mark_dirty();
    list_fill();
    lv_obj_scroll_to_view(lv_obj_get_child(s_t.list, i), LV_ANIM_OFF);
    update_card();
    message("Point added at the current position.", false);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (machine_points_save(&s_pts) != ESP_OK) {
        message("Saving failed: see the console log.", true);
        return;
    }
    s_dirty = false;
    if (machine_store_pending()) {
        hmi_tag_set(s_t.unsaved, "Saved when EtherCAT leaves OP", HMI_CAUTION_BG, HMI_CAUTION);
        message("Kept in RAM: the store is full and is tidied up when EtherCAT leaves OP.", false);
    } else {
        hmi_show(s_t.unsaved, false);
        message("Points saved.", false);
    }
}

static void switch_group(void *arg)
{
    (void)arg;
    if (s_t.root == NULL) {
        return;
    }
    go_stop();
    s_group = (s_group + 1) % (n_groups() > 0 ? n_groups() : 1);
    s_loaded = false;
    s_sel = 0;
    build();
}

static void discard_cb(bool ok, void *user)
{
    (void)user;
    if (ok) {
        lv_async_call(switch_group, NULL);
    }
}

static void group_cb(lv_event_t *e)
{
    (void)e;
    if (n_groups() < 2) {
        message("All axes are in one group.", false);
        return;
    }
    if (s_dirty) {
        hmi_confirm("Discard unsaved points?", "The points of this group were changed and not saved.", "Discard", true,
                    discard_cb, NULL);
        return;
    }
    lv_async_call(switch_group, NULL); /* rebuilds the page this button lives in */
}

/* ---- Jog ---- */

static void jog_cb(lv_event_t *e)
{
    const int v = (int)(intptr_t)lv_event_get_user_data(e);
    const int a = v / 2;
    const int dir = (v % 2) ? 1 : -1;
    const ecat_axis_status_t *x = gax(a);
    if (x == NULL) {
        return;
    }
    if (!x->enabled) {
        char t[64];
        snprintf(t, sizeof(t), "Switch the servo of %s on first (Jog view).", x->name);
        message(t, true);
        return;
    }
    hmi_scale_t sc;
    gscale(a, &sc);
    (void)hmi_axis_cmd(s_t.idx[a], x->name, x->units_per_rev, ECAT_AXIS_MOVE_REL, 0,
                       hmi_from_pos(&sc, dir * hmi_manual_step()), hmi_manual_speed_rpm(x->name));
}

/* ---- Update ---- */

static bool layout_changed(void)
{
    if (s_ax.in_op != s_t.in_op || s_ax.n_axes != s_t.n_axes) {
        return true;
    }
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        if (strcmp(s_ax.axis[i].name, s_t.names[i]) != 0) {
            return true;
        }
    }
    return false;
}

static void update(void)
{
    char t[64];
    snprintf(t, sizeof(t), "%u axes", s_ax.n_axes);
    hmi_tag_set(s_t.status_tag, s_ax.bus_ok ? t : "Bus errors", s_ax.bus_ok ? HMI_SUCCESS_BG : HMI_CRITICAL_BG,
                s_ax.bus_ok ? HMI_SUCCESS : HMI_CRITICAL);
    update_card();
    bool all_on = true;
    for (int a = 0; a < s_t.n_grp; a++) {
        all_on &= gax(a)->enabled;
    }
    lv_obj_set_style_opa(s_t.goto_btn, all_on ? LV_OPA_COVER : LV_OPA_40, 0);
    if (s_t.n_grp > 0) {
        hmi_scale_t sc;
        gscale(0, &sc);
        snprintf(t, sizeof(t), "Step %.2f %s", hmi_manual_step(), sc.pos);
        hmi_set_text(s_t.jog_caption, t);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    ecat_axes_read(&s_ax);
    if (layout_changed()) {
        go_stop();
        build();
        return;
    }
    if (s_t.sel_no) {
        update();
    }
}

/* ---- Layout ---- */

static void open_ethercat_cb(lv_event_t *e)
{
    (void)e;
    hmi_settings_open_ethercat();
}

static void left_card(lv_obj_t *col)
{
    lv_obj_t *c = hmi_card(col, 12, 12, 0);
    lv_obj_set_style_pad_bottom(c, 8, 0);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_flex_grow(c, 1);

    lv_obj_t *top = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(top, LV_PCT(100), HMI_TOUCH_H);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *grp = hmi_control(top, LV_SIZE_CONTENT, HMI_TOUCH_H);
    lv_obj_set_style_pad_left(grp, 12, 0);
    lv_obj_set_style_pad_right(grp, 10, 0);
    char t[48];
    snprintf(t, sizeof(t), "Group %d \xC2\xB7 %s", s_group + 1, gax(0)->name);
    if (s_t.n_grp > 1) {
        snprintf(t + strlen(t), sizeof(t) - strlen(t), "\xE2\x80\x93%s", gax(s_t.n_grp - 1)->name);
    }
    s_t.group_label = hmi_label(grp, t, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    if (n_groups() > 1) {
        hmi_icon(grp, HMI_ICON_CHEVRON_DOWN, 18, HMI_TEXT_2);
    }
    lv_obj_add_event_cb(grp, group_cb, LV_EVENT_CLICKED, NULL);

    s_t.unsaved = hmi_unsaved_tag(top, "Unsaved");
    if (s_dirty) {
        hmi_show(s_t.unsaved, true);
    }
    hmi_spacer(top);
    lv_obj_t *add = hmi_control(top, LV_SIZE_CONTENT, HMI_TOUCH_H);
    lv_obj_set_style_pad_hor(add, 16, 0);
    hmi_icon(add, HMI_ICON_PLUS, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(add, "Add", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save = hmi_filled_button(top, HMI_TOUCH_H, HMI_ACCENT, 4);
    lv_obj_set_width(save, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(save, 18, 0);
    hmi_icon(save, HMI_ICON_SAVE, HMI_ICON_BTN, HMI_WHITE);
    hmi_label(save, "Save", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *head = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(head, LV_PCT(100), 32);
    lv_obj_set_style_margin_top(head, 8, 0);
    lv_obj_set_style_pad_hor(head, 12, 0);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(head, "No", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), COL_NO_W);
    lv_obj_t *name = hmi_label(head, "Name", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(name, 1);
    for (int a = 0; a < s_t.n_grp; a++) {
        hmi_scale_t sc;
        gscale(a, &sc);
        snprintf(t, sizeof(t), "%s %s", gax(a)->name, sc.pos);
        lv_obj_t *l = hmi_label(head, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(l, COL_AXIS_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_t *sp = hmi_label(head, "Speed", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(sp, COL_SPEED_W);
    lv_obj_set_style_text_align(sp, LV_TEXT_ALIGN_RIGHT, 0);

    s_t.list = hmi_box(c, LV_FLEX_FLOW_COLUMN, 0);
    hmi_scroll_area(s_t.list);
    lv_obj_set_width(s_t.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_t.list, 1);
    list_fill();
}

static void point_card(lv_obj_t *col)
{
    lv_obj_t *c = hmi_card(col, 12, 14, 8);
    lv_obj_set_style_pad_bottom(c, 14, 0);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));

    lv_obj_t *title = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(title, LV_PCT(100));
    s_t.sel_no = hmi_label(title, "", HMI_FS_HEAD, HMI_SEMIBOLD, HMI_ACCENT);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_t.sel_name = hmi_label(title, "", HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_flex_grow(s_t.sel_name, 1);
    lv_label_set_long_mode(s_t.sel_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_t.sel_name, lv_font_get_line_height(hmi_font(HMI_FS_HEAD, HMI_SEMIBOLD)));
    lv_obj_t *del = hmi_control(title, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(del, 14, 0);
    hmi_label(del, "Delete", HMI_FS_BODY, HMI_REGULAR, HMI_CRITICAL);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tbl = hmi_box(c, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(tbl, LV_PCT(100));
    lv_obj_t *h = hmi_box(tbl, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(h, LV_PCT(100), 24);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(h, "Axis", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), 48);
    static const char *const heads[] = {"Point", "Current"};
    for (int k = 0; k < 2; k++) {
        lv_obj_t *l = hmi_label(h, heads[k], HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    for (int a = 0; a < s_t.n_grp; a++) {
        lv_obj_t *r = hmi_box(tbl, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_size(r, LV_PCT(100), 38);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_divider_top(r);
        lv_obj_set_width(hmi_label(r, gax(a)->name, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT), 48);
        s_t.pt_val[a] = hmi_label(r, "", HMI_FS_LARGE, HMI_REGULAR, HMI_TEXT);
        s_t.cur_val[a] = hmi_label(r, "", HMI_FS_LARGE, HMI_REGULAR, HMI_SUCCESS);
        lv_obj_set_flex_grow(s_t.pt_val[a], 1);
        lv_obj_set_flex_grow(s_t.cur_val[a], 1);
        lv_obj_set_style_text_align(s_t.pt_val[a], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_align(s_t.cur_val[a], LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_t *r = hmi_box(tbl, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(r, LV_PCT(100), 56);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_divider_top(r);
    lv_obj_t *sl = hmi_label(r, "Go-to speed", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(sl, 1);
    lv_obj_t *sv = hmi_control(r, 100, 44);
    lv_obj_set_flex_align(sv, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(sv, 10, 0);
    s_t.speed_value = hmi_label(sv, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(sv, speed_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btns = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_style_pad_top(btns, 4, 0);
    s_t.teach_btn = hmi_filled_button(btns, 60, HMI_ACCENT, 4);
    lv_obj_set_width(s_t.teach_btn, 0);
    lv_obj_set_flex_grow(s_t.teach_btn, 1);
    hmi_label(s_t.teach_btn, "Teach", HMI_FS_LARGE, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(s_t.teach_btn, teach_cb, LV_EVENT_CLICKED, NULL);
    s_t.goto_btn = hmi_control(btns, 0, 60);
    lv_obj_set_flex_grow(s_t.goto_btn, 1);
    hmi_label(s_t.goto_btn, "Go to (hold)", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(s_t.goto_btn, goto_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_t.goto_btn, goto_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_t.goto_btn, goto_cb, LV_EVENT_PRESS_LOST, NULL);

    s_t.message = hmi_label(c, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_t.message, LV_PCT(100));
}

static void jog_card(lv_obj_t *col)
{
    lv_obj_t *c = hmi_card(col, 10, 12, 8);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_t *title = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    hmi_label(title, "Jog", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    s_t.jog_caption = hmi_label(title, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_t *row = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    for (int a = 0; a < s_t.n_grp; a++) {
        for (int d = 0; d < 2; d++) {
            lv_obj_t *b = hmi_control(row, 0, HMI_TOUCH_H);
            lv_obj_set_flex_grow(b, 1);
            char t[16];
            snprintf(t, sizeof(t), d ? "%s +" : "%s \xE2\x88\x92", gax(a)->name);
            hmi_label(b, t, HMI_FS_LARGE, HMI_SEMIBOLD, HMI_TEXT);
            lv_obj_add_event_cb(b, jog_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(a * 2 + d));
        }
    }
}

static void build(void)
{
    lv_obj_t *root = s_t.root;
    lv_timer_t *poll = s_t.poll;
    memset(&s_t, 0, sizeof(s_t));
    s_t.root = root;
    s_t.poll = poll;
    lv_obj_clean(root);

    s_t.in_op = s_ax.in_op;
    s_t.n_axes = s_ax.n_axes;
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        strlcpy(s_t.names[i], s_ax.axis[i].name, sizeof(s_t.names[i]));
    }

    lv_obj_t *row = hmi_manual_header(root, HMI_MANUAL_TEACH);
    s_t.status_tag = hmi_tag(row, true);
    if (!s_ax.in_op || s_ax.n_axes == 0) {
        hmi_tag_set(s_t.status_tag, s_ax.in_op ? "No drives" : "Not in OP", HMI_CAUTION_BG, HMI_CAUTION);
        hmi_info_card(root, "No axes to teach",
                      s_ax.in_op ? "The accepted topology has no servo or stepper drive."
                                 : "Teaching needs the axes: the EtherCAT bus is not in OP. Scan the bus and accept "
                                   "the topology in Settings \xE2\x80\xBA EtherCAT.",
                      "Open EtherCAT settings", open_ethercat_cb);
        return;
    }

    if (s_group >= n_groups()) {
        s_group = 0;
    }
    const int first = s_group * MACHINE_GROUP_AXES;
    for (int a = 0; a < MACHINE_GROUP_AXES && first + a < s_ax.n_axes; a++) {
        s_t.idx[a] = (uint8_t)(first + a);
        s_t.n_grp++;
    }
    if (!group_matches()) {
        load_group();
    }
    if (s_sel >= s_pts.n_points) {
        s_sel = 0;
    }

    s_t.body = hmi_box(root, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(s_t.body, LV_PCT(100));
    lv_obj_set_flex_grow(s_t.body, 1);
    lv_obj_t *left = hmi_box(s_t.body, LV_FLEX_FLOW_COLUMN, 10);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 1);
    left_card(left);
    jog_card(left);
    lv_obj_t *right = hmi_box(s_t.body, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_size(right, RIGHT_W, LV_PCT(100));
    point_card(right);
    update();
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    go_stop(); /* leaving the page while Go to is held */
    if (s_t.poll) {
        lv_timer_delete(s_t.poll);
    }
    memset(&s_t, 0, sizeof(s_t));
}

void hmi_teach_create(lv_obj_t *root)
{
    memset(&s_t, 0, sizeof(s_t));
    s_t.root = hmi_box(root, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(s_t.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(s_t.root, deleted_cb, LV_EVENT_DELETE, NULL);
    ecat_axes_read(&s_ax);
    build();
    s_t.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
