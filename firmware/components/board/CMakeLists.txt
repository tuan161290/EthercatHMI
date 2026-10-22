idf_component_register(
    SRCS "board_eth.c" "board_display.c" "board_i2c.c" "board_rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth esp_event esp_lcd
    PRIV_REQUIRES esp_driver_ledc esp_driver_i2c esp_hw_support
)
