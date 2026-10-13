/*
 * MIPI-CSI camera on FPC2 (OV02C10 module, docs/BOARD.md 3.8) through esp_video.
 *
 * Call from a task on core 0: the CSI, ISP and I2C interrupts are installed on the calling core,
 * and core 1 belongs to EtherCAT.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sensor of the module; its driver is the only one in the build (components/cam_ov02c10). */
#define BOARD_CAMERA_SENSOR_NAME "OV02C10"

/**
 * Find the sensor on the shared I2C bus and create the esp_video devices: the camera
 * (ESP_VIDEO_MIPI_CSI_DEVICE_NAME, "/dev/video0") and the ISP with its pipeline controller
 * (auto exposure and white balance). Once it succeeded, later calls return ESP_OK at once.
 * @return ESP_ERR_NOT_FOUND (or another error) when no sensor answers
 */
esp_err_t board_camera_init(void);

#ifdef __cplusplus
}
#endif
