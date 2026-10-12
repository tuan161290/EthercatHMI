/*
 * Saved topology in NVS (namespace "ecat", key "topo").
 *
 * Writing NVS stalls both CPU caches, so only call save/erase while the bus is not in OP
 * (CLAUDE.md, architecture rules).
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "ecat_topology.h"
#include "esp_rom_crc.h"
#include "nvs.h"

#define TOPO_NAMESPACE "ecat"
#define TOPO_KEY       "topo"
#define TOPO_MAGIC     0x4F504F54u /* "TOPO" */
#define TOPO_VERSION   1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;          /* sizeof(topo_blob_t), catches layout changes */
    ecat_topo_meta_t meta;
    ecat_topology_t topo;
    uint32_t crc;           /* CRC32 of everything above */
} topo_blob_t;

static uint32_t blob_crc(const topo_blob_t *b)
{
    return esp_rom_crc32_le(0, (const uint8_t *)b, offsetof(topo_blob_t, crc));
}

esp_err_t ecat_topology_load(ecat_topology_t *out, ecat_topo_meta_t *meta)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(TOPO_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    topo_blob_t *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    size_t len = sizeof(*b);
    err = nvs_get_blob(h, TOPO_KEY, b, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH) {
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_OK) {
        bool valid = len == sizeof(*b) && b->magic == TOPO_MAGIC && b->version == TOPO_VERSION &&
                     b->size == sizeof(*b) && b->crc == blob_crc(b) && b->topo.n_slaves <= ECAT_TOPO_MAX_SLAVES &&
                     b->topo.n_racks <= ECAT_TOPO_MAX_RACKS;
        if (valid) {
            *out = b->topo;
            if (meta) {
                *meta = b->meta;
            }
        } else {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    free(b);
    return err;
}

esp_err_t ecat_topology_save(const ecat_topology_t *topo, const ecat_topo_meta_t *meta)
{
    topo_blob_t *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return ESP_ERR_NO_MEM;
    }
    b->magic = TOPO_MAGIC;
    b->version = TOPO_VERSION;
    b->size = sizeof(*b);
    if (meta) {
        b->meta = *meta;
    }
    b->topo = *topo;
    b->crc = blob_crc(b);

    nvs_handle_t h;
    esp_err_t err = nvs_open(TOPO_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, TOPO_KEY, b, sizeof(*b));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(b);
    return err;
}

esp_err_t ecat_topology_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(TOPO_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, TOPO_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
