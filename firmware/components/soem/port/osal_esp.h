/*
 * Internal ESP-IDF hooks of the SOEM OSAL port.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#ifndef _osal_esp_
#define _osal_esp_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the GPTimer used by osal_monotonic_sleep(). Call from the RT core. */
esp_err_t osal_esp_init(void);

#ifdef __cplusplus
}
#endif

#endif
