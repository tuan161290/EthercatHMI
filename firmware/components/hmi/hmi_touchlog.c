/*
 * Touch log for bench diagnosis: the last presses and releases with their screen point and the
 * text of the object hit, kept in a ring and printed with the console command 'touchlog'.
 * Answers "I tapped X and nothing happened" without seeing the screen: was the touch seen, where,
 * and on which widget.
 */

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "hmi_internal.h"

#define LOG_N     32
#define POLL_MS   10
#define WHAT_LEN  28

typedef struct {
    int64_t t_ms;
    int16_t x, y;
    bool down;
    char what[WHAT_LEN];
} touch_ev_t;

static EXT_RAM_BSS_ATTR touch_ev_t s_ev[LOG_N];
static int s_head, s_n;
static lv_indev_t *s_indev;
static bool s_was_down;

/* Text of the first label in `obj` or its children (two levels), to name what was hit. */
static const char *obj_text(lv_obj_t *obj, int depth)
{
    if (obj == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        return lv_label_get_text(obj);
    }
    if (depth == 0) {
        return NULL;
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        const char *t = obj_text(lv_obj_get_child(obj, (int32_t)i), depth - 1);
        if (t && t[0]) {
            return t;
        }
    }
    return NULL;
}

static void record(bool down)
{
    lv_point_t p;
    lv_indev_get_point(s_indev, &p);
    touch_ev_t *e = &s_ev[s_head];
    e->t_ms = esp_timer_get_time() / 1000;
    e->x = (int16_t)p.x;
    e->y = (int16_t)p.y;
    e->down = down;
    /* Topmost clickable object at the point: dialogs on the top layer first, then the page. */
    lv_obj_t *hit = lv_indev_search_obj(lv_layer_top(), &p);
    if (hit == NULL) {
        hit = lv_indev_search_obj(lv_screen_active(), &p);
    }
    const char *t = obj_text(hit, 2);
    if (hit == NULL) {
        strlcpy(e->what, "(nothing clickable)", sizeof(e->what));
    } else {
        strlcpy(e->what, t ? t : "(object without text)", sizeof(e->what));
    }
    s_head = (s_head + 1) % LOG_N;
    if (s_n < LOG_N) {
        s_n++;
    }
}

/* LVGL task: watch the pointer state and record each change. */
static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    const bool down = lv_indev_get_state(s_indev) == LV_INDEV_STATE_PRESSED;
    if (down != s_was_down) {
        s_was_down = down;
        record(down);
    }
}

void hmi_touchlog_start(lv_indev_t *indev)
{
    s_indev = indev;
    if (indev) {
        lv_timer_create(poll_cb, POLL_MS, NULL);
    }
}

void hmi_touchlog_print(void)
{
    if (s_indev == NULL) {
        printf("No touch input (touch controller not found at start-up).\n");
        return;
    }
    if (!lvgl_port_lock(1000)) {
        return;
    }
    if (s_n == 0) {
        printf("No touch seen since start-up.\n");
    }
    for (int k = s_n; k > 0; k--) {
        const touch_ev_t *e = &s_ev[(s_head - k + LOG_N) % LOG_N];
        printf("%8lld ms  %-4s x %4d y %4d  %s\n", (long long)e->t_ms, e->down ? "down" : "up", e->x, e->y, e->what);
    }
    lvgl_port_unlock();
}
