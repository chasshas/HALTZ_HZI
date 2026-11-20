idf_component_register(SRCS "main.c" "wiring_test.c"
                       INCLUDE_DIRS "../include"
                       REQUIRES haltz_proto esp_psram spi_flash esp_partition esp_timer
                                esp_driver_gpio esp_driver_spi esp_driver_uart)
