/* Exercise the real SPI peripheral through its registers. The interrupt
 * controller is replaced by its pending-bit boundary; no ROM is needed. */
#include "dragonball.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

dragonball_state_t db;
uint16_t db_spi_read16(uint16_t offset);
void db_spi_write16(uint16_t offset, uint16_t val);

void db_interrupt_raise(int source) { db.ipr |= 1u << source; }
void db_interrupt_clear(int source) { db.ipr &= ~(1u << source); }

static uint16_t exchange(uint16_t data, int bits)
{
    db_spi_write16(REG_SPIMDATA, data);
    db_spi_write16(REG_SPIMCONT, 0x2200 | SPIM_XCH | SPIM_IRQEN | (bits - 1));
    assert(!(db_spi_read16(REG_SPIMCONT) & SPIM_XCH));
    assert(db_spi_read16(REG_SPIMCONT) & SPIM_SPMIRQ);
    assert(db.ipr & (1u << INT_SPIM));
    return db_spi_read16(REG_SPIMDATA);
}

static void reset(void)
{
    memset(&db, 0, sizeof(db));
    db_spi_reset();
    db_spi_set_battery_sample(0x0C80);
}

int main(void)
{
    /* OS 4.1's actual battery-read sequence: command A4 sent as 52/7 bits,
     * then 0/1 bit, followed by a 16-clock result read. A zero result makes
     * the ROM enter critical-battery sleep on the Welcome screen. */
    reset();
    exchange(0x52, 7);
    assert(db_spi_xch_count() == 0);
    db_spi_write16(REG_SPIMCONT, 0x2240);  /* configuration write, no clocks */
    exchange(0, 1);
    assert(db_spi_xch_count() == 1);
    assert(((exchange(0, 16) >> 3) & 0xFFF) == 0xC80);
    db_spi_write16(REG_SPIMCONT, 0);
    assert(!(db.ipr & (1u << INT_SPIM)));

    /* Every possible split of the eight-bit command, with either PD0 value.
     * This also protects the complete low-byte command used by older ROMs. */
    for (int pd0 = 0; pd0 <= 1; pd0++) {
        for (int first = 1; first <= 8; first++) {
            reset();
            uint8_t cmd = 0xA4 | pd0;
            exchange(0, 1);              /* idle clock before the start bit */
            exchange(cmd >> (8 - first), first);
            if (first < 8)
                exchange(cmd & ((1u << (8 - first)) - 1), 8 - first);
            assert(db_spi_xch_count() == 1);
            assert(exchange(0, 16) == (0xC80 << 3));
        }
    }

    /* Preserve the high-byte digitizer path, result alignment, and PENIRQ
     * power-down control while changing the low-bit command decoder. */
    reset();
    db_spi_set_touch(40, 80, true);
    assert(exchange(0xD000, 16) == (40 * 25 << 4));
    assert(db.ipr & (1u << INT_PEN));
    assert(exchange(0x9100, 16) == (80 * 25 << 4));
    assert(!(db.ipr & (1u << INT_PEN)));
    assert(exchange(0x9000, 16) == (80 * 25 << 4));
    assert(db.ipr & (1u << INT_PEN));
    db_spi_set_touch(-1, -1, false);
    assert(!(db.ipr & (1u << INT_PEN)));

    /* Reset must discard both a partial command and a pending conversion,
     * while retaining the battery reading supplied by the board. */
    exchange(0x52, 7);
    db_spi_set_battery_sample(0xFF0);
    db_spi_reset();
    assert(!db_spi_pen_down() && db_spi_xch_count() == 0);
    exchange(0xA4, 8);
    assert(exchange(0, 16) == (0xFF0 << 3));
    exchange(0xA4, 8);
    db_spi_reset();
    assert(exchange(0, 16) == 0);

    puts("ALL SPI TESTS PASSED");
    return 0;
}
