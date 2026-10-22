/*
 * Shared by camera.c (capture, JPEG, local server) and camera_push.c (push to a server).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera.h"

/** A YUV420 frame for the H.264 encoder: ESP_H264_RAW_FMT_O_UYY_E_VYY, the P4's YUV420 layout. */
typedef struct {
    const uint8_t *buf;
    size_t len; /* width * height * 3 / 2 */
    uint16_t width, height;
    int64_t time_us; /* when it was captured (esp_timer) */
} camera_yuv_frame_t;

/**
 * Start a push session: the camera serves frames, keeps the sensor running and makes YUV420 frames
 * until camera_push_leave(). @return false while the camera does not serve (off, starting, size change)
 */
bool camera_push_enter(void);
void camera_push_leave(void);

/** @return false once the camera wants the push session to end (off, size change) */
bool camera_serving(void);

/**
 * Newest YUV420 frame if it is not `*seen`; `*f` stays valid until camera_yuv_release().
 * @return slot, or -1 if there is no newer frame
 */
int camera_yuv_take(uint32_t *seen, camera_yuv_frame_t *f);
void camera_yuv_release(int slot);

/** Wait at most `ms` for a new frame. */
void camera_frame_wait(uint32_t ms);

/** PSRAM for DMA (PPA, H.264): address and size aligned to the cache line. Free with heap_caps_free(). */
uint8_t *camera_dma_alloc(size_t size, size_t *got);

/** Load the push settings and start its task (camera_start()). */
esp_err_t camera_push_start(void);

/** Push part of camera_status(). */
void camera_push_status(camera_status_t *out);
