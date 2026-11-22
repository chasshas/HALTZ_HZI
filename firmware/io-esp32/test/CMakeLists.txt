# PC tests for the I/O board's portable code (sound sequencer, LED patterns). No ESP-IDF.
#   cmake -S firmware/io-esp32/test -B build-io-test
#   cmake --build build-io-test
#   ./build-io-test/io_fx_tests
cmake_minimum_required(VERSION 3.13)
project(haltz_io_tests C)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../shared/haltz_proto haltz_proto)

add_executable(io_fx_tests test_fx.c ../main/tunes.c ../main/ledfx.c)
target_include_directories(io_fx_tests PRIVATE ../main)
target_link_libraries(io_fx_tests haltz_proto)
set_target_properties(io_fx_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(io_fx_tests PRIVATE -Wall -Wextra)
