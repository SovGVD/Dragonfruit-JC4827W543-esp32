#pragma once
#include "sdkconfig.h"

#if CONFIG_BOARD_JC4827W543
#define BOARD_NAME "JC4827W543"
#define BOARD_PANEL_W 480
#define BOARD_PANEL_H 272
#define BOARD_LCD_CS 45
#define BOARD_LCD_CLK 47
#define BOARD_LCD_D0 21
#define BOARD_LCD_D1 48
#define BOARD_LCD_D2 40
#define BOARD_LCD_D3 39
#define BOARD_LCD_BL 1
#define BOARD_SD_CS 10
#define BOARD_SD_CLK 12
#define BOARD_SD_MISO 13
#define BOARD_SD_MOSI 11
#define BOARD_TOUCH_CS 38
#define BOARD_TOUCH_SDA 8
#define BOARD_TOUCH_SCL 4
#define BOARD_TOUCH_INT 3
#define BOARD_TOUCH_RST 38
#define BOARD_BTN_POWER 0
/* On-board I2S class-D amplifier and speaker connector. */
#define BOARD_HAS_AUDIO 1
#define BOARD_I2S_BCLK 42
#define BOARD_I2S_LRCLK 2
#define BOARD_I2S_DOUT 41
#else
#define BOARD_NAME "M5Paper"
#define BOARD_PANEL_W 960
#define BOARD_PANEL_H 540
#define BOARD_SD_CS 4
#define BOARD_TOUCH_SDA 21
#define BOARD_TOUCH_SCL 22
#define BOARD_TOUCH_INT 36
#define BOARD_TOUCH_RST -1
#define BOARD_BTN_POWER 38
#endif
