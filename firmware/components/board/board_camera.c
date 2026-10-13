/*
 * Camera on FPC2 (docs/BOARD.md 3.8): 15-pin Raspberry Pi style MIPI-CSI connector with an
 * OV02C10 module (driver: components/cam_ov02c10).
 *
 * - SCCB is I2C bus 1 (GPIO7/8 through R30/R32), shared with the touch controller, the codec and
 *   the RTC, at 100 kHz as in the vendor demo (video_lcd_display sdkconfig:777).
 * - No reset or power-down pin: CSI_IO0 only has a pull-up, CSI_IO1 is not connected.
 * - The sensor clock (24 MHz) comes from the module; the P4 drives no XCLK.
 * - The CSI D-PHY is supplied like the DSI D-PHY: esp_video acquires LDO_VO3 at 2500 mV
 *   (esp_video 2.1.0 src/device/esp_video_csi_device.c:31-32), the same channel and voltage as
 *   board_display.c, and the LDO driver counts both users.
 */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "board_camera.h"
#include "board_i2c.h"
#include "esp_check.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_camera";

#define SCCB_FREQ_HZ 100000
#define INIT_FLAGS   (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP)

static bool s_ready;

esp_err_t board_camera_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(board_i2c_bus(&bus), TAG, "I2C bus");
    const esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bus,
            .freq = SCCB_FREQ_HZ,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    const esp_video_init_config_t cfg = {.csi = &csi};
    esp_err_t err = esp_video_init_with_flags(&cfg, INIT_FLAGS);
    if (err == ESP_OK) {
        /* A sensor that does not answer is only logged: esp_video_init() still returns ESP_OK,
         * without the camera device (esp_video 2.1.0 src/esp_video_init.c:256-262). */
        const int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
        if (fd < 0) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            close(fd);
        }
    }
    if (err != ESP_OK) {
        (void)esp_video_deinit_with_flags(INIT_FLAGS); /* so the next call can try again */
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

/* ---- esp_video's ISP task on core 0 ----
 * esp_video creates its ISP pipeline task ("isp_task", priority 11) without a core (esp_video
 * 2.1.0 src/esp_video_isp_pipeline.c:1393-1401); IDF's xTaskCreate/xTaskCreateStatic are inline
 * calls of the PinnedToCore functions with tskNO_AFFINITY. Core 1 belongs to EtherCAT, so the
 * linker routes those two functions through here (--wrap in CMakeLists.txt) and that task is
 * pinned to core 0. Every other task is created exactly as asked. */

#define ISP_TASK_NAME "isp_task"

BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t stack,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out,
                                          const BaseType_t core);
TaskHandle_t __real_xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t stack,
                                                  void *const arg, UBaseType_t prio, StackType_t *const stack_buf,
                                                  StaticTask_t *const tcb, const BaseType_t core);

static BaseType_t core_for(const char *name, BaseType_t core)
{
    return core == tskNO_AFFINITY && name && strcmp(name, ISP_TASK_NAME) == 0 ? 0 : core;
}

BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t stack,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out,
                                          const BaseType_t core)
{
    return __real_xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core_for(name, core));
}

TaskHandle_t __wrap_xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t stack,
                                                  void *const arg, UBaseType_t prio, StackType_t *const stack_buf,
                                                  StaticTask_t *const tcb, const BaseType_t core)
{
    return __real_xTaskCreateStaticPinnedToCore(fn, name, stack, arg, prio, stack_buf, tcb, core_for(name, core));
}
