#pragma once

#include <stdint.h>

/* Synthesized silkscreen overlay: a 160(wide) x 60(tall) 1bpp bitmap, 1 = black
 * ink. Palm-y of row r is 160+r; stride is SILK_STRIDE bytes.
 *
 * On a real Palm IIIx the silkscreen graphics (the Graffiti writing area and the
 * Home/Menu/Calc/Find buttons) are printed on the case, not part of the LCD
 * framebuffer, yet the digitizer treats them as one continuous surface with the
 * 160x160 LCD directly above. The emulator therefore draws its own static
 * overlay here and injects taps on it as ordinary pen points (see layout.h /
 * display.c).
 *
 * Pure C with no ESP / IT8951 / layout dependencies, so the device IT8951 render
 * path and the host UI renderer share one implementation. */
#define SILK_W       160
#define SILK_H        60
#define SILK_STRIDE  (SILK_W / 8)              /* 20 bytes/row */
#define SILK_BYTES   (SILK_H * SILK_STRIDE)    /* 1200 */

/* Render the overlay into `bmp` (must hold >= SILK_BYTES). Idempotent. */
void silkscreen_build(uint8_t *bmp);
