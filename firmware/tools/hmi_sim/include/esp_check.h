/* HMI simulator: ESP-IDF check macros without logging. */
#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, ...) do { if (!(a)) { return err_code; } } while (0)
#define ESP_RETURN_ON_ERROR(x, log_tag, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { return err_rc_; } } while (0)
#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, log_tag, ...) do { if (!(a)) { ret = err_code; goto goto_tag; } } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { ret = err_rc_; goto goto_tag; } } while (0)
