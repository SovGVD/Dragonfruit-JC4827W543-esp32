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
