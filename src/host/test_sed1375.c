/* Exercise the dual-mode LCD controller (DragonBall EZ internal monochrome/gray
 * + Epson SED1375 external color controller) and multi-bpp color surface sampling. */
#include "dragonball.h"
#include "layout.h"
#include "surface.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

dragonball_state_t db;

uint8_t  db_lcd_read8(uint16_t offset);
uint16_t db_lcd_read16(uint16_t offset);
uint32_t db_lcd_read32(uint16_t offset);
void     db_lcd_write8(uint16_t offset, uint8_t val);
void     db_lcd_write16(uint16_t offset, uint16_t val);
void     db_lcd_write32(uint16_t offset, uint32_t val);

static void reset_lcd(void)
{
    memset(&db, 0, sizeof(db));
}

int main(void)
{
    /* 1. Default state: monochrome DragonBall EZ internal LCD controller */
    reset_lcd();
    assert(!db_lcd_is_color());
    assert(db_lcd_bpp() == 1);
    assert(db_lcd_framebuffer_addr() == 0);

    /* Configure internal DragonBall EZ LCD at 0x00010000, 1bpp then 4bpp */
    db_lcd_write32(REG_LSSA, 0x00010000);
    db_lcd_write8(REG_LCKCON, 0x80);
    assert(!db_lcd_is_color());
    assert(db_lcd_framebuffer_addr() == 0x00010000);
    assert(db_lcd_bpp() == 1);
    db_lcd_write8(REG_LPICF, 0x02); /* 4bpp grayscale */
    assert(db_lcd_bpp() == 4);

    /* 2. Palm IIIc boot sequence: writes 0xBADC0DE0 sentinel to LSSA and
     *    programs SED1375 registers at 0x1F01FFE0..0x1F01FFFF. */
    db_lcd_write32(REG_LSSA, 0xBADC0DE0);
    db_lcd_write8(REG_LCKCON, 0x00);
    assert(db_lcd_framebuffer_addr() == 0); /* sentinel rejected before SED1375 active */

    /* Product/revision code at reg 0x00 = 0x24; vertical non-display bit 7 at
     * reg 0x0A must read 1 so Palm OS PrvUpdateCLUT does not spin forever. */
    assert(db_sed1375_read8(0x00) == 0x24);
    assert((db_sed1375_read8(0x0A) & 0x80) == 0x80);

    /* Activating SED1375 switches to color mode at 0x1F000000 */
    db_sed1375_write8(0x01, 0x01);
    db_sed1375_write8(0x02, 0xC0); /* bits 7:6 = 11 -> 8bpp */
    assert(db_lcd_is_color());
    assert(db_lcd_bpp() == 8);
    assert(db_lcd_framebuffer_addr() == 0x1F000000u);

    /* Screen 1 Start Address word offset (regs 0x0C LSB, 0x0D MSB) */
    db_sed1375_write8(0x0C, 0x80);
    db_sed1375_write8(0x0D, 0x3E); /* 0x3E80 words = 0x7D00 bytes (32000) */
    assert(db_lcd_framebuffer_addr() == 0x1F007D00u);
    db_sed1375_write8(0x0C, 0x00);
    db_sed1375_write8(0x0D, 0x00);

    /* Mode 1 bpp decoding: 00->1bpp, 01->2bpp, 10->4bpp, 11->8bpp */
    db_sed1375_write8(0x02, 0x00); assert(db_lcd_bpp() == 1);
    db_sed1375_write8(0x02, 0x40); assert(db_lcd_bpp() == 2);
    db_sed1375_write8(0x02, 0x80); assert(db_lcd_bpp() == 4);
    db_sed1375_write8(0x02, 0xC0); assert(db_lcd_bpp() == 8);

    /* 3. CLUT write/read auto-increment and RGB888/RGB565 conversion */
    db_sed1375_write8(0x15, 42);   /* LUT index 42 */
    db_sed1375_write8(0x17, 0xF0); /* R = 0xFF */
    db_sed1375_write8(0x17, 0x80); /* G = 0x88 */
    db_sed1375_write8(0x17, 0x00); /* B = 0x00 -> auto-advances to 43 */
    db_sed1375_write8(0x17, 0x30); /* entry 43: R = 0x33 */
    db_sed1375_write8(0x17, 0x60); /* entry 43: G = 0x66 */
    db_sed1375_write8(0x17, 0xC0); /* entry 43: B = 0xCC */

    assert(db.sed_lut_rgb[42][0] == 0xFF && db.sed_lut_rgb[42][1] == 0x88 && db.sed_lut_rgb[42][2] == 0x00);
    assert(db.sed_lut_rgb[43][0] == 0x33 && db.sed_lut_rgb[43][1] == 0x66 && db.sed_lut_rgb[43][2] == 0xCC);

    uint16_t exp565_42 = (uint16_t)(((0xFF & 0xF8) << 8) | ((0x88 & 0xFC) << 3) | (0x00 >> 3));
    assert(db.sed_lut_rgb565[42] == exp565_42);

    /* Verify LUT readback also sequences R, G, B and auto-increments */
    db_sed1375_write8(0x15, 42);
    assert(db_sed1375_read8(0x17) == 0xF0);
    assert(db_sed1375_read8(0x17) == 0x80);
    assert(db_sed1375_read8(0x17) == 0x00);
    assert(db_sed1375_read8(0x17) == 0x30);

    /* 4. Surface sampling at 8bpp and 1bpp */
    uint8_t fb8[160 * 160];
    memset(fb8, 0, sizeof(fb8));
    fb8[0] = 42;
    fb8[1] = 43;

    region_t r_screen = {
        .kind = REGION_SCREEN,
        .palm_x = 0, .palm_y = 0, .palm_w = 160, .palm_h = 160,
        .panel_x = 0, .panel_y = 0, .rot = ROT_0,
        .scale_x = 2, .scale_y = 2
    };
    assert(surface_sample_index(&r_screen, fb8, 8, NULL, 0, 0) == 42);
    assert(surface_sample_index(&r_screen, fb8, 8, NULL, 0, 1) == 0);
    assert(surface_sample_index(&r_screen, fb8, 8, NULL, 1, 0) == 43);

    uint8_t row565_be[320 * 2];
    int w = 0;
    surface_fill_row_rgb565_be(&r_screen, fb8, 8, db.sed_lut_rgb565, NULL, 0, row565_be, &w);
    assert(w == 320);
    assert(row565_be[0] == (uint8_t)(exp565_42 >> 8) && row565_be[1] == (uint8_t)exp565_42);
    assert(row565_be[2] == (uint8_t)(exp565_42 >> 8) && row565_be[3] == (uint8_t)exp565_42);

    uint8_t row_rgba[320 * 4];
    surface_fill_row_rgba(&r_screen, fb8, 8, db.sed_lut_rgb, NULL, 0, row_rgba, &w);
    assert(w == 320);
    assert(row_rgba[0] == 0xFF && row_rgba[1] == 0x88 && row_rgba[2] == 0x00 && row_rgba[3] == 0xFF);
    assert(row_rgba[8] == 0x33 && row_rgba[9] == 0x66 && row_rgba[10] == 0xCC && row_rgba[11] == 0xFF);

    puts("ALL SED1375 & COLOR SURFACE TESTS PASSED");
    return 0;
}

