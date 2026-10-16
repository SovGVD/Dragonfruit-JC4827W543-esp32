#include "display.h"
#include "it8951.h"
#include "bus.h"
#include "dragonball.h"
#include "layout.h"
#include "silkscreen.h"
#include "surface.h"
#include "message.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

#define PALM_W      160
#define PALM_H      160
#define PALM_STRIDE 20
#define PALM_FB_SIZE (PALM_STRIDE * PALM_H)

/* Runtime waveform-mode policy (tunable). We poll Palm's framebuffer asynchronously
 * (no "frame done" signal), so two things must be handled: WHEN to push (the quiescence
 * settle in display_refresh) and WHICH waveform to use. The policy favors the fast,
 * NON-flashing DU for live updates and confines the flashing GC16 to three well-defined
 * de-ghost tiers, so it never GC16-flashes a half-drawn frame or flashes gratuitously while
 * the user is mid-edit:
 *  - Live updates (cursor, typing, menus, list scroll) use DU. DU is differential so it
 *    ghosts, but it never flashes. The quiescence settle (not the waveform) is what stopped
 *    the scroll/app-switch tearing, so DU here no longer tears.
 *  Three GC16 de-ghost tiers (a clean slate that also wipes accumulated ghosting):
 *  1. Immediate slate: a SETTLED change touching > FB_LARGE_CHANGE_BYTES is an
 *     app-switch-scale redraw; push it straight as GC16. Only the SETTLED frame qualifies,
 *     so a still-drawing redraw is never GC16-flashed half-complete (it goes out as DU and
 *     the GC16 lands on the finished frame next poll).
 *  2. Idle clean (cheap): ghosting that DU builds up within one screen is cleared during a
 *     genuine PAUSE. du_since_gc16 counts substantial DU redraws (a DU change >
 *     FB_DU_GHOST_BYTES; a blinking cursor / clock tick stays below and never counts). Once
 *     >= GC16_GHOST_REDRAWS have accumulated AND the screen has been settled for
 *     GC16_IDLE_POLLS consecutive polls, one GC16 repaints the current (unchanged) frame.
 *  3. Hard ceiling (bound): if du_since_gc16 reaches GC16_DU_HARD_CAP it forces a GC16 in
 *     the push path regardless of idle/moving. This bounds worst-case ghosting for workloads
 *     that never pause (continuous animation/scroll) or that tick faster than the idle window
 *     (a seconds clock), where tier 2 can never fire — so this one CAN flash mid-activity, by
 *     design, as the safety net tier 2 cannot provide. Raise it to tolerate more ghosting /
 *     flash even less; lower it for a cleaner panel that flashes a bit more often.
 *  - MAX_SETTLE_HOLDS: how many polls a STILL-CHANGING frame may be deferred while it
 *    settles, so a multi-batch redraw is shown once whole. Only SUBSTANTIAL changes
 *    (> FB_DU_GHOST_BYTES) are held — a tiny caret/cursor edit is drawn atomically, can't
 *    tear, and pushes immediately. Bounded so a continuous animation still shows its latest
 *    frame (~MAX_SETTLE_HOLDS * the display poll period in main.c, ~180 ms) instead of being
 *    starved — at the cap the latest sample is pushed even if still changing, so a redraw
 *    stretched past that can briefly show mid-draw (as DU, never a GC16 flash). */
#define FB_DU_GHOST_BYTES      96                      /* a DU change > this is a real ghosting redraw (also the settle-hold floor) */
#define FB_LARGE_CHANGE_BYTES (PALM_FB_SIZE * 3 / 8)   /* ~1200 of 3200 B -> immediate GC16 slate */
#define MAX_SETTLE_HOLDS       3
#define GC16_GHOST_REDRAWS     10                      /* accumulated ghosting DUs that make a cheap idle clean worthwhile */
#define GC16_IDLE_POLLS        30                      /* settled polls (~1.8 s @ DISPLAY_POLL_MS) before the idle clean */
#define GC16_DU_HARD_CAP       32                      /* substantial DUs since last GC16 -> forced clean (~32 * ~258 ms DU ~= 8 s) */

/* All panel placement / rotation / scale now lives in components/layout (the
 * single source of truth shared with input.c). This file only knows how to
 * SAMPLE the two Palm surfaces — the live 160x160 framebuffer and the static
 * silkscreen overlay — and BLIT a region through whatever transform the active
 * layout assigns it. The stock portrait geometry (90 deg CCW, scale 3, screen
 * @148,30 + silk @628,30) is the default layout; see layout_use_default(). */

static it8951_dev_info_t dev_info;
static uint8_t shadow_fb[PALM_FB_SIZE];   /* last frame actually PUSHED to the panel */
static uint8_t prev_fb[PALM_FB_SIZE];     /* framebuffer seen on the previous poll (settle hold) */
static bool first_refresh = true;
static uint32_t refresh_count = 0;
static uint32_t du_since_gc16 = 0;        /* substantial DU redraws since the last GC16 ghost-clear */
static uint8_t settle_holds = 0;          /* consecutive polls a still-changing frame has been deferred */
static uint16_t idle_polls = 0;           /* consecutive settled polls (drives the idle ghost clean) */

/* The silkscreen overlay (Graffiti area + Home/Menu/Calc/Find buttons) is a
 * static 1bpp bitmap synthesized by components/display/silkscreen.c — the same
 * module the host UI uses — into this buffer. display_draw_static() builds it;
 * the surface sampler reads it for REGION_SILK pixels. See silkscreen.h for why
 * the overlay exists (case-printed graphics, one continuous digitizer surface). */
static uint8_t g_silk[SILK_BYTES];   /* 1 = ink (black) */

/* One 4bpp panel row (2 px/byte) plus the 8-bit grayscale row the shared sampler
 * fills before we pack it. The IT8951 long axis is at most 960 px, so a region's
 * box width never exceeds this. Static: the display path is single-threaded (init
 * before the display task starts, then only the display task / the parked-task
 * sleep path call in here), so shared buffers are safe. */
#define LINE_BUF_BYTES (960 / 2)
static uint8_t line_buf[LINE_BUF_BYTES] __attribute__((aligned(4)));
static uint8_t gray_row[960];

/* Load one region into the controller's image buffer through the active layout's
 * transform (NO waveform refresh — the caller triggers display separately). The
 * shared surface sampler (surface.c) walks the region's panel box and produces an
 * 8-bit grayscale row (the SAME inverse map + sample input.c and the host UI use —
 * one source of truth); we pack each row 4bpp for the IT8951. Works for any
 * rotation/scale. Returns the region's panel box width in *bw and height in *bh,
 * or *bw=0 if skipped. */
static void region_load(const region_t *r, const uint8_t *screen_fb,
                        int *bw, int *bh)
{
    int box_w, box_h;
    layout_region_box(r, &box_w, &box_h);
    if (box_w < 2 || box_w > LINE_BUF_BYTES * 2) { *bw = 0; *bh = 0; return; }

    /* Wait for the panel engine to finish any in-flight waveform BEFORE streaming
     * the next frame's pixels into the controller image buffer. it8951_display_area
     * is async, the display task re-polls every ~60 ms, and a DU waveform takes
     * ~258 ms (measured) — so without this wait the new pixels overwrite the same
     * buffer the previous waveform is still scanning onto the panel, painting half
     * old / half new rows (controller-level tearing, distinct from the half-drawn
     * source frame the quiescence settle guards against). it8951_wait_ready() yields
     * via vTaskDelay while polling LUTAFSR, so blocking the display task here is
     * watchdog-safe and just paces the actual e-ink pushes to the panel's waveform
     * rate even though we poll for changes faster. */
    it8951_wait_ready();

    it8951_load_area_start(r->panel_x, r->panel_y, box_w, box_h);

    for (int oy = 0; oy < box_h; oy++) {
        int w;
        surface_fill_row(r, screen_fb, g_silk, oy, gray_row, &w);
        for (int ox = 0; ox < box_w; ox++) {
            uint8_t g = gray_row[ox] ? 0xF : 0x0;   /* blank 0xFF -> 0xF, ink 0x00 -> 0x0 */
            if (ox & 1) line_buf[ox >> 1] |= g;
            else        line_buf[ox >> 1]  = (uint8_t)(g << 4);
        }
        it8951_load_pixels(line_buf, box_w / 2);
    }
    it8951_load_end();
    *bw = box_w; *bh = box_h;
}

/* Blit one region and refresh just its box. Used for the dynamic screen path. */
static void region_blit(const region_t *r, const uint8_t *screen_fb, uint16_t mode)
{
    int bw, bh;
    region_load(r, screen_fb, &bw, &bh);
    if (bw) it8951_display_area(r->panel_x, r->panel_y, bw, bh, mode);
}

/* Draw all static (silkscreen) regions. Called at init and after wake (the sleep
 * path blanks the whole panel). A custom layout can have many silk regions (the
 * landscape preset has 5: four buttons + Graffiti); loading each then doing a
 * SINGLE GC16 display over their union refreshes the panel once instead of
 * flashing each region in turn (which looked slow on wake). */
static void draw_static_regions(void)
{
    int min_x = 0x7FFF, min_y = 0x7FFF, max_x = 0, max_y = 0;
    bool any = false;

    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind != REGION_SILK) continue;
        int bw, bh;
        region_load(r, NULL, &bw, &bh);
        if (!bw) continue;
        if (r->panel_x < min_x) min_x = r->panel_x;
        if (r->panel_y < min_y) min_y = r->panel_y;
        if (r->panel_x + bw > max_x) max_x = r->panel_x + bw;
        if (r->panel_y + bh > max_y) max_y = r->panel_y + bh;
        any = true;
    }
    if (!any) return;

    /* Union X/width must stay 4-aligned for the GC16 display area (same
     * constraint as the per-region boxes; all panel_x are 4-aligned and box
     * widths are multiples of 4, so the union already is — but clamp to be safe
     * against an odd custom region). */
    int x = min_x & ~3;
    int w = ((max_x - x) + 3) & ~3;

    /* Fast path: a SINGLE GC16 over the union refreshes the panel once instead
     * of flashing each region (which looked slow on wake). The union also covers
     * any dead space between scattered regions — harmless, it is white after the
     * clear. The one case to avoid: a custom layout that interleaves silk with
     * the live screen, where the union would enclose (and briefly blank) the
     * screen area. So fall back to per-region refresh only when the union
     * overlaps a REGION_SCREEN box; the built-in presets never do. */
    bool hits_screen = false;
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind != REGION_SCREEN) continue;
        int sw, sh;
        layout_region_box(r, &sw, &sh);
        bool sep = x + w <= r->panel_x || r->panel_x + sw <= x ||
                   max_y <= r->panel_y || r->panel_y + sh <= min_y;
        if (!sep) { hits_screen = true; break; }
    }

    if (!hits_screen) {
        it8951_display_area(x, min_y, w, max_y - min_y, IT8951_MODE_GC16);
        return;
    }

    /* Overlap: refresh each silk box on its own so we never paint over the
     * screen area (same skip condition as region_load, so only loaded boxes). */
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind != REGION_SILK) continue;
        int bw, bh;
        layout_region_box(r, &bw, &bh);
        if (bw < 2 || bw > LINE_BUF_BYTES * 2) continue;
        it8951_display_area(r->panel_x, r->panel_y, bw, bh, IT8951_MODE_GC16);
    }
}

esp_err_t display_init(void)
{
    esp_err_t err = it8951_init(&dev_info);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Clearing screen...");
    it8951_clear(IT8951_MODE_INIT);

    memset(shadow_fb, 0, PALM_FB_SIZE);
    first_refresh = true;

    /* Hardware only — no silkscreen yet. main.c establishes the layout (default
     * or from the SD config) AFTER this, then calls display_draw_static(), so a
     * custom silk placement is honored on the very first draw. */
    ESP_LOGI(TAG, "Display hardware ready: %dx%d", dev_info.panel_w, dev_info.panel_h);
    return ESP_OK;
}

/* Report the real panel size read from the IT8951, so main.c can size the
 * layout to it. Valid only after a successful display_init(). */
void display_get_panel_size(uint16_t *w, uint16_t *h)
{
    if (w) *w = dev_info.panel_w;
    if (h) *h = dev_info.panel_h;
}

/* Build the silkscreen overlay bitmap and draw every static (silk) region of
 * the active layout. Call once after the layout is established (calling again
 * is harmless — the build is idempotent). */
void display_draw_static(void)
{
    silkscreen_build(g_silk);
    draw_static_regions();
    ESP_LOGI(TAG, "static draw: %d layout regions", layout_region_count());
}

/* Blank the panel white in preparation for ESP32 light sleep. Called from the
 * CPU task (the sole sleep driver) while the display task is parked, so there is
 * no concurrent SPI. Unlike the old behavior, blanking is now tied to the ESP32
 * actually powering down — NOT to the emulated device merely sleeping — so the
 * silkscreen stays on screen for as long as the ESP32 is awake. */
void display_blank_for_sleep(void)
{
    ESP_LOGI(TAG, "blanking screen for light sleep");
    it8951_clear(IT8951_MODE_INIT);
    memset(shadow_fb, 0, PALM_FB_SIZE);
    first_refresh = true;   /* full GC16 redraw on wake */
}

/* Restore the static silkscreen after waking from light sleep (the blank wiped
 * it) and force a full framebuffer redraw on the next display_refresh(). */
void display_resume_after_wake(void)
{
    ESP_LOGI(TAG, "restoring screen after light sleep");
    draw_static_regions();
    first_refresh = true;
}

/* Blit a 160x160 1bpp Palm-screen bitmap (stride PALM_STRIDE, bit set = black
 * ink) into every REGION_SCREEN region of the active layout. Shared by the live
 * framebuffer refresh and the boot message screen. */
static void screen_blit_1bpp(const uint8_t *src, uint16_t mode)
{
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind == REGION_SCREEN)
            region_blit(r, src, mode);
    }
}

/* Push a screen frame to the panel and tick the refresh counter. Shared by the live push
 * path (blits the just-captured frame) and the idle ghost-clean (repaints shadow_fb). The
 * caller logs its own reason line and owns the shadow_fb copy — this only blits + counts. */
static void push_screen(const uint8_t *src, uint16_t mode)
{
    refresh_count++;
    screen_blit_1bpp(src, mode);
}

static void show_message_mode(const char *msg, uint16_t mode)
{
    static uint8_t msgfb[PALM_FB_SIZE];
    display_message_bitmap(msg, msgfb);

    ESP_LOGI(TAG, "showing message screen");
    screen_blit_1bpp(msgfb, mode);
}

void display_show_message(const char *msg)      { show_message_mode(msg, IT8951_MODE_GC16); }
void display_show_message_fast(const char *msg) { show_message_mode(msg, IT8951_MODE_A2); }

void display_show_progress(const char *title, const char *phase, int percent)
{
    static uint8_t fb[PALM_FB_SIZE];
    display_progress_bitmap(title, phase, percent, fb);
    screen_blit_1bpp(fb, IT8951_MODE_A2);
}

/* Single pass over the framebuffer answering both questions the policy needs, reading each
 * byte once: *nchanged = bytes differing from the last PUSHED frame (shadow_fb) — the dirty
 * test and the change-size classifier; *moving = whether the frame differs from the previous
 * poll (prev_fb) — the quiescence-settle signal. 3200 bytes is negligible next to the blit. */
static void fb_diff(const uint8_t *fb, size_t *nchanged, bool *moving)
{
    size_t n = 0;
    bool mv = false;
    for (size_t i = 0; i < PALM_FB_SIZE; i++) {
        if (fb[i] != shadow_fb[i]) n++;
        if (fb[i] != prev_fb[i])   mv = true;
    }
    *nchanged = n;
    *moving = mv;
}

void display_refresh(void)
{
    uint32_t fb_addr = db_lcd_framebuffer_addr();
    if (fb_addr == 0 || fb_addr >= BUS_RAM_SIZE)
        return;

    uint8_t *ram = bus_ram_ptr();
    uint8_t *fb = ram + fb_addr;

    size_t nchanged;
    bool still_moving;
    fb_diff(fb, &nchanged, &still_moving);

    if (!first_refresh && nchanged == 0) {
        settle_holds = 0;                   /* screen matches the panel; nothing pending */
        /* Keep the settle detector primed for the next change. prev_fb already equals fb in
         * the common idle case (after a push), so still_moving is false and we skip the copy;
         * it is only stale when a held redraw reverted to the on-panel frame within the settle
         * window, and re-priming then keeps still_moving correct on the next change. */
        if (still_moving) memcpy(prev_fb, fb, PALM_FB_SIZE);
        /* Idle clean (tier 2): the screen is settled. Once enough DU ghosting has
         * accumulated AND we've been settled long enough to be sure the user has paused,
         * repaint the last-pushed frame once with GC16 to clear it — cheap, never mid-edit.
         * The du_since_gc16 = 0 reset stops it re-firing on the following idle polls.
         * (Workloads that never pause are bounded by the tier-3 hard cap in the push path.) */
        if (idle_polls < GC16_IDLE_POLLS) idle_polls++;
        if (idle_polls >= GC16_IDLE_POLLS && du_since_gc16 >= GC16_GHOST_REDRAWS) {
            du_since_gc16 = 0;
            ESP_LOGI(TAG, "refresh #%lu GC16 idle-clean", (unsigned long)(refresh_count + 1));
            push_screen(shadow_fb, IT8951_MODE_GC16);   /* repaint the known frame */
        }
        return;
    }
    idle_polls = 0;                         /* activity resumed */

    /* Quiescence settle: Palm OS draws a new screen over several CPU batches, and we poll
     * its framebuffer asynchronously with no "frame done" signal, so a poll can land
     * mid-redraw. Hold until the frame stops changing since the previous poll (prev_fb == fb)
     * so a settled redraw is pushed whole — that mid-draw sampling, not the controller race
     * region_load() already guards, is what tore on scroll/app-switch. Only SUBSTANTIAL
     * changes (> FB_DU_GHOST_BYTES) are held: a multi-batch redraw is always large, while a
     * tiny caret/cursor edit is drawn atomically and can't tear, so it skips the hold and
     * stays responsive. A held frame that keeps changing is deferred only up to
     * MAX_SETTLE_HOLDS so a continuous animation still shows its latest frame; at that cap we
     * push the latest sample even if it is still changing (a redraw stretched past ~180 ms can
     * therefore briefly show mid-draw, as DU — never a GC16 flash; see the mode policy). */
    memcpy(prev_fb, fb, PALM_FB_SIZE);      /* prime the settle detector for the next poll */
    if (!first_refresh && still_moving && nchanged > FB_DU_GHOST_BYTES &&
        settle_holds < MAX_SETTLE_HOLDS) {
        settle_holds++;
        return;
    }
    settle_holds = 0;

    /* Waveform mode. DU for live updates (fast, non-flashing, but ghosts). GC16 (clean slate)
     * fires for exactly the three de-ghost tiers documented at the policy #defines:
     *  - first frame;
     *  - tier 3, the hard ceiling: enough substantial DUs have accumulated that worst-case
     *    ghosting must be bounded even though the screen never paused (this one CAN fire
     *    mid-activity, by design — the safety net tier 2 can't provide);
     *  - tier 1, an app-switch-scale change that has SETTLED (!still_moving), so we never
     *    GC16-flash a half-drawn redraw: a still-moving large frame at the settle cap goes out
     *    as DU and the GC16 lands on the finished frame next poll.
     * hard_cap reads du_since_gc16 BEFORE it is reset/incremented below. Change size is
     * measured on this frame vs the on-panel shadow. */
    bool hard_cap = du_since_gc16 >= GC16_DU_HARD_CAP;
    bool gc16 = first_refresh || hard_cap ||
                (nchanged > FB_LARGE_CHANGE_BYTES && !still_moving);
    uint16_t mode = gc16 ? IT8951_MODE_GC16 : IT8951_MODE_DU;

    ESP_LOGI(TAG, "refresh #%lu %s (%u B)", (unsigned long)(refresh_count + 1),
             gc16 ? "GC16" : "DU", (unsigned)nchanged);

    memcpy(shadow_fb, fb, PALM_FB_SIZE);

    /* 90 deg CCW blit (see screen_blit_1bpp): each native output row maps to a
     * single Palm framebuffer COLUMN, top row = column 159 .. last = column 0. */
    push_screen(shadow_fb, mode);

    /* Accumulate substantial DU redraws toward the de-ghost tiers — a blinking cursor or
     * clock tick (<= FB_DU_GHOST_BYTES) ghosts negligibly and must not drive a flash. Count
     * settled AND cap-forced DU pushes, so the hard ceiling still advances during continuous
     * animation, where every push is cap-forced and still moving. */
    if (gc16)                              du_since_gc16 = 0;
    else if (nchanged > FB_DU_GHOST_BYTES) du_since_gc16++;
    first_refresh = false;
}

/* ------------------------------------------------------------------ *
 *  Waveform timing benchmark (debug-console `wavetime`).
 *
 *  Measures how long each IT8951 waveform mode takes to COMPLETE on this exact
 *  panel, so the tearing/ghosting work (wait-before-load, settle debounce,
 *  periodic GC16) can be tuned to real numbers instead of the codebase's vague
 *  "GC16 ~2-5 s" guess. Must run on the display task (the sole IT8951 SPI
 *  owner); main.c services the console request flag from that task. Streams one
 *  "= WAVE ..." line per (area,mode) so tools/serial_console.py captures them
 *  like any reply. Disruptive by design (it flashes the panel through a
 *  white->black flip per mode); the live screen + silkscreen are restored at the
 *  end the same way a wake does.
 * ------------------------------------------------------------------ */
static const struct { uint16_t mode; const char *name; } WAVE_MODES[] = {
    { IT8951_MODE_INIT, "INIT" },
    { IT8951_MODE_GC16, "GC16" },
    { IT8951_MODE_GL16, "GL16" },
    { IT8951_MODE_DU,   "DU"   },
    { IT8951_MODE_A2,   "A2"   },
};

static void wave_measure_area(const char *area_name, uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h)
{
    /* 4bpp pack-write needs x/width 4-aligned (it8951_load_area_start); the
     * layout guarantees this for built-in regions, but clamp defensively so a
     * custom region can't corrupt the load or run off the panel. */
    x &= ~3u;
    w = (w + 3u) & ~3u;
    if (x + w > dev_info.panel_w) w = dev_info.panel_w - x;

    for (unsigned i = 0; i < sizeof(WAVE_MODES) / sizeof(WAVE_MODES[0]); i++) {
        /* Baseline the area to a settled WHITE so every mode measures the same
         * full white->black transition. The flat modes (GC16/GL16/INIT) are
         * content-independent; DU/A2 are differential, so a full-area flip is
         * their worst case (a real scroll/typing changes far fewer pixels). */
        it8951_fill_area(x, y, w, h, 0xFF);                 /* white -> buffer */
        it8951_display_area(x, y, w, h, IT8951_MODE_GC16);
        it8951_wait_ready();                                /* settle baseline */

        it8951_fill_area(x, y, w, h, 0x00);                 /* black -> buffer */
        int64_t us = it8951_display_timed(x, y, w, h, WAVE_MODES[i].mode);

        /* esp_timer microseconds are < 6e6 here, so a 32-bit print is exact and
         * avoids depending on %lld / %f formatting in the firmware's printf. */
        unsigned long us_l = (unsigned long)us;
        printf("= WAVE area=%-6s %ux%u mode=%-4s us=%lu ms=%lu.%lu\n",
               area_name, (unsigned)w, (unsigned)h, WAVE_MODES[i].name,
               us_l, us_l / 1000, (us_l % 1000) / 100);
    }
}

void display_run_waveform_benchmark(void)
{
    ESP_LOGI(TAG, "waveform benchmark start");
    printf("= WAVE begin\n");

    /* Area 1: the full panel. */
    wave_measure_area("full", 0, 0, dev_info.panel_w, dev_info.panel_h);

    /* Area 2: the live screen region box from the active layout — the area the
     * real refresh actually pushes, so we learn whether duration tracks area. */
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind != REGION_SCREEN) continue;
        int bw, bh;
        layout_region_box(r, &bw, &bh);
        wave_measure_area("screen", (uint16_t)r->panel_x, (uint16_t)r->panel_y,
                          (uint16_t)bw, (uint16_t)bh);
        break;   /* one screen region is representative */
    }

    printf("= WAVE done\n");
    ESP_LOGI(TAG, "waveform benchmark done; restoring screen");

    /* Restore like a wake: repaint the silkscreen and force a full live redraw
     * on the next display_refresh() (the benchmark left the panel black). */
    draw_static_regions();
    first_refresh = true;
}
