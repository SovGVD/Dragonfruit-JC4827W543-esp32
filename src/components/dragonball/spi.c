#include "dragonball.h"

/*
 * SPI master: used by Palm OS to communicate with the ADS7846
 * resistive touchscreen digitizer. When a pen interrupt occurs and
 * Palm OS reads digitizer coordinates, it issues SPI commands and
 * reads back X/Y values.
 *
 * ADS7846 result format: 12-bit value left-justified in 16 bits
 * (bits 15:4 = result, bits 3:0 = zero). This matches the real
 * ADS7846 output alignment as implemented by the Mu emulator.
 */

static int16_t touch_x = -1;
static int16_t touch_y = -1;
static bool pen_down = false;
static bool pen_irq_enabled = true;
static uint32_t spi_xch_count = 0;

/* HAL reads send the command in the low bits of SPIMDATA, then clock the
 * result through a separate 16-bit exchange. Palm OS 4.1 splits the eight
 * command bits across seven-bit and one-bit exchanges. Retain partial commands
 * until all eight bits arrive. The legacy digitizer path puts the command in
 * the high byte and consumes the result immediately. */
static int  last_channel = 0;
static bool bitbang_pending = false;
static uint8_t command_shift;
static unsigned command_bits;

/* 12-bit ADS7846 sample for the battery (channel 2).  Read by the power-manager
 * via the bit-banged HAL, which computes level = (sample >> 4) and shuts the
 * device down ("battery dead") below the ROM's critical threshold ($138 = 125).
 * Defaults to 0xC80 (level 200, comfortably healthy) until the firmware reports
 * the real M5Paper charge via db_spi_set_battery_sample(); see main.c. */
static uint16_t battery_sample = 0x0C80;

void db_spi_reset(void)
{
    touch_x = touch_y = -1;
    pen_down = false;
    pen_irq_enabled = true;
    spi_xch_count = 0;
    last_channel = 0;
    bitbang_pending = false;
    command_shift = 0;
    command_bits = 0;
    /* The physical battery reading survives an emulated reset. */
}

void db_spi_set_battery_sample(uint16_t sample)
{
    battery_sample = sample & 0x0FFF;
}

/* 12-bit ADS7846 sample for a channel. */
static uint16_t ads7846_sample(int channel)
{
    switch (channel) {
    case 0: return 0x03E2;                                       /* Temp 0   */
    case 1: return pen_down ? (uint16_t)(touch_y * 25) : 0x0FEF; /* Y pos    */
    case 2: return battery_sample;                               /* Battery  */
    case 3: return pen_down ? 0x0600 : 0x0000;                   /* Z1       */
    case 4: return pen_down ? 0x0A00 : 0x0FFF;                   /* Z2       */
    case 5: return pen_down ? (uint16_t)(touch_x * 25) : 0x0309; /* X pos    */
    case 6: return 0x0000;                                       /* Dock/chg */
    case 7: return 0x04A1;                                       /* Temp 1   */
    default: return 0;
    }
}

bool db_spi_pen_down(void) { return pen_down; }
uint32_t db_spi_xch_count(void) { return spi_xch_count; }

void db_spi_set_touch(int16_t x, int16_t y, bool down)
{
    touch_x = x;
    touch_y = y;
    pen_down = down;

    if (down)
        db.port_data[5] &= ~0x02;
    else
        db.port_data[5] |= 0x02;

    if (down)
        db_interrupt_raise(INT_PEN);
    else
        db_interrupt_clear(INT_PEN);
}

uint16_t db_spi_read16(uint16_t offset)
{
    /* Reads are side-effect-free. The SPI master interrupt is managed as a level
     * by db_spim_update_irq() on control writes (see below), so a result read does
     * not need to touch SPMIRQ or INT_SPIM. */
    switch (offset) {
    case REG_SPIMDATA:
        return db.spim_data;
    case REG_SPIMCONT:
        /* The ROM polls SPMIRQ here to detect completion. XCH always reads back
         * clear because the transfer completes synchronously on the write. */
        return db.spim_cont & ~SPIM_XCH;
    default:
        return 0;
    }
}

/* The EZ328 SPI master interrupt request (IPR[SPIM]) is a LEVEL output: asserted
 * only while the transfer-complete status (SPMIRQ) AND the master's own interrupt
 * enable (IRQEN) are both set in SPIMCONT. Re-evaluating it whenever either bit
 * can change mirrors real silicon — so when the ROM disables the SPI by writing
 * SPIMCONT=0 (clearing IRQEN), the request de-asserts on its own. This is why the
 * IIIx, which polls SPMIRQ with SPIM kept masked in the IMR, never takes a SPIM
 * CPU interrupt: it clears IRQEN before it ever unmasks SPIM. Modeling INT_SPIM
 * as a latched edge instead left it asserted across that unmask, and the ROM's
 * level-4 ISR (which never services SPIM) then re-dispatched forever. */
static void db_spim_update_irq(void)
{
    if ((db.spim_cont & (SPIM_SPMIRQ | SPIM_IRQEN)) == (SPIM_SPMIRQ | SPIM_IRQEN))
        db_interrupt_raise(INT_SPIM);
    else
        db_interrupt_clear(INT_SPIM);
}

void db_spi_write16(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_SPIMDATA:
        db.spim_data = val;
        break;

    case REG_SPIMCONT:
        db.spim_cont = val;

        /* If XCH (exchange) bit set, simulate a SPI transfer.
         *
         * ADS7846 command byte:
         *   bit 7    = start (must be 1)
         *   bits 6:4 = channel select (A2:A0)
         *   bit 3    = mode (0=12-bit, 1=8-bit)
         *   bit 2    = SER/DFR (0=differential, 1=single-ended)
         *   bits 1:0 = power-down (PD1:PD0)
         *
         * The command may arrive in the HIGH byte (digitizer routine — result
         * read in the same exchange, as 12-bit value << 4) or in the LOW byte
         * (HAL routine, including commands split across exchanges — result
         * read later and extracted as (SPIMDATA >> 3) & 0xFFF). */
        if (val & SPIM_XCH) {
            uint8_t cmdhi = (uint8_t)(db.spim_data >> 8);
            uint8_t cmdlo = (uint8_t)(db.spim_data & 0xFF);
            uint8_t cmd = 0;
            bool from_low = false;
            if (cmdhi & 0x80) {
                cmd = cmdhi;
                command_shift = 0;
                command_bits = 0;
                bitbang_pending = false;
            } else if ((val & 0x000F) < 8) {
                /* SPIM clocks the low (BIT_COUNT+1) bits, MSB first. Leading
                 * zeroes are idle clocks; a one starts an ADS7846 command. */
                for (int bit = val & 0x000F; bit >= 0; bit--) {
                    unsigned next = (cmdlo >> bit) & 1;
                    if (!command_bits && !next) continue;
                    command_shift = (uint8_t)((command_shift << 1) | next);
                    if (++command_bits == 8) {
                        cmd = command_shift;
                        from_low = true;
                        command_shift = 0;
                        command_bits = 0;
                    }
                }
            }

            if (cmd & 0x80) {
                int channel = (cmd >> 4) & 7;
                last_channel = channel;
                if (from_low) bitbang_pending = true;

                spi_xch_count++;
                db.spim_data = ads7846_sample(channel) << 4;

                /* PD0 controls PENIRQ: enabled when PD0=0 */
                pen_irq_enabled = !(cmd & 0x01);
                if (pen_down && pen_irq_enabled)
                    db_interrupt_raise(INT_PEN);
                else if (!pen_irq_enabled)
                    db_interrupt_clear(INT_PEN);
            } else if (bitbang_pending && (val & 0x000F) == 0x000F) {
                /* Final 16-bit result read of the bit-banged HAL sequence. */
                db.spim_data = ads7846_sample(last_channel) << 3;
                bitbang_pending = false;
            }

            db.spim_cont &= ~SPIM_XCH;     /* transfer done */
            db.spim_cont |=  SPIM_SPMIRQ;  /* completion flagged for the poll */
        }

        /* IPR[SPIM] is a level = SPMIRQ & IRQEN; re-evaluate on every control
         * write so disabling the SPI (IRQEN clear) de-asserts the request. */
        db_spim_update_irq();
        break;
    }
}
