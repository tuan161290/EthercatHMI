cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build what main actually depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(ethercat_hmi)
