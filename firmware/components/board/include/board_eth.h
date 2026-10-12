/*
 * On-board Ethernet (ESP32-P4 EMAC + IP101 PHY) bring-up for EtherCAT use.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the EMAC + IP101 driver without any TCP/IP stack.
 *
 * Call from the task pinned to the real-time core: the EMAC interrupt and its RX task
 * are bound to the core that creates the MAC. The driver is installed but not started.
 *
 * @param rx_task_prio priority of the EMAC RX task; keep it above every task on the same
 *                     core that waits for frames.
 */
esp_err_t board_eth_init(uint32_t rx_task_prio, esp_eth_handle_t *out_eth);

/**
 * @brief Wait for link up on a started driver.
 *
 * @param timeout_ms how long to wait for the link
 * @param[out] full_100m true if the link negotiated 100 Mbit/s full duplex, which EtherCAT requires
 * @return ESP_OK on link up, ESP_ERR_TIMEOUT otherwise
 */
esp_err_t board_eth_wait_link(esp_eth_handle_t eth, uint32_t timeout_ms, bool *full_100m);

/** @brief True while the PHY reports link up. */
bool board_eth_link_is_up(void);

#ifdef __cplusplus
}
#endif
