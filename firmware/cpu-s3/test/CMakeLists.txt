# PC tests for the CPU's portable GPU code (the shadow replay). No ESP-IDF.
#   cmake -S firmware/cpu-s3/test -B build-cpu-test
#   cmake --build build-cpu-test
#   ./build-cpu-test/cpu_gpu_tests
cmake_minimum_required(VERSION 3.13)
project(haltz_cpu_tests C)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../shared/haltz_proto haltz_proto)

add_executable(cpu_gpu_tests
    test_replay.c
    ../src/gpu_replay.c
    ../../gpu-pico/src/gpu_state.c)
target_include_directories(cpu_gpu_tests PRIVATE ../src)
target_link_libraries(cpu_gpu_tests haltz_proto)
set_target_properties(cpu_gpu_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(cpu_gpu_tests PRIVATE -Wall -Wextra)
