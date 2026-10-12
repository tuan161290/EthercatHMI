/*
 * Sequence timers, axis settings and teach points (machine_params.h).
 *
 * The LVGL task, the console and (later) the machine logic all run on core 0; s_lock keeps the
 * tables consistent between them.
 */

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "machine_params.h"
#include "machine_store.h"

static const char *TAG = "machine";

#define BLOB_VERSION 1
#define AXES_VERSION 3 /* 2: ramp times, max speed and distance per revolution; 3: unit */
#define MAX_AXES     16

static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        configASSERT(s_lock);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- Timers ---- */

/* Generic until the machine sequence is known: rename here, keep the order (stored by index). */
static const machine_timer_def_t s_timer_defs[MACHINE_TIMER_COUNT] = {
    {"Timer 1", 0, 60000, 1000}, {"Timer 2", 0, 60000, 1000}, {"Timer 3", 0, 60000, 1000},
    {"Timer 4", 0, 60000, 1000}, {"Timer 5", 0, 60000, 1000}, {"Timer 6", 0, 60000, 1000},
    {"Timer 7", 0, 60000, 1000}, {"Timer 8", 0, 60000, 1000},
};

typedef struct {
    uint16_t version;
    uint16_t n;
    uint32_t ms[MACHINE_TIMER_COUNT];
} timers_blob_t;

static uint32_t s_timer_ms[MACHINE_TIMER_COUNT];

static uint32_t clamp_timer(int i, uint32_t ms)
{
    const machine_timer_def_t *d = &s_timer_defs[i];
    return ms < d->min_ms ? d->min_ms : ms > d->max_ms ? d->max_ms : ms;
}

void machine_timers_init(void)
{
    timers_blob_t b;
    const bool ok = machine_store_load("timers", &b, sizeof(b)) == ESP_OK && b.version == BLOB_VERSION;
    lock();
    for (int i = 0; i < MACHINE_TIMER_COUNT; i++) {
        s_timer_ms[i] = ok && i < b.n ? clamp_timer(i, b.ms[i]) : s_timer_defs[i].def_ms;
    }
    unlock();
}

const machine_timer_def_t *machine_timer_def(int i)
{
    return i >= 0 && i < MACHINE_TIMER_COUNT ? &s_timer_defs[i] : NULL;
}

uint32_t machine_timer_ms(int i)
{
    if (i < 0 || i >= MACHINE_TIMER_COUNT) {
        return 0;
    }
    lock();
    const uint32_t v = s_timer_ms[i];
    unlock();
    return v;
}

esp_err_t machine_timers_set(const uint32_t ms[MACHINE_TIMER_COUNT])
{
    timers_blob_t b = {.version = BLOB_VERSION, .n = MACHINE_TIMER_COUNT};
    lock();
    for (int i = 0; i < MACHINE_TIMER_COUNT; i++) {
        s_timer_ms[i] = clamp_timer(i, ms[i]);
        b.ms[i] = s_timer_ms[i];
    }
    unlock();
    return machine_store_save("timers", &b, sizeof(b));
}

/* ---- Axis settings ---- */

typedef struct {
    char name[MACHINE_AXIS_NAME_LEN];
    machine_axis_cfg_t cfg;
} axis_entry_t;

typedef struct {
    uint16_t version;
    uint16_t n;
    axis_entry_t e[MAX_AXES];
} axes_blob_t;

/* Version 2, read once to keep the settings made before the unit existed. */
typedef struct {
    char name[MACHINE_AXIS_NAME_LEN];
    float max_rpm, acc_ms, dec_ms, jog_high_rpm, dist_per_rev;
} axis_entry_v2_t;

typedef struct {
    uint16_t version;
    uint16_t n;
    axis_entry_v2_t e[MAX_AXES];
} axes_blob_v2_t;

static EXT_RAM_BSS_ATTR axes_blob_t s_axes; /* loaded on first use */
static bool s_axes_loaded;

void machine_axis_defaults(machine_axis_cfg_t *out)
{
    out->max_rpm = 1000.0f;
    out->acc_ms = 1000.0f; /* 1000 rpm/s: 300 rpm in 0.3 s */
    out->dec_ms = 1000.0f;
    out->jog_high_rpm = 300.0f;
    out->dist_per_rev = 0.0f;
    out->unit = MACHINE_UNIT_REV;
}

/* Settings of version 2: a Distance/Rev meant mm. @return true if there were any */
static bool axes_load_v2_locked(void)
{
    axes_blob_v2_t *old = heap_caps_malloc(sizeof(*old), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = old && machine_store_load("axes", old, sizeof(*old)) == ESP_OK && old->version == 2 &&
              old->n <= MAX_AXES;
    if (ok) {
        memset(&s_axes, 0, sizeof(s_axes));
        s_axes.version = AXES_VERSION;
        s_axes.n = old->n;
        for (int i = 0; i < old->n; i++) {
            const axis_entry_v2_t *o = &old->e[i];
            axis_entry_t *e = &s_axes.e[i];
            memcpy(e->name, o->name, sizeof(e->name));
            e->cfg = (machine_axis_cfg_t){
                .max_rpm = o->max_rpm,
                .acc_ms = o->acc_ms,
                .dec_ms = o->dec_ms,
                .jog_high_rpm = o->jog_high_rpm,
                .dist_per_rev = o->dist_per_rev,
                .unit = o->dist_per_rev > 0.0f ? MACHINE_UNIT_MM : MACHINE_UNIT_REV,
            };
        }
        ESP_LOGI(TAG, "axis settings of %d axes taken over from version 2", old->n);
    }
    heap_caps_free(old);
    return ok;
}

static void axes_load_locked(void)
{
    if (s_axes_loaded) {
        return;
    }
    s_axes_loaded = true;
    if (machine_store_load("axes", &s_axes, sizeof(s_axes)) != ESP_OK || s_axes.version != AXES_VERSION ||
        s_axes.n > MAX_AXES) {
        if (!axes_load_v2_locked()) {
            memset(&s_axes, 0, sizeof(s_axes)); /* nothing usable: start from the defaults */
            s_axes.version = AXES_VERSION;
        }
    }
}

void machine_axis_get(const char *name, machine_axis_cfg_t *out)
{
    machine_axis_defaults(out);
    lock();
    axes_load_locked();
    for (int i = 0; i < s_axes.n; i++) {
        if (strncmp(s_axes.e[i].name, name, MACHINE_AXIS_NAME_LEN) == 0) {
            *out = s_axes.e[i].cfg;
            break;
        }
    }
    unlock();
}

esp_err_t machine_axis_set(const char *name, const machine_axis_cfg_t *cfg)
{
    machine_axis_cfg_t c = {
        .max_rpm = clampf(cfg->max_rpm, 1.0f, MACHINE_MAX_RPM_MAX),
        .acc_ms = clampf(cfg->acc_ms, MACHINE_RAMP_MS_MIN, MACHINE_RAMP_MS_MAX),
        .dec_ms = clampf(cfg->dec_ms, MACHINE_RAMP_MS_MIN, MACHINE_RAMP_MS_MAX),
        .jog_high_rpm = clampf(cfg->jog_high_rpm, 1.0f, MACHINE_MAX_RPM_MAX),
        .dist_per_rev = clampf(cfg->dist_per_rev, 0.0f, MACHINE_DIST_MAX),
        .unit = cfg->unit < MACHINE_UNIT_COUNT ? cfg->unit : MACHINE_UNIT_REV,
    };
    lock();
    axes_load_locked();
    int k = -1;
    for (int i = 0; i < s_axes.n && k < 0; i++) {
        if (strncmp(s_axes.e[i].name, name, MACHINE_AXIS_NAME_LEN) == 0) {
            k = i;
        }
    }
    if (k < 0) {
        if (s_axes.n >= MAX_AXES) {
            /* Full: drop the oldest entry (an axis of an earlier topology, most likely). */
            memmove(&s_axes.e[0], &s_axes.e[1], sizeof(s_axes.e[0]) * (MAX_AXES - 1));
            s_axes.n = MAX_AXES - 1;
        }
        k = s_axes.n++;
        memset(&s_axes.e[k], 0, sizeof(s_axes.e[k]));
        strlcpy(s_axes.e[k].name, name, sizeof(s_axes.e[k].name));
    }
    s_axes.e[k].cfg = c;
    axes_blob_t copy = s_axes;
    unlock();
    return machine_store_save("axes", &copy, sizeof(copy));
}

/* ---- Teach points ---- */

typedef struct {
    uint16_t version;
    uint16_t reserved;
    machine_points_t p;
} points_blob_t;

static void points_key(const char *first_axis, char *key, size_t len)
{
    snprintf(key, len, "pt_%.7s", first_axis);
}

void machine_points_load(const char *const names[], int n_axes, machine_points_t *out)
{
    if (n_axes < 1) {
        n_axes = 1;
    }
    if (n_axes > MACHINE_GROUP_AXES) {
        n_axes = MACHINE_GROUP_AXES;
    }
    memset(out, 0, sizeof(*out));
    out->n_axes = (uint8_t)n_axes;
    for (int a = 0; a < n_axes; a++) {
        strlcpy(out->axis[a], names[a], sizeof(out->axis[a]));
    }

    char key[16];
    points_key(names[0], key, sizeof(key));
    static EXT_RAM_BSS_ATTR points_blob_t b; /* ~1 KB: not on the caller's stack; callers hold s_lock */
    lock();
    const bool ok = machine_store_load(key, &b, sizeof(b)) == ESP_OK && b.version == BLOB_VERSION &&
                    b.p.n_points <= MACHINE_MAX_POINTS && b.p.n_axes <= MACHINE_GROUP_AXES;
    if (ok) {
        out->n_points = b.p.n_points;
        for (int i = 0; i < b.p.n_points; i++) {
            machine_point_t *d = &out->pt[i];
            const machine_point_t *s = &b.p.pt[i];
            memcpy(d->name, s->name, sizeof(d->name));
            d->name[sizeof(d->name) - 1] = '\0';
            d->speed_pct = s->speed_pct;
            for (int a = 0; a < n_axes; a++) {
                for (int k = 0; k < b.p.n_axes; k++) {
                    if (strncmp(b.p.axis[k], out->axis[a], MACHINE_AXIS_NAME_LEN) == 0) {
                        d->pos[a] = s->pos[k];
                    }
                }
            }
        }
    }
    unlock();
    if (!ok || out->n_points == 0) {
        out->n_points = 1;
        strlcpy(out->pt[0].name, "HOME", sizeof(out->pt[0].name));
        out->pt[0].speed_pct = 100;
    }
}

esp_err_t machine_points_save(const machine_points_t *p)
{
    if (p->n_axes < 1 || p->n_axes > MACHINE_GROUP_AXES || p->n_points > MACHINE_MAX_POINTS) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    points_key(p->axis[0], key, sizeof(key));
    static EXT_RAM_BSS_ATTR points_blob_t b;
    lock();
    memset(&b, 0, sizeof(b));
    b.version = BLOB_VERSION;
    b.p = *p;
    esp_err_t err = machine_store_save(key, &b, sizeof(b));
    unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "teach points of %s: %u point(s) stored", p->axis[0], p->n_points);
    }
    return err;
}
