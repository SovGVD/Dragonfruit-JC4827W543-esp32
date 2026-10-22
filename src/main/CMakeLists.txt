set(main_srcs "main.c" "debug_console.c" "serial_bridge.c")
if(CONFIG_BOARD_JC4827W543)
    list(APPEND main_srcs "boot_menu.c" "touch_calib.c")
endif()

idf_component_register(
    SRCS ${main_srcs}
    INCLUDE_DIRS "."
    REQUIRES board m68k bus dragonball display input esp_driver_gpio esp_driver_uart esp_adc rom_loader layout config installer ram_state m5_sd palm_card wifi_gateway audio cybiko_app nvs_flash esp_timer
)
