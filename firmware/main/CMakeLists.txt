idf_component_register(
    SRCS "main.c" "app_console.c" "app_alarms.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board hmi soem ecat machine net camera esp_timer esp_eth nvs_flash console
)
