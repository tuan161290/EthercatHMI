/*
 * I2C bus 1, shared by the GT911 touch controller (0x5D), the ES8311 codec (0x18) and the
 * RX8025T RTC (docs/BOARD.md pin table, GPIO7/8). The master driver serialises transactions
 * of different devices on one bus, so the touch task and the RTC can use it at the same time.
 */

#include "board_i2c.h"
#include "board_pins.h"
#include "esp_check.h"

static const char *TAG = "board_i2c";

#define BOARD_I2C_PORT I2C_NUM_1

static i2c_master_bus_handle_t s_bus;

esp_err_t board_i2c_bus(i2c_master_bus_handle_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "no output");
    if (s_bus == NULL) {
        const i2c_master_bus_config_t bus = {
            .i2c_port = BOARD_I2C_PORT,
            .sda_io_num = BOARD_I2C_SDA_GPIO,
            .scl_io_num = BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = false, /* R65/R71 5k1 on the board */
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "I2C bus");
    }
    *out = s_bus;
    return ESP_OK;
}
