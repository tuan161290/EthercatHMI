/*
 * Shared by the HMI sources. Every call here needs the LVGL lock (or runs in the LVGL task).
 */

#pragma once

#include <stdbool.h>
#include <time.h>
#include "hmi.h"
#include "lvgl.h"
#include "machine_params.h"
#include "sdkconfig.h"

/* ---- Colour tokens: docs/HMI_THEME.md section 2 ---- */
#define HMI_TEXT          0x1B1B1B
#define HMI_TEXT_2        0x5D5D5D /* captions, units, secondary text */
#define HMI_TEXT_OFF      0xA0A0A0 /* disabled */
#define HMI_ACCENT        CONFIG_HMI_ACCENT_COLOR
#define HMI_SUCCESS       0x0F7B0F
#define HMI_SUCCESS_BG    0xDFF6DD
#define HMI_CAUTION       0x9D5D00
#define HMI_CAUTION_BG    0xFFF4CE
#define HMI_CRITICAL      0xC42B1C
#define HMI_CRITICAL_BG   0xFDE7E9
#define HMI_RING          0x8A8A8A /* "off" lamps and dots */
#define HMI_WHITE         0xFFFFFF
/* Three layers, as in Windows 11: solid grey background, white cards, controls (docs/HMI_THEME.md 2). */
#define HMI_BG            (0x010101u * ((CONFIG_HMI_BG_LEVEL * 255 + 50) / 100)) /* 80 %: #CCCCCC */
#define HMI_CONTROL       0xFBFBFB /* control fill, one step off the white card */
#define HMI_STROKE_OPA    15       /* black at 6 %: card and control strokes, dividers */
#define HMI_ELEV_OPA      41       /* black at 16 %: darker bottom edge of a control */
#define HMI_FIELD_OPA     115      /* black at 45 %: bottom edge of a text field */
#define HMI_TINT_OPA      10       /* black at 4 %: segmented-control track, pressed fill */

#define HMI_C(hex) lv_color_hex(hex)

/* ---- Geometry of the shell (docs/HMI_THEME.md section 1) ---- */
#define HMI_TITLE_H   56
#define HMI_RAIL_W    96

/* ---- Type ramp and touch sizes ----
 * For the 7" 1024x600 panel (about 170 ppi) read at arm's length at a machine, operated by finger
 * (user, 2026-10-11: text too small, controls not made for touch; was 14 px body, 36-44 px
 * controls). Use these instead of literal sizes. */
#define HMI_FS_TINY   13  /* badge counts */
#define HMI_FS_SMALL  15  /* captions, units, tags, table headers */
#define HMI_FS_BODY   18  /* labels, values, buttons, list rows */
#define HMI_FS_LARGE  20  /* emphasised values, card titles */
#define HMI_FS_HEAD   24  /* dialog titles, large values */
#define HMI_TITLE_PX  28  /* page titles */
#define HMI_FS_BIG    34  /* KPI values */
#define HMI_FS_HUGE   44  /* the clock on Settings > Time */
#define HMI_TOUCH_H   52  /* buttons, fields, segmented controls: about 8 mm */
#define HMI_ROW_H     56  /* list rows */
#define HMI_ICON_BTN  20  /* icon inside a button */

/* ---- Fonts: Open Sans 400/600 rendered by TinyTTF ---- */
typedef enum {
    HMI_REGULAR,
    HMI_SEMIBOLD,
} hmi_weight_t;

/** @return Open Sans at `px` (created on first use and kept) */
const lv_font_t *hmi_font(int px, hmi_weight_t weight);

/* ---- Shared styles (hmi_theme.c) ---- */
extern lv_style_t hmi_st_card;        /* white, 1 px stroke, radius 8 */
extern lv_style_t hmi_st_control;     /* white button / combo, radius 4 */
extern lv_style_t hmi_st_pressed;     /* LV_STATE_PRESSED for white controls */
extern lv_style_t hmi_st_pressed_dim; /* LV_STATE_PRESSED for filled buttons: 20 % shade */

void hmi_theme_init(void);

/** Transparent, non-clickable, non-scrolling container; `flow` < 0 means no flex layout. */
lv_obj_t *hmi_box(lv_obj_t *parent, int flow, int32_t gap);
/**
 * Make `obj` (an hmi_box) a vertical scroll area. It is made clickable too: LVGL starts a drag at
 * the clickable object under the finger and looks for a scrollable one from there upwards, so a
 * list of plain rows inside a non-clickable box never scrolled (IO monitor inputs, 2026-10-11).
 */
void hmi_scroll_area(lv_obj_t *obj);

/** Label in Open Sans; `color` is 0xRRGGBB. */
lv_obj_t *hmi_label(lv_obj_t *parent, const char *text, int px, hmi_weight_t weight, uint32_t color);

/** Card (hmi_st_card) with flex column layout. */
lv_obj_t *hmi_card(lv_obj_t *parent, int32_t pad_ver, int32_t pad_hor, int32_t gap);

/** Clickable white control with flex row layout, centred content. */
lv_obj_t *hmi_control(lv_obj_t *parent, int32_t w, int32_t h);

/**
 * Windows 11 elevation: a 1 px darker edge along the bottom of `obj` (black at `opa`; 0 removes
 * it). Drawn inside the object, so a parent never clips it; left out while pressed and while the
 * object's fill is transparent (unselected segments).
 */
void hmi_elevation(lv_obj_t *obj, lv_opa_t opa);

typedef enum {
    HMI_FIELD_READONLY, /* plain value with a faint outline */
    HMI_FIELD_EDIT,     /* text field: control fill, stroke, dark bottom edge */
    HMI_FIELD_CHANGED,  /* edited but not written or saved: caution colours */
} hmi_field_t;

/** Value box of a settings list (Servo, Time): `value` is its label. */
void hmi_field_style(lv_obj_t *box, lv_obj_t *value, hmi_field_t state);

/** Clickable filled button (Start, Stop, accent actions). */
lv_obj_t *hmi_filled_button(lv_obj_t *parent, int32_t h, uint32_t bg, int32_t radius);

/** Fill an empty row so the next siblings sit at the far end. */
void hmi_spacer(lv_obj_t *parent);

/* ---- Widgets of the mockup (hmi_widgets.c) ---- */

typedef void (*hmi_seg_cb_t)(int index, void *user);

/**
 * Segmented control: tinted track, the selected segment white with accent text. `per_row` > 0
 * wraps the segments into rows of that many (axis pickers). `cb` runs on a tap (may be NULL).
 */
lv_obj_t *hmi_segmented(lv_obj_t *parent, const char *const *labels, int n, int per_row, int32_t h, int selected,
                        hmi_seg_cb_t cb, void *user);
void hmi_segmented_select(lv_obj_t *seg, int index);
/** Cancel a press whose finger slides off the control (no click on release), as on Windows. */
void hmi_touch_slide_cancel(lv_indev_t *indev);

/** A duration for display: "37 µs" up to 100 µs, above that in ms ("0.25 ms", "12.5 ms"). */
void hmi_fmt_us(uint32_t us, char *out, size_t len);

/** Change the text of one segment. */
void hmi_segmented_set_text(lv_obj_t *seg, int index, const char *text);
/** Dimmed segments are shown but cannot be selected. */
void hmi_segmented_set_enabled(lv_obj_t *seg, bool enabled);

/** Filter pill of a page header (40 px, round); `selected` uses the accent. */
lv_obj_t *hmi_pill(lv_obj_t *parent, const char *text, bool selected, bool enabled);

/** Small rounded tag (24 px) with optional dot; recolour it with hmi_tag_set(). */
lv_obj_t *hmi_tag(lv_obj_t *parent, bool dot);
void hmi_tag_set(lv_obj_t *tag, const char *text, uint32_t bg, uint32_t fg);

/** 12 px status lamp: filled when on, an outline ring when off. */
lv_obj_t *hmi_lamp(lv_obj_t *parent);
void hmi_lamp_set(lv_obj_t *lamp, bool on, uint32_t color);

/** Card title: 14 px semibold. */
lv_obj_t *hmi_card_title(lv_obj_t *card, const char *text);

/** Modal confirmation on the top layer. `cb` gets true for the OK button. */
typedef void (*hmi_confirm_cb_t)(bool ok, void *user);
void hmi_confirm(const char *title, const char *body, const char *ok_text, bool danger, hmi_confirm_cb_t cb,
                 void *user);

/** Numeric keypad on the top layer; `cb` runs with the value when OK is pressed and it is in range. */
typedef void (*hmi_keypad_cb_t)(double value, void *user);
void hmi_keypad(const char *title, const char *unit, double current, double min, double max, int decimals,
                hmi_keypad_cb_t cb, void *user);

/** One choice out of a list (`options` separated by newlines), picked on a roller; `cb` gets the index. */
typedef void (*hmi_choice_cb_t)(int index, void *user);
void hmi_choice(const char *title, const char *options, int selected, hmi_choice_cb_t cb, void *user);

/**
 * Drop-down list (combo box) of `options` separated by newlines, `w` wide and HMI_TOUCH_H high,
 * whose list opens under it with touch-sized rows. `cb` gets the index picked (and `user`).
 */
typedef void (*hmi_dropdown_cb_t)(int index, void *user);
lv_obj_t *hmi_dropdown(lv_obj_t *parent, int32_t w, const char *options, int selected, hmi_dropdown_cb_t cb,
                       void *user);
void hmi_dropdown_set_enabled(lv_obj_t *dd, bool enabled);

/** Date and time picker (rollers as in Windows, years 2024..2099); `cb` gets the local time picked. */
typedef void (*hmi_datetime_cb_t)(const struct tm *local, void *user);
void hmi_datetime(const char *title, const struct tm *initial, hmi_datetime_cb_t cb, void *user);

/**
 * Text entry with an on-screen keyboard (printable ASCII) on the top layer. `password` hides the
 * text and adds a Show button. OK is refused below `min_len` characters. `cb` runs with the text
 * on OK and with NULL on Cancel.
 */
typedef void (*hmi_text_cb_t)(const char *text, void *user);
void hmi_text_input(const char *title, const char *hint, const char *initial, bool password, int min_len,
                    int max_len, hmi_text_cb_t cb, void *user);

/** Toggle switch of the mockup (40 x 20 track); the caller makes its row clickable. */
lv_obj_t *hmi_switch(lv_obj_t *parent, bool on);
void hmi_switch_set(lv_obj_t *sw, bool on);

/** Caution tag with a dot ("Unsaved changes"), created hidden; show it with hmi_show(). */
lv_obj_t *hmi_unsaved_tag(lv_obj_t *parent, const char *text);

/** Show or hide an object. */
void hmi_show(lv_obj_t *obj, bool show);

/** Set the text of a label only if it differs (no needless redraw). */
void hmi_set_text(lv_obj_t *label, const char *text);

/** 1 px divider (black 6 %) on the top edge of `obj`. */
void hmi_divider_top(lv_obj_t *obj);

/* ---- Icons: outline glyphs drawn with the LVGL draw API (hmi_icon.c) ---- */
typedef enum {
    HMI_ICON_APP,           /* four rounded squares (title-bar tile) */
    HMI_ICON_HOME,
    HMI_ICON_MOVE,          /* four-way arrows: Manual */
    HMI_ICON_CHIP,          /* IO monitor */
    HMI_ICON_BELL,          /* Alarms */
    HMI_ICON_GEAR,          /* Settings */
    HMI_ICON_SYNC,          /* Auto/Manual mode chip */
    HMI_ICON_CHEVRON_DOWN,
    HMI_ICON_CHEVRON_RIGHT,
    HMI_ICON_PLAY,          /* filled */
    HMI_ICON_STOP,          /* filled */
    HMI_ICON_RESET,
    HMI_ICON_ERROR,         /* filled red disc with a white cross; colour ignored */
    HMI_ICON_INFO,          /* outline circle with "i" */
    /* Settings list (mockup Setting.dc.html) */
    HMI_ICON_CLOCK,         /* Time setting */
    HMI_ICON_GAUGE,         /* Servo setting */
    HMI_ICON_DOCUMENT,      /* Recipe */
    HMI_ICON_USER,          /* User & password */
    HMI_ICON_WRENCH,        /* Maintenance */
    HMI_ICON_NETWORK,       /* EtherCAT */
    HMI_ICON_MONITOR,       /* System */
    HMI_ICON_DOWNLOAD,      /* Backup / restore; Read from drive */
    HMI_ICON_UPLOAD,        /* Write to drive */
    HMI_ICON_SAVE,
    HMI_ICON_PLUS,
    HMI_ICON_WARNING,       /* filled caution triangle with a white "!"; colour ignored */
    HMI_ICON_WIFI,          /* Network */
    HMI_ICON_CHECK,         /* Acknowledge */
} hmi_icon_t;

lv_obj_t *hmi_icon(lv_obj_t *parent, hmi_icon_t icon, int32_t size, uint32_t color);
void hmi_icon_set_color(lv_obj_t *icon, uint32_t color);


/* ---- Shell (title bar, navigation rail, page area) and pages ---- */
typedef enum {
    HMI_PAGE_MAIN,
    HMI_PAGE_MANUAL,
    HMI_PAGE_IO,
    HMI_PAGE_ALARMS,
    HMI_PAGE_SETTINGS,
    HMI_PAGE_COUNT,
} hmi_page_t;

void hmi_shell_create(void);
void hmi_shell_show(hmi_page_t page);

/** Machine mode/state shown in the title-bar chips. */
void hmi_shell_set_machine(bool auto_mode, bool running);
bool hmi_shell_auto_mode(void);
bool hmi_shell_running(void);

/** Page header row: title, then optional caption; returns the row so pages can add actions. */
lv_obj_t *hmi_page_header(lv_obj_t *page, const char *title, const char *caption, uint32_t caption_color);

void hmi_page_main_create(lv_obj_t *page);
void hmi_page_manual_create(lv_obj_t *page);
void hmi_page_io_create(lv_obj_t *page);
void hmi_page_alarms_create(lv_obj_t *page);
void hmi_page_settings_create(lv_obj_t *page);

/* ---- Manual: Jog (hmi_page_manual.c), Teaching (hmi_page_teach.c), Interpolation (hmi_page_interp.c) ---- */
typedef enum {
    HMI_MANUAL_JOG,
    HMI_MANUAL_TEACH,
    HMI_MANUAL_INTERP,
} hmi_manual_view_t;

/** Show a view of the Manual page; asynchronous, after the calling event. */
void hmi_manual_open(hmi_manual_view_t view);
/** Manual page header: title and the Jog / Teaching / Interpolation / Actuators / Homing pills; returns the row. */
lv_obj_t *hmi_manual_header(lv_obj_t *root, hmi_manual_view_t view);
/** Teaching view inside the Manual page root (hmi_page_teach.c). */
void hmi_teach_create(lv_obj_t *root);
/** Two-axis interpolation test inside the Manual page root (hmi_page_interp.c). */
void hmi_interp_create(lv_obj_t *root);
/** Step size (displayed position unit) and speed (motor rpm) chosen on the Jog view; Teaching jogs with them. */
double hmi_manual_step(void);
double hmi_manual_speed_rpm(const char *axis_name);

/* ---- Settings sub-pages ---- */
typedef enum {
    HMI_SET_TIME,
    HMI_SET_SERVO,
    HMI_SET_RECIPE,
    HMI_SET_USERS,
    HMI_SET_MAINTENANCE,
    HMI_SET_ETHERCAT,
    HMI_SET_NETWORK,
    HMI_SET_SYSTEM,
    HMI_SET_INFO,
    HMI_SET_BACKUP,
    HMI_SET_COUNT,
} hmi_settings_sub_t;

/** Open a Settings sub-page from another page; asynchronous, after the calling event. */
void hmi_settings_open(hmi_settings_sub_t sub);
static inline void hmi_settings_open_ethercat(void)
{
    hmi_settings_open(HMI_SET_ETHERCAT);
}

/** Settings > EtherCAT content below the breadcrumb row `head` (hmi_page_ethercat.c). */
void hmi_ethercat_create(lv_obj_t *root, lv_obj_t *head);
/** Settings > Time (hmi_page_time.c). */
void hmi_time_create(lv_obj_t *root, lv_obj_t *head);
/** Settings > Servo (hmi_page_servo.c). */
void hmi_servo_create(lv_obj_t *root, lv_obj_t *head);

/** Settings > Network: Wi-Fi and internet time (hmi_page_network.c). */
void hmi_network_create(lv_obj_t *root, lv_obj_t *head);

/** Settings > System info: memory, firmware, chip, uptime (hmi_page_info.c). */
void hmi_info_create(lv_obj_t *root, lv_obj_t *head);

/** Topology request to the EtherCAT task (hmi_config_t.bus_request). @return false if refused */
bool hmi_bus_request(hmi_bus_req_t req);

/** "Not in the firmware yet" card of the placeholder pages. */
void hmi_todo_card(lv_obj_t *page);

/** Card with an info icon, a bold title, a text and an optional button (`button` NULL = none). */
lv_obj_t *hmi_info_card(lv_obj_t *parent, const char *title, const char *text, const char *button,
                        lv_event_cb_t button_cb);

/* ---- Axis helpers shared by Manual, Teaching and Servo (hmi_axis.c) ---- */
#define HMI_ASSUMED_UNITS_PER_REV 10000u /* when the drive reports none (unverified) */

/** `units_per_rev`, or HMI_ASSUMED_UNITS_PER_REV if it is 0. */
uint32_t hmi_upr(uint32_t units_per_rev);
/** rpm (or rpm/s) to position units/s (or units/s^2), at least 1. */
uint32_t hmi_rpm_to_units(uint32_t upr, double rpm);

/** Display scale of an axis: its unit (mm, deg) once its Distance/Rev is set, else rev and rpm. */
typedef struct {
    double units_per_pos; /* drive position units per displayed position unit */
    double rpm_per_speed; /* motor rpm per displayed speed unit */
    const char *pos;      /* "mm" or "rev" */
    const char *speed;    /* "mm/s" or "rpm" */
    int speed_decimals;
} hmi_scale_t;

void hmi_axis_scale(const char *name, uint32_t units_per_rev, hmi_scale_t *out);
/** The same from settings not saved yet (Settings > Servo while editing). */
void hmi_scale_of(const machine_axis_cfg_t *cfg, uint32_t units_per_rev, hmi_scale_t *out);
/** Short name of a unit: "rev", "mm", "deg". */
const char *hmi_unit_name(uint8_t unit);
double hmi_to_pos(const hmi_scale_t *s, int32_t units);
int32_t hmi_from_pos(const hmi_scale_t *s, double pos);
double hmi_rpm_to_speed(const hmi_scale_t *s, double rpm);
double hmi_speed_to_rpm(const hmi_scale_t *s, double speed);
/** Largest position (displayed unit) that fits the drive's 32-bit position. */
double hmi_pos_limit(const hmi_scale_t *s);

/**
 * Queue a command for axis `index` (speed in motor rpm). The speed is limited to the axis' Max
 * speed and the ramps follow its Acc/Dec times (Settings > Servo). @return false if the queue is full
 */
bool hmi_axis_cmd(int index, const char *name, uint32_t upr, uint8_t code, int8_t dir, int32_t position, double rpm);
/** Tell the axis its new units per revolution after Pulse/Rev was written to the drive. */
bool hmi_axis_set_units_per_rev(int index, uint32_t units_per_rev);

/* ---- Display settings (hmi_display.c) ---- */
/** Load the settings from the machine store and hook the colour balance into `disp`; before the
 *  first frame. */
esp_err_t hmi_display_init(lv_display_t *disp, const hmi_config_t *cfg);

/** Watch `indev` for the screen timeouts (wraps its read callback); `indev` may be NULL. */
void hmi_screen_start(lv_indev_t *indev);

/** Switch the backlight on once the first frame is rendered; LVGL lock held. */
esp_err_t hmi_display_light(void);

/** Frame buffer of `panel` for the console command 'screenshot' (hmi_screenshot.c). */
void hmi_screenshot_init(esp_lcd_panel_handle_t panel);

/** Start measuring CPU load and frames of `disp` (hmi_cpu.c); LVGL lock held. */
void hmi_cpu_start(lv_display_t *disp);

/** Record touches for the console command 'touchlog' (hmi_touchlog.c); `indev` may be NULL. */
void hmi_touchlog_start(lv_indev_t *indev);

/** Seqlock read of the last hmi_publish_bus_status(). @return false if nothing published yet. */
bool hmi_bus_status_read(hmi_bus_status_t *out);
