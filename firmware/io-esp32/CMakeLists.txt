# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.13)

set(ENV{BTSTACK_ROOT} ${CMAKE_CURRENT_SOURCE_DIR}/external/btstack)

# HALTZ: shared protocol library as an extra ESP-IDF component
# APPEND, not set(): PlatformIO passes its src/ dir in EXTRA_COMPONENT_DIRS.
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../shared/haltz_proto)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{BTSTACK_ROOT}/port/esp32/components/btstack/cmake/BTstack_project.cmake)

project(bluepad32_arduino_app_template)
