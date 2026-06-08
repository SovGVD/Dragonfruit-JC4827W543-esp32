idf_component_register(
    SRCS "input.c" "touch_config.c"
    INCLUDE_DIRS "include"
    REQUIRES dragonball layout config
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio
)
