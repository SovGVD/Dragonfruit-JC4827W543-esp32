#include "dragonball.h"

/* Port index from register offset: A=0..G=6, J=7, K=8 */
static int port_index(uint16_t offset)
{
    if (offset >= 0x400 && offset < 0x408) return 0; /* A */
    if (offset >= 0x408 && offset < 0x410) return 1; /* B */
    if (offset >= 0x410 && offset < 0x418) return 2; /* C */
    if (offset >= 0x418 && offset < 0x420) return 3; /* D */
    if (offset >= 0x420 && offset < 0x428) return 4; /* E */
    if (offset >= 0x428 && offset < 0x430) return 5; /* F */
    if (offset >= 0x430 && offset < 0x438) return 6; /* G */
    if (offset >= 0x438 && offset < 0x440) return 7; /* J */
    if (offset >= 0x440 && offset < 0x448) return 8; /* K */
    return -1;
}

static uint16_t port_base(int p)
{
    if (p <= 6) return 0x400 + p * 8;
    if (p == 7) return 0x438;
    return 0x440;
}

/* Assert/deassert a key in the scanned matrix. A held key drives the keyboard
 * interrupt (hwrEZ328IntLoKbd) — the wake source the ROM leaves enabled while
 * asleep, and how the power key wakes the device. */
void db_kbd_set(int row, int col, bool pressed)
{
    uint16_t mask = 1u << (row * 4 + col);
    uint16_t old = db.kbd_keys;
    if (pressed)
        db.kbd_keys |= mask;
    else
        db.kbd_keys &= ~mask;

    /* Edge-triggered: assert KB once on the key-down edge (the ROM's ISR then
     * masks KB and debounces by polling). Clear it once all keys are released.
     * Holding it asserted level-style retriggers the ISR endlessly. */
    if (db.kbd_keys && !old)
        db_interrupt_raise(INT_KB);
    else if (!db.kbd_keys)
        db_interrupt_clear(INT_KB);
}

/* Port D key columns (bits 0-3, active high) for whichever Port F rows are
 * currently driven low (selected). Rows: PortF bit4=Row0, bit5=Row1, bit6=Row2. */
static uint8_t db_kbd_columns(void)
{
    uint8_t cols = 0;
    uint8_t fdir = db.port_dir[5], fdata = db.port_data[5];
    for (int row = 0; row < 3; row++) {
        uint8_t rowbit = PF_KBD_ROW0 << row;
        if ((fdir & rowbit) && !(fdata & rowbit)) {   /* output, driven low = selected */
            for (int col = 0; col < 4; col++)
                if (db.kbd_keys & (1u << (row * 4 + col)))
                    cols |= (1u << col);
        }
    }
    return cols & PD_KBD_COLS;
}

bool db_is_asleep(void)
{
    /* DISPLL bit in PLLCR: when set the PLL/clock is stopped (deep sleep,
     * display off). */
    return (db.pllcr & PLLCR_DISPLL) != 0;
}

void db_cpu_resume(void)
{
    /* The EZ328 restarts the PLL automatically when the CPU wakes from STOP;
     * clear DISPLL so db_is_asleep() reflects that the device is awake. */
    db.pllcr &= ~PLLCR_DISPLL;
}

uint8_t db_gpio_read8(uint16_t offset)
{
    int p = port_index(offset);
    if (p < 0) return 0;

    int sub = offset - port_base(p);
    switch (sub) {
    case 0: return db.port_dir[p];
    case 1: {
        uint8_t dir = db.port_dir[p];
        uint8_t puen = db.port_puen[p];
        uint8_t val = (db.port_data[p] & dir) | (~dir & puen);
        /* Port F bit 1 is PENIRQ — driven externally by ADS7846.
         * Override pull-up with actual pen state from port_data.
         * On Palm IIIc (Austin), Port F bit 0 (LCDPowered) and bit 7 (IXTRNL2,
         * active-low modem sync-port ID) are inputs that must read high so the
         * ROM does not mistake the cradle port for a modem or hang in wake. */
        if (p == 5)
            val = ((val & ~PF_PENIRQ) | (db.port_data[5] & PF_PENIRQ)) | (~dir & 0x81);
        if (p == 3) {
            /* Bits 0-3 = scanned keyboard columns for the selected rows.
             * Bits 4 (Dock button, aka IRQ1), 6 (AdapterInstalled on IIIc), and
             * 7 (Power Fail) are active-low inputs forced HIGH (deasserted): if
             * Power Fail reads low the ROM's battery code powers the device
             * down, and a low Dock bit makes HotSync use the modem instead of
             * serial. */
            val = (val & ~(PD_KBD_COLS | PD_DOCK_IRQ1 | PD_POWER_FAIL))
                  | db_kbd_columns() | PD_DOCK_IRQ1 | PD_POWER_FAIL | (~dir & 0x40);
        }
        return val;
    }
    case 2: return db.port_puen[p];
    case 3: return db.port_sel[p];
    }

    if (p == 3) {
        switch (offset) {
        case REG_PDPOL:     return db.pd_pol;
        case REG_PDIRQEN:   return db.pd_irqen;
        case REG_PDIRQEDGE: return db.pd_irqedge;
        }
    }

    return 0;
}

void db_gpio_write8(uint16_t offset, uint8_t val)
{
    int p = port_index(offset);
    if (p < 0) return;

    int sub = offset - port_base(p);
    switch (sub) {
    case 0: db.port_dir[p] = val; return;
    case 1: db.port_data[p] = val; return;
    case 2: db.port_puen[p] = val; return;
    case 3: db.port_sel[p] = val; return;
    }

    if (p == 3) {
        switch (offset) {
        case REG_PDPOL:     db.pd_pol = val; break;
        case REG_PDIRQEN:   db.pd_irqen = val; break;
        case REG_PDIRQEDGE: db.pd_irqedge = val; break;
        }
    }
}
