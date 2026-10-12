/*
 * Shared I2C bus 1 of the board (private to the board component). Call from core 0 only:
 * the bus interrupt is installed on the calling core.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/** Create the bus on first use; later calls return the same handle. */
esp_err_t board_i2c_bus(i2c_master_bus_handle_t *out);
