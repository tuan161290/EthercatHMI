/*
 * Machine parameters edited on the HMI and used by the machine logic (core 0 only):
 *   - sequence timers T01..T08 (Settings > Time);
 *   - per-axis motion settings the HMI sends with every move (Settings > Servo);
 *   - teach points per axis group (Manual > Teaching).
 *
 * Per-axis data is keyed by axis name, never by cable position (CLAUDE.md, topology rule).
 * Everything is stored through machine_store.h, so a save in OP waits until the bus leaves OP.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MACHINE_AXIS_NAME_LEN 8 /*!< Same as ECAT_TOPO_NAME_LEN */

/* ---- Sequence timers ---- */

#define MACHINE_TIMER_COUNT 8

typedef struct {
    const char *name;  /*!< Until the machine sequence exists: "Timer 1" ... */
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t def_ms;
} machine_timer_def_t;

/** Load the timers (defaults for those never saved). Call once after machine_store_init(). */
void machine_timers_init(void);
const machine_timer_def_t *machine_timer_def(int i);
/** Current value of timer `i` in ms (0 if `i` is out of range). */
uint32_t machine_timer_ms(int i);
/** Set all timers (clamped to their ranges) and save them. */
esp_err_t machine_timers_set(const uint32_t ms[MACHINE_TIMER_COUNT]);

/* ---- Axis settings of the HMI (not drive objects) ---- */

/** Unit the HMI shows an axis in. Only the display changes: drives and teach points keep pulses. */
typedef enum {
    MACHINE_UNIT_REV, /*!< Motor revolutions and rpm */
    MACHINE_UNIT_MM,  /*!< mm and mm/s (linear axis): dist_per_rev = mm per motor revolution (screw lead) */
    MACHINE_UNIT_DEG, /*!< deg and deg/s (rotary axis): dist_per_rev = degrees of the load per motor revolution */
    MACHINE_UNIT_COUNT,
} machine_unit_t;

typedef struct {
    float max_rpm;      /*!< Speed limit of every HMI move and jog, motor rpm; reference of the ramp times */
    float acc_ms;       /*!< Time from standstill to max_rpm (sets 0x6083, sent with every command) */
    float dec_ms;       /*!< Time from max_rpm to standstill (0x6084) */
    float jog_high_rpm; /*!< "High" jog speed, motor rpm; Teaching go-to speeds scale from it */
    float dist_per_rev; /*!< Load travel per motor revolution in `unit` (mm or deg); 0 = not set (shown in rev) */
    uint8_t unit;       /*!< machine_unit_t */
} machine_axis_cfg_t;

#define MACHINE_MAX_RPM_MAX  6000.0f
#define MACHINE_RAMP_MS_MIN  10.0f
#define MACHINE_RAMP_MS_MAX  10000.0f
#define MACHINE_DIST_MAX     10000.0f
#define MACHINE_DEG_PER_REV  360.0f /*!< Distance/Rev given when an axis is switched to deg (direct drive) */

void machine_axis_defaults(machine_axis_cfg_t *out);
/** Settings of axis `name`, or the defaults if none were saved. */
void machine_axis_get(const char *name, machine_axis_cfg_t *out);
/** Store the settings of axis `name` (values clamped). */
esp_err_t machine_axis_set(const char *name, const machine_axis_cfg_t *cfg);

/* ---- Teach points ---- */

#define MACHINE_GROUP_AXES     3
#define MACHINE_MAX_POINTS     32
#define MACHINE_POINT_NAME_LEN 16

typedef struct {
    char name[MACHINE_POINT_NAME_LEN];
    uint8_t speed_pct;                   /*!< Go-to speed in % of each axis' jog_high_rpm */
    int32_t pos[MACHINE_GROUP_AXES];     /*!< Drive position units (0x6064) per group axis */
} machine_point_t;

typedef struct {
    uint8_t n_axes;
    uint8_t n_points;
    char axis[MACHINE_GROUP_AXES][MACHINE_AXIS_NAME_LEN];
    machine_point_t pt[MACHINE_MAX_POINTS];
} machine_points_t;

/**
 * Load the points of the group made of axes `names` (1..MACHINE_GROUP_AXES). Positions are
 * matched by axis name; an axis the stored group did not have gets 0. With nothing stored the
 * group starts with one point "HOME" at 0.
 */
void machine_points_load(const char *const names[], int n_axes, machine_points_t *out);
esp_err_t machine_points_save(const machine_points_t *p);

#ifdef __cplusplus
}
#endif
