# PC tests for the CPU's portable code (shadow replay, console terminal). No ESP-IDF.
#   cmake -S firmware/cpu-s3/test -B build-cpu-test
#   cmake --build build-cpu-test
#   ./build-cpu-test/cpu_gpu_tests
#   ./build-cpu-test/cpu_con_tests
cmake_minimum_required(VERSION 3.13)
project(haltz_cpu_tests C)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../shared/haltz_proto haltz_proto)

add_executable(cpu_gpu_tests
    test_replay.c
    ../src/gpu_replay.c
    ../../gpu-pico/src/gpu_state.c)

add_executable(cpu_con_tests
    test_con_term.c
    ../src/con_term.c
    ../../gpu-pico/src/gpu_state.c)

foreach(t cpu_gpu_tests cpu_con_tests)
  target_include_directories(${t} PRIVATE ../src)
  target_link_libraries(${t} haltz_proto)
  set_target_properties(${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
endforeach()
