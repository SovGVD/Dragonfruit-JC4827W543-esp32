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

static inline int sample_index(const region_t *r, const uint8_t *screen_fb, int bpp,
                               const uint8_t *silk_bmp, int palm_x, int palm_y)
{
    if (r->kind == REGION_SILK) {
        int sy = palm_y - SURFACE_PALM_H;
        uint8_t byte = silk_bmp[sy * SILK_STRIDE + (palm_x >> 3)];
        return (byte >> (7 - (palm_x & 7))) & 1;
    }
    if (r->kind != REGION_SCREEN || !screen_fb) return 0;
    if (bpp == 8) {
        return screen_fb[palm_y * LAYOUT_PALM_W + palm_x];
    } else if (bpp == 4) {
        uint8_t byte = screen_fb[palm_y * (LAYOUT_PALM_W / 2) + (palm_x >> 1)];
        return (byte >> ((1 - (palm_x & 1)) * 4)) & 0x0F;
    } else if (bpp == 2) {
        uint8_t byte = screen_fb[palm_y * (LAYOUT_PALM_W / 4) + (palm_x >> 2)];
        return (byte >> ((3 - (palm_x & 3)) * 2)) & 0x03;
    } else {
        uint8_t byte = screen_fb[palm_y * SURFACE_PALM_STRIDE + (palm_x >> 3)];
        return (byte >> (7 - (palm_x & 7))) & 0x01;
    }
}

int surface_sample_index(const region_t *r, const uint8_t *screen_fb, int bpp,
                         const uint8_t *silk_bmp, int palm_x, int palm_y)
{
    return sample_index(r, screen_fb, bpp, silk_bmp, palm_x, palm_y);
}

void surface_fill_row_rgb565_be(const region_t *r, const uint8_t *screen_fb,
                                int bpp, const uint16_t *clut_rgb565,
                                const uint8_t *silk_bmp, int row,
                                uint8_t *out_rgb565_be, int *out_w)
{
    int box_w, box_h;
    layout_region_box(r, &box_w, &box_h);
    (void)box_h;
    for (int ox = 0; ox < box_w; ox++) {
        int palm_x, palm_y;
        layout_region_panel_to_palm(r, r->panel_x + ox, r->panel_y + row,
                                    &palm_x, &palm_y);
        uint16_t c;
        if (r->kind == REGION_SCREEN && clut_rgb565) {
            int idx = sample_index(r, screen_fb, bpp, silk_bmp, palm_x, palm_y);
            c = clut_rgb565[idx & 0xFF];
        } else {
            int ink = sample_index(r, screen_fb, 1, silk_bmp, palm_x, palm_y);
            c = ink ? 0x0000u : 0xFFFFu;
        }
        out_rgb565_be[ox * 2]     = (uint8_t)(c >> 8);
        out_rgb565_be[ox * 2 + 1] = (uint8_t)c;
    }
    *out_w = box_w;
}

void surface_fill_row_rgba(const region_t *r, const uint8_t *screen_fb,
                           int bpp, const uint8_t clut_rgb[256][3],
                           const uint8_t *silk_bmp, int row,
                           uint8_t *out_rgba, int *out_w)
{
    int box_w, box_h;
    layout_region_box(r, &box_w, &box_h);
    (void)box_h;
    for (int ox = 0; ox < box_w; ox++) {
        int palm_x, palm_y;
        layout_region_panel_to_palm(r, r->panel_x + ox, r->panel_y + row,
                                    &palm_x, &palm_y);
        uint8_t *d = out_rgba + ox * 4;
        if (r->kind == REGION_SCREEN && clut_rgb) {
            int idx = sample_index(r, screen_fb, bpp, silk_bmp, palm_x, palm_y) & 0xFF;
            d[0] = clut_rgb[idx][0];
            d[1] = clut_rgb[idx][1];
            d[2] = clut_rgb[idx][2];
            d[3] = 0xFF;
        } else {
            uint8_t g = sample_index(r, screen_fb, 1, silk_bmp, palm_x, palm_y) ? 0x00 : 0xFF;
            d[0] = g; d[1] = g; d[2] = g; d[3] = 0xFF;
        }
    }
    *out_w = box_w;
}
