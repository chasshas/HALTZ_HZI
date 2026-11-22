# HALTZ I/O firmware. sketch.cpp (the template's Arduino example) is not built: HALTZ runs
# Bluepad32 with its own platform (haltz_platform.c), docs/io-protocol-v1.md section 8.
set(srcs
        "main.c"
        "haltz_selftest.cpp"
        "haltz_wiring_test.c"
        "haltz_platform.c"
        "io_link.c"
        "io_input.c"
        "io_fx.c"
        "io_console.c"
        "tunes.c"
        "ledfx.c")

set(requires "bluepad32" "bluepad32_arduino" "arduino" "btstack" "haltz_proto"
        "esp_adc" "esp_driver_gpio" "esp_driver_ledc" "esp_driver_uart" "esp_timer"
        "console")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES "${requires}")
