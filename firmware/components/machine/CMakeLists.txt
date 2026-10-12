# Machine data shared by the HMI and the (future) machine sequence: append-only flash store
# (machine_store.c), sequence timers, axis settings, teach points, wall clock and the alarm list.

idf_component_register(
    SRCS "machine_store.c" "machine_params.c" "machine_clock.c" "machine_alarm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES board nvs_flash esp_timer esp_partition freertos log heap
)
