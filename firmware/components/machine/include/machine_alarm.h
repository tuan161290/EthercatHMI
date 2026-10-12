/*
 * Alarm list (Alarms page, mockup Alarm.dc.html).
 *
 * Supervisors (app_alarms.c, later the machine sequence) report conditions with
 * machine_alarm_update(); the operator acknowledges. An alarm stays in the list while its
 * condition is present or it is not acknowledged:
 *   Active        condition present, not acknowledged
 *   Acknowledged  condition present, acknowledged
 *   Cleared       condition gone, not acknowledged yet
 * Once it is both gone and acknowledged it moves to the history (RAM only: lost at reset).
 *
 * Core 0 only; never from the cyclic EtherCAT task.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MACHINE_ALARM_ACTIVE_MAX  32
#define MACHINE_ALARM_HISTORY_MAX 100
#define MACHINE_ALARM_CODE_LEN    8
#define MACHINE_ALARM_TITLE_LEN   48
#define MACHINE_ALARM_SOURCE_LEN  64
#define MACHINE_ALARM_CAUSE_LEN   128

typedef enum {
    MACHINE_ALARM_ERROR,   /*!< Stops the machine */
    MACHINE_ALARM_WARNING, /*!< Needs attention, the machine may run */
} machine_alarm_sev_t;

typedef enum {
    MACHINE_ALARM_ST_ACTIVE,
    MACHINE_ALARM_ST_ACK,
    MACHINE_ALARM_ST_CLEARED,
} machine_alarm_state_t;

/** What a supervisor reports. `code` identifies the alarm instance ("A101" = AX1 drive fault). */
typedef struct {
    const char *code;
    uint8_t sev;            /*!< machine_alarm_sev_t */
    const char *title;
    const char *source;
    const char *cause;
    const char *remedy;     /*!< Static text: kept as a pointer */
} machine_alarm_spec_t;

typedef struct {
    uint32_t id;            /*!< Unique, increasing */
    char code[MACHINE_ALARM_CODE_LEN];
    uint8_t sev;
    char title[MACHINE_ALARM_TITLE_LEN];
    char source[MACHINE_ALARM_SOURCE_LEN];
    char cause[MACHINE_ALARM_CAUSE_LEN];
    const char *remedy;
    int64_t raised;         /*!< Unix time, or -uptime in s if the clock was not set */
    int64_t ended;          /*!< When it left the list (history only) */
    bool active;            /*!< Condition present */
    bool acked;
} machine_alarm_t;

esp_err_t machine_alarm_init(void);

/**
 * Report the condition of alarm `spec->code`. Raising an alarm that is already in the list only
 * refreshes its texts (the cause may carry a changing error code).
 */
void machine_alarm_update(const machine_alarm_spec_t *spec, bool active);

void machine_alarm_ack(uint32_t id);
void machine_alarm_ack_all(void);

/** @return state of an entry of the current list */
machine_alarm_state_t machine_alarm_state(const machine_alarm_t *a);

/** Copy the current list, newest first. @return number copied */
int machine_alarm_list(machine_alarm_t *out, int max);

/** Copy the history, newest first. @return number copied */
int machine_alarm_history(machine_alarm_t *out, int max);

/** Entries in the current list, and how many of them are errors / unacknowledged. */
void machine_alarm_counts(int *total, int *errors, int *unacked);

/** +1 on every change of the list or the history (cheap: poll before copying). */
uint32_t machine_alarm_gen(void);

#ifdef __cplusplus
}
#endif
