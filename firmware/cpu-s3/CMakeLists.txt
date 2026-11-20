cmake_minimum_required(VERSION 3.16)
# APPEND, not set(): PlatformIO passes its src/ dir in EXTRA_COMPONENT_DIRS.
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../shared/haltz_proto)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(haltz_cpu)
