idf_component_register(
    SRCS "hmi.c" "hmi_axis.c" "hmi_cpu.c" "hmi_display.c" "hmi_icon.c" "hmi_lv_mem.c"
         "hmi_page_alarms.c" "hmi_page_camera.c" "hmi_page_ethercat.c" "hmi_page_info.c" "hmi_page_interp.c" "hmi_page_io.c" "hmi_page_main.c" "hmi_page_manual.c" "hmi_page_network.c"
         "hmi_page_servo.c" "hmi_page_settings.c" "hmi_page_teach.c" "hmi_page_time.c" "hmi_page_todo.c"
         "hmi_screenshot.c" "hmi_shell.c" "hmi_theme.c" "hmi_touchlog.c" "hmi_widgets.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_timer nvs_flash ecat machine net camera esp_lcd esp_mm esp_app_format esp_psram spi_flash
    EMBED_FILES "fonts/OpenSans-Regular.ttf" "fonts/OpenSans-SemiBold.ttf"
)

# LVGL calls these allocator hooks (CONFIG_LV_USE_CUSTOM_MALLOC); nothing else references the
# object file, so make the linker take it from this library.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
