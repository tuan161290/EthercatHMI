/*
 * LVGL 9 HMI: Windows 11 Fluent light theme (docs/HMI_THEME.md, mockup in CLAUDE.md).
 *
 * Runs on core 0 only (LVGL task, rendering, touch). The EtherCAT side talks to it through
 * hmi_publish_bus_status() (a seqlock snapshot); the HMI never calls ecx_*.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board_display.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HMI_BUS_STARTING = 0, /*!< Scanning or configuring */
    HMI_BUS_NO_LINK,      /*!< No Ethernet link to the first slave */
    HMI_BUS_HELD,         /*!< Held in PRE-OP: topology not accepted or different */
    HMI_BUS_OP,           /*!< Cyclic process data, no errors in the last period */
    HMI_BUS_OP_FAULT,     /*!< In OP with working-counter errors or a slave held after recovery */
} hmi_bus_state_t;

typedef struct {
    hmi_bus_state_t state;
    uint16_t slaves;    /*!< Slaves found by the last scan */
    uint32_t cycle_us;  /*!< Cycle time while in OP */
    /* Cycle timing while in OP, over the last monitor period (1 s); 0 outside OP */
    uint32_t jitter_us;  /*!< Worst start delay of a cycle after its deadline */
    uint32_t rtt_avg_us; /*!< Process-data frame round trip: average since OP ... */
    uint32_t rtt_max_us; /*!< ... and worst in the period */
    uint32_t overruns;   /*!< Cycles that missed their deadline since OP */
} hmi_bus_status_t;

/** Topology requests from Settings > EtherCAT (docs/ETHERCAT_PLAN.md section 3.6). */
typedef enum {
    HMI_BUS_REQ_SCAN,    /*!< Scan the bus (leaves OP first: the axes stop and switch off) */
    HMI_BUS_REQ_ACCEPT,  /*!< Save the last scan as the expected topology, then restart the bus */
    HMI_BUS_REQ_RECHECK, /*!< Scan, compare with the saved topology, go to OP if it matches */
} hmi_bus_req_t;

typedef struct {
    board_lcd_panel_t panel;
    int brightness;     /*!< Backlight 0..100 %, used until display settings are saved */
    /** Pass a topology request to the EtherCAT task; @return false if refused. NULL = none. */
    bool (*bus_request)(hmi_bus_req_t req);
} hmi_config_t;

/* ---- Display settings: backlight, colour balance and screen timeouts, kept in the machine store
 * ("display") ---- */

#define HMI_BRIGHTNESS_MIN       5    /*!< % : never fully dark, so the screen stays usable */
#define HMI_GAIN_MIN             20   /*!< % : lowest colour-balance gain per channel */
#define HMI_SCREEN_TIMEOUT_MAX_S 3600 /*!< s : longest dim / off timeout */

typedef struct {
    uint8_t brightness; /*!< Backlight, HMI_BRIGHTNESS_MIN..100 % */
    uint8_t red;        /*!< Colour-balance gains, HMI_GAIN_MIN..100 %; 100 leaves the channel as is */
    uint8_t green;
    uint8_t blue;
    uint16_t dim_s; /*!< Dim the backlight this long after the last touch, s; 0 = never */
    uint16_t off_s; /*!< Switch the backlight off this long after the last touch, s; 0 = never */
} hmi_display_t;

typedef enum {
    HMI_SCREEN_ON,
    HMI_SCREEN_DIM,
    HMI_SCREEN_OFF,
} hmi_screen_t;

/**
 * Bring up display, touch and LVGL, generate the background and build the UI.
 * Call once from a task on core 0 (app_main). Touch failure is logged, not fatal.
 */
esp_err_t hmi_start(const hmi_config_t *cfg);

/** Publish the EtherCAT status for the title bar. Single writer, any core; never blocks. */
void hmi_publish_bus_status(const hmi_bus_status_t *status);

/** Current display settings (what is on the screen). */
void hmi_display_get(hmi_display_t *out);

/** Factory display settings: brightness from hmi_config_t, all gains 100 %, timeouts from Kconfig. */
void hmi_display_defaults(hmi_display_t *out);

/**
 * Apply display settings at once (values are clamped) and light the screen. With `save` they are
 * also stored (machine_store.h; also while EtherCAT is in OP).
 * Any task on core 0; takes the LVGL lock. ESP_ERR_INVALID_STATE before hmi_start().
 */
esp_err_t hmi_display_set(const hmi_display_t *d, bool save);

/** @return true while a save waits in RAM (the store is full while EtherCAT is in OP) */
bool hmi_display_save_pending(void);

/** Screen state after the dim / off timeouts of hmi_display_t. */
hmi_screen_t hmi_screen_get(void);

/**
 * Light the screen and restart the dim / off countdown, as a touch does (nothing is pressed).
 * Any task on core 0; takes the LVGL lock. No effect before hmi_start().
 */
void hmi_screen_wake(void);

/**
 * Show a page by name, as if it was tapped: main, manual (= jog), teach, interp, io, alarms, settings,
 * time, servo, ethercat, system, network, info. For bench tests from the console. Any task on core 0.
 * @return false for an unknown name or before hmi_start()
 */
bool hmi_open_page(const char *name);

/** CPU load and LVGL frame rate over the last second (hmi_cpu.c). */
typedef struct {
    uint8_t load[2]; /*!< Busy share of core 0 and core 1, % (time outside the idle task) */
    uint16_t fps;    /*!< Frames LVGL rendered; 0 while nothing on the screen changes */
} hmi_cpu_t;

/** @return false until the first second after hmi_start() has been measured. Any task. */
bool hmi_cpu_get(hmi_cpu_t *out);

/** Print the last touches (point, down/up, text of the widget hit) on the console. */
void hmi_touchlog_print(void);

/** Print the screen as compressed text lines for tools/screenshot.py (console task). */
void hmi_screenshot_print(void);

#ifdef __cplusplus
}
#endif
