/*
 * SDO job worker (ecat_sdo_service.h).
 */

#include <string.h>
#include "ecat_sdo.h"
#include "ecat_sdo_service.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "ecat_sdo";

#define WORKER_STACK    8192 /* SOEM CoE keeps mailbox buffers on the stack */
#define WORKER_PRIO     3
#define LOCK_TIMEOUT_MS 3000

static ecx_contextt *s_ctx;
static SemaphoreHandle_t s_bus_lock;
static SemaphoreHandle_t s_lock; /* guards the fields below */
static TaskHandle_t s_task;
static ecat_sdo_job_t s_job;     /* ~300 B */
static uint32_t s_ticket;
static ecat_sdo_state_t s_state = ECAT_SDO_NONE;

static int32_t extend(uint32_t v, uint8_t size, bool is_signed)
{
    if (!is_signed) {
        return (int32_t)v;
    }
    if (size == 1) {
        return (int8_t)(uint8_t)v;
    }
    if (size == 2) {
        return (int16_t)(uint16_t)v;
    }
    return (int32_t)v;
}

static bool slave_matches(const ecat_sdo_job_t *j)
{
    if (j->slave < 1 || j->slave > s_ctx->slavecount) {
        return false;
    }
    const ec_slavet *sl = &s_ctx->slavelist[j->slave];
    const uint16_t st = sl->state & 0x0F;
    return sl->eep_man == j->vendor && sl->eep_id == j->product && (sl->mbx_proto & ECT_MBXPROT_COE) &&
           (st == EC_STATE_PRE_OP || st == EC_STATE_SAFE_OP || st == EC_STATE_OPERATIONAL);
}

/* Empty SOEM's error list; @return the last SDO abort code in it (0 if none). */
static uint32_t pop_abort(void)
{
    uint32_t abort = 0;
    ec_errort e;
    while (ecx_poperror(s_ctx, &e)) {
        if (e.Etype == EC_ERR_TYPE_SDO_ERROR) {
            abort = (uint32_t)e.AbortCode;
        }
    }
    return abort;
}

static void worker(void *arg)
{
    static ecat_sdo_job_t job; /* not on the stack */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        job = s_job;
        xSemaphoreGive(s_lock);

        bool refused = true;
        if (xSemaphoreTake(s_bus_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) == pdTRUE) {
            if (slave_matches(&job)) {
                refused = false;
                (void)pop_abort(); /* errors of earlier traffic are not this job's */
                for (uint8_t i = 0; i < job.n && i < ECAT_SDO_JOB_MAX; i++) {
                    ecat_sdo_item_t *it = &job.item[i];
                    const int size = it->size == 1 || it->size == 2 ? it->size : 4;
                    if (it->write) {
                        const uint32_t v = (uint32_t)it->value;
                        it->ok = ecat_sdo_write(s_ctx, job.slave, it->index, it->sub, &v, size);
                    } else {
                        uint32_t v = 0;
                        it->ok = ecat_sdo_read_uint(s_ctx, job.slave, it->index, it->sub, &v);
                        it->value = extend(v, (uint8_t)size, it->is_signed);
                    }
                    it->abort = it->ok ? 0 : pop_abort();
                }
            }
            xSemaphoreGive(s_bus_lock);
        }
        if (refused) {
            ESP_LOGW(TAG, "job for slave %u refused (bus busy, slave missing or a different device)", job.slave);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_job = job;
        s_state = refused ? ECAT_SDO_REFUSED : ECAT_SDO_DONE;
        xSemaphoreGive(s_lock);
    }
}

void ecat_sdo_service_start(struct ecx_context *ctx, SemaphoreHandle_t bus_lock)
{
    if (s_task) {
        return;
    }
    s_ctx = ctx;
    s_bus_lock = bus_lock;
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);
    xTaskCreatePinnedToCore(worker, "ecat_sdo", WORKER_STACK, NULL, WORKER_PRIO, &s_task, 0);
    configASSERT(s_task);
}

bool ecat_sdo_submit(const ecat_sdo_job_t *job, uint32_t *ticket)
{
    if (s_task == NULL || job == NULL || job->n > ECAT_SDO_JOB_MAX) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool free = s_state != ECAT_SDO_BUSY;
    if (free) {
        s_job = *job;
        s_ticket++;
        s_state = ECAT_SDO_BUSY;
        if (ticket) {
            *ticket = s_ticket;
        }
    }
    xSemaphoreGive(s_lock);
    if (free) {
        xTaskNotifyGive(s_task);
    }
    return free;
}

ecat_sdo_state_t ecat_sdo_poll(uint32_t ticket, ecat_sdo_job_t *out)
{
    if (s_lock == NULL) {
        return ECAT_SDO_NONE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ecat_sdo_state_t st = ticket == s_ticket ? s_state : ECAT_SDO_NONE;
    if (st == ECAT_SDO_DONE && out) {
        *out = s_job;
    }
    xSemaphoreGive(s_lock);
    return st;
}
