#include "layout.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR    /* host builds */
#endif

static const char *TAG = "layout";

/* The IT8951 4bpp pack-write requires panel-X starts and load-area widths to be
 * multiples of 4 (see layout_region_box and it8951.c). Snap an X start DOWN to
 * the 4-pixel grid; round a width UP so content is never clipped. */
#define PANEL_X_ALIGN_DOWN(x) ((x) & ~3)
#define PANEL_X_ALIGN_UP(x)   (((x) + 3) & ~3)

/* ------------------------------------------------------------------ *
 *  Coordinate transform
 *
 *  A region maps source pixels of the Palm surface to the panel through a
 *  rotation + per-axis integer scale + translation. Source-local coords:
 *      u = palm_x - r->palm_x   (palm-x axis, 0..palm_w-1)
 *      v = palm_y - r->palm_y   (palm-y axis, 0..palm_h-1)
 *  Scaled extents along each PALM axis (independent of rotation):
 *      ew = palm_w * scale_x        eh = palm_h * scale_y
 *  Rotation is counter-clockwise. The stock portrait layout is ROT_90:
 *      panel X = panel_x + v*scale_y
 *      panel Y = panel_y + (ew - scale_x) - u*scale_x
 *  which reproduces the pre-refactor display.c map exactly:
 *      native_X = ROT_X_OFF + palm_y*SCALE
 *      native_Y = ROT_Y_OFF + (PALM_H*SCALE - SCALE) - palm_x*SCALE
 *  layout_region_panel_to_palm() is the exact inverse, matching the old
 *  input.c map. The four rotations follow the standard image-rotation
 *  formulas; ROT_90/ROT_270 swap the bounding-box axes (W<->H).
 * ------------------------------------------------------------------ */

static int scale_ceil(int value, int numerator, int denominator)
{
    return (value * numerator + denominator - 1) / denominator;
}

void layout_region_box(const region_t *r, int *box_w, int *box_h)
{
    int den = r->scale_den ? r->scale_den : 1;
    int ew = scale_ceil(r->palm_w, r->scale_x, den);
    int eh = scale_ceil(r->palm_h, r->scale_y, den);
    int w, h;
    if (r->rot == ROT_90 || r->rot == ROT_270) { w = eh; h = ew; }
    else                                        { w = ew; h = eh; }
    /* The IT8951 4bpp pack-write needs the load-area width (along the panel X /
     * long axis) to be a multiple of 4. Round UP: the extra 1..3 columns map
     * (via the inverse) to the clamped source edge, so content is never cut —
     * at worst a hair of edge duplication on a custom layout. Stock dims
     * (480/180/96/84/312) are already multiples of 4, so this is a no-op there.
     * Panel X start is snapped to a 4-boundary at compile time. */
    *box_w = PANEL_X_ALIGN_UP(w);
    *box_h = h;
}

void layout_src_to_panel(const region_t *r, int palm_x, int palm_y,
                         int *px, int *py)
{
    int u = palm_x - r->palm_x;
    int v = palm_y - r->palm_y;
    int den = r->scale_den ? r->scale_den : 1;
    int a = scale_ceil(u, r->scale_x, den);
    int b = scale_ceil(v, r->scale_y, den);
    int ew = scale_ceil(r->palm_w, r->scale_x, den);
    int eh = scale_ceil(r->palm_h, r->scale_y, den);
    int reverse_a = ew - scale_ceil(u + 1, r->scale_x, den);
    int reverse_b = eh - scale_ceil(v + 1, r->scale_y, den);

    switch (r->rot) {
    case ROT_0:   *px = r->panel_x + a;         *py = r->panel_y + b;         break;
    case ROT_90:  *px = r->panel_x + b;         *py = r->panel_y + reverse_a; break;
    case ROT_180: *px = r->panel_x + reverse_a; *py = r->panel_y + reverse_b; break;
    case ROT_270: *px = r->panel_x + reverse_b; *py = r->panel_y + a;         break;
    }
}

bool layout_region_panel_to_palm(const region_t *r, int panel_x, int panel_y,
                                 int *palm_x, int *palm_y)
{
    int box_w, box_h;
    layout_region_box(r, &box_w, &box_h);

    int dx = panel_x - r->panel_x;
    int dy = panel_y - r->panel_y;
    if (dx < 0 || dy < 0 || dx >= box_w || dy >= box_h)
        return false;

    int den = r->scale_den ? r->scale_den : 1;
    int ew = scale_ceil(r->palm_w, r->scale_x, den);
    int eh = scale_ceil(r->palm_h, r->scale_y, den);
    int u, v;

    switch (r->rot) {
    case ROT_0:   u = dx * den / r->scale_x;            v = dy * den / r->scale_y;            break;
    case ROT_90:  u = (ew - 1 - dy) * den / r->scale_x; v = dx * den / r->scale_y;            break;
    case ROT_180: u = (ew - 1 - dx) * den / r->scale_x; v = (eh - 1 - dy) * den / r->scale_y; break;
    case ROT_270: u = dy * den / r->scale_x;            v = (eh - 1 - dx) * den / r->scale_y; break;
    default:      return false;
    }

    if (u < 0) u = 0; else if (u >= r->palm_w) u = r->palm_w - 1;
    if (v < 0) v = 0; else if (v >= r->palm_h) v = r->palm_h - 1;
    *palm_x = r->palm_x + u;
    *palm_y = r->palm_y + v;
    return true;
}

/* ------------------------------------------------------------------ *
 *  Active layout
 * ------------------------------------------------------------------ */

static region_t s_regions[LAYOUT_MAX_REGIONS];
static int      s_region_count = 0;
static uint16_t s_panel_w = 960, s_panel_h = 540;

int             layout_region_count(void)   { return s_region_count; }
const region_t *layout_region(int i)
{
    return (i >= 0 && i < s_region_count) ? &s_regions[i] : NULL;
}
uint16_t layout_panel_w(void) { return s_panel_w; }
uint16_t layout_panel_h(void) { return s_panel_h; }

bool layout_panel_to_palm(int panel_x, int panel_y, int *palm_x, int *palm_y)
{
    for (int i = 0; i < s_region_count; i++) {
        const region_t *r = &s_regions[i];
        if (r->kind == REGION_ACTION)        /* reserved: not a sampled surface */
            continue;
        if (layout_region_panel_to_palm(r, panel_x, panel_y, palm_x, palm_y))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 *  Default layout — stock centered portrait, 90 deg CCW, scale 3.
 *
 *  The Palm screen+silkscreen form one seamless rotated band centered on the
 *  panel. We model it as two regions so the dynamic LCD (refreshed per frame)
 *  is separate from the static silkscreen (drawn once): contiguous on the
 *  panel, they reproduce the pre-refactor geometry exactly.
 *
 *  With SCALE=3 the band is PALM_FULL_H*SCALE = 660 px wide along the panel
 *  long axis and PALM_W*SCALE = 480 px tall: panel_x of the screen region is
 *  ((960-660)/2)&~3 = 148 (4-aligned, matching the old ROT_X_OFF) and panel_y
 *  is (540-480)/2 = 30 (old ROT_Y_OFF). The silk region abuts it at
 *  148 + PALM_H*SCALE = 628 (old SILK_X_OFF). Both are 4-aligned: 148, 628,
 *  and the box widths 480/180 are multiples of 4.
 * ------------------------------------------------------------------ */
#define DEF_SCALE   3
#define LAYOUT_MAX_SCALE 16

static void layout_use_compact(uint16_t panel_w, uint16_t panel_h);

void layout_use_default(uint16_t panel_w, uint16_t panel_h)
{
    if (panel_w == 480 && panel_h == 272) {
        layout_use_compact(panel_w, panel_h);
        return;
    }
    if (panel_w < 660 || panel_h < 480) {
        layout_use_default_upright(panel_w, panel_h);
        return;
    }
    s_panel_w = panel_w;
    s_panel_h = panel_h;

    int block_w = LAYOUT_PALM_FULL_H * DEF_SCALE;     /* 660 along panel long axis */
    int band_h  = LAYOUT_PALM_W * DEF_SCALE;          /* 480 along panel short axis */
    int x_off   = PANEL_X_ALIGN_DOWN(((int)panel_w - block_w) / 2); /* 4-aligned -> 148 */
    int y_off   = ((int)panel_h - band_h) / 2;         /* -> 30 */

    region_t screen = {
        .kind = REGION_SCREEN,
        .palm_x = 0, .palm_y = 0, .palm_w = LAYOUT_PALM_W, .palm_h = LAYOUT_PALM_H,
        .panel_x = (int16_t)x_off, .panel_y = (int16_t)y_off,
        .scale_x = DEF_SCALE, .scale_y = DEF_SCALE, .rot = ROT_90,
    };
    region_t silk = {
        .kind = REGION_SILK,
        .palm_x = 0, .palm_y = LAYOUT_PALM_H, .palm_w = LAYOUT_PALM_W, .palm_h = LAYOUT_SILK_H,
        .panel_x = (int16_t)(x_off + LAYOUT_PALM_H * DEF_SCALE),   /* 628 */
        .panel_y = (int16_t)y_off,
        .scale_x = DEF_SCALE, .scale_y = DEF_SCALE, .rot = ROT_90,
    };

    s_regions[0] = screen;
    s_regions[1] = silk;
    s_region_count = 2;

    ESP_LOGI(TAG, "default layout: screen @%d,%d  silk @%d,%d  (panel %ux%u)",
             screen.panel_x, screen.panel_y, silk.panel_x, silk.panel_y,
             panel_w, panel_h);
}

/* Upright (ROT_0) portrait: the 160x160 LCD stacked directly above the 160x60
 * silkscreen, both at the same integer scale, centered on the panel. Unlike the
 * device default this applies NO rotation, so the Palm screen reads the natural
 * way up — used by the host UI window (a desktop reads/clicks upright). Still two
 * regions through the shared engine, so layout_panel_to_palm / surface sampling /
 * 4-alignment all behave exactly as for any other layout. The caller picks the
 * panel size; the scale is the largest that fits 160x220 within it. */
void layout_use_default_upright(uint16_t panel_w, uint16_t panel_h)
{
    s_panel_w = panel_w;
    s_panel_h = panel_h;

    int sc_w = panel_w / LAYOUT_PALM_W;
    int sc_h = panel_h / LAYOUT_PALM_FULL_H;
    int sc   = sc_w < sc_h ? sc_w : sc_h;
    if (sc < 1)                sc = 1;
    if (sc > LAYOUT_MAX_SCALE) sc = LAYOUT_MAX_SCALE;

    int total_w = LAYOUT_PALM_W * sc;                  /* 160*sc */
    int total_h = LAYOUT_PALM_FULL_H * sc;             /* 220*sc */
    int x_off   = PANEL_X_ALIGN_DOWN(((int)panel_w - total_w) / 2); /* 4-align X (device parity) */
    int y_off   = ((int)panel_h - total_h) / 2;
    if (x_off < 0) x_off = 0;
    if (y_off < 0) y_off = 0;

    region_t screen = {
        .kind = REGION_SCREEN,
        .palm_x = 0, .palm_y = 0, .palm_w = LAYOUT_PALM_W, .palm_h = LAYOUT_PALM_H,
        .panel_x = (int16_t)x_off, .panel_y = (int16_t)y_off,
        .scale_x = (uint8_t)sc, .scale_y = (uint8_t)sc, .rot = ROT_0,
    };
    region_t silk = {
        .kind = REGION_SILK,
        .palm_x = 0, .palm_y = LAYOUT_PALM_H, .palm_w = LAYOUT_PALM_W, .palm_h = LAYOUT_SILK_H,
        .panel_x = (int16_t)x_off,
        .panel_y = (int16_t)(y_off + LAYOUT_PALM_H * sc),
        .scale_x = (uint8_t)sc, .scale_y = (uint8_t)sc, .rot = ROT_0,
    };

    s_regions[0] = screen;
    s_regions[1] = silk;
    s_region_count = 2;

    ESP_LOGI(TAG, "upright layout: screen @%d,%d  silk @%d,%d  scale %d  (panel %ux%u)",
             screen.panel_x, screen.panel_y, silk.panel_x, silk.panel_y, sc,
             panel_w, panel_h);
}

/* ------------------------------------------------------------------ *
 *  Config-driven layouts (INI)
 *
 *  Users never write raw rects: they place named ELEMENTS, each of which has a
 *  fixed Palm source rect (below). Button names are POSITIONAL (top-left etc.),
 *  not by function — the keycode/graphic at each cell differs per device. The
 *  source rects sit inside the ROM pen-button cells, so a tap on a relocated
 *  button still maps back to Palm coords the ROM dispatches. All source dims are
 *  multiples of 4, so box dims = dim*scale stay 4-aligned for any scale.
 * ------------------------------------------------------------------ */
typedef struct {
    const char   *name;
    region_kind_t kind;
    int16_t       px, py, pw, ph;     /* fixed Palm source rect */
} element_def_t;

enum { EL_SCREEN, EL_GRAFFITI, EL_TL, EL_BL, EL_TR, EL_BR, EL_DIVIDER, EL_COUNT };

/* Every element's Palm source rect is DISJOINT from every other's, so no element
 * can ever sample a pixel that belongs to a different concern (e.g. a button
 * grabbing the divider rule). The silkscreen surface (palm rows 160..219) is
 * partitioned cleanly:
 *   divider  rows 160..163 (full width)  — the rule under the LCD, its own element
 *   buttons  rows 164..219 in the side columns (x 0..27 and 132..159)
 *   graffiti rows 164..219 in the centre  (x 28..131)
 * The button cells start at 164 (matching the ROM pen-button cells) so all four
 * are the same height and none include the divider; the divider is a first-class
 * element the layout/config places, hides, or moves like any other. */
static const element_def_t ELEMENTS[EL_COUNT] = {
    [EL_SCREEN]   = { "screen",           REGION_SCREEN,   0,   0, 160, 160 },
    [EL_GRAFFITI] = { "graffiti",         REGION_SILK,    28, 164, 104,  56 },
    [EL_TL]       = { "btn-top-left",     REGION_SILK,     0, 164,  28,  28 },
    [EL_BL]       = { "btn-bottom-left",  REGION_SILK,     0, 192,  28,  28 },
    [EL_TR]       = { "btn-top-right",    REGION_SILK,   132, 164,  28,  28 },
    [EL_BR]       = { "btn-bottom-right", REGION_SILK,   132, 192,  28,  28 },
    [EL_DIVIDER]  = { "divider",          REGION_SILK,     0, 160, 160,   4 },
};

typedef struct {
    bool         present;
    int16_t      panel_x, panel_y;
    uint8_t      scale_x, scale_y;
    region_rot_t rot;
} elem_place_t;

/* Stock placement of one element: scale 3, ROT_90, positioned exactly where the
 * unified default band puts it (so the "custom" base reconstructs the stock
 * image as all 7 elements). Mirrors the default-band math in layout_use_default():
 *   panel X = X_OFF + py*3            (palm-y runs along panel +X)
 *   panel Y = Y_OFF + (480-3) - (px+pw-1)*3   (palm-x runs up panel -Y) */
static void stock_place(uint16_t panel_w, uint16_t panel_h, int e, elem_place_t *o)
{
    int block_w = LAYOUT_PALM_FULL_H * DEF_SCALE;     /* 660 */
    int band_h  = LAYOUT_PALM_W * DEF_SCALE;          /* 480 */
    int x_off   = PANEL_X_ALIGN_DOWN(((int)panel_w - block_w) / 2);
    int y_off   = ((int)panel_h - band_h) / 2;
    const element_def_t *d = &ELEMENTS[e];

    o->present = true;
    o->scale_x = DEF_SCALE;
    o->scale_y = DEF_SCALE;
    o->rot     = ROT_90;
    o->panel_x = (int16_t)(x_off + d->py * DEF_SCALE);
    o->panel_y = (int16_t)(y_off + (band_h - DEF_SCALE) - (d->px + d->pw - 1) * DEF_SCALE);
}

/* --- tiny INI helpers (dependency-free) --- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int find_element(const char *name)
{
    for (int i = 0; i < EL_COUNT; i++)
        if (strcmp(name, ELEMENTS[i].name) == 0) return i;
    return -1;
}

/* Parse a strict integer: true only if `v` is a clean number with no trailing
 * garbage (v is already trimmed of surrounding whitespace by the caller). */
static bool parse_int(const char *v, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0') return false;
    *out = (int)n;
    return true;
}

/* Parse a rotation value. Returns the ROT_* enum, or -1 if `v` is not exactly
 * one of 0/90/180/270 (non-numeric, trailing garbage, or unsupported angle).
 * Strict: a malformed `rotate` rejects the whole config (see layout_load). */
static int parse_rot(const char *v)
{
    int d;
    if (!parse_int(v, &d)) return -1;
    switch (d) { case 0: return ROT_0; case 90: return ROT_90;
                 case 180: return ROT_180; case 270: return ROT_270; }
    return -1;
}

/* Build the elem_place_t[] base for a preset. Returns false for an unknown
 * preset name. "default" is handled by the caller (2-region fast path). */
static bool preset_base(const char *preset, uint16_t panel_w, uint16_t panel_h,
                        elem_place_t place[EL_COUNT])
{
    for (int i = 0; i < EL_COUNT; i++) stock_place(panel_w, panel_h, i, &place[i]);

    if (strcmp(preset, "jc4827w543-landscape") == 0) {
        /* Upright LCD on the left; enlarged Graffiti + four buttons on the
         * right. All geometry stays inside the 480x272 native landscape panel. */
        for (int e = 0; e < EL_COUNT; e++) {
            place[e].present = e != EL_DIVIDER;
            place[e].rot = ROT_0;
            place[e].scale_x = place[e].scale_y = 2;
        }
        place[EL_SCREEN].scale_x = place[EL_SCREEN].scale_y = 1;
        place[EL_SCREEN].panel_x = 16;
        place[EL_SCREEN].panel_y = 56;
        place[EL_GRAFFITI].panel_x = 224;
        place[EL_GRAFFITI].panel_y = 112;
        const int buttons[] = { EL_TL, EL_BL, EL_TR, EL_BR };
        for (int k = 0; k < 4; k++) {
            place[buttons[k]].panel_x = 208 + k * 64;
            place[buttons[k]].panel_y = 40;
        }
        return true;
    }

    if (strcmp(preset, "custom") == 0)
        return true;                       /* stock positions, fully overridable */

    if (strcmp(preset, "landscape-right") == 0 ||
        strcmp(preset, "landscape-left") == 0) {
        /* Screen upright (ROT_0) filling the panel height on one side; the silk
         * area fills the rest. Within the silk area: the four buttons sit in a
         * single horizontal row across the top, evenly spaced, and the Graffiti
         * area is stretched to fill the width below them. left/right picks which
         * side the silk sits. */
        bool right = (strcmp(preset, "landscape-right") == 0);
        const int margin = 4, gap = 8;
        int sc = (panel_h - 2 * margin) / LAYOUT_PALM_H;   /* screen scale to fit height */
        if (sc < 1) sc = 1;
        if (sc > LAYOUT_MAX_SCALE) sc = LAYOUT_MAX_SCALE;
        int screen_w = LAYOUT_PALM_W * sc;
        int screen_h = LAYOUT_PALM_H * sc;
        int screen_y = (panel_h - screen_h) / 2;

        /* Carve the panel into the screen block and the silk area beside it. */
        int screen_x, silk_x0, silk_w;
        if (right) {                                   /* silk on the right */
            screen_x = margin;
            silk_x0  = screen_x + screen_w + gap;
            silk_w   = panel_w - silk_x0 - margin;
        } else {                                       /* silk on the left */
            screen_x = panel_w - screen_w - margin;
            silk_x0  = margin;
            silk_w   = screen_x - gap - silk_x0;
        }
        screen_x &= ~3; silk_x0 &= ~3;

        place[EL_SCREEN].rot = ROT_0;
        place[EL_SCREEN].scale_x = place[EL_SCREEN].scale_y = (uint8_t)sc;
        place[EL_SCREEN].panel_x = (int16_t)screen_x;
        place[EL_SCREEN].panel_y = (int16_t)screen_y;

        /* The silk side is one vertical BLOCK: a horizontal button row directly
         * above the Graffiti area, the two as a unit centered vertically against
         * the screen. Buttons sit over (and span) the Graffiti's width, not the
         * whole silk width, so they read as belonging to it.
         *
         * Size the Graffiti first (it drives the block width). Magnify it
         * UNIFORMLY (equal X/Y, keeping its proportions) to the largest integer
         * scale that fits the silk width AND the height left after the button
         * row. All four buttons are 28 Palm rows tall. */
        place[EL_DIVIDER].present = false;                  /* no rule in this layout */
        int btn_sc = 3;
        int btn_row_h = 28 * btn_sc;                        /* button cell height */

        int g_avail_h = screen_h - btn_row_h - gap;
        int g_fit_x = silk_w / ELEMENTS[EL_GRAFFITI].pw;
        int g_fit_y = g_avail_h / ELEMENTS[EL_GRAFFITI].ph;
        int g_sc = g_fit_x < g_fit_y ? g_fit_x : g_fit_y;  /* uniform */
        if (g_sc < 1) g_sc = 1;
        if (g_sc > LAYOUT_MAX_SCALE) g_sc = LAYOUT_MAX_SCALE;
        int g_w = ELEMENTS[EL_GRAFFITI].pw * g_sc;
        int g_h = ELEMENTS[EL_GRAFFITI].ph * g_sc;

        /* Center the [button row | gap | graffiti] block vertically. */
        int block_h  = btn_row_h + gap + g_h;
        int block_top = screen_y + (screen_h - block_h) / 2;
        int g_x = PANEL_X_ALIGN_DOWN(silk_x0 + (silk_w - g_w) / 2);  /* graffiti centered in silk */

        /* Buttons: evenly spaced across the graffiti width, directly above it,
         * sharing a common top edge. ROT_0 row, so the array IS the left->right
         * on-screen order: Home, Find, Calc, Menu. */
        const int row[4] = { EL_TL, EL_BR, EL_TR, EL_BL };
        int cell = g_w / 4;
        for (int k = 0; k < 4; k++) {
            int e  = row[k];
            int bw = ELEMENTS[e].pw * btn_sc;
            int cx = g_x + cell * k + cell / 2;
            place[e].rot = ROT_0;
            place[e].scale_x = place[e].scale_y = (uint8_t)btn_sc;
            place[e].panel_x = (int16_t)PANEL_X_ALIGN_DOWN(cx - bw / 2);
            place[e].panel_y = (int16_t)block_top;
        }

        place[EL_GRAFFITI].rot = ROT_0;
        place[EL_GRAFFITI].scale_x = (uint8_t)g_sc;
        place[EL_GRAFFITI].scale_y = (uint8_t)g_sc;
        place[EL_GRAFFITI].panel_x = (int16_t)g_x;
        place[EL_GRAFFITI].panel_y = (int16_t)(block_top + btn_row_h + gap);
        return true;
    }

    if (strcmp(preset, "buttons-top") == 0) {
        /* PORTRAIT stack — held the same way as the default (the long 960 panel
         * edge vertical), Palm content rotated 90 deg CCW (ROT_90) so it reads
         * upright. Three bands run top-to-bottom DOWN the long axis:
         *   emulated screen  ->  horizontal button row  ->  full-width Graffiti
         * The trio is one block, centered along the long axis when there is
         * spare room. The short (540) axis is the on-screen WIDTH.
         *
         * Under ROT_90 a region's panel bounding box is:
         *   vert  (extent down the long / panel-X axis) = palm_h * scale
         *   wide  (extent across the short / panel-Y axis) = palm_w * scale
         * The screen (160 wide) is the widest element and sets the group width;
         * buttons spread across it and Graffiti is magnified UNIFORMLY to match.
         * panel_w is the long (vertical) axis here, panel_h the short (width). */
        const int margin = 8, gap = 12;

        int scr_sc = (panel_h - 2 * margin) / LAYOUT_PALM_W;   /* fit screen width */
        if (scr_sc < 1) scr_sc = 1;
        if (scr_sc > LAYOUT_MAX_SCALE) scr_sc = LAYOUT_MAX_SCALE;
        int scr_vert = LAYOUT_PALM_H * scr_sc;
        int grp_w    = LAYOUT_PALM_W * scr_sc;                 /* group width (short axis) */

        /* Divider rule between the screen and the button row, spanning the full
         * group width: scaled at the screen scale so its 160-px length fills the
         * width, with thickness 4*scr_sc down the long axis. */
        int div_sc = scr_sc;
        int div_vert = ELEMENTS[EL_DIVIDER].ph * div_sc;       /* 4 * scr_sc */
        int div_wide = ELEMENTS[EL_DIVIDER].pw * div_sc;       /* == grp_w */

        int btn_sc = 3;
        int btn_vert = 28 * btn_sc;                            /* button cell height */

        int g_sc = grp_w / ELEMENTS[EL_GRAFFITI].pw;           /* fill the group width */
        if (g_sc < 1) g_sc = 1;
        if (g_sc > LAYOUT_MAX_SCALE) g_sc = LAYOUT_MAX_SCALE;
        int g_vert = ELEMENTS[EL_GRAFFITI].ph * g_sc;
        int g_wide = ELEMENTS[EL_GRAFFITI].pw * g_sc;

        /* Center the [screen | divider | buttons | graffiti] block along the
         * long axis (each band separated by `gap`). */
        int block_len = scr_vert + gap + div_vert + gap + btn_vert + gap + g_vert;
        int x = (panel_w - block_len) / 2;
        if (x < margin) x = margin;
        int cy = panel_h / 2;                                  /* width center */

        place[EL_SCREEN].rot = ROT_90;
        place[EL_SCREEN].scale_x = place[EL_SCREEN].scale_y = (uint8_t)scr_sc;
        place[EL_SCREEN].panel_x = (int16_t)x;
        place[EL_SCREEN].panel_y = (int16_t)(cy - grp_w / 2);

        place[EL_DIVIDER].present = true;
        place[EL_DIVIDER].rot = ROT_90;
        place[EL_DIVIDER].scale_x = place[EL_DIVIDER].scale_y = (uint8_t)div_sc;
        place[EL_DIVIDER].panel_x = (int16_t)(x + scr_vert + gap);
        place[EL_DIVIDER].panel_y = (int16_t)(cy - div_wide / 2);

        /* Buttons: a row across the group width, sharing the same band; evenly
         * spaced in cells, between the divider and the Graffiti. Under ROT_90 the
         * on-screen order is the REVERSE of increasing panel_y, so the array is
         * reversed to read Home, Find, Calc, Menu across the row. */
        int btn_x = x + scr_vert + gap + div_vert + gap;
        const int row[4] = { EL_BL, EL_TR, EL_BR, EL_TL };
        int cell = grp_w / 4;
        for (int k = 0; k < 4; k++) {
            int e  = row[k];
            int wide = ELEMENTS[e].pw * btn_sc;
            int ccy  = (cy - grp_w / 2) + cell * k + cell / 2;
            place[e].rot = ROT_90;
            place[e].scale_x = place[e].scale_y = (uint8_t)btn_sc;
            place[e].panel_x = (int16_t)btn_x;
            place[e].panel_y = (int16_t)(ccy - wide / 2);
        }

        place[EL_GRAFFITI].rot = ROT_90;
        place[EL_GRAFFITI].scale_x = (uint8_t)g_sc;
        place[EL_GRAFFITI].scale_y = (uint8_t)g_sc;
        place[EL_GRAFFITI].panel_x = (int16_t)(btn_x + btn_vert + gap);
        place[EL_GRAFFITI].panel_y = (int16_t)(cy - g_wide / 2);
        return true;
    }

    return false;                          /* unknown preset */
}

/* Compile the placement table into the active region list, validating as we go.
 * Returns false (without touching the active layout) if anything is unusable. */
static bool compile_and_validate(elem_place_t place[EL_COUNT],
                                 uint16_t panel_w, uint16_t panel_h)
{
    region_t out[LAYOUT_MAX_REGIONS];
    int n = 0;

    for (int i = 0; i < EL_COUNT; i++) {
        if (!place[i].present) continue;
        if (place[i].scale_x < 1 || place[i].scale_x > LAYOUT_MAX_SCALE ||
            place[i].scale_y < 1 || place[i].scale_y > LAYOUT_MAX_SCALE) {
            ESP_LOGE(TAG, "%s: scale out of range", ELEMENTS[i].name);
            return false;
        }
        region_t r = {
            .kind = ELEMENTS[i].kind,
            .palm_x = ELEMENTS[i].px, .palm_y = ELEMENTS[i].py,
            .palm_w = ELEMENTS[i].pw, .palm_h = ELEMENTS[i].ph,
            .panel_x = (int16_t)PANEL_X_ALIGN_DOWN(place[i].panel_x),   /* 4-align the X start */
            .panel_y = place[i].panel_y,
            .scale_x = place[i].scale_x, .scale_y = place[i].scale_y,
            .rot = place[i].rot,
        };
        int bw, bh;
        layout_region_box(&r, &bw, &bh);
        if (r.panel_x < 0 || r.panel_y < 0 ||
            r.panel_x + bw > panel_w || r.panel_y + bh > panel_h) {
            ESP_LOGE(TAG, "%s: %dx%d @%d,%d falls off the %ux%u panel",
                     ELEMENTS[i].name, bw, bh, r.panel_x, r.panel_y, panel_w, panel_h);
            return false;
        }
        out[n++] = r;
    }
    if (n == 0) { ESP_LOGE(TAG, "layout has no regions"); return false; }

    /* Overlap is allowed (input takes the first region, draw order the last) but
     * warned, since it is usually a mistake. */
    for (int a = 0; a < n; a++) {
        int aw, ah; layout_region_box(&out[a], &aw, &ah);
        for (int b = a + 1; b < n; b++) {
            int bw, bh; layout_region_box(&out[b], &bw, &bh);
            bool sep = out[a].panel_x + aw <= out[b].panel_x ||
                       out[b].panel_x + bw <= out[a].panel_x ||
                       out[a].panel_y + ah <= out[b].panel_y ||
                       out[b].panel_y + bh <= out[a].panel_y;
            if (!sep)
                ESP_LOGW(TAG, "layout: regions %d and %d overlap on the panel", a, b);
        }
    }

    memcpy(s_regions, out, sizeof(region_t) * n);
    s_region_count = n;
    s_panel_w = panel_w;
    s_panel_h = panel_h;
    return true;
}

static void layout_use_compact(uint16_t panel_w, uint16_t panel_h)
{
    /* Portrait with USB at the bottom. Keep the LCD and full silkscreen as
     * one continuous band, rotated clockwise in native landscape coordinates.
     * 160x220 at 1:1, centered on the 480x272 native panel. */
    const int numerator = 1, den = 1;
    int band_w = scale_ceil(LAYOUT_PALM_FULL_H, numerator, den);
    int band_h = scale_ceil(LAYOUT_PALM_W, numerator, den);
    int silk_w = scale_ceil(LAYOUT_SILK_H, numerator, den);
    int left = PANEL_X_ALIGN_DOWN(((int)panel_w - band_w) / 2);
    int top = ((int)panel_h - band_h) / 2;
    if (left < 0 || top < 0) {
        layout_use_default_upright(panel_w, panel_h);
        return;
    }
    s_regions[0] = (region_t){
        .kind = REGION_SCREEN,
        .palm_w = LAYOUT_PALM_W, .palm_h = LAYOUT_PALM_H,
        .panel_x = left + silk_w, .panel_y = top,
        .scale_x = numerator, .scale_y = numerator, .scale_den = den,
        .rot = ROT_270,
    };
    s_regions[1] = (region_t){
        .kind = REGION_SILK,
        .palm_y = LAYOUT_PALM_H, .palm_w = LAYOUT_PALM_W, .palm_h = LAYOUT_SILK_H,
        .panel_x = left, .panel_y = top,
        .scale_x = numerator, .scale_y = numerator, .scale_den = den,
        .rot = ROT_270,
    };
    s_region_count = 2;
    s_panel_w = panel_w; s_panel_h = panel_h;
    ESP_LOGI(TAG, "JC portrait layout: LCD 160x160 (1:1), USB at bottom");
}

bool layout_load(const char *cfg_text, uint16_t panel_w, uint16_t panel_h)
{
    if (!cfg_text) { layout_use_default(panel_w, panel_h); return false; }

    /* The parser tokenizes in place, so work on a mutable copy. One static
     * buffer (not two on the stack — this runs on the limited main task stack),
     * recopied for each of the two passes; layout_load is a one-shot boot call. */
    static EXT_RAM_BSS_ATTR char work[LAYOUT_CFG_MAX];
    size_t len = strlen(cfg_text);
    if (len >= sizeof(work)) {
        ESP_LOGE(TAG, "config too large (%u bytes); using default", (unsigned)len);
        layout_use_default(panel_w, panel_h);
        return false;
    }

    /* Pass 1: find the preset, and note whether any element section appears. */
    char preset[24] = "";
    bool any_element = false;
    {
        memcpy(work, cfg_text, len + 1);
        int cur = -1;
        for (char *line = strtok(work, "\n"); line; line = strtok(NULL, "\n")) {
            char *p = line;
            char *hash = strpbrk(p, "#;"); if (hash) *hash = '\0';
            p = trim(p);
            if (!*p) continue;
            if (*p == '[') {
                char *close = strchr(p, ']'); if (!close) continue;
                *close = '\0';
                char *sec = trim(p + 1);
                cur = find_element(sec);
                if (cur >= 0) any_element = true;
            } else {
                char *eq = strchr(p, '='); if (!eq) continue;
                *eq = '\0';
                char *k = trim(p), *v = trim(eq + 1);
                if (strcmp(k, "preset") == 0) {
                    strncpy(preset, v, sizeof(preset) - 1);
                    preset[sizeof(preset) - 1] = '\0';
                }
                (void)cur;
            }
        }
    }
    if (!preset[0]) strcpy(preset, any_element ? "custom" : "default");

    if (strcmp(preset, "jc4827w543") == 0 || strcmp(preset, "jc4827w543-portrait") == 0) {
        layout_use_compact(panel_w, panel_h);
        return true;
    }

    if (strcmp(preset, "default") == 0) {
        layout_use_default(panel_w, panel_h);
        return true;                       /* a valid, explicit default */
    }

    elem_place_t place[EL_COUNT];
    if (!preset_base(preset, panel_w, panel_h, place)) {
        ESP_LOGE(TAG, "unknown preset '%s'; using default", preset);
        layout_use_default(panel_w, panel_h);
        return false;
    }

    /* Snapshot which elements the preset left visible and which the config gives
     * an explicit place=, so we can warn about re-showing a preset-hidden
     * element at its (stale) stock position without repositioning it. */
    bool preset_present[EL_COUNT];
    bool place_overridden[EL_COUNT] = { false };
    for (int i = 0; i < EL_COUNT; i++) preset_present[i] = place[i].present;

    /* Pass 2: apply per-element overrides (fresh copy — pass 1 chewed up work).
     * Strict: a malformed value on a KNOWN key (bad scale/rotate/place/hide)
     * sets `invalid` and rejects the whole config -> default. Unknown sections
     * and unknown keys stay tolerated (forward-compat); see layout.h. */
    memcpy(work, cfg_text, len + 1);
    int cur = -1;
    bool invalid = false;
    for (char *line = strtok(work, "\n"); line; line = strtok(NULL, "\n")) {
        char *p = line;
        char *hash = strpbrk(p, "#;"); if (hash) *hash = '\0';
        p = trim(p);
        if (!*p) continue;
        if (*p == '[') {
            char *close = strchr(p, ']'); if (!close) continue;
            *close = '\0';
            cur = find_element(trim(p + 1));
            continue;
        }
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = '\0';
        char *k = trim(p), *v = trim(eq + 1);
        if (cur < 0) continue;             /* global keys (preset) already handled */

        if (strcmp(k, "hide") == 0) {
            int b;
            if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0)       place[cur].present = false;
            else if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0)  place[cur].present = true;
            else if (parse_int(v, &b))                                 place[cur].present = (b == 0);
            else { ESP_LOGE(TAG, "%s: bad hide value '%s'", ELEMENTS[cur].name, v); invalid = true; }
        } else if (strcmp(k, "place") == 0) {
            int x, y;
            if (sscanf(v, "%d , %d", &x, &y) == 2 || sscanf(v, "%d %d", &x, &y) == 2) {
                place[cur].panel_x = (int16_t)x; place[cur].panel_y = (int16_t)y;
                place_overridden[cur] = true;
            } else { ESP_LOGE(TAG, "%s: bad place value '%s'", ELEMENTS[cur].name, v); invalid = true; }
        } else if (strcmp(k, "rotate") == 0) {
            int r = parse_rot(v);
            if (r < 0) { ESP_LOGE(TAG, "%s: bad rotate value '%s'", ELEMENTS[cur].name, v); invalid = true; }
            else place[cur].rot = (region_rot_t)r;
        } else if (strcmp(k, "scale") == 0 || strcmp(k, "scale_x") == 0 ||
                   strcmp(k, "scale_y") == 0) {
            int s;
            if (!parse_int(v, &s) || s < 1 || s > LAYOUT_MAX_SCALE) {
                ESP_LOGE(TAG, "%s: bad %s value '%s' (1..%d)", ELEMENTS[cur].name, k, v, LAYOUT_MAX_SCALE);
                invalid = true;
            } else {
                if (k[5] != '_')      { place[cur].scale_x = (uint8_t)s; place[cur].scale_y = (uint8_t)s; }
                else if (k[6] == 'x')   place[cur].scale_x = (uint8_t)s;
                else                    place[cur].scale_y = (uint8_t)s;
            }
        }
    }

    if (invalid) {
        ESP_LOGE(TAG, "config has a malformed value; using default layout");
        layout_use_default(panel_w, panel_h);
        return false;
    }

    /* A preset-hidden element re-shown without its own place= sits at the stale
     * stock (portrait) position, which usually overlaps the active layout. The
     * overlap check below warns generically; name the likely cause here too. */
    for (int i = 0; i < EL_COUNT; i++)
        if (!preset_present[i] && place[i].present && !place_overridden[i])
            ESP_LOGW(TAG, "%s re-shown without place=; using stock position, may overlap",
                     ELEMENTS[i].name);

    if (!compile_and_validate(place, panel_w, panel_h)) {
        ESP_LOGE(TAG, "config invalid; using default layout");
        layout_use_default(panel_w, panel_h);
        return false;
    }
    ESP_LOGI(TAG, "loaded layout preset '%s' (%d regions)", preset, s_region_count);
    return true;
}
