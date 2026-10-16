#include "dragonball.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "dragonball";

dragonball_state_t db;

void dragonball_init(void)
{
    memset(&db, 0, sizeof(db));
    db_spi_reset();

    /* MC68EZ328 reset defaults */
    db.scr = 0x14;              /* EZ328 chip ID */
    db.imr = 0x00FFFFFF;        /* All interrupts masked */
    db.ivr = 0x40;

    /* PLL: report 16.58 MHz */
    db.pllcr = 0x2400;
    db.pllfsr = 0x0123;

    /* Chip selects: CSA maps entire space at reset (ROM everywhere) */
    db.cs[0] = 0x00E0;  /* CSA0: enabled, all sizes */
    db.cs[1] = 0x0000;  /* CSA1 */

    /* LCD defaults: 160x160, 4bpp */
    db.lxmax = 160;
    db.lymax = 160;
    db.lpicf = 0x01;
    db.lvpw = 10;

    /* Port F bit 1 = PENIRQ, active low. Start with pen up (bit = 1). */
    db.port_data[5] = 0x02;

    /* Port K data: bits 7:6 = 1 → HwrPreDebugInit detects 1 card slot */
    db.port_data[8] = 0xC0;

    ESP_LOGI(TAG, "DragonBall EZ initialized");
}

/* Forward declarations for per-peripheral handlers */
extern uint8_t  db_int_read8(uint16_t offset);
extern uint16_t db_int_read16(uint16_t offset);
extern uint32_t db_int_read32(uint16_t offset);
extern void     db_int_write8(uint16_t offset, uint8_t val);
extern void     db_int_write16(uint16_t offset, uint16_t val);
extern void     db_int_write32(uint16_t offset, uint32_t val);

extern uint8_t  db_lcd_read8(uint16_t offset);
extern uint16_t db_lcd_read16(uint16_t offset);
extern uint32_t db_lcd_read32(uint16_t offset);
extern void     db_lcd_write8(uint16_t offset, uint8_t val);
extern void     db_lcd_write16(uint16_t offset, uint16_t val);
extern void     db_lcd_write32(uint16_t offset, uint32_t val);

extern uint8_t  db_timer_read8(uint16_t offset);
extern uint16_t db_timer_read16(uint16_t offset);
extern void     db_timer_write8(uint16_t offset, uint8_t val);
extern void     db_timer_write16(uint16_t offset, uint16_t val);

extern uint8_t  db_rtc_read8(uint16_t offset);
extern uint16_t db_rtc_read16(uint16_t offset);
extern uint32_t db_rtc_read32(uint16_t offset);
extern void     db_rtc_write8(uint16_t offset, uint8_t val);
extern void     db_rtc_write16(uint16_t offset, uint16_t val);
extern void     db_rtc_write32(uint16_t offset, uint32_t val);

extern uint8_t  db_gpio_read8(uint16_t offset);
extern void     db_gpio_write8(uint16_t offset, uint8_t val);

extern uint16_t db_spi_read16(uint16_t offset);
extern void     db_spi_write16(uint16_t offset, uint16_t val);

extern uint16_t db_uart_read16(uint16_t offset);
extern void     db_uart_write16(uint16_t offset, uint16_t val);

/* --- Dispatch reads --- */

uint8_t dragonball_read8(uint16_t offset)
{
    if (offset == REG_SCR) return db.scr;
    if (offset >= 0x300 && offset < 0x320) return db_int_read8(offset);
    if (offset >= 0x400 && offset < 0x450) return db_gpio_read8(offset);
    if (offset >= 0x600 && offset < 0x620) return db_timer_read8(offset);
    if (offset >= 0xA00 && offset < 0xA40) return db_lcd_read8(offset);
    if (offset >= 0xB00 && offset < 0xB20) return db_rtc_read8(offset);

    if (offset == REG_PCTLR) return db.pctlr;

    return 0;
}

uint16_t dragonball_read16(uint16_t offset)
{
    /* Chip select group bases: 0x100-0x106 */
    if (offset >= 0x100 && offset <= 0x106 && !(offset & 1)) {
        return db.grpbase[(offset - 0x100) / 2];
    }
    /* Chip select control: 0x108 */
    if (offset == 0x108) return db.csctrl;
    /* Chip select registers: 0x110-0x11E */
    if (offset >= 0x110 && offset <= 0x11E && !(offset & 1)) {
        return db.cs[(offset - 0x110) / 2];
    }
    if (offset == REG_PLLCR) return db.pllcr;
    if (offset == REG_PLLFSR) {
        db.pllfsr ^= 0x8000; /* CLK32 output toggles on read */
        return db.pllfsr;
    }
    if (offset >= 0x300 && offset < 0x320) return db_int_read16(offset);
    if (offset >= 0x600 && offset < 0x620) return db_timer_read16(offset);
    if (offset >= 0x800 && offset < 0x810) return db_spi_read16(offset);
    if (offset >= 0x900 && offset < 0x910) return db_uart_read16(offset);
    if (offset >= 0xA00 && offset < 0xA40) return db_lcd_read16(offset);
    if (offset >= 0xB00 && offset < 0xB20) return db_rtc_read16(offset);
    if (offset >= 0xC00 && offset < 0xC10) return 0; /* DRAM controller */

    return 0;
}

uint32_t dragonball_read32(uint16_t offset)
{
    if (offset >= 0x300 && offset < 0x320) return db_int_read32(offset);
    if (offset >= 0xA00 && offset < 0xA40) return db_lcd_read32(offset);
    if (offset >= 0xB00 && offset < 0xB20) return db_rtc_read32(offset);

    return ((uint32_t)dragonball_read16(offset) << 16) |
           dragonball_read16(offset + 2);
}

/* --- Dispatch writes --- */

void dragonball_write8(uint16_t offset, uint8_t val)
{
    if (offset == REG_SCR) { db.scr = val; return; }
    if (offset >= 0x300 && offset < 0x320) { db_int_write8(offset, val); return; }
    if (offset >= 0x400 && offset < 0x450) { db_gpio_write8(offset, val); return; }
    if (offset >= 0x600 && offset < 0x620) { db_timer_write8(offset, val); return; }
    if (offset >= 0xA00 && offset < 0xA40) { db_lcd_write8(offset, val); return; }
    if (offset >= 0xB00 && offset < 0xB20) { db_rtc_write8(offset, val); return; }
    if (offset == REG_PCTLR) { db.pctlr = val; return; }
}

void dragonball_write16(uint16_t offset, uint16_t val)
{
    if (offset >= 0x100 && offset <= 0x106 && !(offset & 1)) {
        db.grpbase[(offset - 0x100) / 2] = val;
        return;
    }
    if (offset == 0x108) { db.csctrl = val; return; }
    if (offset >= 0x110 && offset <= 0x11E && !(offset & 1)) {
        db.cs[(offset - 0x110) / 2] = val;
        return;
    }
    if (offset == REG_PLLCR) { db.pllcr = val; return; }
    if (offset == REG_PLLFSR) { db.pllfsr = val; return; }
    if (offset >= 0x300 && offset < 0x320) { db_int_write16(offset, val); return; }
    if (offset >= 0x600 && offset < 0x620) { db_timer_write16(offset, val); return; }
    if (offset >= 0x800 && offset < 0x810) { db_spi_write16(offset, val); return; }
    if (offset >= 0x900 && offset < 0x910) { db_uart_write16(offset, val); return; }
    if (offset >= 0xA00 && offset < 0xA40) { db_lcd_write16(offset, val); return; }
    if (offset >= 0xB00 && offset < 0xB20) { db_rtc_write16(offset, val); return; }
}

void dragonball_write32(uint16_t offset, uint32_t val)
{
    if (offset >= 0x300 && offset < 0x320) { db_int_write32(offset, val); return; }
    if (offset >= 0xA00 && offset < 0xA40) { db_lcd_write32(offset, val); return; }
    if (offset >= 0xB00 && offset < 0xB20) { db_rtc_write32(offset, val); return; }

    dragonball_write16(offset, (uint16_t)(val >> 16));
    dragonball_write16(offset + 2, (uint16_t)val);
}
