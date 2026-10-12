/* HMI simulator: an empty NVS. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void)ns; (void)mode; (void)h; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) { (void)h; (void)key; (void)out; (void)len; return ESP_ERR_NOT_FOUND; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
