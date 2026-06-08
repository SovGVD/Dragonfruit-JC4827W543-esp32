idf_component_register(
    SRCS "m5_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES sdmmc
    PRIV_REQUIRES fatfs esp_driver_sdspi freertos
)
