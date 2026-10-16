#pragma once

#include <stdint.h>
#include "layout.h"

/* Sink-agnostic region rasterizer shared by the device IT8951 path and the host
 * UI renderer. Given a region and the two Palm source surfaces — the live
 * 160x160 1bpp framebuffer (REGION_SCREEN) and the silkscreen overlay bitmap
 * (REGION_SILK) — it walks the region's panel bounding box through the SAME
 * inverse map the blitter and input.c use (layout_region_panel_to_palm), so the
 * geometry is identical across sinks.
 *
 * Pure C, no statics (re-entrant): the row buffer is caller-owned. The device
 * sink packs the grayscale row 4bpp for the controller; the host sink expands it
 * to RGB. A row-fill (rather than a per-pixel callback) keeps the hot inner loop
 * inlinable and each sink's consume pass sequential. */

/* One render-side sample: 1 = black ink, 0 = white. Non-SCREEN/non-SILK kinds
 * (e.g. a future REGION_ACTION) sample as blank. */
int surface_sample(const region_t *r, const uint8_t *screen_fb,
                   const uint8_t *silk_bmp, int palm_x, int palm_y);

/* Fill one panel ROW (panel-y offset `row`, 0..box_h-1) of the region's box with
 * 8-bit grayscale: 0x00 = ink, 0xFF = blank. `out` must hold >= box_w bytes; on
 * return *out_w = the region's panel box width. */
void surface_fill_row(const region_t *r, const uint8_t *screen_fb,
                      const uint8_t *silk_bmp, int row,
                      uint8_t *out, int *out_w);

/* Sample the palette index (0 .. (1<<bpp)-1) at (palm_x, palm_y) from an
 * N-bpp (1/2/4/8) REGION_SCREEN framebuffer, or 0/1 for REGION_SILK. */
int surface_sample_index(const region_t *r, const uint8_t *screen_fb, int bpp,
                         const uint8_t *silk_bmp, int palm_x, int palm_y);

/* Fill one panel ROW of the region's box with big-endian RGB565 pixels (2 bytes
 * per pixel) looked up through `clut_rgb565` for REGION_SCREEN (1/2/4/8 bpp) or
 * black/white for REGION_SILK. */
void surface_fill_row_rgb565_be(const region_t *r, const uint8_t *screen_fb,
                                int bpp, const uint16_t *clut_rgb565,
                                const uint8_t *silk_bmp, int row,
                                uint8_t *out_rgb565_be, int *out_w);

/* Fill one panel ROW of the region's box with RGBA32 pixels (4 bytes per pixel)
 * looked up through `clut_rgb` (256x3 R,G,B) for REGION_SCREEN or black/white
 * for REGION_SILK. */
void surface_fill_row_rgba(const region_t *r, const uint8_t *screen_fb,
                           int bpp, const uint8_t clut_rgb[256][3],
                           const uint8_t *silk_bmp, int row,
                           uint8_t *out_rgba, int *out_w);
