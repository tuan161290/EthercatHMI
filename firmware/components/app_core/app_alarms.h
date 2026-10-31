/*
 * Alarm supervisor: turns the EtherCAT, axis, IO and clock state into alarms of the Alarms
 * page (machine_alarm.h). Runs as a task on core 0 and polls the snapshots every 200 ms.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool link_up;
    bool in_op;
    bool holding;        /*!< Held in PRE-OP, waiting for Scan / Accept / Re-check */
    uint16_t n_blocked;  /*!< Slaves held after a failed identity check */
    uint32_t wkc_errors; /*!< Working-counter errors since OP started */
} app_bus_info_t;

/** Implemented by app_core.c. */
void app_bus_info(app_bus_info_t *out);

void app_alarms_start(void);
