/* HMI simulator: stand-in for ESP-IDF's esp_err.h. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT       0x107
const char *esp_err_to_name(esp_err_t code);
#define ESP_ERROR_CHECK(x) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { fprintf(stderr, "ESP_ERROR_CHECK %s:%d %d\n", __FILE__, __LINE__, err_rc_); abort(); } } while (0)
