#include "host_ui.h"
#include "bus.h"

/* Trap-table layout: globals = ram32[0x122]; handler[sel] = ram32[globals+sel*4].
 * This mirrors the device-side injection in main.c (the sole CPU-state writer
 * there); on the host the emulator is single-threaded so we call it inline. */
bool host_pen_inject(m68k_cpu_t *cpu, int x, int y, bool pen_up)
{
    uint8_t *r = bus_ram_ptr();
    #define RDRAM32(off) (((uint32_t)r[(off)] << 24) | ((uint32_t)r[(off)+1] << 16) | \
                          ((uint32_t)r[(off)+2] << 8) | r[(off)+3])
    uint32_t globals = RDRAM32(0x122);
    if (!globals || globals + 0x273 * 4 + 4 >= BUS_RAM_SIZE) return false;

    uint32_t evtEnq = RDRAM32(globals + 0x126 * 4);   /* EvtEnqueuePenPoint A126 */
    uint32_t penRaw = RDRAM32(globals + 0x273 * 4);   /* PenScreenToRaw     A273 */
    #undef RDRAM32
    if (!bus_addr_in_rom(evtEnq)) return false;

    m68k_pen_tap(cpu, evtEnq, bus_addr_in_rom(penRaw) ? penRaw : 0,
                 (int16_t)x, (int16_t)y, pen_up);
    cpu->regs.stopped = false;   /* wake the doze to drain the new event */
    return true;
}

#ifdef HOST_UI

#include <SDL3/SDL.h>
#include "dragonball.h"
#include "layout.h"
#include "surface.h"
#include "silkscreen.h"

/* Palm LCD framebuffer extent: 160x160 1bpp = 3200 bytes (stride 20). Bounds the
 * change-detection checksum + screen render against the top of RAM. */
#define PALM_FB_BYTES 3200

/* Composite one region into the RGBA window buffer via the shared surface sampler
 * (the SAME inverse-map + sample the device IT8951 path uses). `fb` is the live
 * 160x160 1bpp framebuffer for REGION_SCREEN; `silk` is the silkscreen overlay.
 * Out-of-bounds panel rows/cols are clipped (a valid layout never exceeds W/H). */
static void render_region(uint8_t *rgba, int W, int H, const region_t *r,
                          const uint8_t *fb, const uint8_t *silk)
{
    int box_w, box_h;
    layout_region_box(r, &box_w, &box_h);
    static uint8_t gray[960];
    if (box_w > (int)sizeof gray) return;   /* parity with device region_load's guard */
    for (int oy = 0; oy < box_h; oy++) {
        int py = r->panel_y + oy;
        if (py < 0 || py >= H) continue;
        int w;
        surface_fill_row(r, fb, silk, oy, gray, &w);
        for (int ox = 0; ox < w; ox++) {
            int px = r->panel_x + ox;
            if (px < 0 || px >= W) continue;
            uint8_t *d = rgba + ((size_t)py * W + px) * 4;
            uint8_t g = gray[ox];
            d[0] = g; d[1] = g; d[2] = g; d[3] = 0xFF;
        }
    }
}

static void render_kind(uint8_t *rgba, int W, int H, region_kind_t kind,
                        const uint8_t *fb, const uint8_t *silk)
{
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind == kind) render_region(rgba, W, H, r, fb, silk);
    }
}

/* Map a window pixel to Palm coords and inject a pen-down/move; returns false if
 * the point is in panel dead space (caller emits a pen-up if a stroke was live). */
static bool mouse_to_pen(m68k_cpu_t *cpu, float wx, float wy)
{
    int px, py;
    if (!layout_panel_to_palm((int)wx, (int)wy, &px, &py)) return false;
    return host_pen_inject(cpu, px, py, false);
}

/* Drive the UI's power key, mirroring main.c's power path. ASLEEP: assert the
 * matrix POWER line and keep it held (caller releases it on key-up); the unmasked
 * INT_KB then clears the STOP doze and runs the ROM's wake ISR — we deliberately
 * do NOT force stopped=false (that skips the ISR and crashes). AWAKE: inject
 * vchrHardPower via EvtEnqueueKey so the OS sleeps. A pen tap cannot wake a
 * sleeping IIIx, so without this the window dies once Palm OS auto-sleeps.
 * Returns true if the matrix key is now held (must be released on key-up), false
 * for the one-shot sleep inject (or if the trap table isn't ready yet). */
static bool host_power_press(m68k_cpu_t *cpu)
{
    if (db_is_asleep()) {
        db_kbd_set(KBD_KEY_POWER, true);   /* held across frames until key-up wakes it */
        return true;
    }
    uint8_t *r = bus_ram_ptr();
    #define RDRAM32(off) (((uint32_t)r[(off)] << 24) | ((uint32_t)r[(off)+1] << 16) | \
                          ((uint32_t)r[(off)+2] << 8) | r[(off)+3])
    uint32_t globals = RDRAM32(0x122);
    if (globals && globals + 0x12D * 4 + 4 < BUS_RAM_SIZE) {
        uint32_t evtEnqKey = RDRAM32(globals + 0x12D * 4);   /* EvtEnqueueKey A12D */
        if (bus_addr_in_rom(evtEnqKey)) {
            /* vchrHardPower is a virtual key: it rides the ascii (WChar) field with
             * commandKeyMask set, keycode 0 — same mapping as main.c's power path. */
            m68k_key_enqueue(cpu, evtEnqKey, 0x0208 /*vchrHardPower*/,
                             0 /*keycode*/, 0x0008 /*commandKeyMask*/);
            cpu->regs.stopped = false;   /* wake the doze to drain the event (main.c parity) */
        }
    }
    #undef RDRAM32
    return false;
}

int host_ui_run(m68k_cpu_t *cpu, host_step_fn step, void *ctx)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int W = layout_panel_w(), H = layout_panel_h();
    SDL_Window   *win = SDL_CreateWindow("Palm IIIx - m5paper-ez-palmos", W, H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    SDL_Texture  *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
    if (!win || !ren || !tex) {
        fprintf(stderr, "SDL window/renderer/texture failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    uint8_t *rgba = calloc((size_t)W * H, 4);
    if (!rgba) {
        fprintf(stderr, "out of memory: %dx%d window buffer\n", W, H);
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    uint8_t  silk[SILK_BYTES];
    silkscreen_build(silk);
    render_kind(rgba, W, H, REGION_SILK, NULL, silk);   /* static overlay, drawn once */
    SDL_UpdateTexture(tex, NULL, rgba, W * 4);   /* upload now: a STREAMING texture is
                                                  * undefined until first update, so the
                                                  * silk band shows before the first
                                                  * framebuffer change instead of garbage */

    const double CPU_HZ = 16580000.0;   /* DragonBall EZ ~16.58 MHz */
    uint64_t freq   = SDL_GetPerformanceFrequency();
    uint64_t t_prev = SDL_GetPerformanceCounter();
    double   budget = 0.0;
    uint32_t last_cksum = 0xFFFFFFFFu;  /* force a first screen render */
    bool pen_down = false;
    bool power_held = false;
    int  run = 1;

    while (run) {
        /* 1. input: left button drives the pen, streaming stroke points so the
         *    ROM's Graffiti recognizer sees down + moves + up. */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                run = 0;
                break;
            case SDL_EVENT_KEY_DOWN:
                /* P = power button. Hold briefly to wake a sleeping device; press
                 * while awake to sleep it (see host_power_press). */
                if (e.key.scancode == SDL_SCANCODE_P && !e.key.repeat)
                    power_held = host_power_press(cpu);
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode == SDL_SCANCODE_P && power_held) {
                    db_kbd_set(KBD_KEY_POWER, false);
                    power_held = false;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT)
                    pen_down = mouse_to_pen(cpu, e.button.x, e.button.y);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (pen_down && (e.motion.state & SDL_BUTTON_LMASK)) {
                    if (!mouse_to_pen(cpu, e.motion.x, e.motion.y)) {
                        host_pen_inject(cpu, -1, -1, true);   /* dragged off a region */
                        pen_down = false;
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT && pen_down) {
                    host_pen_inject(cpu, -1, -1, true);       /* pen up */
                    pen_down = false;
                }
                break;
            default: break;
            }
        }

        /* 2. emulate ~real-time worth of cycles since the last frame (cycle-based
         *    clock under HOST_BUILD; pacing here makes emulated time track wall
         *    time so the Palm animates/sleeps naturally). Clamp dt so a window
         *    drag or breakpoint doesn't trigger a huge catch-up burst. */
        uint64_t t_now = SDL_GetPerformanceCounter();
        double dt = (double)(t_now - t_prev) / (double)freq;
        t_prev = t_now;
        if (dt > 0.05) dt = 0.05;
        budget += dt * CPU_HZ;
        while (budget > 0 && run) {
            int ran = step(cpu, ctx);
            if (ran <= 0) { run = 0; break; }
            budget -= ran;
        }

        /* 3. render the live screen regions on framebuffer change. */
        uint32_t fba = db_lcd_framebuffer_addr();
        const uint8_t *fb = NULL;
        uint32_t cksum = 0;
        if (fba && fba + PALM_FB_BYTES <= BUS_RAM_SIZE) {
            fb = bus_ram_ptr() + fba;
            for (int i = 0; i < PALM_FB_BYTES; i++) cksum += fb[i];
        }
        if (fb && cksum != last_cksum) {
            render_kind(rgba, W, H, REGION_SCREEN, fb, silk);
            SDL_UpdateTexture(tex, NULL, rgba, W * 4);
            last_cksum = cksum;
        }

        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);   /* ~60 fps present cap; emulation paced separately */
    }

    free(rgba);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

#endif /* HOST_UI */
