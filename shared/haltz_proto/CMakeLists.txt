# haltz_proto: shared HALTZ protocol library (plain C11, no platform headers).
#
# Three ways to use it:
#   ESP-IDF (cpu-s3, io-esp32): listed in EXTRA_COMPONENT_DIRS, becomes component "haltz_proto".
#   Pico SDK (gpu-pico):        add_subdirectory(), then link "haltz_proto".
#   PC:                         cmake -S shared/haltz_proto -B build-host -DHALTZ_BUILD_TESTS=ON
#
# Firmware projects can include(cmake/haltz_git_hash.cmake) to get the build's git hash.

if(ESP_PLATFORM)
  idf_component_register(SRCS src/packet.c src/crc8.c src/msgs.c src/gpu_msgs.c src/gpu_tables.c src/io_msgs.c
                         INCLUDE_DIRS include)
else()
  cmake_minimum_required(VERSION 3.13)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(haltz_proto C)  # standalone PC build
  endif()

  add_library(haltz_proto STATIC src/packet.c src/crc8.c src/msgs.c src/gpu_msgs.c src/gpu_tables.c src/io_msgs.c)
  target_include_directories(haltz_proto PUBLIC include)
  set_target_properties(haltz_proto PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_options(haltz_proto PRIVATE -Wall -Wextra)
  option(HALTZ_BUILD_TESTS "Build PC unit tests" OFF)
  if(HALTZ_BUILD_TESTS)
    add_executable(haltz_proto_tests test/test_main.c)
    target_link_libraries(haltz_proto_tests haltz_proto)
    set_target_properties(haltz_proto_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(haltz_proto_tests PRIVATE -Wall -Wextra)
  endif()
endif()
