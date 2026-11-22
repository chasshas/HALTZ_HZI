# gpu_state.c is the GPU firmware's portable picture state (no Pico SDK calls): the S3 uses it
# for its shadow copy of the GPU picture (docs/gpu-protocol-v1.md sections 2 and 6).
idf_component_register(SRCS "main.c" "wiring_test.c"
                            "gpu.c" "gpu_replay.c" "gpu_bringup.c"
                            "console.c" "con_term.c"
                            "../../gpu-pico/src/gpu_state.c"
                       INCLUDE_DIRS "../include"
                       REQUIRES haltz_proto esp_psram spi_flash esp_partition esp_timer vfs
                                esp_driver_gpio esp_driver_spi esp_driver_uart
                                esp_driver_usb_serial_jtag)
