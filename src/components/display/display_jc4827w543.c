#include "display.h"
#include "nv3041a.h"
#include "board.h"
#include "bus.h"
#include "dragonball.h"
#include "layout.h"
#include "silkscreen.h"
#include "surface.h"
#include "message.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define FB_BYTES (160 * 20)
static bool ready, first_refresh = true;
static uint8_t silk[SILK_BYTES];
static uint8_t shadow[FB_BYTES];
static uint32_t last_color_hash;
static uint8_t gray[BOARD_PANEL_W];
static DMA_ATTR uint8_t pixels[BOARD_PANEL_W * 2];

static void clear(void)
{
    memset(pixels, 0xff, sizeof(pixels));
    nv3041a_window(0, 0, BOARD_PANEL_W, BOARD_PANEL_H);
    for (int y = 0; y < BOARD_PANEL_H; y++)
        nv3041a_write_row(pixels, BOARD_PANEL_W);
}

static void draw_regions(region_kind_t kind, const uint8_t *fb)
{
    if (!ready) return;
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind != kind) continue;
        int w, h;
        layout_region_box(r, &w, &h);
        if (w <= 0 || r->panel_x < 0 || r->panel_y < 0 ||
            r->panel_x + w > BOARD_PANEL_W || r->panel_y + h > BOARD_PANEL_H)
            continue;
        nv3041a_window(r->panel_x, r->panel_y, w, h);
        for (int y = 0; y < h; y++) {
            int count;
            surface_fill_row(r, fb, silk, y, gray, &count);
            for (int x = 0; x < w; x++)
                pixels[x * 2] = pixels[x * 2 + 1] = gray[x] ? 0xff : 0;
            nv3041a_write_row(pixels, w);
        }
    }
}

static void draw_regions_color(region_kind_t kind, const uint8_t *fb,
                               int bpp, const uint16_t *clut_rgb565)
{
    if (!ready) return;
    for (int i = 0; i < layout_region_count(); i++) {
        const region_t *r = layout_region(i);
        if (r->kind != kind) continue;
        int w, h;
        layout_region_box(r, &w, &h);
        if (w <= 0 || r->panel_x < 0 || r->panel_y < 0 ||
            r->panel_x + w > BOARD_PANEL_W || r->panel_y + h > BOARD_PANEL_H)
            continue;
        nv3041a_window(r->panel_x, r->panel_y, w, h);
        for (int y = 0; y < h; y++) {
            int count;
            surface_fill_row_rgb565_be(r, fb, bpp, clut_rgb565, silk, y, pixels, &count);
            nv3041a_write_row(pixels, w);
        }
    }
}

esp_err_t display_init(void)
{
    esp_err_t err = nv3041a_init();
    ready = err == ESP_OK;
    if (ready) {
        clear();
        nv3041a_backlight(true);
        first_refresh = true;
        ESP_LOGI("display", "NV3041A ready: %dx%d", BOARD_PANEL_W, BOARD_PANEL_H);
    }
    return err;
}

void display_get_panel_size(uint16_t *w, uint16_t *h)
{
    if (w) *w = BOARD_PANEL_W;
    if (h) *h = BOARD_PANEL_H;
}

void display_draw_static(void)
{
    silkscreen_build(silk);
    draw_regions(REGION_SILK, NULL);
}

void display_refresh(void)
{
    uint32_t addr = db_lcd_framebuffer_addr();
    if (!ready || !addr) return;
    if (db_lcd_is_color()) {
        uint8_t *vram = bus_vram_ptr();
        int bpp = db_lcd_bpp();
        size_t fb_bytes = (size_t)(160 * 160 * bpp / 8);
        uint32_t off = addr - BUS_SED1375_BASE;
        if (!vram || off + fb_bytes > BUS_SED1375_VRAM_SIZE) return;
        const uint8_t *fb = vram + off;
        uint32_t h = 2166136261u ^ db.sed_palette_seq ^ (uint32_t)bpp;
        for (size_t i = 0; i < fb_bytes; i++)
            h = (h ^ fb[i]) * 16777619u;
        if (!first_refresh && h == last_color_hash) return;
        last_color_hash = h;
        draw_regions_color(REGION_SCREEN, fb, bpp, db.sed_lut_rgb565);
        first_refresh = false;
        return;
    }
    if (addr > BUS_RAM_SIZE - FB_BYTES) return;
    const uint8_t *fb = bus_ram_ptr() + addr;
    if (!first_refresh && memcmp(shadow, fb, FB_BYTES) == 0) return;
    memcpy(shadow, fb, FB_BYTES);
    draw_regions(REGION_SCREEN, shadow);
    first_refresh = false;
}

void display_show_message(const char *msg)
{
    static uint8_t fb[FB_BYTES];
    display_message_bitmap(msg, fb);
    draw_regions(REGION_SCREEN, fb);
    first_refresh = true;
}

void display_show_message_fast(const char *msg) { display_show_message(msg); }

void display_show_progress(const char *title, const char *phase, int percent)
{
    static uint8_t fb[FB_BYTES];
    display_progress_bitmap(title, phase, percent, fb);
    draw_regions(REGION_SCREEN, fb);
    first_refresh = true;
}

void display_blank_for_sleep(void)
{
    if (!ready) return;
    clear();
    nv3041a_backlight(false);
    first_refresh = true;
}

void display_resume_after_wake(void)
{
    if (!ready) return;
    nv3041a_backlight(true);
    display_draw_static();
    first_refresh = true;
}

void display_run_waveform_benchmark(void)
{
    printf("= WAVE unsupported: JC4827W543 uses an LCD\n");
}
