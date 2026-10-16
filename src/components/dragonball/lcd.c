#include "dragonball.h"

#define SED1375_BASE      0x1F000000u
#define SED1375_VRAM_SIZE 0x00014000u
#define DB_RAM_MAX        0x00C00000u /* RAM lies below the ROM window; renderer bounds actual size */

uint32_t db_lcd_framebuffer_addr(void)
{
    if (db.sed_active) {
        uint32_t word_off = ((uint32_t)db.sed_regs[0x0D] << 8) | db.sed_regs[0x0C];
        return SED1375_BASE + ((word_off * 2) % SED1375_VRAM_SIZE);
    }
    /* Ignore the $BADC0DE0 sentinel written by the Palm IIIc HAL to LSSA. */
    if (db.lssa >= DB_RAM_MAX)
        return 0;
    return db.lssa;
}

bool db_lcd_is_color(void)
{
    return db.sed_active;
}

int db_lcd_bpp(void)
{
    if (db.sed_active) {
        /* SED1375 Mode Register 1 ($02) bits 7:6: 00=1bpp, 01=2bpp, 10=4bpp, 11=8bpp */
        return 1 << ((db.sed_regs[0x02] >> 6) & 3);
    }
    return (db.lpicf & 0x02) ? 4 : ((db.lpicf & 0x01) ? 2 : 1);
}

uint8_t db_sed1375_read8(uint16_t reg)
{
    if (reg >= 32) return 0;
    switch (reg) {
    case 0x00:
        return 0x24;   /* SED1375 product/revision code (0x09 << 2) */
    case 0x0A:
        /* Bit 7 = Vertical Non-Display Period status; always ready so
         * PrvUpdateCLUT and HwrDisplayPalette never spin waiting for VBlank. */
        return db.sed_regs[0x0A] | 0x80;
    case 0x15:
        return db.sed_lut_idx;
    case 0x17: {
        uint8_t v = db.sed_lut_raw[db.sed_lut_idx][db.sed_lut_comp];
        if (++db.sed_lut_comp >= 3) {
            db.sed_lut_comp = 0;
            db.sed_lut_idx++;
        }
        return v;
    }
    default:
        return db.sed_regs[reg];
    }
}

void db_sed1375_write8(uint16_t reg, uint8_t val)
{
    if (reg >= 32) return;
    db.sed_regs[reg] = val;
    switch (reg) {
    case 0x01:
    case 0x02:
        db.sed_active = true;
        db.sed_palette_seq++;
        break;
    case 0x15:
        db.sed_lut_idx = val;
        db.sed_lut_comp = 0;
        break;
    case 0x17: {
        uint8_t idx = db.sed_lut_idx;
        uint8_t comp = db.sed_lut_comp;
        uint8_t hi = val & 0xF0;
        db.sed_lut_raw[idx][comp] = hi;
        db.sed_lut_rgb[idx][comp] = hi | (hi >> 4);
        if (++comp >= 3) {
            uint8_t r = db.sed_lut_rgb[idx][0];
            uint8_t g = db.sed_lut_rgb[idx][1];
            uint8_t b = db.sed_lut_rgb[idx][2];
            db.sed_lut_rgb565[idx] = (uint16_t)(((r & 0xF8) << 8) |
                                                ((g & 0xFC) << 3) |
                                                (b >> 3));
            comp = 0;
            db.sed_lut_idx++;
            db.sed_palette_seq++;
        }
        db.sed_lut_comp = comp;
        break;
    }
    default:
        break;
    }
}

uint8_t db_lcd_read8(uint16_t offset)
{
    switch (offset) {
    case REG_LVPW:   return db.lvpw;
    case REG_LBLKC:  return db.lblkc;
    case REG_LPICF:  return db.lpicf;
    case REG_LPOLCF: return db.lpolcf;
    case REG_LACDRC: return db.lacdrc;
    case REG_LPXCD:  return db.lpxcd;
    case REG_LCKCON: return db.lckcon;
    case REG_LRRA:   return db.lrra;
    case REG_LDMR:   return db.ldmr;
    default: return 0;
    }
}

uint16_t db_lcd_read16(uint16_t offset)
{
    switch (offset) {
    case REG_LXMAX: return db.lxmax;
    case REG_LYMAX: return db.lymax;
    case REG_LCXP:  return db.lcxp;
    case REG_LCYP:  return db.lcyp;
    case REG_LCWCH: return db.lcwch;
    case REG_LGPMR: return db.lgpmr;
    case REG_LSSA:     return (uint16_t)(db.lssa >> 16);
    case REG_LSSA + 2: return (uint16_t)db.lssa;
    default:
        return ((uint16_t)db_lcd_read8(offset) << 8) | db_lcd_read8(offset + 1);
    }
}

uint32_t db_lcd_read32(uint16_t offset)
{
    if (offset == REG_LSSA) return db.lssa;
    return ((uint32_t)db_lcd_read16(offset) << 16) | db_lcd_read16(offset + 2);
}

void db_lcd_write8(uint16_t offset, uint8_t val)
{
    switch (offset) {
    case REG_LVPW:    db.lvpw = val; break;
    case REG_LBLKC:   db.lblkc = val; break;
    case REG_LPICF:   db.lpicf = val; break;
    case REG_LPOLCF:  db.lpolcf = val; break;
    case REG_LACDRC:  db.lacdrc = val; break;
    case REG_LPXCD:   db.lpxcd = val; break;
    case REG_LCKCON:  db.lckcon = val; break;
    case REG_LRRA:    db.lrra = val; break;
    case REG_LDMR:    db.ldmr = val; break;
    }
}

void db_lcd_write16(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_LXMAX:    db.lxmax = val; break;
    case REG_LYMAX:    db.lymax = val; break;
    case REG_LCXP:     db.lcxp = val; break;
    case REG_LCYP:     db.lcyp = val; break;
    case REG_LCWCH:    db.lcwch = val; break;
    case REG_LGPMR:    db.lgpmr = val; break;
    case REG_LSSA:     db.lssa = ((uint32_t)val << 16) | (db.lssa & 0xFFFF); break;
    case REG_LSSA + 2: db.lssa = (db.lssa & 0xFFFF0000) | val; break;
    default:
        db_lcd_write8(offset, (uint8_t)(val >> 8));
        db_lcd_write8(offset + 1, (uint8_t)val);
        break;
    }
}

void db_lcd_write32(uint16_t offset, uint32_t val)
{
    if (offset == REG_LSSA) {
        db.lssa = val;
        return;
    }
    db_lcd_write16(offset, (uint16_t)(val >> 16));
    db_lcd_write16(offset + 2, (uint16_t)val);
}
