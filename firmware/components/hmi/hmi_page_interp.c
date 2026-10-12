/*
 * Manual > Interpolation: run a straight line or a circular arc on two axes in CSP and see how
 * well they stay on the path (ecat_path_* in ecat_axis.h). For bench tests of the interpolation.
 *
 * The axes are picked with two drop-down lists, X and Y. Distances are in X's display unit
 * (Settings > Servo; Y should use the same unit), relative to where the axes are at Start:
 *   - Line: X and Y distance;
 *   - Arc CW / CCW: centre (I, J) and the angle to turn, 360 = a full circle.
 * Speed is along the path and Ramp the time from standstill to it. Both are limited by the axes'
 * Max speed and Acc time (Settings > Servo), also for the centripetal acceleration of an arc.
 *
 * The plot shows the planned path and the actual positions polled during the run. The contour
 * error (distance of the actual position from the path) and the lag are measured every cycle by
 * the cyclic task.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "ecat_axis.h"
#include "hmi_internal.h"
#include "machine_params.h"

#define POLL_MS      40
#define LEFT_W       280
#define RIGHT_W      300
#define PLOT         288 /* plot canvas, px square */
#define PLOT_SAMPLES 96  /* segments of the planned path */
#define SEG_H        48
#define FIELD_H      48
#define RESULT_ROW_H 27
#define PLANNED      0xC8C8C8 /* planned path on the plot */
#define PI_D         3.14159265358979

enum { F_A, F_B, F_ANGLE, F_SPEED, F_RAMP, F_COUNT }; /* A/B: X/Y distance (line) or centre I/J (arc) */
enum { R_STATE, R_PROGRESS, R_SPEED, R_CONTOUR, R_LAG, R_TIME, R_COUNT };

static const char *const s_kind_labels[] = {"Line", "Arc CW", "Arc CCW"};

/* Operator choices: kept while the firmware runs. */
static int s_axis[2] = {0, 1};
static int s_kind = ECAT_PATH_LINE;
static double s_val[F_COUNT] = {10.0, 5.0, 360.0, 5.0, 200.0};

static struct {
    lv_obj_t *root;
    lv_timer_t *poll;
    /* Layout signature: rebuild when the axes change */
    bool in_op;
    uint8_t n_axes;
    char names[ECAT_MAX_AXES][ECAT_TOPO_NAME_LEN];
    /* Widgets */
    lv_obj_t *status_tag;
    lv_obj_t *dd[2];
    lv_obj_t *ax_state[2];
    lv_obj_t *servo_label;
    lv_obj_t *kind_seg;
    lv_obj_t *cell[F_COUNT];
    lv_obj_t *f_box[F_COUNT];
    lv_obj_t *f_cap[F_COUNT];
    lv_obj_t *f_val[F_COUNT];
    lv_obj_t *length;
    lv_obj_t *note;
    lv_obj_t *start_btn;
    lv_obj_t *result[R_COUNT];
    lv_obj_t *canvas;
    lv_draw_buf_t *buf;
    lv_obj_t *marker;
    lv_obj_t *plot_caption;
    /* Plot: centre of the view (path units) and scale */
    float cx, cy, px_per_unit;
    float last[2];
    bool have_last;
    int32_t origin[2]; /* drive positions at the plot's (0, 0) */
    uint32_t trace_gen;
    bool tracing;
    int8_t locked;      /* controls shown locked (path running): -1 = not set yet */
    int8_t start_dim;
} s_i;

static EXT_RAM_BSS_ATTR ecat_axes_status_t s_ax; /* snapshot, PSRAM */

static void build(void);

#define set_text hmi_set_text

/* ---- Axes and units ---- */

static const ecat_axis_status_t *axis(int k)
{
    const int i = s_axis[k];
    return (i >= 0 && i < s_ax.n_axes) ? &s_ax.axis[i] : NULL;
}

static void scale_of(int k, hmi_scale_t *sc)
{
    const ecat_axis_status_t *a = axis(k);
    if (a) {
        hmi_axis_scale(a->name, a->units_per_rev, sc);
    } else {
        *sc = (hmi_scale_t){.units_per_pos = HMI_ASSUMED_UNITS_PER_REV, .rpm_per_speed = 1.0, .pos = "rev",
                            .speed = "rpm"};
    }
}

/* Max speed (path units/s) and the acceleration of the Acc time (path units/s^2) of axis k. */
static void axis_limits(int k, double *vmax, double *amax)
{
    const ecat_axis_status_t *a = axis(k);
    machine_axis_cfg_t cfg;
    machine_axis_get(a ? a->name : "", &cfg);
    hmi_scale_t sc;
    scale_of(k, &sc);
    const double upr = (double)hmi_upr(a ? a->units_per_rev : 0);
    *vmax = cfg.max_rpm / 60.0 * upr / sc.units_per_pos;
    *amax = *vmax * 1000.0 / (cfg.acc_ms > 1.0f ? cfg.acc_ms : 1.0);
}

/* ---- The path of the current settings ---- */

typedef struct {
    ecat_path_cmd_t cmd;
    float length;
    bool limited; /* speed or ramp lowered by the axis limits */
    const char *why; /* NULL if valid */
} plan_t;

static void plan(plan_t *p)
{
    memset(p, 0, sizeof(*p));
    ecat_path_cmd_t *c = &p->cmd;
    c->axis[0] = (uint8_t)s_axis[0];
    c->axis[1] = (uint8_t)s_axis[1];
    c->kind = (uint8_t)s_kind;
    hmi_scale_t sx, sy;
    scale_of(0, &sx);
    scale_of(1, &sy);
    c->k[0] = (float)sx.units_per_pos;
    c->k[1] = (float)sy.units_per_pos;

    double r = 0.0;
    if (s_kind == ECAT_PATH_LINE) {
        c->end[0] = (float)s_val[F_A];
        c->end[1] = (float)s_val[F_B];
    } else {
        const double ci = s_val[F_A];
        const double cj = s_val[F_B];
        r = hypot(ci, cj);
        c->center[0] = (float)ci;
        c->center[1] = (float)cj;
        if (s_val[F_ANGLE] < 360.0) {
            const double sweep = (s_kind == ECAT_PATH_ARC_CCW ? 1.0 : -1.0) * s_val[F_ANGLE] * PI_D / 180.0;
            const double th = atan2(-cj, -ci) + sweep;
            c->end[0] = (float)(ci + r * cos(th));
            c->end[1] = (float)(cj + r * sin(th));
        } /* else end = start: a full circle */
    }

    /* Speed and acceleration within both axes' limits. On a line each axis moves a share of the
     * path; on an arc either axis may move at the full path speed, and the centripetal
     * acceleration v^2 / r must stay within the axes' acceleration too. */
    double v = s_val[F_SPEED];
    double a = v * 1000.0 / s_val[F_RAMP];
    const double len_line = hypot(c->end[0], c->end[1]);
    double a_lim = 1e30;
    for (int k = 0; k < 2; k++) {
        double vmax, amax;
        axis_limits(k, &vmax, &amax);
        double share = 1.0;
        if (s_kind == ECAT_PATH_LINE) {
            share = len_line > 0.0 ? fabs(k == 0 ? c->end[0] : c->end[1]) / len_line : 0.0;
        }
        if (share > 0.0 && v > vmax / share) {
            v = vmax / share;
            p->limited = true;
        }
        if (share > 0.0 && amax / share < a_lim) {
            a_lim = amax / share;
        }
    }
    if (s_kind != ECAT_PATH_LINE && r > 0.0 && v * v / r > a_lim * 0.5) {
        v = sqrt(a_lim * 0.5 * r); /* half the axis acceleration for the turn */
        p->limited = true;
    }
    if (a > a_lim) {
        a = a_lim;
        p->limited = true;
    }
    c->speed = (float)v;
    c->accel = (float)a;
    p->why = ecat_path_check(c, &p->length);
}

/* Points of the planned path (path units), PLOT_SAMPLES + 1 of them. */
static void plan_points(const plan_t *p, float *xs, float *ys)
{
    const ecat_path_cmd_t *c = &p->cmd;
    const double ci = c->center[0];
    const double cj = c->center[1];
    const double r = hypot(ci, cj);
    const double th0 = atan2(-cj, -ci);
    const double sweep = p->length > 0.0f && r > 0.0 ? (c->kind == ECAT_PATH_ARC_CCW ? 1.0 : -1.0) * p->length / r
                                                     : 0.0;
    for (int n = 0; n <= PLOT_SAMPLES; n++) {
        const double f = (double)n / PLOT_SAMPLES;
        if (c->kind == ECAT_PATH_LINE || p->why) {
            xs[n] = (float)(c->end[0] * f);
            ys[n] = (float)(c->end[1] * f);
        } else {
            xs[n] = (float)(ci + r * cos(th0 + sweep * f));
            ys[n] = (float)(cj + r * sin(th0 + sweep * f));
        }
    }
}

/* ---- Plot ---- */

static float map_x(float x)
{
    return PLOT / 2.0f + (x - s_i.cx) * s_i.px_per_unit;
}

static float map_y(float y)
{
    return PLOT / 2.0f - (y - s_i.cy) * s_i.px_per_unit;
}

static void plot_line(lv_layer_t *layer, float x1, float y1, float x2, float y2, uint32_t color, lv_opa_t opa,
                      int32_t w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = HMI_C(color);
    d.opa = opa;
    d.width = w;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static void plot_dot(lv_layer_t *layer, float x, float y, int32_t size, uint32_t color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = HMI_C(color);
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    const lv_area_t a = {(int32_t)x - size / 2, (int32_t)y - size / 2, (int32_t)x + size / 2, (int32_t)y + size / 2};
    lv_draw_rect(layer, &d, &a);
}

/* 1, 2 or 5 times a power of ten, about a sixth of the view. */
static double grid_step(double span)
{
    const double raw = span / 6.0;
    const double p = pow(10.0, floor(log10(raw)));
    const double m = raw / p;
    return p * (m < 1.5 ? 1.0 : m < 3.5 ? 2.0 : m < 7.5 ? 5.0 : 10.0);
}

/* Clear the plot and draw the grid and the planned path of the current settings. */
static void plot_reset(void)
{
    if (s_i.canvas == NULL) {
        return;
    }
    plan_t p;
    plan(&p);
    static float xs[PLOT_SAMPLES + 1], ys[PLOT_SAMPLES + 1];
    plan_points(&p, xs, ys);
    float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
    for (int n = 0; n <= PLOT_SAMPLES; n++) {
        x0 = fminf(x0, xs[n]);
        x1 = fmaxf(x1, xs[n]);
        y0 = fminf(y0, ys[n]);
        y1 = fmaxf(y1, ys[n]);
    }
    float span = fmaxf(x1 - x0, y1 - y0) * 1.2f;
    span = span > 1e-6f ? span : 1.0f;
    s_i.cx = (x0 + x1) / 2.0f;
    s_i.cy = (y0 + y1) / 2.0f;
    s_i.px_per_unit = PLOT / span;
    s_i.have_last = false;

    lv_canvas_fill_bg(s_i.canvas, HMI_C(HMI_WHITE), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(s_i.canvas, &layer);
    const double step = grid_step(span);
    const double gx0 = ceil((s_i.cx - span / 2.0f) / step) * step;
    const double gy0 = ceil((s_i.cy - span / 2.0f) / step) * step;
    for (double g = gx0; g <= s_i.cx + span / 2.0f; g += step) {
        const bool zero = fabs(g) < step / 2.0;
        plot_line(&layer, map_x((float)g), 0, map_x((float)g), PLOT, 0, zero ? LV_OPA_30 : LV_OPA_10, 1);
    }
    for (double g = gy0; g <= s_i.cy + span / 2.0f; g += step) {
        const bool zero = fabs(g) < step / 2.0;
        plot_line(&layer, 0, map_y((float)g), PLOT, map_y((float)g), 0, zero ? LV_OPA_30 : LV_OPA_10, 1);
    }
    if (p.why == NULL) {
        const int step = s_kind == ECAT_PATH_LINE ? PLOT_SAMPLES : 1; /* a line in one stroke: no joints */
        for (int n = 0; n < PLOT_SAMPLES; n += step) {
            plot_line(&layer, map_x(xs[n]), map_y(ys[n]), map_x(xs[n + step]), map_y(ys[n + step]), PLANNED,
                      LV_OPA_COVER, 6);
        }
        plot_dot(&layer, map_x(xs[PLOT_SAMPLES]), map_y(ys[PLOT_SAMPLES]), 12, HMI_RING);
    }
    plot_dot(&layer, map_x(0.0f), map_y(0.0f), 10, HMI_TEXT_2);
    lv_canvas_finish_layer(s_i.canvas, &layer);

    hmi_scale_t sc;
    scale_of(0, &sc);
    char t[64];
    snprintf(t, sizeof(t), "Grid %g %s \xC2\xB7 dark dot = start", step, sc.pos);
    set_text(s_i.plot_caption, t);
}

static void plot_trace(float x, float y)
{
    if (s_i.canvas && s_i.have_last) {
        lv_layer_t layer;
        lv_canvas_init_layer(s_i.canvas, &layer);
        plot_line(&layer, map_x(s_i.last[0]), map_y(s_i.last[1]), map_x(x), map_y(y), HMI_ACCENT, LV_OPA_COVER, 2);
        lv_canvas_finish_layer(s_i.canvas, &layer);
    }
    s_i.last[0] = x;
    s_i.last[1] = y;
    s_i.have_last = true;
}

/* The plot's (0, 0) is where the axes are now: a new path starts here. */
static void origin_here(void)
{
    for (int k = 0; k < 2; k++) {
        const ecat_axis_status_t *a = axis(k);
        s_i.origin[k] = a ? a->position : 0;
    }
}

static void settings_changed(void)
{
    origin_here();
    plot_reset();
}

static void buf_free_async(void *buf)
{
    lv_draw_buf_destroy(buf);
}

/* The canvas destructor still uses the buffer's address: free it after the deletion. The page's
 * own delete callback runs first and has cleared s_i, so the buffer comes as user data. */
static void canvas_deleted_cb(lv_event_t *e)
{
    lv_async_call(buf_free_async, lv_event_get_user_data(e));
}

/* ---- Fields ---- */

static bool running(void)
{
    return s_ax.path.phase != ECAT_PATH_IDLE;
}

static void show_fields(void)
{
    hmi_scale_t sc;
    scale_of(0, &sc);
    const bool arc = s_kind != ECAT_PATH_LINE;
    set_text(s_i.f_cap[F_A], arc ? "Centre X (I)" : "X distance");
    set_text(s_i.f_cap[F_B], arc ? "Centre Y (J)" : "Y distance");
    hmi_show(s_i.cell[F_ANGLE], arc);
    char t[40];
    for (int f = 0; f < F_COUNT; f++) {
        if (f == F_ANGLE) {
            snprintf(t, sizeof(t), "%.1f deg", s_val[f]);
        } else if (f == F_SPEED) {
            snprintf(t, sizeof(t), "%.3f %s/s", s_val[f], sc.pos);
        } else if (f == F_RAMP) {
            snprintf(t, sizeof(t), "%.0f ms", s_val[f]);
        } else {
            snprintf(t, sizeof(t), "%.3f %s", s_val[f], sc.pos);
        }
        set_text(s_i.f_val[f], t);
    }
    plan_t p;
    plan(&p);
    if (p.why) {
        strlcpy(t, "\xE2\x80\x94", sizeof(t));
    } else {
        snprintf(t, sizeof(t), "%.3f %s", p.length, sc.pos);
    }
    set_text(s_i.length, t);
}

static void field_set_cb(double value, void *user)
{
    const int f = (int)(intptr_t)user;
    s_val[f] = value;
    show_fields();
    settings_changed();
}

static void field_cb(lv_event_t *e)
{
    const int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (running()) {
        return;
    }
    hmi_scale_t sc;
    scale_of(0, &sc);
    char unit[16];
    const double lim = hmi_pos_limit(&sc);
    switch (f) {
    case F_ANGLE:
        hmi_keypad("Angle to turn", "deg", s_val[f], 0.1, 360.0, 1, field_set_cb, (void *)(intptr_t)f);
        break;
    case F_SPEED:
        snprintf(unit, sizeof(unit), "%s/s", sc.pos);
        hmi_keypad("Path speed", unit, s_val[f], 0.001, lim, 3, field_set_cb, (void *)(intptr_t)f);
        break;
    case F_RAMP:
        hmi_keypad("Ramp time", "ms", s_val[f], MACHINE_RAMP_MS_MIN, MACHINE_RAMP_MS_MAX, 0, field_set_cb,
                   (void *)(intptr_t)f);
        break;
    default:
        hmi_keypad(lv_label_get_text(s_i.f_cap[f]), sc.pos, s_val[f], -lim, lim, 3, field_set_cb,
                   (void *)(intptr_t)f);
        break;
    }
}

static void kind_cb(int index, void *user)
{
    (void)user;
    if (running()) {
        hmi_segmented_select(s_i.kind_seg, s_kind);
        return;
    }
    s_kind = index;
    show_fields();
    settings_changed();
}

static void axis_pick_cb(int index, void *user)
{
    const int k = (int)(intptr_t)user;
    s_axis[k] = index;
    show_fields();
    settings_changed();
}

/* ---- Commands ---- */

static void axis_cmd(int k, uint8_t code)
{
    const ecat_axis_status_t *a = axis(k);
    if (a) {
        (void)hmi_axis_cmd(s_axis[k], a->name, a->units_per_rev, code, 0, 0, 0);
    }
}

static void servo_cb(lv_event_t *e)
{
    (void)e;
    const ecat_axis_status_t *x = axis(0);
    const ecat_axis_status_t *y = axis(1);
    if (x == NULL || y == NULL) {
        return;
    }
    if (x->enabled && y->enabled) {
        ecat_path_stop();
        axis_cmd(0, ECAT_AXIS_DISABLE);
        axis_cmd(1, ECAT_AXIS_DISABLE);
        return;
    }
    for (int k = 0; k < 2; k++) {
        if (!axis(k)->enabled) {
            axis_cmd(k, ECAT_AXIS_ENABLE);
        }
    }
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    axis_cmd(0, ECAT_AXIS_RESET);
    if (s_axis[1] != s_axis[0]) {
        axis_cmd(1, ECAT_AXIS_RESET);
    }
}

static void start_cb(lv_event_t *e)
{
    (void)e;
    const ecat_axis_status_t *x = axis(0);
    const ecat_axis_status_t *y = axis(1);
    if (x == NULL || y == NULL || running()) {
        return;
    }
    plan_t p;
    plan(&p);
    if (p.why) {
        set_text(s_i.note, p.why);
        return;
    }
    if (!x->enabled || !y->enabled) {
        set_text(s_i.note, "Switch both servos on first");
        return;
    }
    if (!ecat_path_start(&p.cmd)) {
        set_text(s_i.note, "Command not sent: queue full");
    }
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    ecat_path_stop();
}

static void open_ethercat_cb(lv_event_t *e)
{
    (void)e;
    hmi_settings_open_ethercat();
}

/* ---- Update ---- */

static void axis_state_text(const ecat_axis_status_t *a, const hmi_scale_t *sc, char *t, size_t len)
{
    const char *st;
    char fault[24];
    if (!a->available) {
        st = "Unavailable";
    } else if (a->cia_state == ECAT_CIA_FAULT || a->cia_state == ECAT_CIA_FAULT_REACTION) {
        snprintf(fault, sizeof(fault), "Fault 0x%04X", a->error_code);
        st = fault;
    } else if (a->job != ECAT_JOB_IDLE) {
        st = ecat_axis_job_text(a->job);
    } else {
        st = a->enabled ? "Servo on" : "Servo off";
    }
    snprintf(t, len, "%s \xC2\xB7 %.3f %s", st, hmi_to_pos(sc, a->position), sc->pos);
}

static void update(void)
{
    const ecat_axis_status_t *x = axis(0);
    const ecat_axis_status_t *y = axis(1);
    if (x == NULL || y == NULL || s_i.result[R_STATE] == NULL) {
        return;
    }
    const ecat_path_status_t *p = &s_ax.path;
    const bool run = running();
    char t[96];
    hmi_scale_t sx, sy;
    scale_of(0, &sx);
    scale_of(1, &sy);

    snprintf(t, sizeof(t), "%u axes", s_ax.n_axes);
    hmi_tag_set(s_i.status_tag, s_ax.bus_ok ? t : "Bus errors", s_ax.bus_ok ? HMI_SUCCESS_BG : HMI_CRITICAL_BG,
                s_ax.bus_ok ? HMI_SUCCESS : HMI_CRITICAL);

    axis_state_text(x, &sx, t, sizeof(t));
    set_text(s_i.ax_state[0], t);
    axis_state_text(y, &sy, t, sizeof(t));
    set_text(s_i.ax_state[1], t);
    set_text(s_i.servo_label, x->enabled && y->enabled ? "Servo off" : "Servo on");
    /* Only on a change: setting a style redraws the object. */
    if (s_i.locked != (int8_t)run) {
        s_i.locked = (int8_t)run;
        hmi_dropdown_set_enabled(s_i.dd[0], !run);
        hmi_dropdown_set_enabled(s_i.dd[1], !run);
        hmi_segmented_set_enabled(s_i.kind_seg, !run);
        for (int f = 0; f < F_COUNT; f++) {
            lv_obj_set_style_opa(s_i.f_box[f], run ? LV_OPA_50 : LV_OPA_COVER, 0);
        }
    }
    const int8_t dim = run || !x->enabled || !y->enabled;
    if (s_i.start_dim != dim) {
        s_i.start_dim = dim;
        lv_obj_set_style_opa(s_i.start_btn, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    }

    /* A new run: clear the trace and take its start as the plot's (0, 0). */
    if (p->gen != s_i.trace_gen && run) {
        s_i.trace_gen = p->gen;
        s_i.origin[0] = p->start[0];
        s_i.origin[1] = p->start[1];
        plot_reset();
        s_i.tracing = true;
    }
    const float px = (float)((double)((int64_t)x->position - s_i.origin[0]) / sx.units_per_pos);
    const float py = (float)((double)((int64_t)y->position - s_i.origin[1]) / sy.units_per_pos);
    if (s_i.tracing) {
        plot_trace(px, py);
        s_i.tracing = run; /* the last point after the end is drawn too */
    }
    const float mx = map_x(px);
    const float my = map_y(py);
    const bool inside = mx >= 0 && mx < PLOT && my >= 0 && my < PLOT;
    hmi_show(s_i.marker, inside);
    if (inside) {
        lv_obj_set_pos(s_i.marker, (int32_t)mx - 7, (int32_t)my - 7);
    }

    /* Results of the last run */
    const char *u = sx.pos;
    if (run) {
        set_text(s_i.result[R_STATE], ecat_path_phase_text(p->phase));
    } else if (p->gen == 0) {
        set_text(s_i.result[R_STATE], "Not run yet");
    } else {
        set_text(s_i.result[R_STATE], p->done && p->error == ECAT_AXERR_NONE ? "Done"
                                      : p->length > 0.0f                    ? "Stopped"
                                                                            : "Refused");
    }
    if (p->length > 0.0f) {
        snprintf(t, sizeof(t), "%.3f / %.3f %s", p->s, p->length, u);
    } else {
        strlcpy(t, "\xE2\x80\x94", sizeof(t));
    }
    set_text(s_i.result[R_PROGRESS], t);
    snprintf(t, sizeof(t), "%.3f %s/s", p->speed, u);
    set_text(s_i.result[R_SPEED], t);
    snprintf(t, sizeof(t), "%.4f %s", p->contour_max, u);
    set_text(s_i.result[R_CONTOUR], t);
    snprintf(t, sizeof(t), "%.4f %s", p->lag_max, u);
    set_text(s_i.result[R_LAG], t);
    snprintf(t, sizeof(t), "%.2f s", p->time_ms / 1000.0);
    set_text(s_i.result[R_TIME], t);

    /* Note: why the last run stopped, else the limits of the settings. */
    lv_color_t note_color = HMI_C(HMI_TEXT_2);
    if (!run && p->gen && p->error != ECAT_AXERR_NONE) {
        const char *who = p->err_axis < 2 && p->axis[p->err_axis] < s_ax.n_axes
                              ? s_ax.axis[p->axis[p->err_axis]].name : "";
        snprintf(t, sizeof(t), "%s%s%s", who, *who ? ": " : "", ecat_axis_error_text(p->error));
        note_color = HMI_C(HMI_CRITICAL);
    } else if (s_axis[0] == s_axis[1]) {
        strlcpy(t, "Pick two different axes", sizeof(t));
        note_color = HMI_C(HMI_CAUTION);
    } else if (strcmp(sx.pos, sy.pos) != 0) {
        snprintf(t, sizeof(t), "X is in %s and Y in %s: set the same unit in Settings \xE2\x80\xBA Servo", sx.pos,
                 sy.pos);
        note_color = HMI_C(HMI_CAUTION);
    } else {
        plan_t pl;
        plan(&pl);
        if (pl.why) {
            strlcpy(t, pl.why, sizeof(t));
            note_color = HMI_C(HMI_CAUTION);
        } else if (pl.limited) {
            snprintf(t, sizeof(t), "Limited by the axes to %.3f %s/s, ramp %.0f ms", pl.cmd.speed, u,
                     pl.cmd.accel > 0.0f ? pl.cmd.speed * 1000.0f / pl.cmd.accel : 0.0f);
        } else {
            t[0] = '\0';
        }
    }
    set_text(s_i.note, t);
    if (!lv_color_eq(lv_obj_get_style_text_color(s_i.note, 0), note_color)) {
        lv_obj_set_style_text_color(s_i.note, note_color, 0);
    }
}

static bool layout_changed(void)
{
    if (s_ax.in_op != s_i.in_op || s_ax.n_axes != s_i.n_axes) {
        return true;
    }
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        if (strcmp(s_ax.axis[i].name, s_i.names[i]) != 0) {
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

static lv_obj_t *card(lv_obj_t *parent, const char *title, bool grow)
{
    lv_obj_t *c = hmi_card(parent, 12, 14, 8);
    lv_obj_set_width(c, LV_PCT(100));
    if (grow) {
        lv_obj_set_flex_grow(c, 1);
    }
    if (title) {
        hmi_card_title(c, title);
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

static void axes_card(lv_obj_t *col)
{
    static char opts[ECAT_MAX_AXES * (ECAT_TOPO_NAME_LEN + 1)];
    opts[0] = '\0';
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        if (i) {
            strlcat(opts, "\n", sizeof(opts));
        }
        strlcat(opts, s_ax.axis[i].name, sizeof(opts));
    }
    lv_obj_t *c = card(col, NULL, false);
    static const char *const names[] = {"X", "Y"};
    for (int k = 0; k < 2; k++) {
        lv_obj_t *row = hmi_box(c, LV_FLEX_FLOW_ROW, 10);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *l = hmi_label(row, names[k], HMI_FS_LARGE, HMI_SEMIBOLD, HMI_TEXT);
        lv_obj_set_width(l, 24);
        s_i.dd[k] = hmi_dropdown(row, 0, opts, s_axis[k], axis_pick_cb, (void *)(intptr_t)k);
        lv_obj_set_flex_grow(s_i.dd[k], 1);
        s_i.ax_state[k] = hmi_label(c, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(s_i.ax_state[k], LV_PCT(100));
        lv_obj_set_style_pad_left(s_i.ax_state[k], 34, 0);
        lv_label_set_long_mode(s_i.ax_state[k], LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_t *row = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_pad_top(row, 4, 0);
    lv_obj_t *b = hmi_control(row, 0, HMI_TOUCH_H);
    lv_obj_set_flex_grow(b, 1);
    s_i.servo_label = hmi_label(b, "Servo on", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(b, servo_cb, LV_EVENT_CLICKED, NULL);
    b = hmi_control(row, 0, HMI_TOUCH_H);
    lv_obj_set_flex_grow(b, 1);
    hmi_label(b, "Reset", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(b, reset_cb, LV_EVENT_CLICKED, NULL);
}

static void result_card(lv_obj_t *col)
{
    static const char *const names[] = {"Status", "Progress", "Speed", "Contour error", "Lag (max)", "Time"};
    lv_obj_t *c = card(col, NULL, true);
    lv_obj_set_style_pad_row(c, 0, 0);
    for (int r = 0; r < R_COUNT; r++) {
        lv_obj_t *row = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_size(row, LV_PCT(100), RESULT_ROW_H);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_label(row, names[r], HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
        s_i.result[r] = hmi_label(row, "", HMI_FS_BODY, r == R_CONTOUR ? HMI_SEMIBOLD : HMI_REGULAR, HMI_TEXT);
    }
}

static void plot_card(lv_obj_t *col)
{
    lv_obj_t *c = card(col, NULL, true);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *head = hmi_box(c, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_card_title(head, "XY plot");
    hmi_spacer(head);
    static const struct {
        const char *name;
        uint32_t color;
    } legend[] = {{"Planned", PLANNED}, {"Actual", HMI_ACCENT}};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = hmi_box(head, LV_FLEX_FLOW_ROW, 6);
        lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *sw = hmi_box(l, -1, 0);
        lv_obj_set_size(sw, 16, 4);
        lv_obj_set_style_radius(sw, 2, 0);
        lv_obj_set_style_bg_color(sw, HMI_C(legend[i].color), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        hmi_label(l, legend[i].name, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    }

    lv_obj_t *frame = hmi_box(c, -1, 0);
    lv_obj_set_size(frame, PLOT, PLOT);
    s_i.canvas = lv_canvas_create(frame);
    s_i.buf = lv_draw_buf_create(PLOT, PLOT, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (s_i.buf == NULL) {
        lv_obj_delete(s_i.canvas);
        s_i.canvas = NULL;
        hmi_label(frame, "No memory for the plot", HMI_FS_SMALL, HMI_REGULAR, HMI_CRITICAL);
    } else {
        lv_canvas_set_draw_buf(s_i.canvas, s_i.buf);
        lv_obj_add_event_cb(s_i.canvas, canvas_deleted_cb, LV_EVENT_DELETE, s_i.buf);
    }
    s_i.marker = hmi_box(frame, -1, 0);
    lv_obj_set_size(s_i.marker, 14, 14);
    lv_obj_set_style_radius(s_i.marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_i.marker, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_i.marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_i.marker, HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_border_width(s_i.marker, 2, 0);
    s_i.plot_caption = hmi_label(c, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
}

static void field(lv_obj_t *row, int f, int32_t w)
{
    lv_obj_t *cell = hmi_box(row, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(cell, w);
    s_i.cell[f] = cell;
    s_i.f_cap[f] = hmi_label(cell, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_t *box = hmi_box(cell, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(box, LV_PCT(100), FIELD_H);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(box, 10, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(box, &hmi_st_pressed, LV_STATE_PRESSED);
    s_i.f_box[f] = box;
    s_i.f_val[f] = hmi_label(box, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    hmi_field_style(box, s_i.f_val[f], HMI_FIELD_EDIT);
    lv_obj_add_event_cb(box, field_cb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
}

static lv_obj_t *field_row(lv_obj_t *c)
{
    lv_obj_t *row = hmi_box(c, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    return row;
}

static void path_card(lv_obj_t *col)
{
    const int32_t half = (RIGHT_W - 2 * 14 - 2 - 10) / 2; /* card padding and border, gap */
    lv_obj_t *c = card(col, "Path", true);
    s_i.kind_seg = hmi_segmented(c, s_kind_labels, 3, 0, SEG_H, s_kind, kind_cb, NULL);
    lv_obj_t *row = field_row(c);
    field(row, F_A, half);
    field(row, F_B, half);
    row = field_row(c);
    field(row, F_ANGLE, half);
    lv_obj_t *len_cell = hmi_box(row, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(len_cell, half);
    hmi_label(len_cell, "Length", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_t *len_box = hmi_box(len_cell, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(len_box, LV_PCT(100), FIELD_H);
    lv_obj_set_flex_align(len_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(len_box, 10, 0);
    s_i.length = hmi_label(len_box, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    row = field_row(c);
    field(row, F_SPEED, half);
    field(row, F_RAMP, half);
    set_text(s_i.f_cap[F_ANGLE], "Angle");
    set_text(s_i.f_cap[F_SPEED], "Speed");
    set_text(s_i.f_cap[F_RAMP], "Ramp");

    s_i.note = hmi_label(c, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_i.note, LV_PCT(100));
    lv_label_set_long_mode(s_i.note, LV_LABEL_LONG_MODE_WRAP);
    hmi_spacer(c);

    row = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    s_i.start_btn = hmi_filled_button(row, HMI_TOUCH_H, HMI_ACCENT, 4);
    lv_obj_set_width(s_i.start_btn, 0);
    lv_obj_set_flex_grow(s_i.start_btn, 1);
    lv_obj_set_style_pad_column(s_i.start_btn, 8, 0);
    hmi_icon(s_i.start_btn, HMI_ICON_PLAY, HMI_ICON_BTN, HMI_WHITE);
    hmi_label(s_i.start_btn, "Start", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(s_i.start_btn, start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop = hmi_filled_button(row, HMI_TOUCH_H, HMI_CRITICAL, 4);
    lv_obj_set_width(stop, 0);
    lv_obj_set_flex_grow(stop, 1);
    lv_obj_set_style_pad_column(stop, 8, 0);
    hmi_icon(stop, HMI_ICON_STOP, HMI_ICON_BTN, HMI_WHITE);
    hmi_label(stop, "Stop", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    lv_obj_add_event_cb(stop, stop_cb, LV_EVENT_CLICKED, NULL);
}

/* (Re)build for the current axes. Runs from create or the poll timer, never from an event of an
 * object it deletes. */
static void build(void)
{
    lv_obj_t *root = s_i.root;
    lv_timer_t *poll = s_i.poll;
    lv_obj_clean(root); /* the canvas delete event frees the plot buffer */
    memset(&s_i, 0, sizeof(s_i));
    s_i.root = root;
    s_i.poll = poll;
    s_i.locked = -1;
    s_i.start_dim = -1;

    s_i.in_op = s_ax.in_op;
    s_i.n_axes = s_ax.n_axes;
    for (uint8_t i = 0; i < s_ax.n_axes; i++) {
        strlcpy(s_i.names[i], s_ax.axis[i].name, sizeof(s_i.names[i]));
    }

    lv_obj_t *row = hmi_manual_header(root, HMI_MANUAL_INTERP);
    s_i.status_tag = hmi_tag(row, true);
    if (!s_ax.in_op || s_ax.n_axes < 2) {
        hmi_tag_set(s_i.status_tag, s_ax.in_op ? "Too few drives" : "Not in OP", HMI_CAUTION_BG,
                    HMI_CAUTION);
        hmi_info_card(root, "Interpolation needs two axes",
                      s_ax.in_op ? "The accepted topology has fewer than two servo or stepper drives."
                                 : "The EtherCAT bus is not in OP. Scan the bus and accept the topology in "
                                   "Settings \xE2\x80\xBA EtherCAT.",
                      "Open EtherCAT settings", open_ethercat_cb);
        return;
    }
    for (int k = 0; k < 2; k++) {
        if (s_axis[k] >= s_ax.n_axes) {
            s_axis[k] = k;
        }
    }

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_t *left = column(body, LEFT_W);
    axes_card(left);
    result_card(left);
    plot_card(column(body, 0));
    path_card(column(body, RIGHT_W));

    lv_obj_update_layout(root);
    show_fields();
    if (running()) {
        s_i.trace_gen = s_ax.path.gen - 1; /* update() starts the trace of the run in progress */
    } else {
        s_i.trace_gen = s_ax.path.gen;
    }
    settings_changed();
    update();
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_i.poll) {
        lv_timer_delete(s_i.poll);
    }
    memset(&s_i, 0, sizeof(s_i));
}

void hmi_interp_create(lv_obj_t *root)
{
    memset(&s_i, 0, sizeof(s_i));
    s_i.root = hmi_box(root, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(s_i.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(s_i.root, deleted_cb, LV_EVENT_DELETE, NULL);
    ecat_axes_read(&s_ax);
    build();
    s_i.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
