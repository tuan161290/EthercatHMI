/*
 * Serial console for the EtherCAT bring-up (topology Scan -> Accept -> Save).
 * Stands in for SETTING > ETHERCAT until the LVGL HMI exists.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    APP_CMD_SHOW,     /*!< Print the saved topology, the last scan and the comparison */
    APP_CMD_SCAN,     /*!< Rescan the bus (bus must not be in OP) */
    APP_CMD_ACCEPT,   /*!< Save the last scan as the expected topology */
    APP_CMD_RECHECK,  /*!< Rescan and compare; go to OP if it matches */
    APP_CMD_FORGET,   /*!< Erase the saved topology */
    APP_CMD_STOP,     /*!< Leave OP and hold in PRE-OP */
    APP_CMD_SELFTEST, /*!< Run the topology logic self-test */
} app_cmd_t;

/** Implemented by main.c: queue a command for the EtherCAT task. Safe to call from any task. */
bool app_ecat_request(app_cmd_t cmd);

/** Implemented by main.c: show (arg NULL) or store ("new"/"old") the LCD panel variant. */
bool app_panel_request(const char *arg);

/** Start the REPL on the primary console (task on core 0). */
esp_err_t app_console_start(void);
