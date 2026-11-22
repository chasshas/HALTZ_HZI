# PC tests for the GPU's portable core (picture state, renderer, scenes). No Pico SDK.
#   cmake -S firmware/gpu-pico/test -B build-gpu-test
#   cmake --build build-gpu-test
#   ./build-gpu-test/gpu_render_tests [png_dir]
# With png_dir it also writes boot.png and demo frames, to look at the result.
cmake_minimum_required(VERSION 3.13)
project(haltz_gpu_tests C)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../shared/haltz_proto haltz_proto)

add_executable(gpu_render_tests
    test_render.c
    render_reference.c
    ../src/gpu_state.c
    ../src/gpu_render.c
    ../src/gpu_scenes.c)
target_include_directories(gpu_render_tests PRIVATE ../src)
target_link_libraries(gpu_render_tests haltz_proto)
set_target_properties(gpu_render_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(gpu_render_tests PRIVATE -Wall -Wextra)
