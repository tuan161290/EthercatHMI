/*
 * Shared by camera.c (capture, JPEG, local server) and camera_push.c (push to a server).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera.h"

/**
 * Start a push session: the camera serves frames and keeps the sensor running until
 * camera_push_leave(). @return false while the camera does not serve (off, starting, size change)
 */
bool camera_push_enter(void);
void camera_push_leave(void);

/** @return false once the camera wants the push session to end (off, size change) */
bool camera_serving(void);

/**
 * Newest JPEG if it is not `*seen`: `*buf` and `*len` stay valid until camera_frame_release().
 * @return slot, or -1 if there is no newer frame
 */
int camera_frame_take(uint32_t *seen, const uint8_t **buf, size_t *len);
void camera_frame_release(int slot);

/** Wait at most `ms` for a new JPEG. */
void camera_frame_wait(uint32_t ms);

/** Load the push settings and start its task (camera_start()). */
esp_err_t camera_push_start(void);

/** Push part of camera_status(). */
void camera_push_status(camera_status_t *out);
