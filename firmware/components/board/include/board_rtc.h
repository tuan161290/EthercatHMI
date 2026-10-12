/*
 * RX8025T-UC real-time clock with a CR1220 backup cell (docs/BOARD.md: RTC), on I2C bus 1.
 * Its INT# pin is not wired, so only the time registers are used.
 *
 * Call from core 0 (the I2C interrupt is installed on the calling core). Not from the cyclic
 * EtherCAT task.
 */

#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach the RTC to the shared I2C bus and check that it answers. */
esp_err_t board_rtc_init(void);

/**
 * Read the date and time (UTC is stored; years 2000..2099).
 * `valid` is false when the RTC lost power (VLF flag) or holds an impossible date; the time
 * must then be set before it is trusted.
 */
esp_err_t board_rtc_read(struct tm *out, bool *valid);

/** Write the date and time (UTC, years 2000..2099) and clear the low-voltage flag. */
esp_err_t board_rtc_write(const struct tm *tm);

#ifdef __cplusplus
}
#endif
