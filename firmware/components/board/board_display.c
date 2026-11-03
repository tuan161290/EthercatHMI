/*
 * Display, backlight and touch bring-up for the JC1060P470C_I_W_Y.
 *
 * Follows the vendor BSP (docs/BOARD.md section 0: BSP_N.c = New panel, BSP_O.c = Old panel),
 * with two deliberate differences:
 *   - the panel variant is a run-time argument instead of two copies of the BSP;
 *   - the GT911 RST/INT lines (GPIO22/21) are driven, so its I2C address does not depend on
 *     the power-up state of INT (the BSP leaves them NC, docs/BOARD.md 3.3).
 */

#include <inttypes.h>
#include "board_display.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "board_lcd";

#define DSI_LANES               2      /* lanes 2/3 are NC on FPC1 (docs/BOARD.md 3.2) */
#define DSI_PHY_LDO_CHAN        3      /* LDO_VO3 feeds VDD_MIPI_DPHY (vendor include/bsp/display.h:69-70) */
#define DSI_PHY_LDO_MV          2500
#define DPI_CLOCK_MHZ           52     /* BSP_N.c:500, BSP_O.c:618 */
#define BACKLIGHT_LEDC_TIMER    LEDC_TIMER_1
#define BACKLIGHT_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BACKLIGHT_DUTY_MAX      1023   /* 10-bit, as in the BSP */
#define TOUCH_I2C_HZ            400000 /* BSP_N.c:663 (the Old BSP leaves the 100 kHz default) */

/* JD9165 init table for the New panel, copied from BSP_N.c:404-466. */
static const jd9165_lcd_init_cmd_t s_init_new[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x38}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x29}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x29}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x00, (uint8_t[]){0x05}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x02, (uint8_t[]){0x08}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x16}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x07, (uint8_t[]){0x00}, 1, 0},
    {0x08, (uint8_t[]){0x78}, 1, 0},
    {0x09, (uint8_t[]){0x00}, 1, 0},
    {0x0A, (uint8_t[]){0x04}, 1, 0},
    {0x0B, (uint8_t[]){0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09}, 11, 0},
    {0x0C, (uint8_t[]){0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08}, 11, 0},
    {0x0E, (uint8_t[]){0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F}, 11, 0},
    {0x10, (uint8_t[]){0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E}, 11, 0},
    {0x12, (uint8_t[]){0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x13, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x14, (uint8_t[]){0x00, 0x00, 0x41, 0x41}, 4, 0},
    {0x15, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0x85}, 1, 0},
    {0x19, (uint8_t[]){0x06, 0x09}, 2, 0},
    {0x1A, (uint8_t[]){0x05, 0x08}, 2, 0},
    {0x1B, (uint8_t[]){0x0A, 0x04}, 2, 0},
    {0x26, (uint8_t[]){0x00}, 1, 0},
    {0x27, (uint8_t[]){0x00}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x3F, 0x25, 0x27, 0x35, 0x1D, 0x1B, 0x1B, 0x1A, 0x18, 0x0A, 0x2A, 0x21, 0x19, 0x30}, 14, 0},
    {0x13, (uint8_t[]){0x3F, 0x26, 0x27, 0x35, 0x1E, 0x1C, 0x1C, 0x1A, 0x18, 0x0B, 0x2A, 0x21, 0x19, 0x30}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x05}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x05}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},   /* COLMOD: 16-bit RGB565 */
    {0x11, (uint8_t[]){0x00}, 1, 120}, /* sleep out, 120 ms */
    {0x29, (uint8_t[]){0x00}, 1, 20},  /* display on, 20 ms */
};

/* JD9165 init table for the Old panel, copied from BSP_O.c:520-583 (lines commented out there are
 * left out). It matches the HKC QD070AS01-1 DTSI (docs/BOARD.md 5). */
static const jd9165_lcd_init_cmd_t s_init_old[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x19}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x04}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x04}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x0A, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0x18, (uint8_t[]){0x99}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x12, (uint8_t[]){0x3E}, 1, 0},
    {0x13, (uint8_t[]){0x78}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x0C}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x0C}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},   /* COLMOD: 16-bit RGB565 */
    {0x11, (uint8_t[]){0x00}, 1, 120}, /* sleep out, 120 ms */
    {0x29, (uint8_t[]){0x00}, 1, 20},  /* display on, 20 ms */
};

typedef struct {
    const char *name;
    const jd9165_lcd_init_cmd_t *init;
    uint16_t init_count;
    uint32_t hbp;          /* the only timing difference between the panels */
    uint32_t backlight_hz; /* PWM frequency used by each BSP */
} panel_desc_t;

static const panel_desc_t s_panels[] = {
    [BOARD_LCD_PANEL_NEW] = {"New", s_init_new, sizeof(s_init_new) / sizeof(s_init_new[0]), 136, 20000}, /* BSP_N.c:511, 341 */
    [BOARD_LCD_PANEL_OLD] = {"Old", s_init_old, sizeof(s_init_old) / sizeof(s_init_old[0]), 160, 5000},  /* BSP_O.c:629, 446 */
};

static bool s_backlight_ready;

static const panel_desc_t *panel_desc(board_lcd_panel_t panel)
{
    return (panel == BOARD_LCD_PANEL_NEW || panel == BOARD_LCD_PANEL_OLD) ? &s_panels[panel] : NULL;
}

board_lcd_panel_t board_lcd_panel_default(void)
{
#if CONFIG_BOARD_LCD_PANEL_OLD
    return BOARD_LCD_PANEL_OLD;
#else
    return BOARD_LCD_PANEL_NEW;
#endif
}

const char *board_lcd_panel_name(board_lcd_panel_t panel)
{
    const panel_desc_t *d = panel_desc(panel);
    return d ? d->name : "?";
}

static esp_err_t backlight_init(uint32_t freq_hz)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 0, /* off until the first frame is drawn */
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t board_lcd_backlight_set(int percent)
{
    ESP_RETURN_ON_FALSE(s_backlight_ready, ESP_ERR_INVALID_STATE, TAG, "backlight not initialised");
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    uint32_t duty = (uint32_t)(BACKLIGHT_DUTY_MAX * percent / 100);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHANNEL, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHANNEL);
}

esp_err_t board_lcd_init(board_lcd_panel_t panel, board_lcd_t *out)
{
    const panel_desc_t *d = panel_desc(panel);
    ESP_RETURN_ON_FALSE(d && out, ESP_ERR_INVALID_ARG, TAG, "bad panel or output");
    esp_err_t ret = ESP_OK;
    esp_lcd_dsi_bus_handle_t dsi = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t lcd = NULL;

    ESP_LOGI(TAG, "%s panel, %d lanes at %d Mbps, DPI %d MHz, HBP %" PRIu32, d->name, DSI_LANES,
             CONFIG_BOARD_LCD_DSI_LANE_MBPS, DPI_CLOCK_MHZ, d->hbp);
    ESP_RETURN_ON_ERROR(backlight_init(d->backlight_hz), TAG, "backlight");

    /* D-PHY power: from "no power" to "shutdown" state before the DSI bus is created. */
    static esp_ldo_channel_handle_t phy_ldo;
    if (phy_ldo == NULL) {
        const esp_ldo_channel_config_t ldo = {.chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV};
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &phy_ldo), TAG, "D-PHY LDO");
    }

    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = CONFIG_BOARD_LCD_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &dsi), TAG, "DSI bus");

    const esp_lcd_dbi_io_config_t dbi = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi, &io), err, TAG, "DBI io");

    const esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,  /* frame buffer */
        .out_color_format = LCD_COLOR_FMT_RGB565, /* COLMOD 0x55 in both init tables */
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = 24,
            .hsync_back_porch = d->hbp,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 21,
            .vsync_front_porch = 12,
        },
    };
    const jd9165_vendor_config_t vendor = {
        .init_cmds = d->init,
        .init_cmds_size = d->init_count,
        .mipi_config = {.dsi_bus = dsi, .dpi_config = &dpi},
    };
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9165(io, &dev, &lcd), err, TAG, "JD9165 panel");
    /* The JD9165 driver hands back the DPI panel itself (esp_lcd_jd9165.c: esp_lcd_new_panel_dpi). */
    ESP_GOTO_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(lcd), err, TAG, "DPI DMA2D");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd), err, TAG, "panel reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd), err, TAG, "panel init");

    out->panel = lcd;
    out->io = io;
    return ESP_OK;

err:
    if (lcd) {
        esp_lcd_panel_del(lcd);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    if (dsi) {
        esp_lcd_del_dsi_bus(dsi);
    }
    return ret;
}

esp_err_t board_touch_init(esp_lcd_touch_handle_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "no output");
    i2c_master_bus_handle_t i2c;
    ESP_RETURN_ON_ERROR(board_i2c_bus(&i2c), TAG, "I2C bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c, &io_cfg, &io), TAG, "touch io");

    /* With RST and INT both given, the driver holds INT low during reset: address 0x5D. */
    static esp_lcd_touch_io_gt911_config_t gt911 = {.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS};
    const esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .driver_data = &gt911,
    };
    esp_err_t ret = esp_lcd_touch_new_i2c_gt911(io, &cfg, out);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(io);
    }
    return ret;
}
