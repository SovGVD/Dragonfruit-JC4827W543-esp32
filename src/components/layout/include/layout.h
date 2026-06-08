#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ *
 *  Screen layout engine — the single source of truth for the
 *  panel <-> Palm-digitizer coordinate mapping.
 *
 *  Historically display.c (render: palm -> panel) and input.c (touch:
 *  panel -> palm) each hardcoded the same 90 deg CCW / scale-3 / centered
 *  portrait transform and had to be kept byte-for-byte in sync by hand.
 *  This module owns that mapping once. A "layout" is an ordered list of
 *  "regions"; each region maps a rectangle of the logical 160x220 Palm
 *  surface to a placement on the IT8951 panel, with an independent
 *  rotation and (per-axis) integer magnification.
 *
 *  The unified Palm surface is 160 wide x 220 tall:
 *    - rows   0..159  = the live LCD framebuffer        (REGION_SCREEN)
 *    - rows 160..219  = the static silkscreen overlay   (REGION_SILK)
 *  On real hardware the silkscreen has NO LCD behind it (it is case-
 *  printed); our emulator synthesizes that overlay itself, so REGION_SILK
 *  always samples our drawn bitmap, never the framebuffer.
 *
 *  Palm-space semantics are fixed by the ROM (pen-button cells, Graffiti
 *  column), so a layout can only change WHERE on the panel a given Palm
 *  rectangle is drawn and touched — never the Palm coordinates a tap
 *  resolves to. Everything downstream of this module speaks Palm coords.
 * ------------------------------------------------------------------ */

/* Logical Palm surface dimensions (fixed by the device / ROM). */
#define LAYOUT_PALM_W       160
#define LAYOUT_PALM_H       160     /* LCD rows */
#define LAYOUT_SILK_H       60      /* silkscreen rows (palm y 160..219) */
#define LAYOUT_PALM_FULL_H  (LAYOUT_PALM_H + LAYOUT_SILK_H)   /* 220 */

typedef enum {
    REGION_SCREEN,   /* samples the live 160x160 framebuffer (dynamic refresh) */
    REGION_SILK,     /* samples the static silkscreen overlay (drawn at init/wake) */
    REGION_ACTION,   /* RESERVED: custom button — draws its own glyph and fires a
                      * discrete action on tap rather than sampling a surface */
} region_kind_t;

/* Rotation applied to the region's source content, counter-clockwise. The
 * stock portrait layout uses ROT_90 (Palm screen turned 90 deg CCW). */
typedef enum { ROT_0 = 0, ROT_90 = 1, ROT_180 = 2, ROT_270 = 3 } region_rot_t;

typedef struct {
    region_kind_t kind;

    /* Source: rectangle in the unified 160x220 Palm surface. */
    int16_t  palm_x, palm_y, palm_w, palm_h;

    /* Destination: top-left of the region's (post-rotation) panel bounding
     * box. panel_x and the bounding-box width MUST be multiples of 4 — the
     * IT8951 4bpp pack-write packs 4 px per word, so a non-aligned X start
     * shifts every row within its word and corrupts the image. The loader
     * enforces this. */
    int16_t  panel_x, panel_y;

    /* Integer magnification of the PALM axes (independent of rotation):
     * scale_x magnifies the palm-x dimension, scale_y the palm-y dimension. */
    uint8_t  scale_x, scale_y;

    region_rot_t rot;

    /* --- RESERVED for REGION_ACTION; zero/ignored today. --- */
    uint16_t act_ascii, act_keycode, act_modifiers;  /* EvtEnqueueKey payload */
    int16_t  act_tap_x, act_tap_y;                    /* or synthetic palm tap */
    uint8_t  glyph;
} region_t;

#define LAYOUT_MAX_REGIONS 16

/* Max size (bytes, incl. NUL) of a /palm.cfg the loader will accept. Generous
 * enough for a heavily-commented hand-edited config; the shipped example is
 * ~4 KB. Both the SD reader and the parser size their buffers to this. */
#define LAYOUT_CFG_MAX 8192

/* Install the built-in default layout (stock centered portrait, scale 3,
 * 90 deg CCW) for the given panel size. Reproduces the pre-refactor geometry
 * exactly. Always succeeds. */
void layout_use_default(uint16_t panel_w, uint16_t panel_h);

/* Install an upright (ROT_0) portrait layout: the 160x160 LCD stacked above the
 * 160x60 silkscreen at the largest integer scale that fits the panel, centered.
 * No rotation, so the Palm screen reads the natural way up — used by the host UI
 * window. Always succeeds. */
void layout_use_default_upright(uint16_t panel_w, uint16_t panel_h);

/* Parse + validate an INI-style config (NUL-terminated text) and install it.
 * On ANY error logs the reason and installs the default instead, returning
 * false — a bad config can never brick the device.
 *
 * Strictness rule: a malformed VALUE on a known key (bad scale/rotate/place/
 * hide, an off-panel region, an unknown preset) rejects the WHOLE file. But
 * unknown [sections] and unknown keys are tolerated and ignored, so a config
 * written for a future firmware (new element/key) still loads on older builds. */
bool layout_load(const char *cfg_text, uint16_t panel_w, uint16_t panel_h);

/* Iterate the active layout's regions (render side). */
int             layout_region_count(void);
const region_t *layout_region(int i);

uint16_t layout_panel_w(void);
uint16_t layout_panel_h(void);

/* The region's panel bounding box size (after rotation). Width is always a
 * multiple of 4 (the loader guarantees it). */
void layout_region_box(const region_t *r, int *box_w, int *box_h);

/* Render-side forward map: a source pixel (palm_x, palm_y) within `r` -> the
 * panel pixel (*px, *py) of the TOP-LEFT of its scaled block. (palm coords are
 * absolute, not region-local.)
 *
 * NOTE: not on the render hot path — the blitter walks the panel box and uses
 * the inverse map (layout_region_panel_to_palm) to sample. This forward map is
 * retained as the spec/oracle that the host round-trip self-test checks the
 * inverse against (and pins to the pre-refactor geometry); see host/test_layout.c. */
void layout_src_to_panel(const region_t *r, int palm_x, int palm_y,
                         int *px, int *py);

/* Per-region inverse map: panel pixel (panel_x, panel_y) -> Palm coords, IF it
 * falls inside this region's bounding box. Returns false otherwise. The
 * blitter walks a region's box and uses this to sample the source surface;
 * layout_panel_to_palm() is just this applied across all regions in order. */
bool layout_region_panel_to_palm(const region_t *r, int panel_x, int panel_y,
                                 int *palm_x, int *palm_y);

/* Input-side inverse map: a panel touch (panel_x, panel_y) -> Palm coords.
 * Searches regions in order; returns true and fills (*palm_x,*palm_y) for the
 * first containing REGION_SCREEN/REGION_SILK region, false if the touch lands
 * in dead space (caller ignores it). REGION_ACTION is reserved and skipped. */
bool layout_panel_to_palm(int panel_x, int panel_y, int *palm_x, int *palm_y);
