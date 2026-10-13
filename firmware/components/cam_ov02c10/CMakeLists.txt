# OV02C10 camera sensor driver from the Guition vendor package (VENDOR.md), registered with
# esp_cam_sensor's auto-detection so esp_video finds it on the MIPI-CSI port.
idf_component_register(
    SRCS "ov02c10.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_cam_sensor
    PRIV_REQUIRES esp_driver_gpio
)

# Nothing calls the detect function; esp_video finds it through the linker section that
# esp_cam_sensor's linker.lf collects from every library.
if(CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")
endif()
