/*
 * Wall clock (machine_clock.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "board_rtc.h"
#include "esp_log.h"
#include "machine_clock.h"
#include "machine_store.h"

static const char *TAG = "clock";

#define CLOCK_VALID_EPOCH 1700000000 /* 2023-11: anything earlier was never set */
#define TZ_VERSION        1
#define TZ_DEFAULT        (7 * 60) /* Vietnam, UTC+07:00 */

typedef struct {
    uint16_t version;
    int16_t minutes;
} tz_blob_t;

static int s_tz = TZ_DEFAULT;
static bool s_rtc;

static void apply_tz(int minutes)
{
    /* POSIX TZ counts west positive: UTC+07:00 is "UTC-07:00". */
    char tz[16];
    const int a = abs(minutes);
    snprintf(tz, sizeof(tz), "UTC%c%02d:%02d", minutes >= 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
}

void machine_clock_tz_text(int minutes, char *out, size_t len)
{
    const int a = abs(minutes);
    snprintf(out, len, "UTC%c%02d:%02d", minutes >= 0 ? '+' : '-', a / 60, a % 60);
}

esp_err_t machine_clock_init(void)
{
    tz_blob_t b;
    if (machine_store_load("tz", &b, sizeof(b)) == ESP_OK && b.version == TZ_VERSION &&
        b.minutes >= MACHINE_TZ_MIN_MINUTES && b.minutes <= MACHINE_TZ_MAX_MINUTES) {
        s_tz = b.minutes;
    }
    apply_tz(s_tz);

    esp_err_t err = board_rtc_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC not found (%s): the clock is not set at start-up", esp_err_to_name(err));
        return err;
    }
    s_rtc = true;
    struct tm utc;
    bool valid = false;
    err = board_rtc_read(&utc, &valid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading the RTC failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!valid) {
        ESP_LOGW(TAG, "RTC lost its time (backup cell?): set the clock in Settings > Time or with 'clock'");
        return ESP_OK;
    }
    /* The RTC holds UTC: timegm() is not in newlib, so convert with TZ temporarily at UTC. */
    setenv("TZ", "UTC0", 1);
    tzset();
    const time_t t = mktime(&utc);
    apply_tz(s_tz);
    const struct timeval tv = {.tv_sec = t};
    settimeofday(&tv, NULL);
    struct tm local;
    localtime_r(&t, &local);
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "clock set from the RTC: %s (local)", text);
    return ESP_OK;
}

bool machine_clock_valid(void)
{
    return time(NULL) >= CLOCK_VALID_EPOCH;
}

bool machine_clock_rtc_present(void)
{
    return s_rtc;
}

esp_err_t machine_clock_set_local(const struct tm *local)
{
    struct tm l = *local;
    l.tm_isdst = 0;
    const time_t t = mktime(&l);
    if (t < CLOCK_VALID_EPOCH) {
        return ESP_ERR_INVALID_ARG;
    }
    const struct timeval tv = {.tv_sec = t};
    settimeofday(&tv, NULL);
    if (!s_rtc) {
        ESP_LOGW(TAG, "clock set; no RTC, so it is lost at the next reset");
        return ESP_ERR_NOT_FOUND;
    }
    struct tm utc;
    gmtime_r(&t, &utc);
    return board_rtc_write(&utc);
}

esp_err_t machine_clock_save_to_rtc(void)
{
    const time_t t = time(NULL);
    if (t < CLOCK_VALID_EPOCH) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_rtc) {
        return ESP_ERR_NOT_FOUND;
    }
    struct tm utc;
    gmtime_r(&t, &utc);
    return board_rtc_write(&utc);
}

int machine_clock_tz_minutes(void)
{
    return s_tz;
}

esp_err_t machine_clock_set_tz(int minutes)
{
    if (minutes < MACHINE_TZ_MIN_MINUTES || minutes > MACHINE_TZ_MAX_MINUTES || minutes % 15 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tz = minutes;
    apply_tz(minutes);
    const tz_blob_t b = {.version = TZ_VERSION, .minutes = (int16_t)minutes};
    return machine_store_save("tz", &b, sizeof(b));
}
