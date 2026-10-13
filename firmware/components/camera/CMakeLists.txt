idf_component_register(
    SRCS "camera.c" "camera_push.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_driver_jpeg esp_driver_ppa esp_http_client esp_http_server esp_timer machine mbedtls net
)
