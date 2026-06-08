#include "dragonball.h"
#include "esp_log.h"
#ifndef HOST_BUILD
#include "esp_timer.h"   /* esp_timer_get_time() — real wall-clock microseconds */
#endif

extern void db_rtc_advance(uint32_t cpu_cycles);
static uint32_t total_timer_cycles = 0;

/* Input-clock divider from the TCTL clock-source field times the prescaler.
 * The IIIx ROM drives Timer 1 from SYSCLK direct (÷1) or SYSCLK/16; any other
 * encoding is treated as ÷16. (TPRER+1) prescales on top. Single source of
 * truth for both the tick path and the register-poll path below. */
static int timer_divisor(void)
{
    int clk_src = (db.tctl >> TCTL_CLKSRC_SHIFT) & TCTL_CLKSRC_MASK;
    int div = (clk_src == TCTL_CLK_SYSCLK) ? 1 : 16;
    return div * (db.tprer + 1);
}

void db_timer_advance(uint32_t cpu_cycles)
{
    /* Pace the Palm 100 Hz system tick to real wall-clock, not to emulated CPU
     * cycles.  The ROM programmed Timer1 to compare-fire every ~165800 SYSCLK
     * (16.58 MHz) cycles = 100 Hz.  On the ESP32 we emulate the 68k below real
     * speed, so feeding raw emulated cycles makes the tick — and everything
     * counted in ticks (TimGetTicks, auto-off, key debounce/repeat, animation)
     * — run slow.  Instead we convert real elapsed microseconds to the 16.58
     * MHz cycle-equivalent the ROM expects, so the tick fires at true 100 Hz
     * regardless of emulation speed (the Mu/CloudPilot timebase model).  The
     * host build stays cycle-based so the test harness runs as fast as it can.
     * The RTC time-of-day is already real-time (db_rtc_advance via host time()). */
#ifdef HOST_BUILD
    uint32_t eff_cycles = cpu_cycles;
#else
    (void)cpu_cycles;
    static int64_t last_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (last_us == 0) last_us = now_us;
    int64_t delta_us = now_us - last_us;
    last_us = now_us;
    if (delta_us < 0) delta_us = 0;
    if (delta_us > 100000) delta_us = 100000;   /* cap catch-up after a stall */
    uint32_t eff_cycles = (uint32_t)((delta_us * 1658) / 100);  /* 16.58 cyc/us */
#endif

    total_timer_cycles += eff_cycles;
    if (total_timer_cycles > 20000000)
        db_rtc_advance(eff_cycles);

    if (!(db.tctl & TCTL_TEN))
        return;

    int divisor = timer_divisor();

    db.timer_cycles += eff_cycles;
    uint32_t timer_ticks = db.timer_cycles / divisor;
    db.timer_cycles %= divisor;

    if (timer_ticks == 0)
        return;

    if (db.tcmp > 0) {
        uint32_t total = (uint32_t)db.tcn + timer_ticks;
        if (total >= db.tcmp) {
            db.tstat |= TSTAT_COMP;   /* compare bit set on every match so
                                       * polled busy-wait delays (IRQ_EN=0) work */
            db.tcn = (uint16_t)(total % db.tcmp);
            /* On the IIIx (68EZ328) Timer 1 IS the ~100 Hz system tick: the ROM
             * enables the compare interrupt (TCTL IRQ_EN) and its ISR advances
             * the kernel tick and fires software timers.  So just deliver the
             * interrupt when EnInterrupt is set. */
            if (db.tctl & TCTL_IRQEN)
                db_interrupt_raise(INT_TMR);
        } else {
            db.tcn = (uint16_t)total;
        }
    } else {
        db.tcn += (uint16_t)timer_ticks;
    }
}

static uint16_t db_timer_read16_impl(uint16_t offset);

uint8_t db_timer_read8(uint16_t offset)
{
    return (uint8_t)db_timer_read16_impl(offset & ~1u);
}

uint16_t db_timer_read16(uint16_t offset)
{
    return db_timer_read16_impl(offset);
}

/* Advance timer when registers are polled — Palm OS busy-waits on
 * TSTAT/TCN in various delay loops. */
static void timer_poll_advance(void)
{
    if (db.tcmp > 0 && (db.tctl & TCTL_TEN)) {
        int divisor = timer_divisor();
        db.timer_cycles += 100;
        uint32_t ticks = db.timer_cycles / divisor;
        db.timer_cycles %= divisor;
        if (ticks > 0) {
            uint32_t total = (uint32_t)db.tcn + ticks;
            if (total >= db.tcmp) {
                db.tstat |= TSTAT_COMP;
                db.tcn = (uint16_t)(total % db.tcmp);  /* preserve remainder, as the tick path does */
            } else {
                db.tcn = (uint16_t)total;
            }
        }
    }
}

static uint16_t db_timer_read16_impl(uint16_t offset)
{
    switch (offset) {
    case REG_TCTL:  return db.tctl;
    case REG_TPRER: return db.tprer;
    case REG_TCMP:  return db.tcmp;
    case REG_TCR:   return db.tcr;
    case REG_TCN:   timer_poll_advance(); return db.tcn;
    case REG_TSTAT:
        timer_poll_advance();
        db.tstat_latched |= db.tstat;
        return db.tstat;
    case REG_WCTLR: return db.wctlr;
    case REG_WCMPR: return db.wcmpr;
    case REG_WCNTR: return db.wcntr;
    default: return 0;
    }
}

static void db_timer_write16_impl(uint16_t offset, uint16_t val);

void db_timer_write8(uint16_t offset, uint8_t val)
{
    db_timer_write16_impl(offset & ~1u, (uint16_t)val);
}

void db_timer_write16(uint16_t offset, uint16_t val)
{
    db_timer_write16_impl(offset, val);
}

static void db_timer_write16_impl(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_TCTL:
        db.tctl = val;
        if (val & TCTL_TEN) db.tcn = 0;
        break;
    case REG_TPRER: db.tprer = val; break;
    case REG_TCMP:  db.tcmp = val; break;
    case REG_TCR:   db.tcr = val; break;
    case REG_TSTAT:
        /* Read-then-clear semantics (matching real hardware and CloudPilot):
         * only clear bits that were previously observed via a read.  A blind
         * "clr.w TSTAT" without a prior read is effectively a no-op. */
        db.tstat &= (val | ~db.tstat_latched);
        db.tstat_latched = 0;
        if (!(db.tstat & TSTAT_COMP))
            db_interrupt_clear(INT_TMR);
        break;
    case REG_WCTLR: db.wctlr = val; break;
    case REG_WCMPR: db.wcmpr = val; break;
    case REG_WCNTR: db.wcntr = val; break;
    }
}
