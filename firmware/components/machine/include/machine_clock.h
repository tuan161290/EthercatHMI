/*
 * Wall clock: the RX8025T RTC (board_rtc.h) keeps UTC, the system clock follows it, and the
 * time zone (UTC offset, stored in machine_store) turns it into local time for the HMI.
 * With Wi-Fi, NTP sets the system clock (components/net), which then calls
 * machine_clock_save_to_rtc().
 *
 * Core 0 only.
 */

#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MACHINE_TZ_MIN_MINUTES (-12 * 60)
#define MACHINE_TZ_MAX_MINUTES (14 * 60)

/** Apply the stored time zone and set the system clock from the RTC. Call after machine_store_init(). */
esp_err_t machine_clock_init(void);

/** @return true once the clock holds a real date (RTC valid at start-up, or set since) */
bool machine_clock_valid(void);

/** @return true if the RTC answered at start-up */
bool machine_clock_rtc_present(void);

/** Set the clock from a local date and time: system clock and RTC. */
esp_err_t machine_clock_set_local(const struct tm *local);

/**
 * Copy the system clock, set by someone else (NTP), into the RTC.
 * @return ESP_ERR_INVALID_STATE if the clock is not set, ESP_ERR_NOT_FOUND without an RTC
 */
esp_err_t machine_clock_save_to_rtc(void);

/** UTC offset in minutes (east positive), e.g. 420 for UTC+07:00. */
int machine_clock_tz_minutes(void);

/** Change and store the UTC offset (MACHINE_TZ_MIN_MINUTES..MAX, multiples of 15 min). */
esp_err_t machine_clock_set_tz(int minutes);

/** "UTC+07:00" */
void machine_clock_tz_text(int minutes, char *out, size_t len);

#ifdef __cplusplus
}
#endif
