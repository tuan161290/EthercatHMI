/*
 * ESP-IDF binding for SOEM: attach an installed esp_eth interface to the master.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rx_frames;        /*!< EtherCAT frames queued for SOEM */
    uint32_t rx_dropped;       /*!< EtherCAT frames dropped because the RX queue was full */
    uint32_t rx_non_ecat;      /*!< Non-EtherCAT frames discarded */
    uint32_t tx_frames;        /*!< Frames handed to the EMAC */
    uint32_t tx_errors;        /*!< esp_eth_transmit failures */
} soem_esp_stats_t;

/**
 * @brief Route raw frames of an installed (not necessarily started) esp_eth interface to SOEM.
 *
 * Takes over the interface's input path, so lwIP must not be attached to it.
 * Call from a task pinned to CONFIG_SOEM_RT_CORE: it also creates the timer used for
 * precise cycle sleeps, whose interrupt lands on the calling core.
 * Must be called before ecx_init().
 */
esp_err_t soem_esp_attach(esp_eth_handle_t eth);

/** @brief Stop routing frames to SOEM and drop queued frames. */
void soem_esp_detach(void);

/** @brief Snapshot of the frame counters. */
void soem_esp_get_stats(soem_esp_stats_t *out);

/** Process data layout supplied by the application for one slave. */
typedef struct {
    uint16_t out_bits;     /*!< SM2 (RxPDO) size */
    uint16_t in_bits;      /*!< SM3 (TxPDO) size */
    uint16_t sm2_addr;     /*!< Used only if the SII defines no SM2 */
    uint8_t sm2_control;
    uint16_t sm3_addr;     /*!< Used only if the SII defines no SM3 */
    uint8_t sm3_control;
} soem_esp_pdo_layout_t;

/**
 * @brief Make ecx_config_map_group() use this layout for a slave instead of the size it reads.
 *
 * SOEM normally sizes SM2/SM3 from the PDO mapping objects (CoE) or the SII. Modular couplers
 * such as the Leadshine R1EC do not serve their mapping objects, so the size must come from
 * the ESI. Call from the slave's PO2SOconfig hook or before ecx_config_map_group().
 * Implemented by the ESP port's copy of ec_config.c (see SOEM_UPSTREAM.md).
 *
 * @param slave  SOEM slave number (1-based)
 */
esp_err_t soem_esp_set_pdo_layout(uint16_t slave, const soem_esp_pdo_layout_t *layout);

/** @brief Forget every layout set with soem_esp_set_pdo_layout(). */
void soem_esp_clear_pdo_layouts(void);

#ifdef __cplusplus
}
#endif
