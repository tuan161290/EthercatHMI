/*
 * Axis helpers of the HMI: display scale and commands with the settings of Settings > Servo
 * (machine_params.h).
 *
 * Display units: the axis' Unit of Settings > Servo, mm (mm/s) or deg (deg/s), once its
 * Distance/Rev (travel of the load per motor revolution) is set; else motor revolutions and rpm. Drive units per motor revolution (Pulse/Rev) come from
 * the drive's factor group (ecat_axis_status_t.units_per_rev).
 * Speeds are kept in motor rpm everywhere else (settings, presets) and converted for display.
 */

#include "ecat_axis.h"
#include "hmi_internal.h"
#include "machine_params.h"

#define UNITS_MAX 2147483000.0

uint32_t hmi_upr(uint32_t units_per_rev)
{
    return units_per_rev ? units_per_rev : HMI_ASSUMED_UNITS_PER_REV;
}

uint32_t hmi_rpm_to_units(uint32_t upr, double rpm)
{
    const double v = rpm / 60.0 * (double)hmi_upr(upr);
    return v < 1.0 ? 1u : v > 2.0e9 ? 2000000000u : (uint32_t)(v + 0.5);
}

const char *hmi_unit_name(uint8_t unit)
{
    return unit == MACHINE_UNIT_MM ? "mm" : unit == MACHINE_UNIT_DEG ? "deg" : "rev";
}

void hmi_scale_of(const machine_axis_cfg_t *cfg, uint32_t units_per_rev, hmi_scale_t *out)
{
    const double upr = (double)hmi_upr(units_per_rev);
    if (cfg->unit != MACHINE_UNIT_REV && cfg->dist_per_rev > 0.0f) {
        out->units_per_pos = upr / cfg->dist_per_rev;
        out->rpm_per_speed = 60.0 / cfg->dist_per_rev;
        out->pos = hmi_unit_name(cfg->unit);
        out->speed = cfg->unit == MACHINE_UNIT_MM ? "mm/s" : "deg/s";
        out->speed_decimals = 1;
    } else {
        out->units_per_pos = upr;
        out->rpm_per_speed = 1.0;
        out->pos = "rev";
        out->speed = "rpm";
        out->speed_decimals = 0;
    }
}

void hmi_axis_scale(const char *name, uint32_t units_per_rev, hmi_scale_t *out)
{
    machine_axis_cfg_t cfg;
    machine_axis_get(name, &cfg);
    hmi_scale_of(&cfg, units_per_rev, out);
}

double hmi_to_pos(const hmi_scale_t *s, int32_t units)
{
    return (double)units / s->units_per_pos;
}

int32_t hmi_from_pos(const hmi_scale_t *s, double pos)
{
    double v = pos * s->units_per_pos;
    v = v > UNITS_MAX ? UNITS_MAX : v < -UNITS_MAX ? -UNITS_MAX : v;
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}

double hmi_rpm_to_speed(const hmi_scale_t *s, double rpm)
{
    return rpm / s->rpm_per_speed;
}

double hmi_speed_to_rpm(const hmi_scale_t *s, double speed)
{
    return speed * s->rpm_per_speed;
}

double hmi_pos_limit(const hmi_scale_t *s)
{
    return UNITS_MAX / s->units_per_pos;
}

bool hmi_axis_cmd(int index, const char *name, uint32_t upr, uint8_t code, int8_t dir, int32_t position, double rpm)
{
    machine_axis_cfg_t cfg;
    machine_axis_get(name, &cfg);
    if (rpm > cfg.max_rpm) {
        rpm = cfg.max_rpm; /* Max speed of Settings > Servo limits every HMI move */
    }
    /* Acc/Dec time: from standstill to Max speed, so the ramp is the same whatever the speed. */
    const double max_units = (double)hmi_rpm_to_units(upr, cfg.max_rpm);
    const double acc = max_units * 1000.0 / cfg.acc_ms;
    const double dec = max_units * 1000.0 / cfg.dec_ms;
    const ecat_axis_cmd_t c = {
        .axis = (uint8_t)index,
        .code = code,
        .dir = dir,
        .position = position,
        .velocity = rpm > 0 ? hmi_rpm_to_units(upr, rpm) : 0,
        .accel = acc < 1.0 ? 1u : acc > 2.0e9 ? 2000000000u : (uint32_t)acc,
        .decel = dec < 1.0 ? 1u : dec > 2.0e9 ? 2000000000u : (uint32_t)dec,
    };
    return ecat_axis_command(&c);
}

bool hmi_axis_set_units_per_rev(int index, uint32_t units_per_rev)
{
    const ecat_axis_cmd_t c = {.axis = (uint8_t)index, .code = ECAT_AXIS_SET_SCALE, .velocity = units_per_rev};
    return ecat_axis_command(&c);
}
