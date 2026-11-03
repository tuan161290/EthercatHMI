# General App: the EtherCAT HMI without a machine sequence. A machine app is a copy of this folder
# with its own project name (docs/SETUP.md, "App mới").
cmake_minimum_required(VERSION 3.16)

# Components shared by every app; this folder adds only main/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
# Settings shared by every app (firmware/sdkconfig.defaults), then this app's own, which win.
# A -D SDKCONFIG_DEFAULTS=... on the command line replaces both (e.g. a test build).
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
# The configuration lives in the build folder, so builds with other IDF versions or settings do
# not share one sdkconfig.
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build what main actually depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(ehmi_general)
