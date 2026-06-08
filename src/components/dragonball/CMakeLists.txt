idf_component_register(
    SRCS
        "dragonball.c"
        "interrupt.c"
        "lcd.c"
        "timer.c"
        "rtc.c"
        "gpio.c"
        "spi.c"
        "uart.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)
