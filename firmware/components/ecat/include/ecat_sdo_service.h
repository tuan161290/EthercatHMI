/*
 * SDO jobs for the HMI (Settings > Servo): read or write a list of drive objects.
 *
 * The HMI never calls ecx_* (CLAUDE.md): it submits a job, a worker task on core 0 runs it
 * through the mailbox while holding the bus lock, and the HMI polls for the result. The bus
 * lock keeps jobs away from scans and reconfiguration; mailbox traffic runs next to the cyclic
 * process data like the slave supervision does.
 *
 * One job at a time. A job names the slave by position and identity (vendor/product) and is
 * refused if the slave at that position is a different device.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_SDO_JOB_MAX 24

typedef struct {
    uint16_t index;
    uint8_t sub;
    uint8_t size;      /*!< 1, 2 or 4 bytes */
    bool is_signed;    /*!< Sign-extend a read value */
    bool write;
    bool ok;           /*!< Result: the SDO succeeded */
    int32_t value;     /*!< Value to write, or the value read */
    uint32_t abort;    /*!< Result of a failed item: the slave's SDO abort code, 0 if it sent none */
} ecat_sdo_item_t;

typedef struct {
    uint16_t slave;
    uint32_t vendor;
    uint32_t product;
    uint8_t n;
    ecat_sdo_item_t item[ECAT_SDO_JOB_MAX];
} ecat_sdo_job_t;

typedef enum {
    ECAT_SDO_NONE,     /*!< Unknown ticket */
    ECAT_SDO_BUSY,     /*!< Queued or running */
    ECAT_SDO_DONE,     /*!< Finished; see item[].ok */
    ECAT_SDO_REFUSED,  /*!< Bus busy (scan, configuration), slave missing or a different device */
} ecat_sdo_state_t;

struct ecx_context;

/** Start the worker (core 0). `bus_lock` is the mutex the ecat task holds while it changes the bus. */
void ecat_sdo_service_start(struct ecx_context *ctx, SemaphoreHandle_t bus_lock);

/** Queue a job. @return false if a job is still running */
bool ecat_sdo_submit(const ecat_sdo_job_t *job, uint32_t *ticket);

/** @return the state of `ticket`; with ECAT_SDO_DONE the results are copied to `out` */
ecat_sdo_state_t ecat_sdo_poll(uint32_t ticket, ecat_sdo_job_t *out);

#ifdef __cplusplus
}
#endif
