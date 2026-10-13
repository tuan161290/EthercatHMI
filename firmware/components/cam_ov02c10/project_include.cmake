# Image tuning (IPA) for the ISP pipeline controller, as the vendor's esp_cam_sensor
# project_include.cmake adds it. Only the file for P4 silicon below v3 is kept: every image of
# this project targets v1.x (sdkconfig.defaults, CLAUDE.md "Chip revision").
if(CONFIG_CAMERA_OV02C10)
    if(CONFIG_CAMERA_OV02C10_DEFAULT_IPA_JSON_CONFIGURATION_FILE)
        if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
            idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH
                                   "${COMPONENT_PATH}/cfg/ov02c10_default_p4_eco4.json" APPEND)
        else()
            message(FATAL_ERROR "cam_ov02c10: copy sensors/ov02c10/cfg/ov02c10_default_p4_eco5.json from the "
                                "vendor esp_cam_sensor (VENDOR.md) into cfg/ for P4 v3.x")
        endif()
    elseif(CONFIG_CAMERA_OV02C10_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH
                               ${CONFIG_CAMERA_OV02C10_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH} APPEND)
    endif()
endif()
