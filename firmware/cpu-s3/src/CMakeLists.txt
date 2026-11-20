idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "../include"
                       REQUIRES haltz_proto esp_psram spi_flash esp_partition esp_timer)
