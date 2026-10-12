idf_component_register(
    SRCS "net.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event lwip machine
)

# Nothing else references net.c's constructor (it pins esp_hosted's tasks to core 0) before
# main calls net_start(); keep the object anyway so the constructor can never be dropped.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u net_start")
