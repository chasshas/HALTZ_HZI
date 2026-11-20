cmake_minimum_required(VERSION 3.13)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(haltz_gpu C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../shared/haltz_proto haltz_proto)

add_executable(haltz_gpu src/main.c)
target_compile_options(haltz_gpu PRIVATE -Wall -Wextra)
target_link_libraries(haltz_gpu
    pico_stdlib pico_multicore
    hardware_spi hardware_dma hardware_pio hardware_pwm
    haltz_proto)
pico_enable_stdio_uart(haltz_gpu 1)   # UART0 on GP0/GP1 goes to the debug probe
pico_enable_stdio_usb(haltz_gpu 0)
pico_add_extra_outputs(haltz_gpu)

# First 8 hex chars of the git commit, or "00000000" without git.
include(${CMAKE_CURRENT_LIST_DIR}/../../shared/haltz_proto/cmake/haltz_git_hash.cmake)
haltz_git_hash_header(haltz_gpu)  # build/haltz_generated/haltz_git_hash.h
