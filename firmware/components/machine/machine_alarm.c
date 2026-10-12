/*
 * Alarm list and history (machine_alarm.h). The tables live in PSRAM (about 33 KB).
 */

#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "machine_alarm.h"
#include "machine_clock.h"

static const char *TAG = "alarm";

static SemaphoreHandle_t s_lock;
static machine_alarm_t *s_list;     /* current list, oldest first */
static int s_n;
static machine_alarm_t *s_hist;     /* ring */
static int s_hist_head;             /* next write */
static int s_hist_n;
static uint32_t s_next_id = 1;
static atomic_uint s_gen;

static int64_t now_stamp(void)
{
    if (machine_clock_valid()) {
        return (int64_t)time(NULL);
    }
    return -(esp_timer_get_time() / 1000000); /* seconds since start-up, shown as such */
}

esp_err_t machine_alarm_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_list = heap_caps_calloc(MACHINE_ALARM_ACTIVE_MAX, sizeof(machine_alarm_t), MALLOC_CAP_SPIRAM);
    s_hist = heap_caps_calloc(MACHINE_ALARM_HISTORY_MAX, sizeof(machine_alarm_t), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_list == NULL || s_hist == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void changed(void)
{
    atomic_fetch_add(&s_gen, 1);
}

static int find(const char *code)
{
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_list[i].code, code) == 0) {
            return i;
        }
    }
    return -1;
}

/* Move entry i of the list to the history. */
static void retire(int i)
{
    machine_alarm_t *a = &s_list[i];
    a->ended = now_stamp();
    s_hist[s_hist_head] = *a;
    s_hist_head = (s_hist_head + 1) % MACHINE_ALARM_HISTORY_MAX;
    if (s_hist_n < MACHINE_ALARM_HISTORY_MAX) {
        s_hist_n++;
    }
    memmove(&s_list[i], &s_list[i + 1], sizeof(s_list[0]) * (size_t)(s_n - i - 1));
    s_n--;
}

static void fill(machine_alarm_t *a, const machine_alarm_spec_t *spec)
{
    strlcpy(a->code, spec->code, sizeof(a->code));
    a->sev = spec->sev;
    strlcpy(a->title, spec->title ? spec->title : "", sizeof(a->title));
    strlcpy(a->source, spec->source ? spec->source : "", sizeof(a->source));
    strlcpy(a->cause, spec->cause ? spec->cause : "", sizeof(a->cause));
    a->remedy = spec->remedy ? spec->remedy : "";
}

void machine_alarm_update(const machine_alarm_spec_t *spec, bool active)
{
    if (s_lock == NULL || spec == NULL || spec->code == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find(spec->code);
    if (active) {
        if (i >= 0 && s_list[i].active) {
            machine_alarm_t *a = &s_list[i];
            if (strcmp(a->cause, spec->cause ? spec->cause : "") != 0 ||
                strcmp(a->title, spec->title ? spec->title : "") != 0) {
                fill(a, spec);
                changed();
            }
        } else {
            if (i >= 0) {
                retire(i); /* came back after it cleared: a new occurrence */
            }
            if (s_n >= MACHINE_ALARM_ACTIVE_MAX) {
                retire(0); /* list full: the oldest goes to the history */
            }
            machine_alarm_t *a = &s_list[s_n++];
            memset(a, 0, sizeof(*a));
            fill(a, spec);
            a->id = s_next_id++;
            a->raised = now_stamp();
            a->active = true;
            ESP_LOGW(TAG, "%s %s: %s | %s", a->code, a->sev == MACHINE_ALARM_ERROR ? "ERROR" : "WARNING", a->title,
                     a->cause);
            changed();
        }
    } else if (i >= 0 && s_list[i].active) {
        s_list[i].active = false;
        ESP_LOGI(TAG, "%s cleared%s", s_list[i].code, s_list[i].acked ? "" : " (not acknowledged yet)");
        if (s_list[i].acked) {
            retire(i);
        }
        changed();
    }
    xSemaphoreGive(s_lock);
}

void machine_alarm_ack(uint32_t id)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        if (s_list[i].id == id && !s_list[i].acked) {
            s_list[i].acked = true;
            if (!s_list[i].active) {
                retire(i);
            }
            changed();
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

void machine_alarm_ack_all(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool any = false;
    for (int i = s_n - 1; i >= 0; i--) {
        if (!s_list[i].acked) {
            s_list[i].acked = true;
            any = true;
            if (!s_list[i].active) {
                retire(i);
            }
        }
    }
    if (any) {
        changed();
    }
    xSemaphoreGive(s_lock);
}

machine_alarm_state_t machine_alarm_state(const machine_alarm_t *a)
{
    if (!a->active) {
        return MACHINE_ALARM_ST_CLEARED;
    }
    return a->acked ? MACHINE_ALARM_ST_ACK : MACHINE_ALARM_ST_ACTIVE;
}

int machine_alarm_list(machine_alarm_t *out, int max)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = s_n - 1; i >= 0 && n < max; i--) {
        out[n++] = s_list[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

int machine_alarm_history(machine_alarm_t *out, int max)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int k = 0; k < s_hist_n && n < max; k++) {
        const int i = (s_hist_head - 1 - k + MACHINE_ALARM_HISTORY_MAX) % MACHINE_ALARM_HISTORY_MAX;
        out[n++] = s_hist[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

void machine_alarm_counts(int *total, int *errors, int *unacked)
{
    int t = 0, e = 0, u = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        t = s_n;
        for (int i = 0; i < s_n; i++) {
            e += s_list[i].sev == MACHINE_ALARM_ERROR;
            u += !s_list[i].acked;
        }
        xSemaphoreGive(s_lock);
    }
    if (total) {
        *total = t;
    }
    if (errors) {
        *errors = e;
    }
    if (unacked) {
        *unacked = u;
    }
}

uint32_t machine_alarm_gen(void)
{
    return atomic_load(&s_gen);
}
