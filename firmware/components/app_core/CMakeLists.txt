# What every app does (firmware/apps/*): machine data, the HMI, the EtherCAT master tasks
# (topology Scan -> Accept -> Save, OP, cyclic process data, slave recovery), alarms, Wi-Fi and
# the console. An app's main calls app_core_start() (include/app_core.h).

idf_component_register(
    SRCS "app_core.c" "app_console.c" "app_alarms.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES board hmi soem ecat machine net esp_timer esp_eth nvs_flash console
)
