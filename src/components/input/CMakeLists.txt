if(CONFIG_TOUCH_XPT2046)
    set(physical_input "input_xpt2046.c" "resistive_touch.c")
else()
    set(physical_input "input.c" "touch_config.c")
endif()

idf_component_register(
    SRCS ${physical_input} "input_events.c"
    INCLUDE_DIRS "include"
    REQUIRES dragonball layout config board
    PRIV_REQUIRES m5_sd esp_driver_spi esp_driver_i2c esp_driver_gpio
)
