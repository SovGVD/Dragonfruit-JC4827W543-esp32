idf_component_register(
    SRCS "display.c" "it8951.c" "silkscreen.c" "surface.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bus dragonball layout
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer
)
