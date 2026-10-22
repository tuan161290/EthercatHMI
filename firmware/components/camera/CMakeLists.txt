idf_component_register(
    SRCS "camera.c" "camera_push.c" "camera_ts.c" "camera_h264_mem.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp-tls esp_driver_jpeg esp_driver_ppa esp_http_client esp_http_server esp_timer machine mbedtls net
)

# The H.264 encoder's large buffers go to PSRAM instead of internal RAM (camera_h264_mem.c). Only
# esp_h264 calls the __wrap_ functions, and its library comes after this one: -u pulls them in.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_h264_aligned_calloc"
    "-Wl,--wrap=esp_h264_calloc_prefer"
    "-u __wrap_esp_h264_aligned_calloc"
    "-u __wrap_esp_h264_calloc_prefer")
