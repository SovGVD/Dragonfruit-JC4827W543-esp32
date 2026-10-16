#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
esp_err_t nv3041a_init(void);
/* Set a window once, then stream one RGB565 (big endian) row at a time. */
void nv3041a_window(int x, int y, int w, int h);
void nv3041a_write_row(const uint8_t *rgb565, int width);
void nv3041a_backlight(bool on);
