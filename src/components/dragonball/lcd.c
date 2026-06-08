#include "dragonball.h"

uint32_t db_lcd_framebuffer_addr(void)
{
    return db.lssa;
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
