#pragma once
#include <stdint.h>

/* Direct panel drawing for screens that are not the Palm framebuffer: the boot
 * menu and the Cybiko screen/keyboard. JC4827W543 (RGB565 LCD) only. Coordinates
 * are native panel pixels. Callers own the panel: the boot menu runs before any
 * task starts, and in Cybiko mode only the display task draws. */

/* Height of the built-in font in dots (glyphs are 5 dots wide plus a gap). */
#define UI_FONT_H 7

#define UI_RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

void display_ui_fill(int x, int y, int w, int h, uint16_t color);
/* Draws `s` in the built-in 5x7 font, `scale` pixels per font dot, with each
 * character cell (including the 1-dot gap) filled with `bg`. '\n' is not
 * interpreted. Returns the x just past the last cell. */
int display_ui_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg);
int display_ui_text_width(const char *s, int scale);
/* Stream an image: set a window once, then write `w` big-endian RGB565 pixels
 * per row, top to bottom. */
void display_ui_window(int x, int y, int w, int h);
void display_ui_write_row(const uint8_t *rgb565_be, int w);
