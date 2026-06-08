#include "surface.h"
#include "silkscreen.h"

/* Palm LCD surface geometry (fixed by the device/ROM). The silkscreen overlay
 * sits at palm-y 160.., so REGION_SILK subtracts PALM_H to index silk rows. */
#define SURFACE_PALM_H       160
#define SURFACE_PALM_STRIDE  (LAYOUT_PALM_W / 8)   /* 20 bytes/row */

/* Hot kernel — kept static inline so it inlines into surface_fill_row exactly as
 * the old display.c region_sample inlined into region_load. */
static inline int sample(const region_t *r, const uint8_t *screen_fb,
                         const uint8_t *silk_bmp, int palm_x, int palm_y)
{
    if (r->kind == REGION_SILK) {
        int sy = palm_y - SURFACE_PALM_H;          /* silk row 0..SILK_H-1 */
        uint8_t byte = silk_bmp[sy * SILK_STRIDE + (palm_x >> 3)];
        return (byte >> (7 - (palm_x & 7))) & 1;
    }
    if (r->kind != REGION_SCREEN) return 0;        /* forward-safe: REGION_ACTION etc. */
    uint8_t byte = screen_fb[palm_y * SURFACE_PALM_STRIDE + (palm_x >> 3)];
    return (byte >> (7 - (palm_x & 7))) & 1;
}

int surface_sample(const region_t *r, const uint8_t *screen_fb,
                   const uint8_t *silk_bmp, int palm_x, int palm_y)
{
    return sample(r, screen_fb, silk_bmp, palm_x, palm_y);
}

void surface_fill_row(const region_t *r, const uint8_t *screen_fb,
                      const uint8_t *silk_bmp, int row,
                      uint8_t *out, int *out_w)
{
    int box_w, box_h;
    layout_region_box(r, &box_w, &box_h);
    (void)box_h;                                   /* row is supplied by the caller */
    for (int ox = 0; ox < box_w; ox++) {
        int palm_x, palm_y;
        layout_region_panel_to_palm(r, r->panel_x + ox, r->panel_y + row,
                                    &palm_x, &palm_y);   /* always in-box */
        out[ox] = sample(r, screen_fb, silk_bmp, palm_x, palm_y) ? 0x00 : 0xFF;
    }
    *out_w = box_w;
}
