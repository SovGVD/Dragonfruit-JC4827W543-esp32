#include "dragonball.h"
#include "esp_log.h"

/* Map internal peripheral sources to the IRQ-level bits that Palm OS
 * checks in the ISR register's upper word.  On the real MC68EZ328,
 * the ISR reflects both the specific source bit AND the IRQ-level bit. */
static uint32_t irq_level_bit(int source)
{
    /* Each EZ328 interrupt source has its OWN ISR/IPR bit; sources are NOT
     * aliased onto the external IRQ1/3/6 bits.  The interrupt LEVEL (used to
     * pick the CPU priority + vector) is computed separately in
     * db_interrupt_pending_level.  Aliasing Timer/PWM onto IRQ6 (bit 19) would
     * be fatal: the ROM's shared level-6 ISR (0x10C70160) reads ISR-high bit 3
     * (=IRQ6) as the *sleep/power* signal, so a timer tick that also set IRQ6
     * would make every tick suspend the device.  Keep sources on their own bits. */
    (void)source;
    return 0;
}

void db_interrupt_raise(int source)
{
    db.ipr |= (1u << source);
    db.isr |= (1u << source) | irq_level_bit(source);
}

void db_interrupt_clear(int source)
{
    db.ipr &= ~(1u << source);
    db.isr &= ~((1u << source) | irq_level_bit(source));
}

int db_interrupt_pending_level(void)
{
    uint32_t active = db.ipr & ~db.imr;
    if (active == 0) return -1;

    /* MC68EZ328 interrupt priority mapping (from POSE/CloudPilot).
     * Level 7: NMI (not used)
     * Level 6: IRQ6, Timer, PWM
     * Level 5: PEN (digitizer pen-down)
     * Level 4: SPIM, UART, RTC, Keyboard, WDT
     * Level 3: IRQ3
     * Level 2: IRQ2
     * Level 1: IRQ1
     */
    if (active & (1u << INT_IRQ6))  return 6;
    if (active & (1u << INT_TMR))   return 6;
    if (active & (1u << INT_PWM))   return 6;
    if (active & (1u << INT_PEN))   return 5;
    if (active & (1u << INT_SPIM))  return 4;
    if (active & (1u << INT_UART))  return 4;
    if (active & (1u << INT_RTC))   return 4;
    if (active & (1u << INT_KB))    return 4;
    if (active & (1u << INT_WDT))   return 4;
    if (active & (1u << INT_IRQ3))  return 3;
    if (active & (1u << INT_IRQ2))  return 2;
    if (active & (1u << INT_IRQ1))  return 1;

    return 1;
}

bool db_iwr_wake_pending(void)
{
    /* Honour the IMR mask. The EZ328 has no real IWR (we synthesise one to keep
     * timer/PEN as STOP wake sources during light doze — see db_int_write*), but
     * a source the ROM has MASKED must never wake the CPU. In particular, during
     * PLL-off deep sleep the ROM sets IMR=0xFFFEFFBF (only KB+IRQ1 enabled); if a
     * still-pending, IWR-forced source (e.g. the timer) could wake through this
     * path it would spuriously pull the device out of deep sleep. Masking with
     * ~db.imr makes doze wakes (timer unmasked) work while deep sleep (timer
     * masked) stays down. */
    return (db.ipr & db.iwr & ~db.imr) != 0;
}

/* DragonBall vectored-interrupt base: vector = (IVR & 0xF8) + level. */
uint32_t db_int_vector_base(void)
{
    return (uint32_t)(db.ivr & 0xF8);
}

uint32_t db_int_ivr_raw(void) { return db.ivr; }

uint16_t db_int_read16(uint16_t offset);
void     db_int_write16(uint16_t offset, uint16_t val);

uint8_t db_int_read8(uint16_t offset)
{
    if (offset == REG_IVR) return db.ivr;
    if (offset == REG_IVR + 1) return 0;
    uint16_t w = db_int_read16((uint16_t)(offset & ~1u));
    return (offset & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8);
}

uint16_t db_int_read16(uint16_t offset)
{
    switch (offset) {
    case REG_ICR:  return db.icr;
    case REG_IMR:  return (uint16_t)(db.imr >> 16);
    case REG_IMR + 2: return (uint16_t)db.imr;
    case REG_IWR:  return (uint16_t)(db.iwr >> 16);
    case REG_IWR + 2: return (uint16_t)db.iwr;
    case REG_ISR:
        /* Upper word of the 32-bit ISR (bits 16-31), raw.  Do NOT synthesize
         * IRQ6 (bit 19) from the timer compare flag — IRQ6 is the ROM's
         * sleep/power signal, distinct from the Timer interrupt (ISR-low
         * bit 1).  See irq_level_bit(). */
        return (uint16_t)(db.isr >> 16);
    case REG_ISR + 2: return (uint16_t)db.isr;
    case REG_IPR:  return (uint16_t)(db.ipr >> 16);
    case REG_IPR + 2: return (uint16_t)db.ipr;
    default: return 0;
    }
}

uint32_t db_int_read32(uint16_t offset)
{
    switch (offset) {
    case REG_IMR: return db.imr;
    case REG_IWR: return db.iwr;
    case REG_ISR: return db.isr;
    case REG_IPR: return db.ipr;
    default: return 0;
    }
}

void db_int_write8(uint16_t offset, uint8_t val)
{
    if (offset == REG_IVR) { db.ivr = val; return; }
    if (offset == REG_IVR + 1) return;
    /* ISR is write-1-to-clear: only set clear bits in the written byte. */
    if (offset >= REG_ISR && offset < REG_ISR + 4) {
        uint16_t mask = (offset & 1) ? (uint16_t)val : ((uint16_t)val << 8);
        db_int_write16((uint16_t)(offset & ~1u), mask);
        return;
    }
    uint16_t base = (uint16_t)(offset & ~1u);
    uint16_t cur  = db_int_read16(base);
    uint16_t next = (offset & 1)
                        ? ((cur & 0xFF00) | val)
                        : (((uint16_t)val << 8) | (cur & 0x00FF));
    db_int_write16(base, next);
}

void db_int_write16(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_ICR:       db.icr = val; break;
    case REG_IMR:       db.imr = ((uint32_t)val << 16) | (db.imr & 0xFFFF); break;
    case REG_IMR + 2:   db.imr = (db.imr & 0xFFFF0000) | val; break;
    case REG_IWR:       db.iwr = ((uint32_t)(val | (1u << (INT_PEN - 16))) << 16) | (db.iwr & 0xFFFF); break;
    case REG_IWR + 2:
        /* Keep timer, UART, and PEN bits in IWR so they can wake from STOP. */
        db.iwr = (db.iwr & 0xFFFF0000) | (val | (1u << INT_TMR) | (1u << INT_UART));
        break;
    case REG_ISR:       db.isr &= ~((uint32_t)val << 16); break;
    case REG_ISR + 2:   db.isr &= ~val; break;
    }
}

void db_int_write32(uint16_t offset, uint32_t val)
{
    switch (offset) {
    case REG_IMR: db.imr = val; break;
    case REG_IWR: db.iwr = val | (1u << INT_TMR) | (1u << INT_UART) | (1u << INT_PEN); break;
    case REG_ISR: db.isr &= ~val; break;  /* Write-1-to-clear */
    }
}
