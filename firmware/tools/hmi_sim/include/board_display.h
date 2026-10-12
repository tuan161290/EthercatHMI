/* HMI simulator: board_display.h without the ESP LCD driver types. */
#pragma once
#include "esp_err.h"
#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600
typedef enum {
    BOARD_LCD_PANEL_NEW = 1,
    BOARD_LCD_PANEL_OLD = 2,
} board_lcd_panel_t;
typedef void *esp_lcd_panel_handle_t;
const char *board_lcd_panel_name(board_lcd_panel_t panel);
esp_err_t board_lcd_backlight_set(int percent);
