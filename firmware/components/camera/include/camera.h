/*
 * Camera: the OV02C10 on FPC2 as an MJPEG stream over Wi-Fi, for a recorder on the server
 * (docs/CAMERA.md: go2rtc on TrueNAS).
 *
 * Switched on or off, and the picture size chosen, in Settings > Camera (saved in the machine store,
 * key "camera"). While on,
 * an HTTP server on the Wi-Fi address serves
 *   /              a page with the live picture
 *   /stream        MJPEG (multipart/x-mixed-replace), at most CONFIG_CAMERA_MAX_FPS frames/s
 *   /snapshot.jpg  one JPEG
 * The sensor only runs while someone watches (or asks for a snapshot), so an idle camera costs no
 * memory bandwidth. Off, the server is stopped and the camera's memory is freed.
 *
 * Everything runs on core 0 in the "camera" task and the stream tasks; the public functions only
 * post requests and copy the status.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_REASON_MAX 63

/**
 * Picture size sent. The sensor always runs at 1920x1080; smaller sizes are scaled down by the
 * PPA before compression. The PPA scales in steps of 1/16, so the smaller sizes lose 3-11 % at
 * the edges; 720x480 is 3:2 and loses the sides.
 */
typedef enum {
    CAMERA_RES_1920X1080,
    CAMERA_RES_1280X720,
    CAMERA_RES_1024X576,
    CAMERA_RES_720X480,
    CAMERA_RES_640X360,
    CAMERA_RES_COUNT,
} camera_res_t;

typedef enum {
    CAMERA_OFF,       /*!< switched off in the settings */
    CAMERA_STARTING,  /*!< finding the sensor, allocating buffers */
    CAMERA_FAULT,     /*!< sensor not found or start failed (see reason); switch off and on to retry */
    CAMERA_WAIT_WIFI, /*!< on, waiting for a Wi-Fi connection to start the server */
    CAMERA_READY,     /*!< server running, nobody watching: the sensor is stopped */
    CAMERA_STREAMING, /*!< sending frames */
} camera_state_t;

typedef struct {
    bool on;            /*!< setting */
    camera_res_t res;   /*!< setting */
    camera_state_t state;
    char reason[CAMERA_REASON_MAX + 1]; /*!< why it failed, "" if it did not */
    char sensor[16];                    /*!< "OV02C10" once found, "" before */
    uint16_t width, height;             /*!< picture size sent, once started */
    uint16_t port;                      /*!< HTTP port of the server */
    uint8_t max_fps;                    /*!< CONFIG_CAMERA_MAX_FPS */
    uint8_t viewers;                    /*!< open /stream connections */
    uint16_t fps_x10;                   /*!< JPEG frames made over the last second, x10 */
    uint32_t kbit_s;                    /*!< data sent to the viewers over the last second */
    uint32_t frame_bytes;               /*!< size of the last JPEG */
    uint32_t frames;                    /*!< JPEG frames since start-up */
} camera_status_t;

/** Load the setting and start the camera task (core 0). The camera starts if it is switched on. */
esp_err_t camera_start(void);

/** Snapshot of the setting and the state. Any task. */
void camera_status(camera_status_t *out);

/** Switch the camera on or off (saved; machine_store.h, also while EtherCAT is in OP). Core 0. */
esp_err_t camera_set_on(bool on);

/**
 * Picture size (saved). If the camera runs, its streams stop and the open connections close;
 * viewers such as go2rtc connect again by themselves. Core 0.
 */
esp_err_t camera_set_res(camera_res_t res);

/** Width and height of `res`. */
void camera_res_size(camera_res_t res, uint16_t *w, uint16_t *h);

/** "Off", "Streaming", ... */
const char *camera_state_text(camera_state_t s);

#ifdef __cplusplus
}
#endif
