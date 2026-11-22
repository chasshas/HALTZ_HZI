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

# The GPU firmware (docs/gpu-protocol-v1.md). gpu_state/gpu_render/gpu_scenes are portable
# C with PC tests in test/ (they build without the Pico SDK).
add_executable(haltz_gpu
    src/main.c
    src/link.c
    src/tft.c
    src/gpu_state.c
    src/gpu_render.c
    src/gpu_scenes.c)
target_compile_options(haltz_gpu PRIVATE -Wall -Wextra)
target_compile_definitions(haltz_gpu PRIVATE
    PICO_STACK_SIZE=0x1000                 # core 0: 4 KB
    GPU_RENDER_IN_RAM=1                    # gpu_render.c runs from RAM, not the flash cache
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=2000) # a stalled terminal must not stall the link
target_link_libraries(haltz_gpu
    pico_stdlib pico_multicore
    hardware_spi hardware_dma hardware_irq hardware_sync hardware_pwm
    haltz_proto)
# Console on both: the board's own USB port (no extra wires; also lets picotool reboot it
# into BOOTSEL for flashing) and UART0 on GP0/GP1 (for a debug probe, if one is wired).
pico_enable_stdio_uart(haltz_gpu 1)
pico_enable_stdio_usb(haltz_gpu 1)
pico_add_extra_outputs(haltz_gpu)

# First 8 hex chars of the git commit, or "00000000" without git.
include(${CMAKE_CURRENT_LIST_DIR}/../../shared/haltz_proto/cmake/haltz_git_hash.cmake)
haltz_git_hash_header(haltz_gpu)  # build/haltz_generated/haltz_git_hash.h

# Wiring test firmware (src/wiring_test.c): flash build/haltz_gpu_wiring.uf2 to check the
# TFT (stage 2), the link to the S3 (stage 3) and the probe UART (stage 6).
add_executable(haltz_gpu_wiring src/wiring_test.c)
target_compile_options(haltz_gpu_wiring PRIVATE -Wall -Wextra)
target_link_libraries(haltz_gpu_wiring
    pico_stdlib pico_multicore hardware_spi hardware_pwm
    haltz_proto)
target_include_directories(haltz_gpu_wiring PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/haltz_generated")
pico_enable_stdio_uart(haltz_gpu_wiring 1)
pico_enable_stdio_usb(haltz_gpu_wiring 0)
pico_add_extra_outputs(haltz_gpu_wiring)
