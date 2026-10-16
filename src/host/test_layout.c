/* Standalone numeric self-test for components/layout.
 *
 * Compile (from repo root), e.g.:
 *   gcc -I components/layout/include -I host host/test_layout.c \
 *       components/layout/layout.c -o host/test_layout && host/test_layout
 *
 * Verifies (1) the default layout reproduces the pre-refactor geometry exactly
 * and (2) the forward/inverse transforms round-trip for every rotation. */
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int find_kind(region_kind_t k) {
    for (int i = 0; i < layout_region_count(); i++)
        if (layout_region(i)->kind == k) return i;
    return -1;
}
/* graffiti region = the REGION_SILK with palm_x == 28 */
static int find_graffiti(void) {
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind == REGION_SILK && r->palm_x == 28) return i;
    }
    return -1;
}

/* The exact pre-refactor map from the old display.c (scale 3, ROT_90). */
#define OLD_SCALE 3
#define OLD_X_OFF 148
#define OLD_Y_OFF 30
#define OLD_SCALED_H (160 * OLD_SCALE)   /* 480 */

int main(void)
{
    layout_use_default(960, 540);

    /* --- default geometry --- */
    CHECK(layout_region_count() == 2, "region count %d != 2", layout_region_count());
    const region_t *screen = layout_region(0);
    const region_t *silk   = layout_region(1);
    CHECK(screen->kind == REGION_SCREEN, "region0 not SCREEN");
    CHECK(silk->kind == REGION_SILK, "region1 not SILK");
    CHECK(screen->panel_x == 148 && screen->panel_y == 30,
          "screen panel @%d,%d != 148,30", screen->panel_x, screen->panel_y);
    CHECK(silk->panel_x == 628 && silk->panel_y == 30,
          "silk panel @%d,%d != 628,30", silk->panel_x, silk->panel_y);
    CHECK(screen->rot == ROT_90 && screen->scale_x == 3 && screen->scale_y == 3,
          "screen rot/scale wrong");

    /* --- forward map matches the old display.c CCW formula for every LCD px --- */
    for (int py = 0; py < 160; py++) {
        for (int px = 0; px < 160; px++) {
            int X, Y;
            layout_src_to_panel(screen, px, py, &X, &Y);
            int wantX = OLD_X_OFF + py * OLD_SCALE;
            int wantY = OLD_Y_OFF + (OLD_SCALED_H - OLD_SCALE) - px * OLD_SCALE;
            if (X != wantX || Y != wantY) {
                CHECK(0, "fwd palm(%d,%d) -> (%d,%d) want (%d,%d)",
                      px, py, X, Y, wantX, wantY);
                px = py = 1000;   /* stop after first mismatch */
            }
        }
    }

    /* --- inverse map matches the old input.c formula on the screen band --- */
    for (int ey = 30; ey < 30 + 480; ey += 7) {
        for (int ex = 148; ex < 148 + 480; ex += 7) {
            int palm_x, palm_y;
            int ok = layout_region_panel_to_palm(screen, ex, ey, &palm_x, &palm_y);
            CHECK(ok, "inv screen miss @%d,%d", ex, ey);
            int wantY = (ex - OLD_X_OFF) / OLD_SCALE;                       /* palm_y */
            int wantX = (OLD_SCALED_H - 1 - (ey - OLD_Y_OFF)) / OLD_SCALE;  /* palm_x */
            CHECK(palm_x == wantX && palm_y == wantY,
                  "inv (%d,%d) -> palm(%d,%d) want (%d,%d)",
                  ex, ey, palm_x, palm_y, wantX, wantY);
        }
    }

    /* --- routing: a touch in the silk band resolves to palm_y >= 160 --- */
    {
        int palm_x, palm_y;
        int ok = layout_panel_to_palm(628 + 90, 30 + 240, &palm_x, &palm_y);
        CHECK(ok, "silk routing miss");
        CHECK(palm_y >= 160 && palm_y < 220, "silk routing palm_y=%d", palm_y);
    }

    /* --- round-trip for all four rotations on a representative region --- */
    for (int rot = ROT_0; rot <= ROT_270; rot++) {
        region_t r = { .kind = REGION_SILK, .palm_x = 28, .palm_y = 160,
                       .palm_w = 104, .palm_h = 60, .panel_x = 40, .panel_y = 300,
                       .scale_x = 4, .scale_y = 2, .rot = (region_rot_t)rot };
        for (int v = 0; v < r.palm_h; v++) {
            for (int u = 0; u < r.palm_w; u++) {
                int sx = r.palm_x + u, sy = r.palm_y + v;
                int X, Y, bx, by;
                layout_src_to_panel(&r, sx, sy, &X, &Y);
                int ok = layout_region_panel_to_palm(&r, X, Y, &bx, &by);
                if (!ok || bx != sx || by != sy) {
                    CHECK(0, "rot%d round-trip palm(%d,%d)->panel(%d,%d)->palm(%d,%d) ok=%d",
                          rot, sx, sy, X, Y, bx, by, ok);
                    u = v = 100000;
                }
            }
        }
    }

    /* ---- upright host layout: ROT_0 stack, screen above the silkscreen ---- */
    {
        layout_use_default_upright(480, 660);
        CHECK(layout_region_count() == 2, "upright region count %d != 2",
              layout_region_count());
        const region_t *us = layout_region(0);
        const region_t *uk = layout_region(1);
        CHECK(us->kind == REGION_SCREEN && uk->kind == REGION_SILK,
              "upright regions should be SCREEN then SILK");
        CHECK(us->rot == ROT_0 && uk->rot == ROT_0, "upright regions should be ROT_0");
        CHECK(us->scale_x == 3 && us->scale_y == 3,
              "upright scale should fit to 3 for 480x660 (got %d)", us->scale_x);
        CHECK(us->panel_x == uk->panel_x, "upright screen/silk should share panel_x");
        CHECK((us->panel_x & 3) == 0, "upright panel_x should be 4-aligned (%d)",
              us->panel_x);
        CHECK(uk->panel_y == us->panel_y + 160 * us->scale_y,
              "upright silk should abut below the screen (silk_y=%d screen_y=%d)",
              uk->panel_y, us->panel_y);
        int palm_x, palm_y;
        int ok = layout_panel_to_palm(uk->panel_x + 10, uk->panel_y + 10, &palm_x, &palm_y);
        CHECK(ok && palm_y >= 160 && palm_y < 220,
              "upright silk tap should map to palm_y in [160,220) (got %d)", palm_y);
        ok = layout_panel_to_palm(us->panel_x + 10, us->panel_y + 10, &palm_x, &palm_y);
        CHECK(ok && palm_y >= 0 && palm_y < 160,
              "upright screen tap should map to palm_y in [0,160) (got %d)", palm_y);
        layout_use_default(960, 540);   /* restore default for the config tests below */
    }

    /* ---------------- config parsing / presets ---------------- */

    /* NULL / empty -> default (2 regions, returns false = "used fallback"). */
    CHECK(layout_load(NULL, 960, 540) == false && layout_region_count() == 2,
          "NULL config should fall back to 2-region default");
    /* Empty text parses to no preset -> default; that is a valid (not error)
     * outcome, so it returns true with the 2-region default installed. */
    CHECK(layout_load("", 960, 540) == true && layout_region_count() == 2,
          "empty config should yield the 2-region default");

    /* Explicit preset=default -> 2 regions, returns true (valid choice). */
    CHECK(layout_load("preset = default\n", 960, 540) == true &&
          layout_region_count() == 2, "preset=default should be 2 valid regions");

    /* preset=custom -> 7 regions reconstructing the stock band (screen, graffiti,
     * 4 buttons, divider); screen @148,30. */
    CHECK(layout_load("preset = custom\n", 960, 540) == true &&
          layout_region_count() == 7, "custom should expand to 7 regions");
    {
        int s = find_kind(REGION_SCREEN);
        CHECK(s >= 0 && layout_region(s)->panel_x == 148 &&
              layout_region(s)->panel_y == 30,
              "custom screen should sit at the stock 148,30");
    }

    /* Override a per-axis scale. An element section with no preset implies
     * custom. (Stock graffiti is ROT_90, where scale_x scales the palm-x axis
     * -> panel-vertical; scale_x=4 still fits, so this is a clean override.) */
    CHECK(layout_load("[graffiti]\nscale_x = 4\n", 960, 540) == true,
          "element-only config should load as custom");
    {
        int g = find_graffiti();
        CHECK(g >= 0 && layout_region(g)->scale_x == 4,
              "graffiti scale_x override should be 4");
    }

    /* hide drops an element. */
    CHECK(layout_load("preset=custom\n[btn-top-left]\nhide=true\n", 960, 540) == true &&
          layout_region_count() == 6, "hide should drop one region (7->6)");

    /* A relocated element: tap inside its panel box maps back into its palm
     * source rect (so the ROM still dispatches it). Place graffiti upright. */
    CHECK(layout_load("[graffiti]\nplace = 100,300\nrotate = 0\nscale = 3\n",
                      960, 540) == true, "relocate graffiti load");
    {
        int g = find_graffiti();
        CHECK(g >= 0, "graffiti present after relocate");
        const region_t *r = layout_region(g);
        CHECK(r->panel_x == 100 && r->panel_y == 300 && r->rot == ROT_0,
              "graffiti relocated to 100,300 upright");
        int palm_x, palm_y;
        int ok = layout_panel_to_palm(100 + 30, 300 + 30, &palm_x, &palm_y);
        CHECK(ok && palm_x >= 28 && palm_x < 28 + 104 &&
              palm_y >= 160 && palm_y < 220,
              "tap on relocated graffiti maps into its palm rect (%d,%d)", palm_x, palm_y);
    }

    /* Invalid (off-panel) config -> false + default fallback. */
    CHECK(layout_load("[screen]\nplace = 900,400\nscale = 6\n", 960, 540) == false &&
          layout_region_count() == 2,
          "off-panel config should fall back to default");

    /* Unknown preset -> false + default. */
    CHECK(layout_load("preset = bogus\n", 960, 540) == false &&
          layout_region_count() == 2, "unknown preset should fall back");

    /* Built-in presets should all validate and stay on-panel. */
    const char *presets[] = { "buttons-top", "landscape-left", "landscape-right" };
    for (int pi = 0; pi < 3; pi++) {
        char cfg[64];
        snprintf(cfg, sizeof(cfg), "preset = %s\n", presets[pi]);
        int ok = layout_load(cfg, 960, 540);
        CHECK(ok, "preset '%s' should load", presets[pi]);
        for (int i = 0; i < layout_region_count(); i++) {
            const region_t *r = layout_region(i);
            int bw, bh; layout_region_box(r, &bw, &bh);
            CHECK(r->panel_x >= 0 && r->panel_y >= 0 &&
                  r->panel_x + bw <= 960 && r->panel_y + bh <= 540,
                  "preset '%s' region %d off-panel", presets[pi], i);
            CHECK((r->panel_x & 3) == 0 && (bw & 3) == 0,
                  "preset '%s' region %d not 4-aligned (x=%d w=%d)",
                  presets[pi], i, r->panel_x, bw);
        }
    }

    /* ---- landscape buttons: one row, evenly spaced, no overlap ---- */
    for (int side = 0; side < 2; side++) {
        const char *p = side ? "preset=landscape-right\n" : "preset=landscape-left\n";
        layout_load(p, 960, 540);
        /* collect the 4 button regions (REGION_SILK, palm_h<=32) by panel_x */
        int bx[4], n = 0, by = -1, bw = -1;
        for (int i = 0; i < layout_region_count(); i++) {
            const region_t *r = layout_region(i);
            if (r->kind == REGION_SILK && r->palm_w == 28) {     /* a button cell */
                int w, h; layout_region_box(r, &w, &h);
                if (n < 4) { bx[n] = r->panel_x; }
                if (by < 0) { by = r->panel_y; bw = w; }
                CHECK(r->panel_y == by, "%s buttons not on a common top edge", p);
                n++;
            }
        }
        CHECK(n == 4, "%s expected 4 buttons, got %d", p, n);
        /* sort the 4 x-positions ascending (tiny insertion sort) */
        for (int a = 1; a < 4; a++)
            for (int b = a; b > 0 && bx[b-1] > bx[b]; b--) {
                int t = bx[b-1]; bx[b-1] = bx[b]; bx[b] = t;
            }
        /* adjacent buttons must not overlap and should be roughly evenly spaced */
        int step0 = bx[1] - bx[0];
        for (int a = 0; a < 3; a++) {
            CHECK(bx[a] + bw <= bx[a+1], "%s buttons %d,%d overlap (x=%d,%d w=%d)",
                  p, a, a+1, bx[a], bx[a+1], bw);
            int step = bx[a+1] - bx[a];
            CHECK(step >= step0 - 4 && step <= step0 + 4,
                  "%s buttons unevenly spaced: steps %d vs %d", p, step, step0);
        }
    }

    /* ---------------- strict malformed-value rejection ---------- */

    /* Out-of-range scale must reject the whole config. The classic trap is
     * `scale = 257` silently wrapping to (uint8_t)1; the parser rejects it. */
    CHECK(layout_load("[screen]\nscale = 257\n", 960, 540) == false &&
          layout_region_count() == 2, "scale=257 must reject (was uint8 wrap to 1)");
    CHECK(layout_load("[screen]\nscale = 256\n", 960, 540) == false &&
          layout_region_count() == 2, "scale=256 must reject (wraps to 0)");
    CHECK(layout_load("[screen]\nscale = 17\n", 960, 540) == false &&
          layout_region_count() == 2, "scale=17 must reject (> max)");
    CHECK(layout_load("[screen]\nscale = 0\n", 960, 540) == false &&
          layout_region_count() == 2, "scale=0 must reject");
    CHECK(layout_load("[screen]\nscale = -1\n", 960, 540) == false &&
          layout_region_count() == 2, "scale=-1 must reject");
    /* Upper bound accepted (isolate the range check from the on-panel check by
     * placing a small element that fits at scale 16). */
    CHECK(layout_load("[btn-top-left]\nplace = 0,0\nrotate = 0\nscale = 16\n",
                      960, 540) == true, "scale=16 (max) on a placed button must load");
    CHECK(layout_load("[graffiti]\nscale_x = 257\n", 960, 540) == false &&
          layout_region_count() == 2, "scale_x=257 must reject");
    CHECK(layout_load("[graffiti]\nscale_y = 257\n", 960, 540) == false &&
          layout_region_count() == 2, "scale_y=257 must reject");

    /* Rotate: non-numeric / trailing garbage / unsupported angle all reject;
     * a valid non-stock angle still loads. */
    CHECK(layout_load("[graffiti]\nrotate = abc\n", 960, 540) == false &&
          layout_region_count() == 2, "rotate=abc must reject (was silent ROT_0)");
    CHECK(layout_load("[graffiti]\nrotate = 90deg\n", 960, 540) == false &&
          layout_region_count() == 2, "rotate=90deg must reject (trailing garbage)");
    CHECK(layout_load("[graffiti]\nrotate = 45\n", 960, 540) == false &&
          layout_region_count() == 2, "rotate=45 must reject (unsupported angle)");
    CHECK(layout_load("[graffiti]\nrotate = 270\n", 960, 540) == true,
          "rotate=270 must load");

    /* Place: malformed rejects; the documented `X,Y` and `X Y` forms still load. */
    CHECK(layout_load("[graffiti]\nplace = 100\n", 960, 540) == false &&
          layout_region_count() == 2, "place=100 must reject (one coord)");
    CHECK(layout_load("[graffiti]\nplace = x,y\n", 960, 540) == false &&
          layout_region_count() == 2, "place=x,y must reject");
    CHECK(layout_load("[graffiti]\nplace = 100, 300\nrotate = 0\nscale = 3\n",
                      960, 540) == true, "place=100, 300 (spaced) must still load");

    /* Leniency boundary: unknown KEY and unknown SECTION are tolerated so a
     * config for a newer firmware still loads on an older build. */
    CHECK(layout_load("preset=custom\n[graffiti]\nbogus_key = 5\n", 960, 540) == true &&
          layout_region_count() == 7, "unknown key on a known section is ignored");
    CHECK(layout_load("preset=custom\n[no-such-element]\nscale = 3\n", 960, 540) == true &&
          layout_region_count() == 7, "unknown section is ignored");

    /* Re-show: landscape hides the divider; hide=false brings it back (at its
     * stock position) and the config still loads. */
    CHECK(layout_load("preset=landscape-right\n[divider]\nhide=false\n", 960, 540) == true,
          "re-showing the divider should load");
    {
        int d = -1;
        for (int i = 0; i < layout_region_count(); i++) {
            const region_t *r = layout_region(i);
            if (r->kind == REGION_SILK && r->palm_h == 4) d = i;  /* divider: ph==4 */
        }
        CHECK(d >= 0, "divider region present after hide=false");
    }

    /* JC4827W543: default, explicit preset, and invalid-config fallback all
     * stay on-panel. Every visible source pixel must also route back through
     * the global input transform, catching overlaps that steal another region. */
    const char *compact_cfgs[] = { NULL, "preset=jc4827w543\n",
        "preset=default\n", "preset=unknown\n",
        "preset=jc4827w543\n[screen]\nscale=3\n" };
    for (unsigned c = 0; c < sizeof(compact_cfgs)/sizeof(compact_cfgs[0]); c++) {
        if (compact_cfgs[c]) layout_load(compact_cfgs[c], 480, 272);
        else layout_use_default(480, 272);
        CHECK(layout_region_count() == 2, "compact config %u region count", c);
        for (int i = 0; i < layout_region_count(); i++) {
            const region_t *r = layout_region(i);
            int w, h;
            layout_region_box(r, &w, &h);
            CHECK(r->panel_x >= 0 && r->panel_y >= 0 &&
                  r->panel_x + w <= 480 && r->panel_y + h <= 272,
                  "compact region %d out of bounds", i);
            for (int y = r->palm_y; y < r->palm_y + r->palm_h; y++) {
                for (int x = r->palm_x; x < r->palm_x + r->palm_w; x++) {
                    int px, py, bx = -1, by = -1;
                    layout_src_to_panel(r, x, y, &px, &py);
                    bool hit = layout_panel_to_palm(px, py, &bx, &by);
                    CHECK(hit && bx == x && by == y,
                          "compact routing region %d at %d,%d", i, x, y);
                }
            }
        }
    }

    /* Fractional scale must keep source pixels reachable through all rotations.
     * Every source pixel must remain reachable through the inverse map. */
    for (int rot = ROT_0; rot <= ROT_270; rot++) {
        region_t r = { .kind = REGION_SCREEN, .palm_w = 160, .palm_h = 160,
            .panel_x = 16, .panel_y = 8, .scale_x = 8, .scale_y = 9,
            .scale_den = 5, .rot = (region_rot_t)rot };
        for (int y = 0; y < 160; y++) for (int x = 0; x < 160; x++) {
            int px, py, bx = -1, by = -1;
            layout_src_to_panel(&r, x, y, &px, &py);
            bool hit = layout_region_panel_to_palm(&r, px, py, &bx, &by);
            CHECK(hit && bx == x && by == y, "fractional round trip rot=%d src=%d,%d got=%d,%d", rot, x, y, bx, by);
        }
    }
    layout_use_default(480, 272);
    {
        const region_t *lcd = layout_region(0), *silk = layout_region(1);
        int w, h; layout_region_box(lcd, &w, &h);
        CHECK(w == 160 && h == 160 && lcd->rot == ROT_270, "JC portrait LCD dimensions/rotation");
        CHECK(lcd->scale_x == 1 && lcd->scale_y == 1 && lcd->scale_den == 1,
              "JC portrait must render at 1:1");
        CHECK(silk->panel_x + 60 == lcd->panel_x, "portrait screen/silk must be continuous");
        int x, y;
        layout_src_to_panel(lcd, 10, 10, &x, &y);
        CHECK(x == 337 && y == 66, "portrait top-left target native position");
    }

    /* ---- geometry dump (manual inspection of preset visuals) ---- */
    {
        const char *names[] = { "default", "custom", "buttons-top",
                                "landscape-left", "landscape-right" };
        for (int pi = 0; pi < 5; pi++) {
            char cfg[64]; snprintf(cfg, sizeof(cfg), "preset = %s\n", names[pi]);
            layout_load(cfg, 960, 540);
            printf("\n== preset %s (%d regions) ==\n", names[pi], layout_region_count());
            for (int i = 0; i < layout_region_count(); i++) {
                const region_t *r = layout_region(i);
                int bw, bh; layout_region_box(r, &bw, &bh);
                printf("  kind=%d palm(%d,%d %dx%d) -> panel(%d,%d %dx%d) rot=%d sc=%dx%d\n",
                       r->kind, r->palm_x, r->palm_y, r->palm_w, r->palm_h,
                       r->panel_x, r->panel_y, bw, bh, r->rot, r->scale_x, r->scale_y);
            }
        }
    }

    if (failures == 0) printf("ALL LAYOUT TESTS PASSED\n");
    else               printf("%d FAILURE(S)\n", failures);
    return failures ? 1 : 0;
}
