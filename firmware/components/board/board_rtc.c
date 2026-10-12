/*
 * RX8025T driver (board_rtc.h).
 *
 * UNVERIFIED on the board: the RX8025T datasheet is not in the repo (vendor package
 * 4-Driver_IC_Data_Sheet has none, and no vendor demo uses the RTC). The register map below is
 * the one of the Epson RX-8025T application manual:
 *   7-bit address 0x32; 0x00 SEC, 0x01 MIN, 0x02 HOUR (24 h), 0x03 WEEK (one bit per day,
 *   bit 0 = Sunday), 0x04 DAY, 0x05 MONTH, 0x06 YEAR (00..99), all BCD; 0x0D extension,
 *   0x0E flags (bit 1 VLF: oscillation stopped / supply too low, bit 0 VDET), 0x0F control.
 * Reads and writes auto-increment the register address.
 */

#include <string.h>
#include "board_i2c.h"
#include "board_rtc.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_rtc";

#define RTC_ADDR       0x32
#define RTC_HZ         100000
#define RTC_TIMEOUT_MS 50
#define REG_SEC        0x00
#define REG_EXT        0x0D
#define REG_FLAG       0x0E
#define FLAG_VLF       0x02
#define FLAG_VDET      0x01

static i2c_master_dev_handle_t s_dev;

static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin2bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, RTC_TIMEOUT_MS);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[1 + 16];
    if (n > 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    tx[0] = reg;
    memcpy(tx + 1, buf, n);
    return i2c_master_transmit(s_dev, tx, n + 1, RTC_TIMEOUT_MS);
}

esp_err_t board_rtc_init(void)
{
    if (s_dev) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(board_i2c_bus(&bus), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, RTC_ADDR, RTC_TIMEOUT_MS), TAG, "no RTC at 0x%02x", RTC_ADDR);
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = RTC_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev, &s_dev);
}

esp_err_t board_rtc_read(struct tm *out, bool *valid)
{
    ESP_RETURN_ON_FALSE(s_dev && out && valid, ESP_ERR_INVALID_STATE, TAG, "not ready");
    uint8_t r[7], flag;
    ESP_RETURN_ON_ERROR(rd(REG_SEC, r, sizeof(r)), TAG, "read time");
    ESP_RETURN_ON_ERROR(rd(REG_FLAG, &flag, 1), TAG, "read flags");

    memset(out, 0, sizeof(*out));
    out->tm_sec = bcd2bin(r[0] & 0x7F);
    out->tm_min = bcd2bin(r[1] & 0x7F);
    out->tm_hour = bcd2bin(r[2] & 0x3F);
    out->tm_mday = bcd2bin(r[4] & 0x3F);
    out->tm_mon = bcd2bin(r[5] & 0x1F) - 1;
    out->tm_year = bcd2bin(r[6]) + 100;
    out->tm_isdst = 0;
    *valid = !(flag & FLAG_VLF) && out->tm_sec < 60 && out->tm_min < 60 && out->tm_hour < 24 &&
             out->tm_mday >= 1 && out->tm_mday <= 31 && out->tm_mon >= 0 && out->tm_mon < 12;
    return ESP_OK;
}

esp_err_t board_rtc_write(const struct tm *tm)
{
    ESP_RETURN_ON_FALSE(s_dev && tm, ESP_ERR_INVALID_STATE, TAG, "not ready");
    ESP_RETURN_ON_FALSE(tm->tm_year >= 100 && tm->tm_year < 200, ESP_ERR_INVALID_ARG, TAG, "year out of range");
    const uint8_t r[7] = {
        bin2bcd(tm->tm_sec), bin2bcd(tm->tm_min), bin2bcd(tm->tm_hour),
        (uint8_t)(1u << (tm->tm_wday % 7)),
        bin2bcd(tm->tm_mday), bin2bcd(tm->tm_mon + 1), bin2bcd(tm->tm_year - 100),
    };
    ESP_RETURN_ON_ERROR(wr(REG_SEC, r, sizeof(r)), TAG, "write time");
    /* Clear VLF/VDET and the alarm/timer flags; extension register: TEST bit must stay 0. */
    uint8_t ext;
    ESP_RETURN_ON_ERROR(rd(REG_EXT, &ext, 1), TAG, "read ext");
    ext &= 0x7F;
    const uint8_t ext_flag[2] = {ext, 0x00};
    ESP_RETURN_ON_ERROR(wr(REG_EXT, ext_flag, sizeof(ext_flag)), TAG, "clear flags");
    ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    return ESP_OK;
}
