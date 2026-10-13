idf_component_register(
    SRCS "board_eth.c" "board_display.c" "board_i2c.c" "board_rtc.c" "board_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth esp_event esp_lcd
    PRIV_REQUIRES esp_driver_ledc esp_driver_i2c esp_hw_support cam_ov02c10
)

# esp_video's ISP task goes to core 0 (board_camera.c): its two task-create calls come here first.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore"
                      "-Wl,--wrap=xTaskCreateStaticPinnedToCore")
