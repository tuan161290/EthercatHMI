/*
 * HMI simulator: builds the firmware's HMI (components/hmi) on top of LVGL in a memory frame
 * buffer, opens pages by name as the console command 'page' does, and writes each screen as raw
 * RGB565 (sim_build.py turns them into PNG files). Machine data comes from sim_fake.c.
 *
 *   hmi_sim.exe <out dir> [page ...]
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "hmi_internal.h"

#define W BOARD_LCD_H_RES
#define H BOARD_LCD_V_RES

static uint16_t s_fb[W * H];

void sim_fake_init(void); /* sim_machine.c */

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

/* Let LVGL run `ms` of simulated time: timers, async calls, animations. */
static void pump(int ms)
{
    for (int t = 0; t < ms; t += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static void shot(const char *dir, const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.raw", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fwrite(s_fb, sizeof(s_fb[0]), W * H, f);
    fclose(f);
}

/* Click the first object whose label reads `text`: its parent gets LV_EVENT_CLICKED. */
static bool click_text(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
        lv_obj_send_event(lv_obj_get_parent(obj), LV_EVENT_CLICKED, NULL);
        return true;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        if (click_text(lv_obj_get_child(obj, i), text)) {
            return true;
        }
    }
    return false;
}

/* Open the first drop-down list of the screen. */
static bool open_dropdown(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_dropdown_open(obj);
        return true;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        if (open_dropdown(lv_obj_get_child(obj, i))) {
            return true;
        }
    }
    return false;
}

/* Scenes: a page with something done on it. */
static bool scene(const char *name)
{
    lv_obj_t *scr = lv_screen_active();
    if (strcmp(name, "interp_run") == 0) {
        /* A full circle counter-clockwise around (10, 5), traced while it runs */
        hmi_open_page("interp");
        pump(300);
        click_text(scr, "Arc CCW");
        pump(100);
        click_text(scr, "Start");
        pump(15000);
    } else if (strcmp(name, "dropdown") == 0) {
        hmi_open_page("interp");
        pump(300);
        open_dropdown(scr);
        pump(300);
    } else {
        return false;
    }
    return true;
}

static void text_cb(const char *text, void *user)
{
    (void)text;
    (void)user;
}

static void keypad_cb(double value, void *user)
{
    (void)value;
    (void)user;
}

/* Dialogs on the top layer, drawn over the page that is open. */
static bool open_dialog(const char *name)
{
    if (strcmp(name, "keyboard") == 0) {
        hmi_text_input("Connect to OpenWrt", "Password of the network (8 to 63 characters)", "factory2026", true, 8,
                       63, text_cb, NULL);
    } else if (strcmp(name, "keypad") == 0) {
        hmi_keypad("Max speed", "rpm", 3000, 1, 6000, 0, keypad_cb, NULL);
    } else if (strcmp(name, "choice") == 0) {
        hmi_choice("Axis", "AX1\nAX2\nAX3\nAX4\nAX5\nAX6\nAX7\nAX8\nAX9", 2, NULL, NULL);
    } else if (strcmp(name, "datetime") == 0) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        hmi_datetime("Set date and time", &tm, NULL, NULL);
    } else if (strcmp(name, "confirm") == 0) {
        hmi_confirm("Delete P01?", "The point HOME of group 1 is removed. Save the points to keep the change.",
                    "Delete", true, NULL, NULL);
    } else {
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    static const char *const all[] = {"main",     "jog",    "teach",  "interp", "io",      "alarms",
                                      "settings", "time",   "servo",  "ethercat", "system", "network",
                                      "info",     "camera",   "keyboard", "interp_run", "dropdown"};
    if (argc < 2) {
        fprintf(stderr, "use: hmi_sim <out dir> [page ...]\n");
        return 2;
    }
    const char *dir = argv[1];

    sim_fake_init();
    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    const hmi_config_t cfg = {.panel = BOARD_LCD_PANEL_NEW, .brightness = 80};
    hmi_display_init(d, &cfg);
    hmi_theme_init();
    hmi_shell_create();
    pump(500);

    const int n = argc > 2 ? argc - 2 : (int)(sizeof(all) / sizeof(all[0]));
    for (int i = 0; i < n; i++) {
        const char *name = argc > 2 ? argv[i + 2] : all[i];
        if (scene(name)) {
            shot(dir, name);
            continue;
        }
        if (open_dialog(name)) {
            pump(300);
            shot(dir, name);
            lv_obj_clean(lv_layer_top());
            pump(100);
            continue;
        }
        if (!hmi_open_page(name)) {
            fprintf(stderr, "unknown page: %s\n", name);
            continue;
        }
        pump(2000);
        shot(dir, name);
    }
    return 0;
}
