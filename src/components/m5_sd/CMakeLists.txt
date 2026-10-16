idf_component_register(
    SRCS "m5_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES sdmmc board
    PRIV_REQUIRES fatfs esp_driver_sdspi esp_driver_spi esp_driver_gpio freertos
)
