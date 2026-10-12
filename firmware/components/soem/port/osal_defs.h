/*
 * SOEM OSAL definitions for ESP-IDF (FreeRTOS SMP).
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#ifndef _osal_defs_
#define _osal_defs_

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if defined(CONFIG_SOEM_DEBUG) && !defined(EC_DEBUG)
#define EC_DEBUG
#endif

// define if debug printf is needed
#ifdef EC_DEBUG
#include <stdio.h>
#define EC_PRINT printf
#else
#define EC_PRINT(...) \
   do                 \
   {                  \
   } while (0)
#endif

#ifndef OSAL_PACKED
#define OSAL_PACKED_BEGIN
#define OSAL_PACKED __attribute__((__packed__))
#define OSAL_PACKED_END
#endif

#define ec_timet            struct timespec

#define OSAL_THREAD_HANDLE  TaskHandle_t
#define OSAL_THREAD_FUNC    void
#define OSAL_THREAD_FUNC_RT void

/* SOEM stores mutexes as `osal_mutext *`, which makes this a SemaphoreHandle_t. */
#define osal_mutext         struct QueueDefinition

#ifdef __cplusplus
}
#endif

#endif
