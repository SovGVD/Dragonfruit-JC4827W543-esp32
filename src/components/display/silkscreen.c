#include "silkscreen.h"
#include <string.h>

/* silk coords: sx = palm x (0..159), sy = silkscreen row = palm y - 160 (0..59).
 * Button rects (palm coords, from the ROM's pen-button table); names here are by
 * position, not function (the keycode/graphic differs per device):
 *   top-left 0,164,27,28   bottom-left 0,192,27,28
 *   top-right 133,164,27,28  bottom-right 133,192,27,28 */
static void silk_set(uint8_t *bmp, int sx, int sy, int on)
{
    if (sx < 0 || sx >= SILK_W || sy < 0 || sy >= SILK_H) return;
    uint8_t m = 0x80 >> (sx & 7);
    if (on) bmp[sy * SILK_STRIDE + (sx >> 3)] |= m;
    else    bmp[sy * SILK_STRIDE + (sx >> 3)] &= ~m;
}
static void silk_hline(uint8_t *bmp, int x0, int x1, int y) { for (int x = x0; x <= x1; x++) silk_set(bmp, x, y, 1); }
static void silk_vline(uint8_t *bmp, int x, int y0, int y1) { for (int y = y0; y <= y1; y++) silk_set(bmp, x, y, 1); }
static void silk_rect(uint8_t *bmp, int x0, int y0, int x1, int y1)
{
    silk_hline(bmp, x0, x1, y0); silk_hline(bmp, x0, x1, y1);
    silk_vline(bmp, x0, y0, y1); silk_vline(bmp, x1, y0, y1);
}
static void silk_fill(uint8_t *bmp, int x0, int y0, int x1, int y1) { for (int y = y0; y <= y1; y++) silk_hline(bmp, x0, x1, y); }

/* Small ~16px icons, centered in each button cell. */
static void silk_icon_home(uint8_t *bmp, int cx, int cy)        /* house */
{
    for (int d = 0; d <= 6; d++) { silk_set(bmp, cx - d, cy - 5 + d, 1); silk_set(bmp, cx + d, cy - 5 + d, 1); }
    silk_rect(bmp, cx - 4, cy + 1, cx + 4, cy + 7);
    silk_fill(bmp, cx - 1, cy + 4, cx + 1, cy + 7);             /* door */
}
static void silk_icon_menu(uint8_t *bmp, int cx, int cy)        /* three bars */
{
    silk_rect(bmp, cx - 7, cy - 6, cx + 7, cy + 6);
    silk_hline(bmp, cx - 5, cx + 5, cy - 3);
    silk_hline(bmp, cx - 5, cx + 5, cy);
    silk_hline(bmp, cx - 5, cx + 5, cy + 3);
}
static void silk_icon_calc(uint8_t *bmp, int cx, int cy)        /* calculator */
{
    silk_rect(bmp, cx - 5, cy - 7, cx + 5, cy + 7);
    silk_hline(bmp, cx - 3, cx + 3, cy - 4);                    /* display */
    for (int gy = 0; gy < 3; gy++)
        for (int gx = 0; gx < 3; gx++)
            silk_set(bmp, cx - 3 + gx * 3, cy - 1 + gy * 3, 1); /* keypad dots */
}
static void silk_icon_find(uint8_t *bmp, int cx, int cy)        /* magnifier */
{
    int r = 4, ox = cx - 1, oy = cy - 2;
    int x = r, y = 0, e = 1 - r;                                /* midpoint circle */
    while (x >= y) {
        silk_set(bmp, ox + x, oy + y, 1); silk_set(bmp, ox + y, oy + x, 1);
        silk_set(bmp, ox - x, oy + y, 1); silk_set(bmp, ox - y, oy + x, 1);
        silk_set(bmp, ox - x, oy - y, 1); silk_set(bmp, ox - y, oy - x, 1);
        silk_set(bmp, ox + x, oy - y, 1); silk_set(bmp, ox + y, oy - x, 1);
        y++; if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; }
    }
    for (int d = 1; d <= 4; d++) silk_set(bmp, ox + r - 1 + d, oy + r - 1 + d, 1);  /* handle */
}

void silkscreen_build(uint8_t *bmp)
{
    memset(bmp, 0, SILK_BYTES);
    silk_hline(bmp, 0, SILK_W - 1, 0);         /* divider line under the LCD */

    /* Graffiti writing area (central column) with the letter/number split and
     * the four corner keyboard-activation dots. */
    silk_rect(bmp, 28, 4, 131, 59);
    silk_vline(bmp, 98, 4, 59);
    silk_fill(bmp, 30, 6, 31, 7);   silk_fill(bmp, 128, 6, 129, 7);
    silk_fill(bmp, 30, 57, 31, 58); silk_fill(bmp, 128, 57, 129, 58);

    /* Buttons, centered in their ROM-defined cells (palm y -> silk y is -160).
     * Home 0,164 / Menu 0,192 on the left; Calc 133,164 / Find 133,192 right. */
    silk_icon_home(bmp, 13, 164 - 160 + 14);
    silk_icon_menu(bmp, 13, 192 - 160 + 14);
    silk_icon_calc(bmp, 146, 164 - 160 + 14);
    silk_icon_find(bmp, 146, 192 - 160 + 14);
}
