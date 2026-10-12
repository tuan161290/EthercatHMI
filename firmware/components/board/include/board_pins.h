/*
 * Guition JC1060P470C_I_W_Y pin map (ESP32-P4 in module U3 "JC-ESP32P4-M3").
 *
 * Every GPIO number used by the firmware lives here. Sources:
 *   SCH = vendor/JC1060/5-Schematic/JC1060P470C_I_W_Y-V1.0.pdf p.1
 *   docs/BOARD.md section 2 has the full reconciled table.
 */

#pragma once

/* ---- Ethernet: IP101GR-class PHY (U4, inferred from pin-out), RMII ---- */
/* SCH net-label box next to U3; identical to ETH_ESP32_EMAC_DEFAULT_CONFIG() for P4 in IDF 5.5.4. */
#define BOARD_ETH_MDC_GPIO        31
#define BOARD_ETH_MDIO_GPIO       52   /* R41 1.5k pull-up */
#define BOARD_ETH_PHY_RST_GPIO    51   /* no RC on carrier; IDF default (5) is wrong for this board */
#define BOARD_ETH_PHY_ADDR        1    /* PHY_AD0 pulled up by R47 */
#define BOARD_ETH_REF_CLK_GPIO    50   /* 50 MHz from PHY 50M_CLKO (25 MHz crystal) -> EMAC_CLK_EXT_IN */
#define BOARD_ETH_TX_EN_GPIO      49
#define BOARD_ETH_TXD0_GPIO       34
#define BOARD_ETH_TXD1_GPIO       35   /* also the BOOT button (SW1): do not press it at runtime */
#define BOARD_ETH_CRS_DV_GPIO     28
#define BOARD_ETH_RXD0_GPIO       29
#define BOARD_ETH_RXD1_GPIO       30

/* ---- Display: 7" 1024x600 MIPI-DSI panel, JD9165 driver IC, on FPC1 (docs/BOARD.md 3.2) ---- */
/* The DSI data/clock lanes are dedicated pads, not GPIOs. */
#define BOARD_LCD_RST_GPIO        0    /* FPC1 pin 4 (SCH DSI_LCD; BSP-H:91). Vendor Arduino/video demos use 5: wrong */
#define BOARD_LCD_BACKLIGHT_GPIO  23   /* MP3202 boost EN, LEDC PWM (SCH; BSP-H:90) */

/* ---- I2C bus: GT911 touch (0x5D), ES8311 codec (0x18), RX8025T RTC ---- */
#define BOARD_I2C_SDA_GPIO        7    /* BSP-H:70, FPC3 pin 5 */
#define BOARD_I2C_SCL_GPIO        8    /* BSP-H:69, FPC3 pin 4 */
#define BOARD_TOUCH_INT_GPIO      21   /* FPC3 pin 2 (SCH Capacitive touch); the vendor BSP leaves it NC */
#define BOARD_TOUCH_RST_GPIO      22   /* FPC3 pin 3 (SCH Capacitive touch); the vendor BSP leaves it NC */

/* ---- Free header pins (JP1) usable as scope/debug outputs ---- */
#define BOARD_DEBUG_GPIO_A        32
#define BOARD_DEBUG_GPIO_B        33
