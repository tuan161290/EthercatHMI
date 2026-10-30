/*
 * Settings > Servo (mockup ServoSetting.dc.html): per-axis drive objects (CiA402, read and
 * written with SDO jobs, ecat_sdo_service.h) and the HMI's own axis settings
 * (machine_params.h: max speed, acc/dec times, high jog speed, distance per revolution).
 *
 * Layout: one vertical, scrolling list (user, 2026-10-10, instead of the mockup's 2x2 cards):
 * the name on the left, the value with its unit on the right, a line between the groups.
 *
 * Scaling, as on a PLC positioning module:
 *   - Pulse/Rev: command pulses (position units) per motor revolution = the drive's feed constant
 *     0x6092:1 with 0x6092:2 = 1 (gear 1:1). Written to the drive; the axis uses it at once.
 *   - Unit: mm (linear axis), deg (rotary axis) or rev, an HMI setting;
 *   - Distance/Rev: travel of the load per motor revolution in that unit (screw lead in mm, or
 *     360 / gear ratio in deg), an HMI setting. With both set, every page shows the unit (mm/s,
 *     deg/s for speeds); with Unit rev or Distance/Rev 0, motor revolutions and rpm.
 *   - Acc/Dec time: ms from standstill to Max speed (and back), for every move and jog.
 *
 * Buttons:
 *   - Read from drive: reads every object of the four cards (also on opening and on a new axis);
 *   - Write to drive: writes the changed objects, then reads them back;
 *   - Save: stores the HMI settings (NVS) and asks the drive to store its parameters
 *     (0x1010:1 = "save", ESI DT1010; unverified that Leadshine drives keep the 0x6xxx objects).
 * Writing needs the axis' servo off: the factor group (0x6092) and polarity (0x607E) change the
 * meaning of positions under a running axis.
 *
 * Ramps and max speed are not drive objects here: 0x6081/0x6083/0x6084 are in the RxPDO
 * (ecat_drive.h), so the HMI sends them with every move and an SDO write would be overwritten.
 *
 * Pulse/Rev for the conversions: the bound axis' in OP (what the motion uses), else the one
 * computed from the objects read (0x608F, 0x6091, 0x6092).
 * Objects a drive does not have (CL3B: 0x607D, 0x6072, 0x6065, 0x6091) show "n/a".
 */

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "ecat_axis.h"
#include "ecat_busview.h"
#include "ecat_drive.h"
#include "ecat_sdo_service.h"
#include "hmi_internal.h"
#include "machine_params.h"
#include "machine_store.h"

#define POLL_MS     200
#define ROW_H       52
#define GROUP_H     44 /* group title, below the line that separates it from the group above */
#define VALUE_W     170
#define VALUE_H     44
#define UNIT_W      64
#define OBJ_W       84
#define AXIS_COMBO_W 150
#define STORE_SAVE  0x65766173u /* "save" in ASCII, little-endian (CiA 301 0x1010) */
#define UNITS_LIMIT 2147483000.0

/* Drive objects, in job order. */
enum {
    O_607D1, O_607D2, O_6072, O_6065, O_6098, O_607C, O_60991, O_60992,
    O_60921, O_60922, O_608F1, O_608F2, O_60911, O_60912, O_607E, O_COUNT,
};

static const struct {
    uint16_t index;
    uint8_t sub;
    uint8_t size;
    bool sgn;
} s_obj[O_COUNT] = {
    [O_607D1] = {0x607D, 1, 4, true}, [O_607D2] = {0x607D, 2, 4, true},
    [O_6072] = {0x6072, 0, 2, false},  [O_6065] = {0x6065, 0, 4, false}, [O_6098] = {0x6098, 0, 1, true},
    [O_607C] = {0x607C, 0, 4, true},   [O_60991] = {0x6099, 1, 4, false}, [O_60992] = {0x6099, 2, 4, false},
    [O_60921] = {0x6092, 1, 4, false}, [O_60922] = {0x6092, 2, 4, false}, [O_608F1] = {0x608F, 1, 4, false},
    [O_608F2] = {0x608F, 2, 4, false}, [O_60911] = {0x6091, 1, 4, false}, [O_60912] = {0x6091, 2, 4, false},
    [O_607E] = {0x607E, 0, 1, false},
};

typedef enum {
    K_SPEED,    /* units/s, shown in rpm or mm/s */
    K_POS,      /* position units, shown in rev or mm */
    K_POS_U,    /* unsigned position units */
    K_TORQUE,   /* per mille of rated torque, shown in % */
    K_INT,
    K_UNITS,    /* plain unsigned number */
    K_PULSE,    /* Pulse/Rev: 0x6092:1, written with 0x6092:2 = 1 */
    K_DIR,      /* 0x607E bits 7/6 */
    K_HMI_MAX,  /* HMI settings from here on */
    K_HMI_ACC,
    K_HMI_DEC,
    K_HMI_JOG,
    K_HMI_DIST,
    K_HMI_UNIT,
} kind_t;

typedef struct {
    const char *name;
    int8_t obj;       /* O_*, -1 for HMI settings */
    uint8_t kind;
    bool writable;
    const char *unit;  /* NULL: the axis' position or speed unit */
    const char *label; /* object column */
} row_def_t;

#define N_GROUPS 4
#define N_ROWS   5 /* most per group; a group with fewer ends with an empty entry (name NULL) */

static const char *const s_group_titles[N_GROUPS] = {"Motion", "Limits", "Homing", "Mechanical"};
static const row_def_t s_rows[N_GROUPS][N_ROWS] = {
    {
        {"Max speed", -1, K_HMI_MAX, true, NULL, "HMI"},
        {"Acc time (0 to max)", -1, K_HMI_ACC, true, "ms", "HMI"},
        {"Dec time (max to 0)", -1, K_HMI_DEC, true, "ms", "HMI"},
        {"Jog speed (high)", -1, K_HMI_JOG, true, NULL, "HMI"},
    },
    {
        {"Soft limit \xE2\x88\x92", O_607D1, K_POS, true, NULL, "0x607D:1"},
        {"Soft limit +", O_607D2, K_POS, true, NULL, "0x607D:2"},
        {"Torque limit", O_6072, K_TORQUE, true, "%", "0x6072"},
        {"Following error window", O_6065, K_POS_U, true, NULL, "0x6065"},
    },
    {
        {"Homing method", O_6098, K_INT, true, "", "0x6098"},
        {"Home offset", O_607C, K_POS, true, NULL, "0x607C"},
        {"Search speed", O_60991, K_SPEED, true, NULL, "0x6099:1"},
        {"Zero speed", O_60992, K_SPEED, true, NULL, "0x6099:2"},
    },
    {
        {"Unit", -1, K_HMI_UNIT, true, "", "HMI"},
        {"Pulse/Rev", O_60921, K_PULSE, true, "pulse", "0x6092:1"},
        {"Distance/Rev", -1, K_HMI_DIST, true, NULL, "HMI"},
        {"Encoder resolution", O_608F1, K_UNITS, false, "inc/rev", "0x608F:1"},
        {"Direction", O_607E, K_DIR, true, "", "0x607E"},
    },
};

typedef enum {
    PH_IDLE,
    PH_READ,
    PH_WRITE,       /* then PH_READ */
} phase_t;

typedef struct {
    char name[ECAT_TOPO_NAME_LEN];
    uint16_t slave;
    uint32_t vendor, product;
    const char *device;
} drive_t;

static int s_sel; /* kept while the firmware runs */
static ecat_busview_t *s_bv;    /* ~3 KB, LVGL heap (PSRAM) */
static EXT_RAM_BSS_ATTR ecat_axes_status_t s_ax; /* ~1 KB */

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *root;
    lv_obj_t *status_dot, *status_text;
    lv_obj_t *message;
    lv_obj_t *box[N_GROUPS][N_ROWS];
    lv_obj_t *val[N_GROUPS][N_ROWS];
    lv_obj_t *unit[N_GROUPS][N_ROWS];
    lv_timer_t *poll;
    uint32_t bv_gen;
    uint8_t n;
    drive_t d[ECAT_MAX_AXES];
    /* values of the selected drive */
    bool read_done;
    bool ok[O_COUNT];
    int32_t v[O_COUNT];
    bool dirty[O_COUNT];
    int32_t nv[O_COUNT];
    machine_axis_cfg_t cfg;
    bool cfg_dirty;
    /* SDO job */
    phase_t phase;
    uint32_t ticket;
    bool store_after; /* Save: store 0x1010 in the drive after the writes */
    char note[112];   /* outcome of a write, shown once the read-back is done */
    bool note_err;
} s_s; /* ~1.5 KB, PSRAM */

static void build(void);
static void start_read(void);

/* ---- Drives ---- */

static void drives_collect(void)
{
    s_s.n = 0;
    const ecat_topology_t *t = &s_bv->bus;
    for (uint16_t k = 0; k < t->n_slaves && s_s.n < ECAT_MAX_AXES; k++) {
        const ecat_topo_slave_t *s = &t->slave[k];
        if (!ecat_kind_is_drive((ecat_kind_t)s->kind)) {
            continue;
        }
        drive_t *d = &s_s.d[s_s.n++];
        strlcpy(d->name, s->name, sizeof(d->name));
        d->slave = s->position;
        d->vendor = s->vendor;
        d->product = s->product;
        d->device = ecat_topology_device_name(s);
    }
    /* Cable order (the bus list is in it), as the axes of the Manual page. */
    if (s_sel >= s_s.n) {
        s_sel = 0;
    }
}

static const drive_t *drive(void)
{
    return s_sel < s_s.n ? &s_s.d[s_sel] : NULL;
}

static const ecat_axis_status_t *axis_of(const drive_t *d, int *index)
{
    for (uint8_t i = 0; d && s_ax.in_op && i < s_ax.n_axes; i++) {
        if (strcmp(s_ax.axis[i].name, d->name) == 0) {
            if (index) {
                *index = i;
            }
            return &s_ax.axis[i];
        }
    }
    return NULL;
}

static bool bus_usable(void)
{
    return s_bv->phase == ECAT_BUS_PHASE_HELD || s_bv->phase == ECAT_BUS_PHASE_OP;
}

/* Units per revolution from the objects read, 0 if unknown. */
static uint32_t upr_read(void)
{
    if (!s_s.read_done) {
        return 0;
    }
    ecat_drive_info_t info = {0};
    if (s_s.ok[O_608F1] && s_s.ok[O_608F2]) {
        info.enc_inc = (uint32_t)s_s.v[O_608F1];
        info.enc_rev = (uint32_t)s_s.v[O_608F2];
    }
    if (s_s.ok[O_60911] && s_s.ok[O_60912]) {
        info.gear_motor = (uint32_t)s_s.v[O_60911];
        info.gear_shaft = (uint32_t)s_s.v[O_60912];
    }
    if (s_s.ok[O_60921] && s_s.ok[O_60922]) {
        info.feed = (uint32_t)s_s.v[O_60921];
        info.feed_rev = (uint32_t)s_s.v[O_60922];
    }
    return ecat_drive_units_per_rev(&info);
}

/* Units per revolution for the conversions: the bound axis' in OP, else the one read. */
static uint32_t upr(void)
{
    const ecat_axis_status_t *a = axis_of(drive(), NULL);
    if (a && a->units_per_rev) {
        return a->units_per_rev;
    }
    return hmi_upr(upr_read());
}

/* Display scale of the page: Unit and Distance/Rev as edited (not saved yet), Pulse/Rev as above. */
static void scale(hmi_scale_t *sc)
{
    hmi_scale_of(&s_s.cfg, upr(), sc);
}

static bool unit_is_rev(void)
{
    return s_s.cfg.unit == MACHINE_UNIT_REV;
}

static const char *unit_of(const row_def_t *r, const hmi_scale_t *sc)
{
    if (r->unit) {
        return r->unit;
    }
    if (r->kind == K_HMI_DIST) {
        return unit_is_rev() ? "" : hmi_unit_name(s_s.cfg.unit); /* per motor revolution */
    }
    return r->kind == K_POS || r->kind == K_POS_U ? sc->pos : sc->speed;
}

static void message(const char *text, bool error)
{
    hmi_set_text(s_s.message, text);
    lv_obj_set_style_text_color(s_s.message, HMI_C(error ? HMI_CRITICAL : HMI_TEXT_2), 0);
}

/* ---- Values ---- */

static float *cfg_field(uint8_t kind)
{
    switch (kind) {
    case K_HMI_MAX:
        return &s_s.cfg.max_rpm;
    case K_HMI_ACC:
        return &s_s.cfg.acc_ms;
    case K_HMI_DEC:
        return &s_s.cfg.dec_ms;
    case K_HMI_DIST:
        return &s_s.cfg.dist_per_rev;
    case K_HMI_JOG:
    default:
        return &s_s.cfg.jog_high_rpm;
    }
}

static void value_text(const row_def_t *r, char *t, size_t len, bool *changed)
{
    hmi_scale_t sc;
    scale(&sc);
    *changed = false;
    if (r->kind == K_HMI_UNIT) {
        snprintf(t, len, "%s", hmi_unit_name(s_s.cfg.unit));
        *changed = s_s.cfg_dirty;
        return;
    }
    if (r->kind == K_HMI_DIST && unit_is_rev()) {
        snprintf(t, len, "\xE2\x80\x94"); /* not used */
        return;
    }
    if (r->obj < 0) {
        const float v = *cfg_field(r->kind);
        if (r->kind == K_HMI_MAX || r->kind == K_HMI_JOG) {
            snprintf(t, len, "%.*f", sc.speed_decimals, hmi_rpm_to_speed(&sc, v));
        } else if (r->kind == K_HMI_DIST) {
            snprintf(t, len, "%.3f", v);
        } else {
            snprintf(t, len, "%.0f", v);
        }
        *changed = s_s.cfg_dirty;
        return;
    }
    if (!s_s.read_done) {
        snprintf(t, len, "\xE2\x80\x94");
        return;
    }
    if (!s_s.ok[r->obj]) {
        snprintf(t, len, "n/a");
        return;
    }
    *changed = s_s.dirty[r->obj];
    const int32_t v = *changed ? s_s.nv[r->obj] : s_s.v[r->obj];
    const double u = (double)upr();
    switch (r->kind) {
    case K_SPEED:
        snprintf(t, len, "%.*f", sc.speed_decimals + 1, hmi_rpm_to_speed(&sc, (double)(uint32_t)v * 60.0 / u));
        break;
    case K_POS:
        snprintf(t, len, "%.3f", (double)v / sc.units_per_pos);
        break;
    case K_POS_U:
        snprintf(t, len, "%.3f", (double)(uint32_t)v / sc.units_per_pos);
        break;
    case K_TORQUE:
        snprintf(t, len, "%.1f", (double)(uint32_t)v / 10.0);
        break;
    case K_INT:
        snprintf(t, len, "%ld", (long)v);
        break;
    case K_UNITS:
    case K_PULSE:
        snprintf(t, len, "%lu", (unsigned long)(uint32_t)v);
        break;
    case K_DIR:
        snprintf(t, len, (v & 0xC0) ? "Reverse" : "Normal");
        break;
    default:
        t[0] = '\0';
        break;
    }
}

static void values_show(void)
{
    char t[32];
    hmi_scale_t sc;
    scale(&sc);
    for (int g = 0; g < N_GROUPS; g++) {
        for (int i = 0; i < N_ROWS && s_rows[g][i].name; i++) {
            const row_def_t *r = &s_rows[g][i];
            bool ch;
            value_text(r, t, sizeof(t), &ch);
            hmi_set_text(s_s.val[g][i], t);
            hmi_set_text(s_s.unit[g][i], unit_of(r, &sc));
            const bool editable = r->writable && (r->obj < 0 || (s_s.read_done && s_s.ok[r->obj])) &&
                                  !(r->kind == K_HMI_DIST && unit_is_rev());
            hmi_field_style(s_s.box[g][i], s_s.val[g][i],
                            ch ? HMI_FIELD_CHANGED : editable ? HMI_FIELD_EDIT : HMI_FIELD_READONLY);
        }
    }
}

/* ---- Editing ---- */

static const row_def_t *s_edit; /* row open in the keypad or choice */

static void edit_done(double value)
{
    const row_def_t *r = s_edit;
    if (r == NULL || s_s.root == NULL) {
        return;
    }
    hmi_scale_t sc;
    scale(&sc);
    if (r->obj < 0) {
        const bool speed = r->kind == K_HMI_MAX || r->kind == K_HMI_JOG;
        *cfg_field(r->kind) = (float)(speed ? hmi_speed_to_rpm(&sc, value) : value);
        s_s.cfg_dirty = true;
        values_show();
        if (r->kind == K_HMI_DIST) {
            message(value > 0 ? "Every page shows the new unit once saved." :
                                "Distance/Rev 0: positions in motor revolutions and speeds in rpm.", false);
        }
        return;
    }
    const double u = (double)upr();
    double raw;
    switch (r->kind) {
    case K_SPEED:
        raw = hmi_speed_to_rpm(&sc, value) * u / 60.0;
        break;
    case K_POS:
    case K_POS_U:
        raw = value * sc.units_per_pos;
        break;
    case K_TORQUE:
        raw = value * 10.0;
        break;
    default:
        raw = value;
        break;
    }
    raw = raw > UNITS_LIMIT ? UNITS_LIMIT : raw < -UNITS_LIMIT ? -UNITS_LIMIT : raw;
    s_s.nv[r->obj] = (int32_t)(raw < 0 ? raw - 0.5 : raw + 0.5);
    s_s.dirty[r->obj] = s_s.nv[r->obj] != s_s.v[r->obj];
    if (r->kind == K_PULSE && s_s.ok[O_60922] && s_s.v[O_60922] != 1) {
        s_s.nv[O_60922] = 1; /* Pulse/Rev is per motor revolution: feed constant over 1 revolution */
        s_s.dirty[O_60922] = true;
    }
    values_show();
}

static void keypad_cb(double value, void *user)
{
    (void)user;
    edit_done(value);
}

static void dir_cb(int index, void *user)
{
    (void)user;
    edit_done(index ? 0xC0 : 0x00);
}

/* The Distance/Rev of one unit means nothing in another: deg starts at one load turn per motor
 * turn, mm has to be entered (screw lead). */
static void unit_cb(int index, void *user)
{
    (void)user;
    if (s_s.root == NULL || index < 0 || index >= MACHINE_UNIT_COUNT || index == s_s.cfg.unit) {
        return;
    }
    s_s.cfg.unit = (uint8_t)index;
    s_s.cfg.dist_per_rev = index == MACHINE_UNIT_DEG ? MACHINE_DEG_PER_REV : 0.0f;
    s_s.cfg_dirty = true;
    values_show();
    if (index == MACHINE_UNIT_MM) {
        message("Enter Distance/Rev: mm of travel per motor revolution (the screw lead), then Save.", false);
    } else if (index == MACHINE_UNIT_DEG) {
        message("Distance/Rev 360 deg: one load turn per motor turn. With a gearbox enter 360 / ratio, then Save.",
                false);
    } else {
        message("Positions in motor revolutions and speeds in rpm once saved.", false);
    }
}

static void row_cb(lv_event_t *e)
{
    const int gi = (int)(intptr_t)lv_event_get_user_data(e);
    const row_def_t *r = &s_rows[gi / N_ROWS][gi % N_ROWS];
    if (!r->writable) {
        message("Read only: this object describes the motor.", false);
        return;
    }
    if (r->kind == K_HMI_DIST && unit_is_rev()) {
        message("Choose the Unit (mm or deg) first: in rev, Distance/Rev is not used.", false);
        return;
    }
    if (r->obj >= 0 && !s_s.read_done) {
        message("Read from the drive first.", true);
        return;
    }
    if (r->obj >= 0 && !s_s.ok[r->obj]) {
        message("This drive does not have the object.", true);
        return;
    }
    s_edit = r;
    char title[64];
    const drive_t *d = drive();
    snprintf(title, sizeof(title), "%s \xC2\xB7 %s", d ? d->name : "", r->name);
    hmi_scale_t sc;
    scale(&sc);
    const char *unit = unit_of(r, &sc);
    const double pos_max = UNITS_LIMIT / sc.units_per_pos;
    const double speed_max = hmi_rpm_to_speed(&sc, MACHINE_MAX_RPM_MAX);
    double cur = 0;
    bool ch;
    char t[32];
    value_text(r, t, sizeof(t), &ch);
    sscanf(t, "%lf", &cur);
    switch (r->kind) {
    case K_HMI_MAX:
    case K_HMI_JOG:
        hmi_keypad(title, unit, cur, hmi_rpm_to_speed(&sc, 1.0), speed_max, sc.speed_decimals, keypad_cb, NULL);
        break;
    case K_HMI_ACC:
    case K_HMI_DEC:
        hmi_keypad(title, unit, cur, MACHINE_RAMP_MS_MIN, MACHINE_RAMP_MS_MAX, 0, keypad_cb, NULL);
        break;
    case K_HMI_DIST:
        hmi_keypad(title, unit, cur, 0, MACHINE_DIST_MAX, 3, keypad_cb, NULL);
        break;
    case K_SPEED:
        hmi_keypad(title, unit, cur, 0, speed_max, sc.speed_decimals + 1, keypad_cb, NULL);
        break;
    case K_POS:
        hmi_keypad(title, unit, cur, -pos_max, pos_max, 3, keypad_cb, NULL);
        break;
    case K_POS_U:
        hmi_keypad(title, unit, cur, 0, pos_max, 3, keypad_cb, NULL);
        break;
    case K_TORQUE:
        hmi_keypad(title, unit, cur, 0, 300, 1, keypad_cb, NULL);
        break;
    case K_INT:
        hmi_keypad(title, "", cur, -128, 127, 0, keypad_cb, NULL);
        break;
    case K_PULSE:
        hmi_keypad(title, "pulse/rev", cur, 1, UNITS_LIMIT, 0, keypad_cb, NULL);
        break;
    case K_DIR: {
        const int32_t v = s_s.dirty[r->obj] ? s_s.nv[r->obj] : s_s.v[r->obj];
        hmi_choice(title, "Normal\nReverse", (v & 0xC0) ? 1 : 0, dir_cb, NULL);
        break;
    }
    case K_HMI_UNIT:
        s_edit = NULL; /* not through edit_done() */
        hmi_choice(title, "rev (motor revolutions)\nmm (linear axis)\ndeg (rotary axis)", s_s.cfg.unit, unit_cb,
                   NULL);
        break;
    default:
        break;
    }
}

/* ---- SDO jobs ---- */

static bool submit(ecat_sdo_job_t *job, phase_t phase)
{
    const drive_t *d = drive();
    if (d == NULL) {
        return false;
    }
    if (!bus_usable()) {
        message("The bus is starting or scanning: try again in a moment.", true);
        return false;
    }
    job->slave = d->slave;
    job->vendor = d->vendor;
    job->product = d->product;
    if (!ecat_sdo_submit(job, &s_s.ticket)) {
        message("Busy: an SDO job is still running.", true);
        return false;
    }
    s_s.phase = phase;
    return true;
}

static void start_read(void)
{
    static ecat_sdo_job_t job;
    memset(&job, 0, sizeof(job));
    for (int o = 0; o < O_COUNT; o++) {
        job.item[o].index = s_obj[o].index;
        job.item[o].sub = s_obj[o].sub;
        job.item[o].size = s_obj[o].size;
        job.item[o].is_signed = s_obj[o].sgn;
    }
    job.n = O_COUNT;
    if (submit(&job, PH_READ)) {
        message("Reading from the drive\xE2\x80\xA6", false);
    }
}

static int n_dirty(void)
{
    int n = 0;
    for (int o = 0; o < O_COUNT; o++) {
        n += s_s.dirty[o];
    }
    return n;
}

/* Writes of the changed objects, plus the store command for Save. */
static void start_write(bool store)
{
    const drive_t *d = drive();
    const ecat_axis_status_t *a = axis_of(d, NULL);
    if (a && a->enabled) {
        char t[64];
        snprintf(t, sizeof(t), "Switch the servo of %s off first.", d->name);
        message(t, true);
        return;
    }
    static ecat_sdo_job_t job;
    memset(&job, 0, sizeof(job));
    for (int o = 0; o < O_COUNT; o++) {
        if (s_s.dirty[o]) {
            ecat_sdo_item_t *it = &job.item[job.n++];
            it->index = s_obj[o].index;
            it->sub = s_obj[o].sub;
            it->size = s_obj[o].size;
            it->write = true;
            it->value = s_s.nv[o];
        }
    }
    if (store) {
        ecat_sdo_item_t *it = &job.item[job.n++];
        it->index = 0x1010;
        it->sub = 1;
        it->size = 4;
        it->write = true;
        it->value = (int32_t)STORE_SAVE;
    }
    if (job.n == 0) {
        message("Nothing changed to write.", false);
        return;
    }
    s_s.store_after = store;
    if (submit(&job, PH_WRITE)) {
        message("Writing to the drive\xE2\x80\xA6", false);
    }
}

static void job_poll(void)
{
    if (s_s.phase == PH_IDLE) {
        return;
    }
    static ecat_sdo_job_t job;
    const ecat_sdo_state_t st = ecat_sdo_poll(s_s.ticket, &job);
    if (st == ECAT_SDO_BUSY) {
        return;
    }
    const phase_t ph = s_s.phase;
    s_s.phase = PH_IDLE;
    if (st != ECAT_SDO_DONE) {
        message("The drive could not be reached (bus busy, or the slave changed). Try again.", true);
        return;
    }
    char t[96];
    if (ph == PH_READ) {
        int n_ok = 0;
        for (int o = 0; o < O_COUNT && o < job.n; o++) {
            s_s.ok[o] = job.item[o].ok;
            s_s.v[o] = job.item[o].value;
            s_s.dirty[o] = false;
            n_ok += job.item[o].ok;
        }
        s_s.read_done = true;
        int index = 0;
        const ecat_axis_status_t *a = axis_of(drive(), &index);
        const uint32_t ur = upr_read();
        if (a && ur && ur != a->units_per_rev && hmi_axis_set_units_per_rev(index, ur)) {
            /* New Pulse/Rev: the axis converts with it at once (no bus restart needed). */
            snprintf(t, sizeof(t), "Pulse/Rev is now %lu. Teach points of %s taught before keep their pulses.",
                     (unsigned long)ur, a->name);
            message(t, false);
        } else if (s_s.note[0]) {
            message(s_s.note, s_s.note_err);
        } else {
            snprintf(t, sizeof(t), "Read %d of %d objects from the drive.", n_ok, O_COUNT);
            message(t, false);
        }
        s_s.note[0] = '\0';
        values_show();
        return;
    }
    /* PH_WRITE */
    int failed = 0;
    bool store_failed = false;
    for (int i = 0; i < job.n; i++) {
        if (!job.item[i].ok) {
            if (job.item[i].index == 0x1010) {
                store_failed = true;
            } else {
                failed++;
            }
        }
    }
    s_s.note_err = failed || store_failed;
    if (failed) {
        snprintf(s_s.note, sizeof(s_s.note), "%d object(s) were refused by the drive (value out of its range?).",
                 failed);
    } else if (store_failed) {
        strlcpy(s_s.note, "Written, but the drive refused to store its parameters (0x1010).", sizeof(s_s.note));
    } else if (s_s.store_after) {
        strlcpy(s_s.note, "Saved: HMI settings, and the drive stored its parameters.", sizeof(s_s.note));
    } else {
        strlcpy(s_s.note, "Written to the drive (not stored: press Save to keep it after power-off).",
                sizeof(s_s.note));
    }
    message(s_s.note, s_s.note_err);
    start_read(); /* show what the drive holds now; the note stays */
}

/* ---- Buttons ---- */

static void read_cb(lv_event_t *e)
{
    (void)e;
    start_read();
}

static void write_cb(lv_event_t *e)
{
    (void)e;
    if (n_dirty() == 0) {
        message("Nothing changed to write.", false);
        return;
    }
    start_write(false);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    const drive_t *d = drive();
    if (d == NULL) {
        return;
    }
    if (s_s.cfg_dirty) {
        if (machine_axis_set(d->name, &s_s.cfg) != ESP_OK) {
            message("Saving the HMI settings failed: see the console log.", true);
            return;
        }
        s_s.cfg_dirty = false;
        values_show();
    }
    /* Drive changes need the servo off (start_write()); with nothing changed in the drive and
     * the servo on, only the HMI settings are saved and the drive is left alone. */
    const ecat_axis_status_t *ax = axis_of(d, NULL);
    if (n_dirty() > 0 || (s_s.read_done && !(ax && ax->enabled))) {
        start_write(true);
    } else {
        message(machine_store_pending() ? "HMI settings kept; saved when EtherCAT leaves OP." : "HMI settings saved.",
                false);
    }
}

static void build_async(void *arg)
{
    (void)arg;
    build();
}

static void axis_pick_cb(int index, void *user)
{
    (void)user;
    if (index == s_sel) {
        return;
    }
    s_sel = index;
    lv_async_call(build_async, NULL); /* rebuilds the page, axis combo included */
}

/* Axis combo: the drives on a roller (any number, big touch targets). */
static void axis_combo_cb(lv_event_t *e)
{
    (void)e;
    static char opts[ECAT_MAX_AXES * (ECAT_TOPO_NAME_LEN + 1)];
    opts[0] = '\0';
    for (int i = 0; i < s_s.n; i++) {
        if (i) {
            strlcat(opts, "\n", sizeof(opts));
        }
        strlcat(opts, s_s.d[i].name, sizeof(opts));
    }
    hmi_choice("Axis", opts, s_sel, axis_pick_cb, NULL);
}

/* ---- Status ---- */

static void status_update(void)
{
    const drive_t *d = drive();
    if (d == NULL) {
        return;
    }
    const ecat_axis_status_t *a = axis_of(d, NULL);
    char t[128];
    uint32_t dot = HMI_RING;
    if (a) {
        snprintf(t, sizeof(t), "Slave %u \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 Error 0x%04X", d->slave, d->name, d->device,
                 a->available ? ecat_cia_state_text(a->cia_state) : "Unavailable", a->error_code);
        dot = a->cia_state == ECAT_CIA_FAULT ? HMI_CRITICAL : a->enabled ? HMI_SUCCESS : HMI_TEXT_2;
    } else {
        snprintf(t, sizeof(t), "Slave %u \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %s", d->slave, d->name, d->device,
                 s_bv->phase == ECAT_BUS_PHASE_HELD ? "PRE-OP" : "starting");
    }
    hmi_set_text(s_s.status_text, t);
    lv_obj_set_style_bg_color(s_s.status_dot, HMI_C(dot), 0);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    ecat_axes_read(&s_ax);
    if (ecat_busview_gen() != s_s.bv_gen) {
        ecat_busview_read(s_bv);
        s_s.bv_gen = s_bv->gen;
        /* Rebuild when the drive list changed (new scan, other topology). */
        drive_t old[ECAT_MAX_AXES];
        const uint8_t n_old = s_s.n;
        memcpy(old, s_s.d, sizeof(old));
        drives_collect();
        bool same = n_old == s_s.n;
        for (int i = 0; same && i < s_s.n; i++) {
            same = strcmp(old[i].name, s_s.d[i].name) == 0 && old[i].slave == s_s.d[i].slave;
        }
        if (!same && s_s.phase == PH_IDLE) {
            build();
            return;
        }
    }
    job_poll();
    if (s_s.status_text) {
        status_update();
    }
}

/* ---- Layout ---- */

static void open_ethercat_cb(lv_event_t *e)
{
    (void)e;
    hmi_settings_open_ethercat();
}

static lv_obj_t *tool_button(lv_obj_t *row, const char *text, hmi_icon_t icon, int style, lv_event_cb_t cb)
{
    lv_obj_t *b;
    if (style == 2) {
        b = hmi_filled_button(row, HMI_TOUCH_H, HMI_ACCENT, 4);
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        hmi_icon(b, icon, HMI_ICON_BTN, HMI_WHITE);
        hmi_label(b, text, HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    } else {
        b = hmi_control(row, LV_SIZE_CONTENT, HMI_TOUCH_H);
        const uint32_t c = style == 1 ? HMI_ACCENT : HMI_TEXT;
        if (style == 1) {
            lv_obj_set_style_border_color(b, HMI_C(HMI_ACCENT), 0);
            lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        }
        hmi_icon(b, icon, HMI_ICON_BTN, c);
        hmi_label(b, text, HMI_FS_BODY, style == 1 ? HMI_SEMIBOLD : HMI_REGULAR, c);
    }
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* One group of the list: its title (under a line, except the first), then a row per setting
 * with the name on the left and the value on the right. The whole row opens the editor. */
static void group_rows(lv_obj_t *list, int g)
{
    lv_obj_t *h = hmi_box(list, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(h, LV_PCT(100), GROUP_H);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_bottom(h, 6, 0);
    if (g > 0) {
        hmi_divider_top(h);
        lv_obj_set_style_margin_top(h, 8, 0);
    }
    hmi_label(h, s_group_titles[g], HMI_FS_BODY, HMI_SEMIBOLD, HMI_ACCENT);

    for (int i = 0; i < N_ROWS && s_rows[g][i].name; i++) {
        const row_def_t *r = &s_rows[g][i];
        lv_obj_t *row = hmi_box(list, LV_FLEX_FLOW_ROW, 12);
        lv_obj_set_clickable(row, true);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row, HMI_C(HMI_WHITE), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_style(row, &hmi_st_pressed, LV_STATE_PRESSED);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(g * N_ROWS + i));

        lv_obj_t *name = hmi_label(row, r->name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(name, lv_font_get_line_height(hmi_font(HMI_FS_BODY, HMI_REGULAR)));

        lv_obj_t *obj = hmi_label(row, r->label, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(obj, OBJ_W);
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_t *b = hmi_box(row, LV_FLEX_FLOW_ROW, 0);
        lv_obj_set_size(b, VALUE_W, VALUE_H);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        s_s.box[g][i] = b;
        s_s.val[g][i] = hmi_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);

        s_s.unit[g][i] = hmi_label(row, r->unit ? r->unit : "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(s_s.unit[g][i], UNIT_W);
    }
}

static void build(void)
{
    if (s_s.root == NULL) {
        return;
    }
    lv_obj_t *root = s_s.root;
    lv_timer_t *poll = s_s.poll;
    lv_obj_t *dot = s_s.status_dot, *text = s_s.status_text;
    memset(&s_s, 0, sizeof(s_s));
    s_s.root = root;
    s_s.poll = poll;
    s_s.status_dot = dot;
    s_s.status_text = text;
    lv_obj_clean(root);

    ecat_busview_read(s_bv);
    s_s.bv_gen = s_bv->gen;
    ecat_axes_read(&s_ax);
    drives_collect();

    if (s_s.n == 0) {
        hmi_set_text(s_s.status_text, "");
        hmi_info_card(root, "No drives",
                      s_bv->bus.n_slaves ? "The scanned bus has no servo or stepper drive."
                                        : "Scan the bus and accept the topology in Settings \xE2\x80\xBA EtherCAT first.",
                      "Open EtherCAT settings", open_ethercat_cb);
        return;
    }
    const drive_t *d = drive();
    machine_axis_get(d->name, &s_s.cfg);

    lv_obj_t *row = hmi_box(root, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(row, "Axis", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_t *combo = hmi_control(row, AXIS_COMBO_W, HMI_TOUCH_H);
    lv_obj_set_flex_align(combo, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(combo, 14, 0);
    hmi_label(combo, d->name, HMI_FS_LARGE, HMI_SEMIBOLD, HMI_TEXT);
    hmi_icon(combo, HMI_ICON_CHEVRON_DOWN, 20, HMI_TEXT_2);
    lv_obj_add_event_cb(combo, axis_combo_cb, LV_EVENT_CLICKED, NULL);
    hmi_spacer(row);
    tool_button(row, "Read", HMI_ICON_DOWNLOAD, 0, read_cb);
    tool_button(row, "Write", HMI_ICON_UPLOAD, 1, write_cb);
    tool_button(row, "Save", HMI_ICON_SAVE, 2, save_cb);

    lv_obj_t *card = hmi_card(root, 6, 16, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_t *list = hmi_box(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    hmi_scroll_area(list);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO); /* thin bar: there is more below */
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_30, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(list, 10, 0); /* the bar does not cover the units */
    lv_obj_set_style_pad_bottom(list, 6, 0);
    for (int g = 0; g < N_GROUPS; g++) {
        group_rows(list, g);
    }
    s_s.message = hmi_label(root, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_s.message, LV_PCT(100));

    values_show();
    status_update();
    if (bus_usable()) {
        start_read();
    } else {
        message("The bus is starting: values are read once it is in PRE-OP or OP.", false);
    }
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_s.poll) {
        lv_timer_delete(s_s.poll);
    }
    memset(&s_s, 0, sizeof(s_s));
    s_edit = NULL;
}

void hmi_servo_create(lv_obj_t *root, lv_obj_t *head)
{
    if (s_bv == NULL) {
        s_bv = lv_malloc(sizeof(*s_bv));
        LV_ASSERT_MALLOC(s_bv);
    }
    memset(&s_s, 0, sizeof(s_s));
    hmi_spacer(head);
    lv_obj_t *st = hmi_box(head, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_s.status_dot = hmi_box(st, -1, 0);
    lv_obj_set_size(s_s.status_dot, 10, 10);
    lv_obj_set_style_radius(s_s.status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_s.status_dot, LV_OPA_COVER, 0);
    s_s.status_text = hmi_label(st, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);

    s_s.root = hmi_box(root, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_width(s_s.root, LV_PCT(100));
    lv_obj_set_flex_grow(s_s.root, 1);
    lv_obj_add_event_cb(s_s.root, deleted_cb, LV_EVENT_DELETE, NULL);
    build();
    s_s.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
