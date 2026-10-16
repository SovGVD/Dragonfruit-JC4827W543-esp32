if(CONFIG_BOARD_JC4827W543)
    set(board_display "display_jc4827w543.c" "nv3041a.c")
else()
    set(board_display "display.c" "it8951.c")
endif()

idf_component_register(
    SRCS ${board_display} "silkscreen.c" "surface.c" "message.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bus dragonball layout board
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer
)
