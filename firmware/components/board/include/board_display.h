/*
 * 7" 1024x600 MIPI-DSI panel (JD9165 driver IC), GT911 touch and LEDC backlight.
 * Hardware facts and sources: docs/BOARD.md sections 3.2 (display), 3.3 (touch) and 5 (panel variants).
 *
 * Call everything from a task on core 0: the DSI/DMA2D and I2C interrupts are installed on the
 * calling core, and core 1 belongs to EtherCAT.
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

/**
 * The board ships with one of two glass panels behind the same JD9165. They need different
 * init tables and horizontal timing, and cannot be told apart at run time (docs/BOARD.md 5).
 */
typedef enum {
    BOARD_LCD_PANEL_NEW = 1, /*!< "V2" label on the back; factory firmware V3.7 */
    BOARD_LCD_PANEL_OLD = 2, /*!< HKC QD070AS01-1 (inferred); factory firmware V2.x */
} board_lcd_panel_t;

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
} board_lcd_t;

/** @return the panel selected in menuconfig (Board > LCD panel variant) */
board_lcd_panel_t board_lcd_panel_default(void);

/** @return "New", "Old" or "?" */
const char *board_lcd_panel_name(board_lcd_panel_t panel);

/**
 * Bring up the display for `panel`: backlight PWM (off), D-PHY power, DSI bus, JD9165 init.
 * The panel then shows its frame buffer; turn the backlight on with board_lcd_backlight_set().
 */
esp_err_t board_lcd_init(board_lcd_panel_t panel, board_lcd_t *out);

/** Backlight brightness 0..100 %. Requires board_lcd_init(). */
esp_err_t board_lcd_backlight_set(int percent);

/**
 * I2C bus 1 (shared with the ES8311 codec and the RTC) and the GT911 touch controller.
 * RST and INT are driven so the controller always comes up at address 0x5D.
 */
esp_err_t board_touch_init(esp_lcd_touch_handle_t *out);

#ifdef __cplusplus
}
#endif
