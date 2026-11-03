/*
 * Application core shared by every app in firmware/apps: machine data (machine_store, timers,
 * clock, alarms), the LVGL HMI, the EtherCAT master (topology Scan -> Accept -> Save, OP with
 * cyclic process data on core 1, slave recovery, axes, IO and SDO jobs), the alarm supervisor,
 * Wi-Fi with internet time, and the serial console.
 *
 * The General App (apps/general) is this and nothing else. A machine app starts as a copy of it
 * (docs/SETUP.md) and adds its own code after app_core_start().
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start everything, in the order internal RAM needs (the HMI's draw buffer first, then the
 * EtherCAT tasks). Call once from app_main(); returns when the tasks run.
 */
void app_core_start(void);

#ifdef __cplusplus
}
#endif
