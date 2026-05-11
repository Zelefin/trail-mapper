idf_component_register(SRCS "trail-mapper.c"
                    INCLUDE_DIRS "."
                    REQUIRES fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio esp_driver_uart esp_timer)
