# EtherCAT application layer on top of SOEM: device catalog (generated from ESI),
# topology Scan -> Accept -> Save, modular coupler configuration, CiA402 drives and axes,
# IO image and SDO jobs for the HMI, and the topology view for the HMI.
# Regenerate ecat_catalog_gen.c with tools/esi2catalog.py after adding an ESI file.

idf_component_register(
    SRCS
        "ecat_catalog.c"
        "ecat_catalog_gen.c"
        "ecat_topology.c"
        "ecat_topology_store.c"
        "ecat_scan.c"
        "ecat_coupler.c"
        "ecat_drive.c"
        "ecat_axis.c"
        "ecat_io.c"
        "ecat_sdo_service.c"
        "ecat_busview.c"
        "ecat_selftest.c"
    INCLUDE_DIRS "include"
    REQUIRES soem esp_common freertos
    PRIV_REQUIRES nvs_flash esp_rom log
)
