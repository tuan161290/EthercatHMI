/* HMI simulator: one thread, so the LVGL lock always succeeds. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
static inline bool lvgl_port_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
static inline void lvgl_port_unlock(void) {}
