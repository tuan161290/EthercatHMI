/*
 * CiA402 axis control in the cyclic task (ecat_axis.h).
 *
 * Every cycle, after the process data exchange: read the drive inputs, take queued commands,
 * advance each axis' job, and write the outputs for the next frame. Nothing here allocates,
 * logs or blocks (core 1 rules, CLAUDE.md); logging happens in bind/unbind only.
 *
 * Handshakes follow CiA402 and the traps the PGM project hit (docs/REFERENCE_PGM.md 3.3):
 *   - enable: Shutdown 0x06 -> Switch on 0x07 -> Enable operation 0x0F, driven by the statusword;
 *   - PP: 0x607A first, then controlword bit 4 up until statusword bit 12 (set-point acknowledge),
 *     bit 4 down, and only then trust bit 10 (target reached), which may still be set from before;
 *   - HM: bit 4 starts homing; bit 12 = attained, bit 13 = error. A stale "attained" is ignored
 *     until bit 12 was seen low or HOMING_SETTLE_US has passed;
 *   - halt (bit 8) stops PP/PV/HM with the profile deceleration and stays set until the next
 *     motion, so a halted move is never resumed by clearing it.
 *
 * Two-axis paths (CONFIG_ECAT_PATH, off by default) run in CSP: the drives take 0x607A every
 * cycle, so the targets are computed here from a trapezoid speed profile along the path
 * (path_cycle). A path never uses halt: a stop decelerates along the path, so the axes stay on it
 * while they stop. Without the option no axis ever leaves PP/PV/HM.
 */

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "ecat_axis.h"
#include "ecat_drive.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soem/soem.h"

static const char *TAG = "ecat_axis";

#define CMD_QUEUE_LEN      16
#define CMDS_PER_CYCLE     4
#define BUS_ERR_CYCLES     3          /* consecutive WKC errors before every axis is switched off */
#define ENABLE_TIMEOUT_US  2000000
#define RESET_PULSE_US     20000
#define RESET_TIMEOUT_US   1000000
#define MODE_TIMEOUT_US    200000
#define ACK_TIMEOUT_US     200000
#define STOP_MIN_US        10000
#define STOP_TIMEOUT_US    5000000
#define QSTOP_TIMEOUT_US   3000000
#define HOMING_TIMEOUT_US  120000000LL
#define HOMING_SETTLE_US   500000

#define SW_TARGET_REACHED  (1u << 10)
#define SW_BIT12           (1u << 12) /* PP: set-point acknowledge; HM: homing attained */
#define SW_BIT13           (1u << 13) /* HM: homing error */

#define CW_DISABLE_VOLTAGE 0x0000
#define CW_QUICK_STOP      0x0002
#define CW_SHUTDOWN        0x0006
#define CW_SWITCH_ON       0x0007
#define CW_ENABLE_OP       0x000F
#define CW_BIT4            (1u << 4) /* PP: new set-point; HM: start homing */
#define CW_BIT5            (1u << 5) /* PP: change set immediately */
#define CW_FAULT_RESET     (1u << 7)
#define CW_HALT            (1u << 8)

#define MODE_PP  1
#define MODE_PV  3
#define MODE_HM  6
#define MODE_CSP 8

#if CONFIG_ECAT_PATH
#define PATH_QUEUE_LEN     2
#define PATH_EXIT_HOLD_US  50000  /* last target held before the axes go back to PP */
#define PATH_TWO_PI        6.28318531f
#endif

typedef struct {
    ecat_axis_status_t st;    /* published part */
    ecat_drive_out_t *out;    /* NULL if the layout is not the expected one */
    const ecat_drive_in_t *in;
    bool has_vendor_error;
    volatile bool blocked;
    bool power;               /* operator wants Operation enabled */
    bool halt;
    bool setpoint;            /* controlword bit 4 */
    bool saw_bit12_low;
    uint8_t phase;
    int64_t t_phase;
    int64_t jog_deadline;
    int8_t mode;
    int32_t tpos;
    int32_t tvel;
    uint32_t pvel, pacc, pdec;
} axis_t;

static axis_t s_ax[ECAT_MAX_AXES];
static uint8_t s_n;
static bool s_in_op;
static volatile bool s_testing; /* ecat_axis_selftest() owns the axes */
static bool s_bus_ok = true;
static uint8_t s_wkc_bad;
static volatile bool s_shutdown;
static QueueHandle_t s_q;
static uint32_t s_cycle_us = 1000;

#if CONFIG_ECAT_PATH
/* Geometry of a path, from ecat_path_check(). */
typedef struct {
    float length;
    float r;     /* arcs: radius */
    float th0;   /* arcs: angle of the start point seen from the centre */
    float sweep; /* arcs: signed angle, CCW positive */
} path_geom_t;

/* The two-axis path, run by the cyclic task. */
typedef struct {
    ecat_path_status_t st; /* published part */
    ecat_path_cmd_t cmd;
    path_geom_t g;
    int64_t t_phase;
    float vel;
    float last[2];         /* last target point, path units */
    bool stop_req;
} path_t;

static path_t s_path;
static QueueHandle_t s_pq;
static atomic_bool s_path_stop; /* ecat_path_stop() from core 0 */
#endif

/* Status snapshot, seqlock: one writer (cyclic task), readers on core 0. */
static atomic_uint s_seq;
static ecat_axes_status_t s_snap;

/* ---- Helpers ---- */

static uint8_t cia_state(uint16_t sw)
{
    if ((sw & 0x4F) == 0x40) {
        return ECAT_CIA_SWITCH_ON_DISABLED;
    }
    if ((sw & 0x4F) == 0x0F) {
        return ECAT_CIA_FAULT_REACTION;
    }
    if ((sw & 0x4F) == 0x08) {
        return ECAT_CIA_FAULT;
    }
    switch (sw & 0x6F) {
    case 0x21:
        return ECAT_CIA_READY;
    case 0x23:
        return ECAT_CIA_SWITCHED_ON;
    case 0x27:
        return ECAT_CIA_OP_ENABLED;
    case 0x07:
        return ECAT_CIA_QUICK_STOP;
    default:
        return ECAT_CIA_NOT_READY;
    }
}

static bool in_fault(uint8_t st)
{
    return st == ECAT_CIA_FAULT || st == ECAT_CIA_FAULT_REACTION;
}

static bool is_motion(uint8_t job)
{
    return job == ECAT_JOB_MOVING || job == ECAT_JOB_JOGGING || job == ECAT_JOB_HOMING || job == ECAT_JOB_PATH;
}

static int32_t sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static void start_job(axis_t *a, uint8_t job, int64_t now)
{
    a->st.job = job;
    a->phase = 0;
    a->t_phase = now;
}

static void next_phase(axis_t *a, int64_t now)
{
    a->phase++;
    a->t_phase = now;
}

static void end_job(axis_t *a, uint8_t err)
{
    a->st.job = ECAT_JOB_IDLE;
    a->phase = 0;
    a->setpoint = false;
    a->st.error = err;
}

/* Stop at once: halt, no new set-point, servo stays as it is. */
static void halt_now(axis_t *a, int64_t now, uint8_t err)
{
    a->halt = true;
    a->tvel = 0;
    a->setpoint = false;
    start_job(a, ECAT_JOB_STOPPING, now);
    a->st.error = err;
}

static void switch_off(axis_t *a, uint8_t err)
{
    a->power = false;
    a->halt = false;
    end_job(a, err);
}

static void set_profile(axis_t *a, const ecat_axis_cmd_t *c)
{
    a->pvel = c->velocity ? c->velocity : 1;
    /* A zero ramp would make PP/PV never move: default to 0.2 s from standstill. */
    const uint64_t ramp = (uint64_t)a->pvel * 5u;
    a->pacc = c->accel ? c->accel : (ramp > UINT32_MAX ? UINT32_MAX : (uint32_t)ramp);
    a->pdec = c->decel ? c->decel : a->pacc;
}

/* A jog released a moment ago is still decelerating: pressing again resumes it. */
static bool jog_can_resume(const axis_t *a)
{
    return a->st.job == ECAT_JOB_JOGGING || (a->st.job == ECAT_JOB_STOPPING && a->mode == MODE_PV);
}

/* ---- Commands ---- */

static void apply_cmd(axis_t *a, const ecat_axis_cmd_t *c, int64_t now)
{
    const uint8_t st = a->st.cia_state;
    const bool enabled = st == ECAT_CIA_OP_ENABLED;
    const bool motion_cmd = c->code == ECAT_AXIS_JOG || c->code == ECAT_AXIS_MOVE_ABS ||
                            c->code == ECAT_AXIS_MOVE_REL || c->code == ECAT_AXIS_HOME;

    if (c->code == ECAT_AXIS_SET_SCALE) {
        if (c->velocity) {
            a->st.units_per_rev = c->velocity; /* display only: the axis logic works in drive units */
        }
        return;
    }
    if (!a->st.available && c->code != ECAT_AXIS_DISABLE) {
        a->st.error = ECAT_AXERR_UNAVAILABLE;
        return;
    }
    if (!s_bus_ok && (c->code == ECAT_AXIS_ENABLE || motion_cmd)) {
        a->st.error = ECAT_AXERR_BUS;
        return;
    }
    if (motion_cmd && !(c->code == ECAT_AXIS_JOG && c->dir == 0)) {
        if (!enabled || !a->power || a->st.job == ECAT_JOB_ENABLING) {
            a->st.error = ECAT_AXERR_NOT_ENABLED;
            return;
        }
        if (a->st.job != ECAT_JOB_IDLE && !(c->code == ECAT_AXIS_JOG && jog_can_resume(a))) {
            a->st.error = ECAT_AXERR_BUSY;
            return;
        }
    }

    switch (c->code) {
    case ECAT_AXIS_ENABLE:
        if (in_fault(st)) {
            a->st.error = ECAT_AXERR_FAULT;
            break;
        }
        if (!(a->power && enabled)) {
            a->power = true;
            start_job(a, ECAT_JOB_ENABLING, now);
        }
        a->st.error = ECAT_AXERR_NONE;
        break;
    case ECAT_AXIS_DISABLE:
        switch_off(a, ECAT_AXERR_NONE);
        break;
    case ECAT_AXIS_RESET:
        a->st.error = ECAT_AXERR_NONE;
        if (!in_fault(st)) {
            break; /* no drive fault: only the last error is cleared, the servo stays as it is */
        }
        a->power = false;
        a->halt = false;
        start_job(a, ECAT_JOB_RESETTING, now);
        break;
    case ECAT_AXIS_STOP:
#if CONFIG_ECAT_PATH
        if (a->st.job == ECAT_JOB_PATH) {
            s_path.stop_req = true; /* both axes decelerate along the path */
            break;
        }
#endif
        if (is_motion(a->st.job)) {
            halt_now(a, now, ECAT_AXERR_NONE);
        } else if (a->st.job == ECAT_JOB_ENABLING) {
            switch_off(a, ECAT_AXERR_NONE);
        }
        break;
    case ECAT_AXIS_QUICK_STOP:
        if (enabled) {
            a->setpoint = false;
            start_job(a, ECAT_JOB_QUICK_STOP, now);
            a->st.error = ECAT_AXERR_NONE;
        } else {
            switch_off(a, ECAT_AXERR_NONE);
        }
        break;
    case ECAT_AXIS_JOG:
        if (c->dir == 0) {
            if (a->st.job == ECAT_JOB_JOGGING) {
                halt_now(a, now, ECAT_AXERR_NONE);
            }
            break;
        }
        set_profile(a, c);
        a->tvel = c->dir > 0 ? sat32(a->pvel) : -sat32(a->pvel);
        a->jog_deadline = now + (int64_t)ECAT_JOG_DEADMAN_MS * 1000;
        a->halt = false;
        if (a->st.job != ECAT_JOB_JOGGING) {
            a->mode = MODE_PV;
            a->halt = false;
            a->setpoint = false;
            start_job(a, ECAT_JOB_JOGGING, now);
            a->st.error = ECAT_AXERR_NONE;
        }
        break;
    case ECAT_AXIS_MOVE_ABS:
    case ECAT_AXIS_MOVE_REL:
        set_profile(a, c);
        a->tpos = c->code == ECAT_AXIS_MOVE_ABS ? c->position : sat32((int64_t)a->st.position + c->position);
        a->st.target = a->tpos;
        a->mode = MODE_PP;
        a->halt = false;
        a->setpoint = false;
        start_job(a, ECAT_JOB_MOVING, now);
        a->st.error = ECAT_AXERR_NONE;
        break;
    case ECAT_AXIS_HOME:
        a->mode = MODE_HM;
        a->halt = false;
        a->setpoint = false;
        a->saw_bit12_low = false;
        a->st.homed = false;
        start_job(a, ECAT_JOB_HOMING, now);
        a->st.error = ECAT_AXERR_NONE;
        break;
    default:
        break;
    }
}

/* ---- Jobs ---- */

static void run_job(axis_t *a, uint16_t sw, int64_t now)
{
    const uint8_t st = a->st.cia_state;
    const int64_t dt = now - a->t_phase;

    switch (a->st.job) {
    case ECAT_JOB_ENABLING:
        if (st == ECAT_CIA_OP_ENABLED) {
            end_job(a, ECAT_AXERR_NONE);
        } else if (dt > ENABLE_TIMEOUT_US) {
            switch_off(a, ECAT_AXERR_ENABLE_TIMEOUT);
        }
        break;

    case ECAT_JOB_RESETTING:
        if (a->phase == 0) {
            if (dt > RESET_PULSE_US) {
                next_phase(a, now);
            }
        } else if (!in_fault(st)) {
            end_job(a, ECAT_AXERR_NONE);
        } else if (dt > RESET_TIMEOUT_US) {
            end_job(a, ECAT_AXERR_RESET_FAILED);
        }
        break;

    case ECAT_JOB_MOVING:
        switch (a->phase) {
        case 0: /* wait for the drive to be in PP */
            if (a->st.mode_display == MODE_PP) {
                a->setpoint = true;
                next_phase(a, now);
            } else if (dt > MODE_TIMEOUT_US) {
                halt_now(a, now, ECAT_AXERR_MODE_TIMEOUT);
            }
            break;
        case 1: /* new set-point up until acknowledged */
            if (sw & SW_BIT12) {
                a->setpoint = false;
                next_phase(a, now);
            } else if (dt > ACK_TIMEOUT_US) {
                halt_now(a, now, ECAT_AXERR_SETPOINT_TIMEOUT);
            }
            break;
        case 2: /* acknowledge back down (or not, on some drives) */
            if (!(sw & SW_BIT12) || dt > ACK_TIMEOUT_US) {
                next_phase(a, now);
            }
            break;
        default: /* moving */
            if (sw & SW_TARGET_REACHED) {
                end_job(a, ECAT_AXERR_NONE);
            }
            break;
        }
        break;

    case ECAT_JOB_JOGGING:
        if (now > a->jog_deadline) {
            halt_now(a, now, ECAT_AXERR_JOG_TIMEOUT);
        }
        break;

    case ECAT_JOB_HOMING:
        if (a->phase == 0) {
            if (a->st.mode_display == MODE_HM) {
                a->setpoint = true;
                next_phase(a, now);
            } else if (dt > MODE_TIMEOUT_US) {
                halt_now(a, now, ECAT_AXERR_MODE_TIMEOUT);
            }
            break;
        }
        if (!(sw & SW_BIT12)) {
            a->saw_bit12_low = true;
        }
        if (sw & SW_BIT13) {
            halt_now(a, now, ECAT_AXERR_HOMING);
        } else if ((a->saw_bit12_low || dt > HOMING_SETTLE_US) && (sw & SW_BIT12) && (sw & SW_TARGET_REACHED)) {
            a->st.homed = true;
            a->setpoint = false;
            a->mode = MODE_PP;
            a->tpos = a->st.position;
            a->st.target = a->tpos;
            end_job(a, ECAT_AXERR_NONE);
        } else if (dt > HOMING_TIMEOUT_US) {
            halt_now(a, now, ECAT_AXERR_HOMING_TIMEOUT);
        }
        break;

    case ECAT_JOB_STOPPING:
        if ((dt > STOP_MIN_US && (sw & SW_TARGET_REACHED)) || dt > STOP_TIMEOUT_US) {
            end_job(a, a->st.error); /* keep the reason of the stop; halt stays set */
        }
        break;

    case ECAT_JOB_QUICK_STOP:
        if (st == ECAT_CIA_SWITCH_ON_DISABLED || dt > QSTOP_TIMEOUT_US) {
            switch_off(a, ECAT_AXERR_NONE);
        }
        break;

    default:
        break;
    }
}

static uint16_t controlword(const axis_t *a, uint8_t st)
{
    if (a->st.job == ECAT_JOB_RESETTING) {
        return a->phase == 0 ? CW_FAULT_RESET : CW_DISABLE_VOLTAGE;
    }
    if (a->st.job == ECAT_JOB_QUICK_STOP) {
        return CW_QUICK_STOP;
    }
    if (!a->power) {
        return st == ECAT_CIA_QUICK_STOP ? CW_DISABLE_VOLTAGE : CW_SHUTDOWN;
    }
    switch (st) {
    case ECAT_CIA_READY:
        return CW_SWITCH_ON;
    case ECAT_CIA_SWITCHED_ON:
        return CW_ENABLE_OP;
    case ECAT_CIA_OP_ENABLED: {
        uint16_t cw = CW_ENABLE_OP;
        if (a->setpoint) {
            cw |= CW_BIT4;
        }
        if (a->mode == MODE_PP) {
            cw |= CW_BIT5;
        }
        if (a->halt) {
            cw |= CW_HALT;
        }
        return cw;
    }
    case ECAT_CIA_QUICK_STOP:
        return CW_DISABLE_VOLTAGE; /* back to Switch on disabled, then the enable sequence again */
    default:
        return CW_SHUTDOWN;
    }
}

/* Read the drive, apply the fail-safe rules and advance the job; outputs come later. */
static void axis_update(axis_t *a, int64_t now)
{
    if (a->blocked && a->st.available) {
        a->st.available = false;
        switch_off(a, ECAT_AXERR_UNAVAILABLE);
    }
    if (a->in == NULL || a->out == NULL) {
        return;
    }
    if (!a->st.available) {
        a->out->controlword = CW_SHUTDOWN; /* blocked slave: never enabled again in this OP */
        return;
    }

    const ecat_drive_in_t *in = a->in;
    const uint16_t sw = in->statusword;
    const uint8_t prev = a->st.cia_state;
    const uint8_t st = cia_state(sw);
    a->st.statusword = sw;
    a->st.cia_state = st;
    a->st.error_code = in->error_code;
    a->st.vendor_error = a->has_vendor_error ? in->vendor_error : 0;
    a->st.mode_display = in->mode_display;
    a->st.position = in->position;
    a->st.velocity = in->velocity;
    a->st.digital_inputs = in->digital_inputs;

    /* Fail safe: a fault, or Operation enabled lost without being asked, switches the axis off. */
    if (in_fault(st) && a->st.job != ECAT_JOB_RESETTING) {
        if (!in_fault(prev) || a->power || a->st.job != ECAT_JOB_IDLE) {
            switch_off(a, ECAT_AXERR_FAULT);
        }
    } else if (a->power && prev == ECAT_CIA_OP_ENABLED && st != ECAT_CIA_OP_ENABLED &&
               a->st.job != ECAT_JOB_ENABLING && a->st.job != ECAT_JOB_QUICK_STOP) {
        switch_off(a, ECAT_AXERR_DROPPED);
    }

    if (s_shutdown) {
        if (a->st.job == ECAT_JOB_PATH) {
            /* path_cycle() decelerates along the path, then the axis is idle and switched off */
        } else if (is_motion(a->st.job)) {
            halt_now(a, now, ECAT_AXERR_NONE);
        } else if (a->st.job != ECAT_JOB_STOPPING) {
            switch_off(a, a->st.error);
        }
    }

    run_job(a, sw, now);

    /* Until the drive runs, follow it: Enable operation starts in PP at the actual position. */
    if (st != ECAT_CIA_OP_ENABLED) {
        a->mode = MODE_PP;
        a->tpos = in->position;
        a->tvel = 0;
        a->setpoint = false;
    }
}

static void axis_output(axis_t *a)
{
    if (a->in == NULL || a->out == NULL || !a->st.available) {
        return;
    }
    const uint8_t st = a->st.cia_state;
    const uint16_t cw = controlword(a, st);
    a->st.controlword = cw;
    a->st.enabled = st == ECAT_CIA_OP_ENABLED;
    a->st.busy = is_motion(a->st.job) || a->st.job == ECAT_JOB_STOPPING;

    ecat_drive_out_t *out = a->out;
    out->controlword = cw;
    out->target_position = a->tpos;
    out->target_velocity = a->tvel;
    out->profile_velocity = a->pvel;
    out->profile_accel = a->pacc;
    out->profile_decel = a->pdec;
    out->mode = a->mode;
}

/* ---- Two-axis path (CSP) ---- */

#if CONFIG_ECAT_PATH
/* No path (bind, unbind, self-test); `gen` keeps counting so a waiting HMI sees the change. */
static void path_reset(void)
{
    const uint32_t gen = s_path.st.gen;
    memset(&s_path, 0, sizeof(s_path));
    s_path.st.gen = gen;
    s_path.st.err_axis = 0xFF;
}

static const char *path_geometry(const ecat_path_cmd_t *p, path_geom_t *g)
{
    memset(g, 0, sizeof(*g));
    if (p->axis[0] == p->axis[1]) {
        return "Pick two different axes";
    }
    const float k0 = fabsf(p->k[0]);
    const float k1 = fabsf(p->k[1]);
    if (!(k0 > 0.0f) || !(k1 > 0.0f) || !isfinite(k0) || !isfinite(k1)) {
        return "Axis scale unknown";
    }
    if (!(p->speed > 0.0f) || !(p->accel > 0.0f) || !isfinite(p->speed) || !isfinite(p->accel)) {
        return "Speed and acceleration must be above zero";
    }
    const float kmax = k0 > k1 ? k0 : k1;
    const float kmin = k0 < k1 ? k0 : k1;
    const float ex = p->end[0];
    const float ey = p->end[1];
    if (!isfinite(ex) || !isfinite(ey)) {
        return "End point not valid";
    }
    if (p->kind == ECAT_PATH_LINE) {
        g->length = hypotf(ex, ey);
    } else if (p->kind == ECAT_PATH_ARC_CW || p->kind == ECAT_PATH_ARC_CCW) {
        const float cx = p->center[0];
        const float cy = p->center[1];
        if (!isfinite(cx) || !isfinite(cy)) {
            return "Centre not valid";
        }
        g->r = hypotf(cx, cy);
        if (g->r * kmax < 1.0f) {
            return "The centre is at the start point";
        }
        /* Both ends on one circle, within 0.1 % of the radius plus 2 position units. */
        if (fabsf(hypotf(ex - cx, ey - cy) - g->r) > 0.001f * g->r + 2.0f / kmin) {
            return "The end point is not on the circle";
        }
        g->th0 = atan2f(-cy, -cx);
        float sweep = atan2f(ey - cy, ex - cx) - g->th0;
        const bool full = hypotf(ex, ey) * kmax < 1.0f; /* end = start: a full circle */
        if (p->kind == ECAT_PATH_ARC_CCW) {
            while (sweep <= 0.0f) {
                sweep += PATH_TWO_PI;
            }
            while (sweep > PATH_TWO_PI) {
                sweep -= PATH_TWO_PI;
            }
            sweep = full ? PATH_TWO_PI : sweep;
        } else {
            while (sweep >= 0.0f) {
                sweep -= PATH_TWO_PI;
            }
            while (sweep < -PATH_TWO_PI) {
                sweep += PATH_TWO_PI;
            }
            sweep = full ? -PATH_TWO_PI : sweep;
        }
        g->sweep = sweep;
        g->length = fabsf(sweep) * g->r;
    } else {
        return "Unknown path type";
    }
    if (g->length * kmax < 1.0f) {
        return "The path is shorter than one position unit";
    }
    return NULL;
}

/* Every target of the path fits the drives' 32-bit positions. */
static bool path_in_range(const path_t *p)
{
    for (int i = 0; i < 2; i++) {
        float lo;
        float hi;
        if (p->cmd.kind == ECAT_PATH_LINE) {
            lo = p->cmd.end[i] < 0.0f ? p->cmd.end[i] : 0.0f;
            hi = p->cmd.end[i] > 0.0f ? p->cmd.end[i] : 0.0f;
        } else {
            lo = p->cmd.center[i] - p->g.r;
            hi = p->cmd.center[i] + p->g.r;
        }
        const float a = lo * p->cmd.k[i];
        const float b = hi * p->cmd.k[i];
        if (fabsf(a) > 2.0e9f || fabsf(b) > 2.0e9f) {
            return false;
        }
        const int64_t ta = (int64_t)p->st.start[i] + (int64_t)a;
        const int64_t tb = (int64_t)p->st.start[i] + (int64_t)b;
        if (ta < INT32_MIN || ta > INT32_MAX || tb < INT32_MIN || tb > INT32_MAX) {
            return false;
        }
    }
    return true;
}

/* Point of the path at distance `s` from the start, path units relative to the start. */
static void path_point(const path_t *p, float s, float xy[2])
{
    if (p->cmd.kind == ECAT_PATH_LINE) {
        const float f = s / p->g.length;
        xy[0] = p->cmd.end[0] * f;
        xy[1] = p->cmd.end[1] * f;
    } else {
        const float th = p->g.th0 + p->g.sweep * (s / p->g.length);
        xy[0] = p->cmd.center[0] + p->g.r * cosf(th);
        xy[1] = p->cmd.center[1] + p->g.r * sinf(th);
    }
}

/* Distance of a point from the path: from the line, or from the circle. */
static float path_contour(const path_t *p, const float xy[2])
{
    if (p->cmd.kind == ECAT_PATH_LINE) {
        return fabsf(xy[0] * p->cmd.end[1] - xy[1] * p->cmd.end[0]) / p->g.length;
    }
    return fabsf(hypotf(xy[0] - p->cmd.center[0], xy[1] - p->cmd.center[1]) - p->g.r);
}

static bool path_axis_ok(const axis_t *a)
{
    return a->st.job == ECAT_JOB_PATH && a->st.cia_state == ECAT_CIA_OP_ENABLED;
}

/* Both axes back to PP, holding their last target. */
static void path_finish(path_t *p)
{
    for (int i = 0; i < 2; i++) {
        axis_t *a = &s_ax[p->st.axis[i]];
        if (a->st.job != ECAT_JOB_PATH) {
            continue;
        }
        a->mode = MODE_PP;
        a->st.target = a->tpos;
        uint8_t err = ECAT_AXERR_NONE;
        if (p->st.error != ECAT_AXERR_NONE) {
            err = p->st.err_axis == i ? p->st.error : ECAT_AXERR_PATH_PARTNER;
        }
        end_job(a, err);
    }
    p->st.phase = ECAT_PATH_IDLE;
}

static void path_start(const ecat_path_cmd_t *c, int64_t now)
{
    path_t *p = &s_path;
    if (p->st.phase != ECAT_PATH_IDLE) {
        return; /* the HMI never starts a second path; the running one is left alone */
    }
    const uint32_t gen = p->st.gen + 1;
    memset(p, 0, sizeof(*p));
    p->cmd = *c;
    p->st.gen = gen;
    p->st.kind = c->kind;
    p->st.err_axis = 0xFF;
    for (int i = 0; i < 2; i++) {
        p->st.axis[i] = c->axis[i];
        p->st.k[i] = c->k[i];
    }

    uint8_t err = ECAT_AXERR_NONE;
    for (int i = 0; i < 2 && err == ECAT_AXERR_NONE; i++) {
        const axis_t *a = c->axis[i] < s_n ? &s_ax[c->axis[i]] : NULL;
        if (a == NULL || !a->st.available || s_shutdown) {
            err = ECAT_AXERR_UNAVAILABLE;
        } else if (!s_bus_ok) {
            err = ECAT_AXERR_BUS;
        } else if (!a->power || a->st.cia_state != ECAT_CIA_OP_ENABLED || a->st.job == ECAT_JOB_ENABLING) {
            err = ECAT_AXERR_NOT_ENABLED;
        } else if (a->st.job != ECAT_JOB_IDLE) {
            err = ECAT_AXERR_BUSY;
        }
        if (err != ECAT_AXERR_NONE) {
            p->st.err_axis = (uint8_t)i;
        }
    }
    if (err == ECAT_AXERR_NONE && path_geometry(c, &p->g) != NULL) {
        err = ECAT_AXERR_PATH_INVALID;
    }
    if (err == ECAT_AXERR_NONE) {
        for (int i = 0; i < 2; i++) {
            p->st.start[i] = s_ax[c->axis[i]].st.position;
        }
        if (!path_in_range(p)) {
            err = ECAT_AXERR_PATH_INVALID;
        }
    }
    if (err != ECAT_AXERR_NONE) {
        p->st.error = err;
        return;
    }

    p->st.length = p->g.length;
    for (int i = 0; i < 2; i++) {
        axis_t *a = &s_ax[c->axis[i]];
        a->tpos = p->st.start[i]; /* CSP starts where the axis is: no jump */
        a->st.target = a->tpos;
        a->mode = MODE_CSP;
        a->halt = false;
        a->setpoint = false;
        start_job(a, ECAT_JOB_PATH, now);
        a->st.error = ECAT_AXERR_NONE;
    }
    p->st.phase = ECAT_PATH_ENTERING;
    p->t_phase = now;
}

/* One cycle of RUNNING / STOPPING. @return true when the path has ended (end point or stopped) */
static bool path_step(path_t *p, axis_t *const ax[2], int64_t now)
{
    const float dt = (float)s_cycle_us * 1e-6f;
    const float len = p->g.length;
    const float acc = p->cmd.accel;
    if (p->stop_req) {
        p->st.phase = ECAT_PATH_STOPPING;
    }

    /* Trapezoid along the path: ramp to the speed, and never faster than the end allows. */
    const float goal = p->st.phase == ECAT_PATH_STOPPING ? 0.0f : p->cmd.speed;
    float v = p->vel < goal ? fminf(goal, p->vel + acc * dt) : fmaxf(goal, p->vel - acc * dt);
    const float rest = len - p->st.s;
    const float v_end = sqrtf(2.0f * acc * (rest > 0.0f ? rest : 0.0f));
    v = v > v_end ? v_end : v;
    float s = p->st.s + 0.5f * (p->vel + v) * dt;
    const float k0 = fabsf(p->cmd.k[0]);
    const float k1 = fabsf(p->cmd.k[1]);
    const bool at_end = s >= len - 0.5f / (k0 > k1 ? k0 : k1); /* within half a position unit */
    if (at_end) {
        s = len;
        v = 0.0f;
    }
    p->vel = v;
    p->st.s = s;
    p->st.speed = v;
    p->st.time_ms = (uint32_t)((now - p->t_phase) / 1000);

    float xy[2];
    path_point(p, s, xy);
    if (at_end && p->cmd.kind == ECAT_PATH_LINE) {
        xy[0] = p->cmd.end[0];
        xy[1] = p->cmd.end[1];
    }
    for (int i = 0; i < 2; i++) {
        if (path_axis_ok(ax[i])) {
            const float d = xy[i] * p->cmd.k[i];
            ax[i]->tpos = sat32((int64_t)p->st.start[i] + (int64_t)(d < 0.0f ? d - 0.5f : d + 0.5f));
            ax[i]->st.target = ax[i]->tpos;
            ax[i]->mode = MODE_CSP;
        }
    }
    p->last[0] = xy[0];
    p->last[1] = xy[1];
    if (at_end) {
        p->st.done = true;
        return true;
    }
    return p->st.phase == ECAT_PATH_STOPPING && v <= 0.0f;
}

/* Contour error and lag from the actual positions (both axes still on the path). */
static void path_measure(path_t *p, axis_t *const ax[2])
{
    float act[2];
    for (int i = 0; i < 2; i++) {
        act[i] = (float)((int64_t)ax[i]->st.position - p->st.start[i]) / p->cmd.k[i];
    }
    const float c = path_contour(p, act);
    const float lag = hypotf(p->last[0] - act[0], p->last[1] - act[1]);
    p->st.contour = c;
    p->st.contour_max = c > p->st.contour_max ? c : p->st.contour_max;
    p->st.lag_max = lag > p->st.lag_max ? lag : p->st.lag_max;
}

/* Every cycle after axis_update(): advance the path and set both axes' targets. */
static void path_cycle(int64_t now)
{
    path_t *p = &s_path;
    if (p->st.phase == ECAT_PATH_IDLE) {
        atomic_store_explicit(&s_path_stop, false, memory_order_relaxed); /* a stop without a path is not kept */
        return;
    }
    axis_t *const ax[2] = {&s_ax[p->st.axis[0]], &s_ax[p->st.axis[1]]};
    int n_ok = 0;
    for (int i = 0; i < 2; i++) {
        if (path_axis_ok(ax[i])) {
            n_ok++;
        } else if (p->st.error == ECAT_AXERR_NONE) {
            /* Switched off by the fail-safe rules, the operator or a quick stop. */
            p->st.error = ax[i]->st.error != ECAT_AXERR_NONE ? ax[i]->st.error : ECAT_AXERR_NOT_ENABLED;
            p->st.err_axis = (uint8_t)i;
        }
    }
    if (atomic_exchange_explicit(&s_path_stop, false, memory_order_relaxed) || s_shutdown || n_ok < 2) {
        p->stop_req = true; /* the axis left on the path decelerates along it */
    }
    if (n_ok == 0) {
        path_finish(p);
        return;
    }

    switch (p->st.phase) {
    case ECAT_PATH_ENTERING: {
        if (p->stop_req) {
            path_finish(p); /* nothing has moved yet */
            break;
        }
        int waiting = -1;
        for (int i = 1; i >= 0; i--) {
            if (ax[i]->st.mode_display != MODE_CSP) {
                waiting = i;
            }
        }
        if (waiting < 0) {
            p->st.phase = ECAT_PATH_RUNNING;
            p->t_phase = now;
        } else if (now - p->t_phase > MODE_TIMEOUT_US) {
            p->st.error = ECAT_AXERR_MODE_TIMEOUT;
            p->st.err_axis = (uint8_t)waiting;
            path_finish(p);
        }
        break;
    }
    case ECAT_PATH_RUNNING:
    case ECAT_PATH_STOPPING:
        if (n_ok == 2) {
            path_measure(p, ax); /* before path_step(): against the target the drives have seen */
        }
        if (path_step(p, ax, now)) {
            p->st.phase = ECAT_PATH_LEAVING;
            p->t_phase = now;
        }
        break;
    case ECAT_PATH_LEAVING:
        if (n_ok == 2) {
            path_measure(p, ax);
        }
        if (now - p->t_phase > PATH_EXIT_HOLD_US) {
            path_finish(p);
        }
        break;
    default:
        path_finish(p);
        break;
    }
}
#else
static void path_reset(void)
{
}
#endif

static void publish(void)
{
    unsigned seq = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, seq + 1, memory_order_relaxed); /* odd: write in progress */
    atomic_thread_fence(memory_order_release);
    s_snap.in_op = s_in_op;
    s_snap.bus_ok = s_bus_ok;
    s_snap.n_axes = s_n;
    for (uint8_t i = 0; i < s_n; i++) {
        s_snap.axis[i] = s_ax[i].st;
    }
#if CONFIG_ECAT_PATH
    s_snap.path = s_path.st;
#endif
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s_seq, seq + 2, memory_order_relaxed);
}

/* ---- Core 1 API ---- */

void ecat_axes_init(uint32_t cycle_us)
{
    s_cycle_us = cycle_us ? cycle_us : 1000;
    if (s_q == NULL) {
        s_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(ecat_axis_cmd_t));
        configASSERT(s_q);
#if CONFIG_ECAT_PATH
        s_pq = xQueueCreate(PATH_QUEUE_LEN, sizeof(ecat_path_cmd_t));
        configASSERT(s_pq);
#endif
    }
    publish();
}

void ecat_axes_bind(struct ecx_context *ctx, const ecat_topology_t *bus)
{
    while (s_testing) {
        vTaskDelay(1); /* a self-test runs for a few ms */
    }
    memset(s_ax, 0, sizeof(s_ax));
    s_n = 0;
    path_reset();
    for (uint16_t k = 0; k < bus->n_slaves && s_n < ECAT_MAX_AXES; k++) {
        const ecat_topo_slave_t *s = &bus->slave[k];
        if (!ecat_kind_is_drive((ecat_kind_t)s->kind)) {
            continue;
        }
        axis_t *a = &s_ax[s_n++];
        strlcpy(a->st.name, s->name, sizeof(a->st.name));
        a->st.slave = s->position;
        a->st.kind = s->kind;
        const ecat_drive_info_t *info = ecat_drive_info(s->position);
        if (info) {
            a->has_vendor_error = info->has_vendor_error;
            a->st.units_per_rev = ecat_drive_units_per_rev(info);
            a->st.home_method = info->home_method;
            a->st.home_method_valid = info->home_method_valid;
        }
        if (ecat_drive_layout_ok(ctx, s->position)) {
            a->out = (ecat_drive_out_t *)ctx->slavelist[s->position].outputs;
            a->in = (const ecat_drive_in_t *)ctx->slavelist[s->position].inputs;
            a->st.available = true;
            a->tpos = a->in->position;
        } else {
            a->st.error = ECAT_AXERR_UNAVAILABLE;
        }
        a->mode = MODE_PP;
    }
    /* bus->slave[] is in cable order, so the axes are too: AX1 is the first drive on the chain. */
    s_wkc_bad = 0;
    s_bus_ok = true;
    s_shutdown = false;
    xQueueReset(s_q); /* commands sent before OP are stale */
#if CONFIG_ECAT_PATH
    xQueueReset(s_pq);
#endif
    s_in_op = true;
    publish();

    for (uint8_t i = 0; i < s_n; i++) {
        const ecat_axis_status_t *st = &s_ax[i].st;
        ESP_LOGI(TAG, "%s: slave %u %s, %" PRIu32 " units/rev%s, homing method %d", st->name, st->slave,
                 st->available ? "ready" : "UNAVAILABLE (PDO layout)", st->units_per_rev,
                 st->units_per_rev ? "" : " (unknown)", st->home_method);
    }
}

void ecat_axes_unbind(void)
{
    s_in_op = false;
    s_n = 0;
    path_reset();
    publish();
}

void ecat_axes_block_slave(uint16_t slave)
{
    for (uint8_t i = 0; i < s_n; i++) {
        if (s_ax[i].st.slave == slave) {
            s_ax[i].blocked = true;
        }
    }
}

void ecat_axes_cycle(bool wkc_ok, int64_t now_us)
{
    s_wkc_bad = wkc_ok ? 0 : (s_wkc_bad < UINT8_MAX ? s_wkc_bad + 1 : s_wkc_bad);
    const bool bus_ok = s_wkc_bad < BUS_ERR_CYCLES;
    if (!bus_ok && s_bus_ok) {
        for (uint8_t i = 0; i < s_n; i++) {
            if (s_ax[i].power || s_ax[i].st.job != ECAT_JOB_IDLE) {
                switch_off(&s_ax[i], ECAT_AXERR_BUS);
            }
        }
    }
    s_bus_ok = bus_ok;

    ecat_axis_cmd_t cmd;
    for (int k = 0; k < CMDS_PER_CYCLE && xQueueReceive(s_q, &cmd, 0) == pdTRUE; k++) {
        if (cmd.axis < s_n && !s_shutdown) {
            apply_cmd(&s_ax[cmd.axis], &cmd, now_us);
        }
    }
#if CONFIG_ECAT_PATH
    ecat_path_cmd_t path;
    if (xQueueReceive(s_pq, &path, 0) == pdTRUE) {
        path_start(&path, now_us);
    }
#endif
    for (uint8_t i = 0; i < s_n; i++) {
        axis_update(&s_ax[i], now_us);
    }
#if CONFIG_ECAT_PATH
    path_cycle(now_us);
#endif
    for (uint8_t i = 0; i < s_n; i++) {
        axis_output(&s_ax[i]);
    }
    if (!s_testing) {
        publish();
    }
}

void ecat_axes_shutdown(void)
{
    s_shutdown = true;
}

bool ecat_axes_all_off(void)
{
    for (uint8_t i = 0; i < s_n; i++) {
        if (s_ax[i].st.available && s_ax[i].st.cia_state == ECAT_CIA_OP_ENABLED) {
            return false;
        }
    }
    return true;
}

/* ---- Core 0 API ---- */

bool ecat_axis_command(const ecat_axis_cmd_t *cmd)
{
    if (s_q == NULL || cmd == NULL || cmd->axis >= ECAT_MAX_AXES) {
        return false;
    }
    return xQueueSend(s_q, cmd, 0) == pdTRUE;
}

void ecat_axes_read(ecat_axes_status_t *out)
{
    unsigned before, after;
    do {
        before = atomic_load_explicit(&s_seq, memory_order_acquire);
        out->in_op = s_snap.in_op;
        out->bus_ok = s_snap.bus_ok;
        out->n_axes = s_snap.n_axes <= ECAT_MAX_AXES ? s_snap.n_axes : ECAT_MAX_AXES;
        memcpy(out->axis, s_snap.axis, sizeof(out->axis[0]) * out->n_axes);
        out->path = s_snap.path;
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_seq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
}

const char *ecat_axis_error_text(uint8_t err)
{
    switch (err) {
    case ECAT_AXERR_NONE:             return "No error";
    case ECAT_AXERR_UNAVAILABLE:      return "Axis not available";
    case ECAT_AXERR_BUS:              return "EtherCAT error: motion stopped, servo off";
    case ECAT_AXERR_FAULT:            return "Drive fault";
    case ECAT_AXERR_ENABLE_TIMEOUT:   return "Servo on timed out: check STO, 24 V and main power";
    case ECAT_AXERR_NOT_ENABLED:      return "Servo is off";
    case ECAT_AXERR_BUSY:             return "Axis is busy";
    case ECAT_AXERR_MODE_TIMEOUT:     return "Drive did not change the operation mode";
    case ECAT_AXERR_SETPOINT_TIMEOUT: return "Drive did not accept the target";
    case ECAT_AXERR_HOMING:           return "Homing error";
    case ECAT_AXERR_HOMING_TIMEOUT:   return "Homing timed out";
    case ECAT_AXERR_RESET_FAILED:     return "Fault still active after reset";
    case ECAT_AXERR_JOG_TIMEOUT:      return "Jog stopped: no refresh from the panel";
    case ECAT_AXERR_DROPPED:          return "Drive switched the servo off (STO, main power?)";
    case ECAT_AXERR_PATH_INVALID:     return "Path not valid (axes, size or position range)";
    case ECAT_AXERR_PATH_PARTNER:     return "Path stopped: the other axis stopped";
    default:                          return "Unknown error";
    }
}

const char *ecat_cia_state_text(uint8_t state)
{
    switch (state) {
    case ECAT_CIA_SWITCH_ON_DISABLED: return "Switch on disabled";
    case ECAT_CIA_READY:              return "Ready to switch on";
    case ECAT_CIA_SWITCHED_ON:        return "Switched on";
    case ECAT_CIA_OP_ENABLED:         return "Operation enabled";
    case ECAT_CIA_QUICK_STOP:         return "Quick stop active";
    case ECAT_CIA_FAULT_REACTION:     return "Fault reaction active";
    case ECAT_CIA_FAULT:              return "Fault";
    case ECAT_CIA_NOT_READY:
    default:                          return "Not ready to switch on";
    }
}

const char *ecat_axis_job_text(uint8_t job)
{
    switch (job) {
    case ECAT_JOB_ENABLING:   return "Switching on";
    case ECAT_JOB_RESETTING:  return "Resetting";
    case ECAT_JOB_MOVING:     return "Moving";
    case ECAT_JOB_JOGGING:    return "Jogging";
    case ECAT_JOB_HOMING:     return "Homing";
    case ECAT_JOB_STOPPING:   return "Stopping";
    case ECAT_JOB_QUICK_STOP: return "Quick stop";
    case ECAT_JOB_PATH:       return "Interpolating";
    case ECAT_JOB_IDLE:
    default:                  return "Idle";
    }
}

/* ---- Core 0: two-axis path ---- */

#if CONFIG_ECAT_PATH
const char *ecat_path_check(const ecat_path_cmd_t *p, float *length)
{
    path_geom_t g;
    const char *why = p ? path_geometry(p, &g) : "No path";
    if (length) {
        *length = why ? 0.0f : g.length;
    }
    return why;
}

bool ecat_path_start(const ecat_path_cmd_t *p)
{
    if (s_pq == NULL || p == NULL) {
        return false;
    }
    return xQueueSend(s_pq, p, 0) == pdTRUE;
}

void ecat_path_stop(void)
{
    atomic_store_explicit(&s_path_stop, true, memory_order_relaxed);
}

const char *ecat_path_phase_text(uint8_t phase)
{
    switch (phase) {
    case ECAT_PATH_ENTERING: return "Switching to CSP";
    case ECAT_PATH_RUNNING:  return "Running";
    case ECAT_PATH_STOPPING: return "Stopping";
    case ECAT_PATH_LEAVING:  return "Settling";
    case ECAT_PATH_IDLE:
    default:                 return "Idle";
    }
}
#endif

/* ---- Self-test against a simulated CiA402 drive ---- */

/*
 * The simulated drive follows the CiA402 state machine and runs PP (constant speed, set-point
 * handshake), PV (instant speed) and HM (homes to 0 after 50 ms). Like real drives it keeps
 * "target reached" set while idle and "homing attained" set after a homing, so the checks also
 * cover the stale-bit traps of docs/REFERENCE_PGM.md 3.3.
 */
typedef struct {
    ecat_drive_out_t out;
    ecat_drive_in_t in;
    uint8_t state;
    uint16_t prev_cw;
    bool inject_fault;
    bool inject_drop;
    int64_t pos;
    int32_t vel;
    int32_t target;
    bool pp_active;
    bool ack;
    bool homing;
    bool homed_bit;
    int home_ms;
    bool refuse_csp;      /* stays in PP when asked for CSP */
    int64_t min_pos;      /* range and largest step per cycle, for the path checks */
    int64_t max_pos;
    int64_t max_step;
} sim_t;

static int s_fail;
static int64_t s_t_now;

static uint16_t sim_statusword(uint8_t st)
{
    switch (st) {
    case ECAT_CIA_SWITCH_ON_DISABLED: return 0x0240;
    case ECAT_CIA_READY:              return 0x0221;
    case ECAT_CIA_SWITCHED_ON:        return 0x0233;
    case ECAT_CIA_OP_ENABLED:         return 0x0237;
    case ECAT_CIA_QUICK_STOP:         return 0x0217;
    case ECAT_CIA_FAULT:              return 0x0208;
    default:                          return 0x0000;
    }
}

static void sim_step(sim_t *d)
{
    const uint16_t cw = d->out.controlword;
    uint8_t st = d->state;
    if (d->inject_fault) {
        d->inject_fault = false;
        st = ECAT_CIA_FAULT;
    } else if (d->inject_drop) {
        d->inject_drop = false;
        st = ECAT_CIA_SWITCH_ON_DISABLED;
    } else if (st == ECAT_CIA_FAULT) {
        if ((cw & CW_FAULT_RESET) && !(d->prev_cw & CW_FAULT_RESET)) {
            st = ECAT_CIA_SWITCH_ON_DISABLED;
        }
    } else if ((cw & 0x02) == 0) {
        st = ECAT_CIA_SWITCH_ON_DISABLED; /* disable voltage */
    } else if ((cw & 0x06) == 0x02) {
        st = st == ECAT_CIA_OP_ENABLED ? ECAT_CIA_QUICK_STOP : ECAT_CIA_SWITCH_ON_DISABLED;
    } else if ((cw & 0x07) == 0x06) {
        if (st == ECAT_CIA_SWITCH_ON_DISABLED || st == ECAT_CIA_SWITCHED_ON || st == ECAT_CIA_OP_ENABLED) {
            st = ECAT_CIA_READY;
        }
    } else if ((cw & 0x0F) == 0x07) {
        if (st == ECAT_CIA_READY || st == ECAT_CIA_OP_ENABLED) {
            st = ECAT_CIA_SWITCHED_ON;
        }
    } else if ((cw & 0x0F) == 0x0F && st == ECAT_CIA_SWITCHED_ON) {
        st = ECAT_CIA_OP_ENABLED;
    }
    if (st != ECAT_CIA_OP_ENABLED) {
        d->vel = 0;
        d->pp_active = false;
        d->homing = false;
        d->ack = false;
    }
    d->state = st;

    const int8_t mode = d->out.mode;
    const bool halt = (cw & CW_HALT) != 0;
    const bool b4 = (cw & CW_BIT4) != 0;
    const bool b4_rise = b4 && !(d->prev_cw & CW_BIT4);
    uint16_t bits = 0;
    if (st == ECAT_CIA_OP_ENABLED && mode == MODE_PP) {
        if (b4_rise && !halt) {
            d->target = d->out.target_position;
            d->pp_active = true;
            d->ack = true;
        }
        if (!b4) {
            d->ack = false;
        }
        if (halt) {
            d->pp_active = false;
        }
        if (d->pp_active) {
            const int64_t step = (int64_t)d->out.profile_velocity / 1000;
            const int64_t diff = (int64_t)d->target - d->pos;
            if (diff <= step && diff >= -step) {
                d->pos = d->target;
                d->pp_active = false;
            } else {
                d->pos += diff > 0 ? step : -step;
            }
        }
        bits |= (d->ack ? SW_BIT12 : 0) | (d->pp_active ? 0 : SW_TARGET_REACHED);
    } else if (st == ECAT_CIA_OP_ENABLED && mode == MODE_PV) {
        d->vel = halt ? 0 : d->out.target_velocity;
        d->pos += d->vel / 1000;
        bits |= SW_TARGET_REACHED;
    } else if (st == ECAT_CIA_OP_ENABLED && mode == MODE_CSP && !d->refuse_csp) {
        /* CSP: a perfect drive, one cycle behind the target */
        const int64_t step = (int64_t)d->out.target_position - d->pos;
        const int64_t mag = step < 0 ? -step : step;
        d->max_step = mag > d->max_step ? mag : d->max_step;
        d->pos = d->out.target_position;
        bits |= SW_BIT12; /* CSP: the drive follows the target */
    } else if (st == ECAT_CIA_OP_ENABLED && mode == MODE_HM) {
        if (b4_rise) {
            d->homing = true;
            d->homed_bit = false;
            d->home_ms = 0;
        }
        if (halt) {
            d->homing = false;
        }
        if (d->homing && ++d->home_ms >= 50) {
            d->pos = 0;
            d->homing = false;
            d->homed_bit = true;
        }
        bits |= (d->homed_bit && !d->homing) ? (SW_BIT12 | SW_TARGET_REACHED) : 0;
    } else {
        bits |= SW_TARGET_REACHED;
    }
    d->in.statusword = sim_statusword(st) | bits;
    d->in.mode_display = mode == MODE_CSP && d->refuse_csp ? MODE_PP : mode;
    d->min_pos = d->pos < d->min_pos ? d->pos : d->min_pos;
    d->max_pos = d->pos > d->max_pos ? d->pos : d->max_pos;
    d->in.position = (int32_t)d->pos;
    d->in.velocity = d->vel;
    d->in.error_code = st == ECAT_CIA_FAULT ? 0x7500 : 0;
    d->prev_cw = cw;
}

static void sim_run(sim_t *d, int cycles, bool wkc_ok)
{
    for (int i = 0; i < cycles; i++) {
        s_t_now += 1000; /* 1 ms cycle */
        ecat_axes_cycle(wkc_ok, s_t_now);
        sim_step(d);
    }
}

static bool sim_run_until_idle(sim_t *d, int max_cycles)
{
    for (int i = 0; i < max_cycles; i++) {
        sim_run(d, 1, true);
        if (s_ax[0].st.job == ECAT_JOB_IDLE) {
            return true;
        }
    }
    return false;
}

static void sim_cmd(uint8_t code, int8_t dir, int32_t position, uint32_t velocity)
{
    const ecat_axis_cmd_t c = {.axis = 0, .code = code, .dir = dir, .position = position, .velocity = velocity};
    (void)ecat_axis_command(&c);
}

static void check(bool ok, const char *what)
{
    const ecat_axis_status_t *st = &s_ax[0].st;
    if (ok) {
        ESP_LOGI(TAG, "  ok   %s", what);
    } else {
        s_fail++;
        ESP_LOGE(TAG, "  FAIL %s (job %s, error %s, %s, pos %ld)", what, ecat_axis_job_text(st->job),
                 ecat_axis_error_text(st->error), ecat_cia_state_text(st->cia_state), (long)st->position);
    }
}

/* ---- Two-axis path self-test: two simulated drives in CSP ---- */

#if CONFIG_ECAT_PATH
static sim_t s_sim[2];

static void sim2_run(int cycles, bool wkc_ok)
{
    for (int i = 0; i < cycles; i++) {
        s_t_now += 1000;
        ecat_axes_cycle(wkc_ok, s_t_now);
        sim_step(&s_sim[0]);
        sim_step(&s_sim[1]);
    }
}

static bool sim2_run_path(int max_cycles)
{
    for (int i = 0; i < max_cycles; i++) {
        sim2_run(1, true);
        if (s_path.st.phase == ECAT_PATH_IDLE) {
            return true;
        }
    }
    return false;
}

static void sim2_cmd(uint8_t axis, uint8_t code)
{
    const ecat_axis_cmd_t c = {.axis = axis, .code = code};
    (void)ecat_axis_command(&c);
}

/* Start a path and note where both drives are. */
static void sim2_path(const ecat_path_cmd_t *c, int64_t start[2])
{
    for (int i = 0; i < 2; i++) {
        start[i] = s_sim[i].pos;
        s_sim[i].min_pos = s_sim[i].pos;
        s_sim[i].max_pos = s_sim[i].pos;
        s_sim[i].max_step = 0;
    }
    (void)ecat_path_start(c);
}

static bool both_idle_pp(void)
{
    return s_ax[0].st.job == ECAT_JOB_IDLE && s_ax[1].st.job == ECAT_JOB_IDLE && s_ax[0].mode == MODE_PP &&
           s_ax[1].mode == MODE_PP;
}

static void check_path(bool ok, const char *what)
{
    const ecat_path_status_t *p = &s_path.st;
    if (ok) {
        ESP_LOGI(TAG, "  ok   %s", what);
        return;
    }
    s_fail++;
    ESP_LOGE(TAG, "  FAIL %s (path %s, error %s on %d, s %.4f of %.4f, contour max %.4f, %lu ms, pos %ld %ld)",
             what, ecat_path_phase_text(p->phase), ecat_axis_error_text(p->error), p->err_axis, (double)p->s,
             (double)p->length, (double)p->contour_max, (unsigned long)p->time_ms, (long)s_sim[0].pos,
             (long)s_sim[1].pos);
}

static void path_selftest(void)
{
    const float k = 100.0f; /* position units per path unit */
    const float tol = 1.5f / k;
    int64_t p0[2];

    memset(s_sim, 0, sizeof(s_sim));
    memset(s_ax, 0, sizeof(s_ax));
    path_reset();
    for (int i = 0; i < 2; i++) {
        sim_t *d = &s_sim[i];
        d->state = ECAT_CIA_SWITCH_ON_DISABLED;
        d->pos = i == 0 ? 5000 : -3000;
        sim_step(d);
        axis_t *a = &s_ax[i];
        strlcpy(a->st.name, i == 0 ? "SIMX" : "SIMY", sizeof(a->st.name));
        a->out = &d->out;
        a->in = &d->in;
        a->st.available = true;
        a->mode = MODE_PP;
    }
    s_n = 2;
    s_bus_ok = true;
    s_wkc_bad = 0;
    s_shutdown = false;
    xQueueReset(s_q);
    xQueueReset(s_pq);
    ESP_LOGI(TAG, "two-axis path self-test (CSP, 100 units per path unit):");

    ecat_path_cmd_t line = {
        .axis = {0, 1}, .kind = ECAT_PATH_LINE, .k = {k, k}, .end = {30.0f, -40.0f}, .speed = 50.0f, .accel = 500.0f};
    sim2_path(&line, p0);
    sim2_run_path(5);
    check_path(s_path.st.error == ECAT_AXERR_NOT_ENABLED && s_path.st.err_axis == 0 && s_sim[0].pos == p0[0],
               "path refused while the servos are off");

    sim2_cmd(0, ECAT_AXIS_ENABLE);
    sim2_cmd(1, ECAT_AXIS_ENABLE);
    sim2_run(20, true);

    sim2_path(&line, p0);
    bool ok = sim2_run_path(3000);
    check_path(ok && s_path.st.done && s_path.st.error == ECAT_AXERR_NONE && s_sim[0].pos == p0[0] + 3000 &&
                   s_sim[1].pos == p0[1] - 4000 && both_idle_pp(),
               "line (30, -40) ends on its end point, both axes back in PP");
    check_path(s_path.st.contour_max <= tol, "line: the axes stay on the line");
    /* 50/s is 0.05 per 1 ms cycle along the path: 3 units on X and 4 on Y, plus rounding. */
    check_path(s_sim[0].max_step <= 4 && s_sim[1].max_step <= 5 && s_path.st.time_ms >= 1050 &&
                   s_path.st.time_ms <= 1200,
               "line: path speed 50/s, ramps 0.1 s (1.1 s)");

    ecat_path_cmd_t arc = {.axis = {0, 1},
                           .kind = ECAT_PATH_ARC_CW,
                           .k = {k, k},
                           .end = {10.0f, 10.0f},
                           .center = {10.0f, 0.0f},
                           .speed = 50.0f,
                           .accel = 500.0f};
    sim2_path(&arc, p0);
    ok = sim2_run_path(3000);
    check_path(ok && s_path.st.done && s_sim[0].pos == p0[0] + 1000 && s_sim[1].pos == p0[1] + 1000 &&
                   s_path.st.contour_max <= tol && s_sim[1].min_pos >= p0[1] - 1,
               "quarter arc clockwise around (10, 0) to (10, 10)");

    arc.kind = ECAT_PATH_ARC_CCW;
    arc.end[0] = 0.0f;
    arc.end[1] = 0.0f;
    arc.center[0] = 0.0f;
    arc.center[1] = 5.0f;
    sim2_path(&arc, p0);
    ok = sim2_run_path(3000);
    check_path(ok && s_path.st.done && s_sim[0].pos == p0[0] && s_sim[1].pos == p0[1] &&
                   s_path.st.contour_max <= tol && s_sim[0].max_pos >= p0[0] + 499 &&
                   s_sim[0].min_pos <= p0[0] - 499 && s_sim[1].max_pos >= p0[1] + 999,
               "full circle counter-clockwise, radius 5");

    line.end[0] = 200.0f;
    line.end[1] = 0.0f;
    sim2_path(&line, p0);
    sim2_run(500, true);
    ecat_path_stop();
    ok = sim2_run_path(1000);
    check_path(ok && !s_path.st.done && s_path.st.error == ECAT_AXERR_NONE && s_sim[0].pos < p0[0] + 20000 &&
                   s_sim[1].pos == p0[1] && both_idle_pp() && s_ax[0].power && s_ax[1].power,
               "stop: decelerates along the path, servos stay on");

    sim2_path(&line, p0);
    sim2_run(300, true);
    sim2_cmd(1, ECAT_AXIS_STOP);
    ok = sim2_run_path(1000);
    check_path(ok && !s_path.st.done && s_path.st.error == ECAT_AXERR_NONE && both_idle_pp(),
               "STOP of one axis stops the path");

    line.end[1] = 100.0f;
    sim2_path(&line, p0);
    sim2_run(300, true);
    s_sim[1].inject_drop = true;
    ok = sim2_run_path(1000);
    check_path(ok && s_path.st.error == ECAT_AXERR_DROPPED && s_path.st.err_axis == 1 &&
                   s_ax[0].st.error == ECAT_AXERR_PATH_PARTNER && s_ax[0].power && !s_ax[1].power && both_idle_pp(),
               "a dropped axis stops the path; the other decelerates and stays on");
    sim2_cmd(1, ECAT_AXIS_ENABLE);
    sim2_run(20, true);

    s_sim[1].refuse_csp = true;
    sim2_path(&line, p0);
    ok = sim2_run_path(1000);
    check_path(ok && s_path.st.error == ECAT_AXERR_MODE_TIMEOUT && s_path.st.err_axis == 1 &&
                   s_sim[0].pos == p0[0] && s_sim[1].pos == p0[1] && both_idle_pp(),
               "a drive that stays out of CSP: path refused, nothing moves");
    s_sim[1].refuse_csp = false;

    arc.kind = ECAT_PATH_ARC_CW;
    arc.end[0] = 5.0f;
    arc.end[1] = 5.0f;
    arc.center[0] = 10.0f;
    arc.center[1] = 0.0f;
    sim2_path(&arc, p0);
    sim2_run_path(5);
    check_path(s_path.st.error == ECAT_AXERR_PATH_INVALID && s_sim[0].pos == p0[0] && both_idle_pp(),
               "arc whose end point is off the circle is refused");
    line.axis[1] = 0;
    sim2_path(&line, p0);
    sim2_run_path(5);
    check_path(s_path.st.error == ECAT_AXERR_PATH_INVALID, "path on one axis twice is refused");
    line.axis[1] = 1;

    sim2_path(&line, p0);
    sim2_run(300, true);
    sim2_run(BUS_ERR_CYCLES, false);
    sim2_run(2, true);
    check_path(s_path.st.phase == ECAT_PATH_IDLE && s_path.st.error == ECAT_AXERR_BUS && !s_ax[0].power &&
                   !s_ax[1].power,
               "working counter errors end the path, servos off");

    sim2_cmd(0, ECAT_AXIS_ENABLE);
    sim2_cmd(1, ECAT_AXIS_ENABLE);
    sim2_run(20, true);
    sim2_path(&line, p0);
    sim2_run(300, true);
    ecat_axes_shutdown();
    sim2_run(2000, true);
    check_path(s_path.st.phase == ECAT_PATH_IDLE && !s_path.st.done && !s_ax[0].power && !s_ax[1].power,
               "leaving OP: the path decelerates, then both servos switch off");
    s_shutdown = false;
}
#endif

int ecat_axis_selftest(void)
{
    if (s_in_op) {
        ESP_LOGW(TAG, "axis self-test skipped: the bus is in OP");
        return -1;
    }
    static sim_t d;
    s_testing = true;
    memset(&d, 0, sizeof(d));
    d.state = ECAT_CIA_SWITCH_ON_DISABLED;
    sim_step(&d);
    memset(s_ax, 0, sizeof(s_ax));
    s_n = 1;
    axis_t *a = &s_ax[0];
    strlcpy(a->st.name, "SIM", sizeof(a->st.name));
    a->out = &d.out;
    a->in = &d.in;
    a->st.available = true;
    a->mode = MODE_PP;
    path_reset();
    s_bus_ok = true;
    s_wkc_bad = 0;
    s_shutdown = false;
    s_fail = 0;
    s_t_now = 0;
    xQueueReset(s_q);
#if CONFIG_ECAT_PATH
    xQueueReset(s_pq);
#endif
    ESP_LOGI(TAG, "axis self-test with a simulated CiA402 drive:");

    sim_cmd(ECAT_AXIS_MOVE_ABS, 0, 1000, 100000);
    sim_run(&d, 3, true);
    check(a->st.error == ECAT_AXERR_NOT_ENABLED && d.pos == 0, "move refused while the servo is off");

    sim_cmd(ECAT_AXIS_ENABLE, 0, 0, 0);
    sim_run(&d, 20, true);
    check(a->st.enabled && a->st.job == ECAT_JOB_IDLE && a->st.error == ECAT_AXERR_NONE, "servo on");

    sim_cmd(ECAT_AXIS_MOVE_ABS, 0, 1000, 100000);
    sim_run(&d, 2, true);
    check(a->st.job == ECAT_JOB_MOVING, "a stale target-reached bit does not end a new move");
    check(sim_run_until_idle(&d, 200) && a->st.position == 1000 && a->st.error == ECAT_AXERR_NONE,
          "absolute move to 1000");

    sim_cmd(ECAT_AXIS_MOVE_REL, 0, -400, 100000);
    check(sim_run_until_idle(&d, 200) && a->st.position == 600, "relative move by -400");

    sim_cmd(ECAT_AXIS_MOVE_ABS, 0, 100600, 10000);
    sim_run(&d, 5, true);
    sim_cmd(ECAT_AXIS_MOVE_ABS, 0, 0, 10000);
    sim_run(&d, 2, true);
    check(a->st.error == ECAT_AXERR_BUSY && a->st.job == ECAT_JOB_MOVING, "second move refused while moving");
    sim_cmd(ECAT_AXIS_STOP, 0, 0, 0);
    sim_run_until_idle(&d, 100);
    int32_t p = a->st.position;
    sim_run(&d, 50, true);
    check(a->st.job == ECAT_JOB_IDLE && a->st.position == p && p < 100600, "stop halts the move, no resume");
    sim_cmd(ECAT_AXIS_MOVE_ABS, 0, 0, 1000000);
    check(sim_run_until_idle(&d, 300) && a->st.position == 0, "move after a stop");

    for (int i = 0; i < 50; i++) {
        if (i % 10 == 0) {
            sim_cmd(ECAT_AXIS_JOG, 1, 0, 1000);
        }
        sim_run(&d, 1, true);
    }
    check(a->st.job == ECAT_JOB_JOGGING && a->st.position > 0, "jog runs while refreshed");
    sim_cmd(ECAT_AXIS_JOG, 0, 0, 0);
    bool ok = sim_run_until_idle(&d, 100);
    p = a->st.position;
    sim_run(&d, 20, true);
    check(ok && a->st.error == ECAT_AXERR_NONE && a->st.position == p, "jog stops on release");

    sim_cmd(ECAT_AXIS_JOG, -1, 0, 1000);
    sim_run(&d, ECAT_JOG_DEADMAN_MS + 50, true);
    p = a->st.position;
    sim_run(&d, 20, true);
    check(a->st.error == ECAT_AXERR_JOG_TIMEOUT && a->st.job == ECAT_JOB_IDLE && a->st.position == p,
          "jog stops by itself without refresh (dead-man)");

    sim_cmd(ECAT_AXIS_HOME, 0, 0, 0);
    check(sim_run_until_idle(&d, 2000) && a->st.homed && a->st.position == 0 && a->st.error == ECAT_AXERR_NONE,
          "homing");
    sim_cmd(ECAT_AXIS_HOME, 0, 0, 0);
    sim_run(&d, 10, true);
    check(a->st.job == ECAT_JOB_HOMING && !a->st.homed, "a stale homing-attained bit does not end a new homing");
    check(sim_run_until_idle(&d, 2000) && a->st.homed, "second homing");

    d.inject_fault = true;
    sim_run(&d, 3, true);
    check(a->st.error == ECAT_AXERR_FAULT && !a->power && !a->st.enabled, "a drive fault switches the axis off");
    sim_cmd(ECAT_AXIS_ENABLE, 0, 0, 0);
    sim_run(&d, 3, true);
    check(a->st.error == ECAT_AXERR_FAULT && !a->st.enabled, "servo on refused while in fault");
    sim_cmd(ECAT_AXIS_RESET, 0, 0, 0);
    check(sim_run_until_idle(&d, 100) && a->st.error == ECAT_AXERR_NONE && a->st.cia_state != ECAT_CIA_FAULT,
          "fault reset");
    sim_cmd(ECAT_AXIS_ENABLE, 0, 0, 0);
    sim_run(&d, 20, true);
    check(a->st.enabled, "servo on after the reset");

    d.inject_drop = true;
    sim_run(&d, 20, true);
    check(a->st.error == ECAT_AXERR_DROPPED && !a->st.enabled, "servo dropped by the drive is not switched on again");

    sim_cmd(ECAT_AXIS_ENABLE, 0, 0, 0);
    sim_run(&d, 20, true);
    sim_run(&d, BUS_ERR_CYCLES, false);
    check(a->st.error == ECAT_AXERR_BUS && !a->power, "working counter errors switch the axis off");
    sim_run(&d, 20, true);
    check(!a->st.enabled, "servo stays off after the bus recovers");

    sim_cmd(ECAT_AXIS_ENABLE, 0, 0, 0);
    sim_run(&d, 20, true);
    sim_cmd(ECAT_AXIS_QUICK_STOP, 0, 0, 0);
    check(sim_run_until_idle(&d, 100) && !a->st.enabled && !a->power && a->st.error == ECAT_AXERR_NONE,
          "quick stop ends with the servo off");

#if CONFIG_ECAT_PATH
    path_selftest();
#endif

    memset(s_ax, 0, sizeof(s_ax));
    s_n = 0;
    path_reset();
    s_testing = false;
    publish();
    if (s_fail) {
        ESP_LOGE(TAG, "axis self-test: %d check(s) FAILED", s_fail);
    } else {
        ESP_LOGI(TAG, "axis self-test: all checks passed");
    }
    return s_fail;
}
