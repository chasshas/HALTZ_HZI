set(srcs
        "main.c"
        "sketch.cpp"
        "haltz_selftest.cpp"
        "haltz_wiring_test.c")

set(requires "bluepad32" "bluepad32_arduino" "arduino" "btstack" "haltz_proto"
        "esp_adc" "esp_driver_gpio" "esp_driver_ledc" "esp_driver_uart")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES "${requires}")
