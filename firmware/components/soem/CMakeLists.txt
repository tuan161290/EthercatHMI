# SOEM (Simple Open EtherCAT Master) with an ESP-IDF port.
# Upstream sources are vendored unmodified under SOEM/ (see SOEM_UPSTREAM.md);
# everything platform specific lives in port/. port/ec_config.c is a patched copy
# of SOEM/src/ec_config.c and is compiled instead of it.

set(soem_dir SOEM)

idf_component_register(
    SRCS
        "${soem_dir}/src/ec_base.c"
        "${soem_dir}/src/ec_coe.c"
        "port/ec_config.c"
        "${soem_dir}/src/ec_dc.c"
        "${soem_dir}/src/ec_eoe.c"
        "${soem_dir}/src/ec_foe.c"
        "${soem_dir}/src/ec_main.c"
        "${soem_dir}/src/ec_print.c"
        "${soem_dir}/src/ec_soe.c"
        "port/osal.c"
        "port/oshw.c"
        "port/nicdrv.c"
        "port/pdo_layout.c"
    INCLUDE_DIRS
        "port/include"
        "port"
        "${soem_dir}/include"
        "${soem_dir}/osal"
    REQUIRES
        esp_eth
        esp_timer
        esp_driver_gptimer
)

# Upstream code is compiled as-is; silence the warnings it was never built against.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-format
)
