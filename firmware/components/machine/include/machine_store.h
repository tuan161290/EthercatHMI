/*
 * Machine data store: sequence timers, axis settings of the HMI, teach points, time zone,
 * display settings. Values are versioned blobs, one per key.
 *
 * The values live in RAM and every save is appended to a log in the flash partition "mstore"
 * (machine_store.c). A save is written at once, also while EtherCAT is in OP: appending only
 * programs flash pages, it never erases. Erasing (compaction) waits until flash_ok() allows it;
 * only if the log is full while in OP does a save wait in RAM for that.
 *
 * Core 0 only (HMI, console, app tasks).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Asked before flash is erased (and to pace writes); false while the bus is in OP. NULL = always. */
typedef bool (*machine_flash_ok_t)(void);

/** Call once at start-up, after nvs_flash_init(), before the other machine_* modules. */
esp_err_t machine_store_init(machine_flash_ok_t flash_ok);

/**
 * Read blob `key` (at most 15 characters) into `buf`.
 * @return ESP_ERR_NOT_FOUND if not stored, ESP_ERR_INVALID_SIZE if the stored size differs.
 */
esp_err_t machine_store_load(const char *key, void *buf, size_t len);

/** Store blob `key` (at most 4096 bytes). @return ESP_OK when written, or kept to be written later */
esp_err_t machine_store_save(const char *key, const void *buf, size_t len);

/** Forget `key`. @return ESP_ERR_NOT_FOUND if it is not stored */
esp_err_t machine_store_remove(const char *key);

/** Compact the log now (erases flash). @return ESP_ERR_INVALID_STATE while flash_ok() says no (bus in OP) */
esp_err_t machine_store_compact(void);

/** @return true while a save waits in RAM (the log is full and the bus is in OP) */
bool machine_store_pending(void);

/** @return true if `key` has a save waiting in RAM */
bool machine_store_key_pending(const char *key);

typedef struct {
    bool ok;          /*!< The "mstore" partition is in use */
    uint8_t half;     /*!< Active half of the log */
    uint32_t gen;     /*!< Compactions so far */
    uint32_t used;    /*!< Bytes used in the active half */
    uint32_t size;    /*!< Bytes in a half */
    uint8_t keys;
    uint8_t waiting;  /*!< Keys waiting in RAM */
} machine_store_info_t;

/** State of the log, for the console. */
void machine_store_info(machine_store_info_t *out);

/** Key number `i` and its size. @return false past the last key */
bool machine_store_key_at(int i, char *key, size_t key_len, size_t *len);

#ifdef __cplusplus
}
#endif
