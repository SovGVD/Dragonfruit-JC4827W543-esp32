/* H8S/2323 on-chip peripherals: 16-bit timer pulse unit (TPU), 8-bit
 * timers (TMR), watchdog (WDT), serial ports (SCI) and the A/D converter.
 *
 * Everything runs off the CPU's state count. cy_periph_advance() moves the
 * peripherals forward and cy_periph_next_event() tells the run loop how far
 * it may let the CPU go before something can change, so slices end exactly
 * on timer events. Flag/interrupt behaviour follows MAME's h8_timer16,
 * h8_timer8, h8_watchdog, h8_sci and h8_adc devices. */
#include <string.h>
#include "cy_internal.h"

#define NO_EVENT 0xffffffffu

/* A CPU read of a register that only changes at a known time (a timer
 * counter's next count, a serial frame ending). When the same instruction
 * read the same value only POLL_GAP states ago, the program is spinning until
 * the register changes (CyOS's delay routine on TPU5 and its clock on TMR0 do
 * this for most of the time a video plays, the MP3 player waits for each byte
 * to go out), so let the time until the change pass at once, no further than
 * the slice end (the next peripheral event). The loop then sees the new value
 * as soon as it would have, minus the spinning. */
#define POLL_GAP 256

static void poll_read(cybiko_t *m, cy_poll_t *p, uint16_t value, uint32_t until_count)
{
    uint64_t now = h8s_time(&m->cpu);
    if (m->dxc) return;
    if (p->pc == m->cpu.ppc && p->value == value && now - p->at <= POLL_GAP) {
        int32_t skip = until_count < (uint32_t)m->cpu.icount ? (int32_t)until_count : m->cpu.icount;
        if (skip > 0) {
            m->cpu.icount -= skip;
            now += (uint32_t)skip;
        }
    }
    p->pc = m->cpu.ppc;
    p->value = value;
    p->at = now;
}

/* ------------------------------------------------------------------ */
/* TPU                                                                 */
/* ------------------------------------------------------------------ */

static const struct {
    uint32_t base;
    uint8_t ntgr, tier_mask, vec;
    int8_t shift[8];          /* prescaler as a power of two; -1 = pin input */
} tpu_cfg[6] = {
    { 0xffffd0, 4, 0x60, 32, { 0, 2, 4, 6, -1, -1, -1, -1 } },
    { 0xffffe0, 2, 0x4c, 40, { 0, 2, 4, 6, -1, -1,  8, -1 } },
    { 0xfffff0, 2, 0x4c, 44, { 0, 2, 4, 6, -1, -1, -1, 10 } },
    { 0xfffe80, 4, 0x60, 48, { 0, 2, 4, 6, -1, 10,  8, 12 } },
    { 0xfffe90, 2, 0x4c, 56, { 0, 2, 4, 6, -1, -1, 10, -1 } },
    { 0xfffea0, 2, 0x4c, 60, { 0, 2, 4, 6, -1, -1,  8, -1 } },
};

static int tpu_shift(const cybiko_t *m, int ch)
{
    if (!(m->tstr >> ch & 1)) return -1;
    const cy_tpu_ch_t *t = &m->tpu[ch];
    int s = tpu_cfg[ch].shift[t->tcr & 7];
    if (s > 0 && (t->tcr & 0x10)) s--;          /* count on both edges */
    return s;
}

static int tpu_clear_tgr(const cybiko_t *m, int ch)
{
    uint8_t tcr = m->tpu[ch].tcr;
    switch (tcr & 0x60) {
    case 0x20: return (tpu_cfg[ch].ntgr > 2 && (tcr & 0x80)) ? 2 : 0;
    case 0x40: return (tpu_cfg[ch].ntgr > 2 && (tcr & 0x80)) ? 3 : 1;
    default: return -1;
    }
}

static uint32_t tpu_cycle(const cybiko_t *m, int ch)
{
    int clr = tpu_clear_tgr(m, ch);
    return clr >= 0 ? (uint32_t)m->tpu[ch].tgr[clr] + 1 : 0x10000;
}

/* Counts from c to the next compare match, clear or overflow. */
static uint32_t tpu_step(const cybiko_t *m, int ch, uint32_t c, uint32_t cycle, uint32_t *limit_out)
{
    const cy_tpu_ch_t *t = &m->tpu[ch];
    uint32_t limit = c < cycle ? cycle : 0x10000;
    uint32_t step = limit - c;
    for (int i = 0; i < tpu_cfg[ch].ntgr; i++) {
        uint32_t cmp = (uint32_t)t->tgr[i] + 1;
        if (cmp > c && cmp <= limit && cmp - c < step) step = cmp - c;
    }
    *limit_out = limit;
    return step;
}

static void tpu_flag(cybiko_t *m, int ch, int flag)
{
    cy_tpu_ch_t *t = &m->tpu[ch];
    t->tsr |= (uint8_t)(1 << flag);
    if (!(t->tier >> flag & 1)) return;
    int vec;
    if (tpu_cfg[ch].ntgr == 4) vec = flag <= 4 ? tpu_cfg[ch].vec + flag : -1;
    else vec = flag < 2 ? tpu_cfg[ch].vec + flag : flag == 4 ? tpu_cfg[ch].vec + 2 : -1;
    if (vec >= 0) cy_irq(m, vec);
}

static void tpu_count(cybiko_t *m, int ch, uint32_t n)
{
    cy_tpu_ch_t *t = &m->tpu[ch];
    uint32_t cycle = tpu_cycle(m, ch);
    uint32_t c = t->tcnt;
    while (n) {
        uint32_t limit;
        uint32_t step = tpu_step(m, ch, c, cycle, &limit);
        if (step > n) { c += n; break; }
        c += step;
        n -= step;
        for (int i = 0; i < tpu_cfg[ch].ntgr; i++)
            if ((uint32_t)t->tgr[i] + 1 == c) tpu_flag(m, ch, i);
        if (c == limit) {
            if (limit == 0x10000) tpu_flag(m, ch, 4);
            c = 0;
        }
    }
    t->tcnt = (uint16_t)c;
}

static void tpu_advance(cybiko_t *m, uint32_t states)
{
    for (int ch = 0; ch < 6; ch++) {
        int s = tpu_shift(m, ch);
        if (s < 0) continue;
        cy_tpu_ch_t *t = &m->tpu[ch];
        uint64_t acc = (uint64_t)t->acc + states;
        uint32_t n = (uint32_t)(acc >> s);
        t->acc = (uint32_t)(acc & ((1u << s) - 1));
        if (n) tpu_count(m, ch, n);
    }
}

static uint32_t tpu_next(const cybiko_t *m)
{
    uint32_t best = NO_EVENT;
    for (int ch = 0; ch < 6; ch++) {
        int s = tpu_shift(m, ch);
        if (s < 0) continue;
        const cy_tpu_ch_t *t = &m->tpu[ch];
        if (!(t->tier & 0x1f)) continue;        /* nothing can interrupt */
        uint32_t limit;
        uint64_t st = ((uint64_t)tpu_step(m, ch, t->tcnt, tpu_cycle(m, ch), &limit) << s) - t->acc;
        if (st < best) best = (uint32_t)st;
    }
    return best;
}

static bool tpu_read(cybiko_t *m, uint32_t a, uint8_t *v)
{
    if (a == 0xffffc0) { *v = m->tstr; return true; }
    if (a == 0xffffc1) { *v = m->tsyr; return true; }
    for (int ch = 0; ch < 6; ch++) {
        uint32_t off = a - tpu_cfg[ch].base;
        if (off >= 16) continue;
        cy_tpu_ch_t *t = &m->tpu[ch];
        switch (off) {
        case 0: *v = t->tcr; break;
        case 1: *v = t->tmdr; break;
        case 2: case 3: *v = t->tior[off - 2]; break;
        case 4: *v = (uint8_t)((t->tier & ~tpu_cfg[ch].tier_mask) | 0x40); break;
        case 5: *v = (uint8_t)(0xc0 | t->tsr); break;
        case 6: *v = (uint8_t)(t->tcnt >> 8); break;
        case 7: {
            *v = (uint8_t)t->tcnt;
            int s = tpu_shift(m, ch);
            if (s >= 0) poll_read(m, &t->poll, t->tcnt, (1u << s) - t->acc);
            break;
        }
        default: {
            int i = (int)(off - 8) >> 1;
            *v = i < tpu_cfg[ch].ntgr ? (uint8_t)(off & 1 ? t->tgr[i] : t->tgr[i] >> 8) : 0xff;
        }
        }
        return true;
    }
    return false;
}

static bool tpu_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    if (a == 0xffffc0) {
        for (int ch = 0; ch < 6; ch++)
            if ((v ^ m->tstr) >> ch & 1) m->tpu[ch].acc = 0;
        m->tstr = (uint8_t)(v & 0x3f);
        return true;
    }
    if (a == 0xffffc1) { m->tsyr = v; return true; }
    for (int ch = 0; ch < 6; ch++) {
        uint32_t off = a - tpu_cfg[ch].base;
        if (off >= 16) continue;
        cy_tpu_ch_t *t = &m->tpu[ch];
        switch (off) {
        case 0: t->tcr = v; break;
        case 1: t->tmdr = v; break;
        case 2: case 3: t->tior[off - 2] = v; break;
        case 4: t->tier = (uint8_t)(v & ~tpu_cfg[ch].tier_mask); break;
        case 5: t->tsr &= (uint8_t)(v | tpu_cfg[ch].tier_mask | 0xc0); break;
        case 6: t->tcnt = (uint16_t)((t->tcnt & 0x00ff) | v << 8); break;
        case 7: t->tcnt = (uint16_t)((t->tcnt & 0xff00) | v); break;
        default: {
            int i = (int)(off - 8) >> 1;
            if (i < tpu_cfg[ch].ntgr)
                t->tgr[i] = off & 1 ? (uint16_t)((t->tgr[i] & 0xff00) | v)
                                    : (uint16_t)((t->tgr[i] & 0x00ff) | v << 8);
        }
        }
        return true;
    }
    return false;
}

float cy_tpu_speaker_tone(const cybiko_t *m, float *duty)
{
    int s = tpu_shift(m, 1);
    if (s < 0) return 0;
    const cy_tpu_ch_t *t = &m->tpu[1];
    uint8_t iob = t->tior[0] >> 4;
    uint32_t cycle = tpu_cycle(m, 1);
    float f = (float)(CY_CLOCK_HZ >> s);
    if ((t->tmdr & 0x0f) == 0x03 && (iob & 3) && !(iob & 8)) {   /* PWM mode 2 */
        if (tpu_clear_tgr(m, 1) == 1) return 0;                 /* B is the period */
        *duty = (float)((uint32_t)t->tgr[1] + 1) / (float)cycle;
        return f / (float)cycle;
    }
    if ((iob & 3) == 3 && !(iob & 8) && (uint32_t)t->tgr[1] < cycle) {  /* toggle on match */
        *duty = 0.5f;
        return f / (2.0f * (float)cycle);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* TMR (8-bit timers)                                                  */
/* ------------------------------------------------------------------ */

static int tmr_shift(const cybiko_t *m, int ch)
{
    static const int8_t shift[8] = { -1, 3, 6, 13, -1, -1, -1, -1 };
    return shift[m->tmr[ch].tcr & 7];
}

static bool tmr_chained(const cybiko_t *m, int ch)
{
    return (m->tmr[ch].tcr & 7) == 4;
}

static uint32_t tmr_cycle(const cy_tmr_ch_t *t)
{
    switch (t->tcr & 0x18) {
    case 0x08: return (uint32_t)t->tcor[0] + 1;
    case 0x10: return (uint32_t)t->tcor[1] + 1;
    default: return 0x100;
    }
}

static uint32_t tmr_step(const cy_tmr_ch_t *t, uint32_t c, uint32_t cycle, uint32_t *limit_out)
{
    uint32_t limit = c < cycle ? cycle : 0x100;
    uint32_t step = limit - c;
    for (int i = 0; i < 2; i++) {
        uint32_t cmp = (uint32_t)t->tcor[i] + 1;
        if (cmp > c && cmp <= limit && cmp - c < step) step = cmp - c;
    }
    *limit_out = limit;
    return step;
}

static void tmr_flag(cybiko_t *m, int ch, uint8_t flag, uint8_t enable, int vec_off)
{
    cy_tmr_ch_t *t = &m->tmr[ch];
    if (t->tcsr & flag) return;
    t->tcsr |= flag;
    if (t->tcr & enable) cy_irq(m, CY_VEC_TMR0 + 4 * ch + vec_off);
}

static void tmr_count(cybiko_t *m, int ch, uint32_t n)
{
    cy_tmr_ch_t *t = &m->tmr[ch];
    uint32_t cycle = tmr_cycle(t);
    uint32_t c = t->tcnt;
    while (n) {
        uint32_t limit;
        uint32_t step = tmr_step(t, c, cycle, &limit);
        if (step > n) { c += n; break; }
        c += step;
        n -= step;
        if ((uint32_t)t->tcor[0] + 1 == c) {
            /* TMR1 can count TMR0 compare match A */
            if (ch == 0 && tmr_chained(m, 1)) tmr_count(m, 1, 1);
            tmr_flag(m, ch, 0x40, 0x40, 0);
        }
        if ((uint32_t)t->tcor[1] + 1 == c) tmr_flag(m, ch, 0x80, 0x80, 1);
        if (c == limit) {
            if (limit == 0x100) {
                /* TMR0 can count TMR1 overflows (16-bit mode) */
                if (ch == 1 && tmr_chained(m, 0)) tmr_count(m, 0, 1);
                tmr_flag(m, ch, 0x20, 0x20, 2);
            }
            c = 0;
        }
    }
    t->tcnt = (uint8_t)c;
}

static void tmr_advance(cybiko_t *m, uint32_t states)
{
    for (int ch = 0; ch < 2; ch++) {
        int s = tmr_shift(m, ch);
        if (s < 0) continue;
        cy_tmr_ch_t *t = &m->tmr[ch];
        uint64_t acc = (uint64_t)t->acc + states;
        uint32_t n = (uint32_t)(acc >> s);
        t->acc = (uint32_t)(acc & ((1u << s) - 1));
        if (n) tmr_count(m, ch, n);
    }
}

static uint32_t tmr_next(const cybiko_t *m)
{
    uint32_t best = NO_EVENT;
    for (int ch = 0; ch < 2; ch++) {
        int s = tmr_shift(m, ch);
        if (s < 0) continue;
        const cy_tmr_ch_t *t = &m->tmr[ch];
        /* A chained partner also needs exact compare/overflow timing, and
         * so does a compare match that starts the A/D converter (ADTE). */
        if (!(t->tcr & 0xe0) && !(ch == 0 && (t->tcsr & 0x10)) && !tmr_chained(m, ch ^ 1)) continue;
        uint32_t limit;
        uint64_t st = ((uint64_t)tmr_step(t, t->tcnt, tmr_cycle(t), &limit) << s) - t->acc;
        if (st < best) best = (uint32_t)st;
    }
    return best;
}

static bool tmr_read(cybiko_t *m, uint32_t a, uint8_t *v)
{
    if (a < 0xffffb0 || a > 0xffffb9) return false;
    int ch = (int)(a & 1);
    cy_tmr_ch_t *t = &m->tmr[ch];
    switch (a & ~1u) {
    case 0xffffb0: *v = t->tcr; break;
    case 0xffffb2: *v = t->tcsr; break;
    case 0xffffb4: *v = t->tcor[0]; break;
    case 0xffffb6: *v = t->tcor[1]; break;
    case 0xffffb8: {
        *v = t->tcnt;
        int s = tmr_shift(m, ch);
        if (s >= 0) poll_read(m, &t->poll, t->tcnt, (1u << s) - t->acc);
        break;
    }
    }
    return true;
}

static bool tmr_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    if (a < 0xffffb0 || a > 0xffffb9) return false;
    int ch = (int)(a & 1);
    cy_tmr_ch_t *t = &m->tmr[ch];
    switch (a & ~1u) {
    case 0xffffb0: {
        uint8_t prev = t->tcr;
        if ((prev ^ v) & 7) t->acc = 0;
        t->tcr = v;
        /* enabling an interrupt whose flag is already set fires it */
        if (!(prev & 0x40) && (v & 0x40) && (t->tcsr & 0x40)) cy_irq(m, CY_VEC_TMR0 + 4 * ch);
        if (!(prev & 0x80) && (v & 0x80) && (t->tcsr & 0x80)) cy_irq(m, CY_VEC_TMR0 + 4 * ch + 1);
        if (!(prev & 0x20) && (v & 0x20) && (t->tcsr & 0x20)) cy_irq(m, CY_VEC_TMR0 + 4 * ch + 2);
        break;
    }
    case 0xffffb2: {
        uint8_t mask = ch == 0 ? 0x1f : 0x0f;   /* ADTE only on TMR0 */
        t->tcsr = (uint8_t)((t->tcsr & ~mask) | (v & mask));
        t->tcsr &= (uint8_t)(v | 0x1f);          /* flags clear by writing 0 */
        break;
    }
    case 0xffffb4: t->tcor[0] = v; break;
    case 0xffffb6: t->tcor[1] = v; break;
    case 0xffffb8: t->tcnt = v; break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Watchdog                                                            */
/* ------------------------------------------------------------------ */

static const uint8_t wdt_shift[8] = { 1, 6, 7, 9, 11, 13, 15, 17 };

static void wdt_advance(cybiko_t *m, uint32_t states)
{
    if (!(m->wdt_tcsr & 0x20)) return;
    int s = wdt_shift[m->wdt_tcsr & 7];
    uint64_t acc = (uint64_t)m->wdt_acc + states;
    uint64_t n = acc >> s;
    m->wdt_acc = (uint32_t)(acc & ((1u << s) - 1));
    if (!n) return;
    uint64_t c = m->wdt_tcnt + n;
    m->wdt_tcnt = (uint8_t)c;
    if (c < 0x100) return;
    if (m->wdt_tcsr & 0x40) {                  /* watchdog timer mode */
        m->wdt_rst |= 0x80;                    /* WOVF */
        if (m->wdt_rst & 0x40) {               /* RSTE: reset the chip */
            m->reset_pending = true;
            h8s_end_slice(&m->cpu);
        }
    } else if (!(m->wdt_tcsr & 0x80)) {        /* interval timer mode */
        m->wdt_tcsr |= 0x80;
        cy_irq(m, CY_VEC_WOVI);
    }
}

static uint32_t wdt_next(const cybiko_t *m)
{
    if (!(m->wdt_tcsr & 0x20)) return NO_EVENT;
    int s = wdt_shift[m->wdt_tcsr & 7];
    uint64_t st = ((uint64_t)(0x100 - m->wdt_tcnt) << s) - m->wdt_acc;
    return st > NO_EVENT ? NO_EVENT : (uint32_t)st;
}

static bool wdt_read(cybiko_t *m, uint32_t a, uint8_t *v)
{
    switch (a) {
    case 0xffffbc: *v = (uint8_t)(m->wdt_tcsr | 0x18); return true;
    case 0xffffbd: *v = m->wdt_tcnt; return true;
    case 0xffffbe: *v = 0xff; return true;
    case 0xffffbf: *v = (uint8_t)(m->wdt_rst | 0x1f); return true;
    }
    return false;
}

/* The WDT only takes word writes with a key byte; byte writes are lost. */
static bool wdt_write16(cybiko_t *m, uint32_t a, uint16_t v)
{
    uint8_t key = (uint8_t)(v >> 8), d = (uint8_t)v;
    if (a == 0xffffbc) {
        if (key == 0xa5) {
            if (!(m->wdt_tcsr & 0x20) && (d & 0x20)) m->wdt_acc = 0;
            m->wdt_tcsr = (uint8_t)((m->wdt_tcsr & d & 0x80) | (d & 0x7f));
            if (!(m->wdt_tcsr & 0x20)) m->wdt_tcnt = 0;
        } else if (key == 0x5a) {
            if (m->wdt_tcsr & 0x20) { m->wdt_tcnt = d; m->wdt_acc = 0; }
        }
        return true;
    }
    if (a == 0xffffbe) {
        if (key == 0xa5) m->wdt_rst &= (uint8_t)(d | 0x7f);     /* clear WOVF */
        else if (key == 0x5a) m->wdt_rst = (uint8_t)((m->wdt_rst & 0x80) | (d & 0x60));
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* SCI                                                                 */
/* ------------------------------------------------------------------ */

#define SSR_TDRE 0x80
#define SSR_RDRF 0x40
#define SSR_ORER 0x20
#define SSR_FER  0x10
#define SSR_PER  0x08
#define SSR_TEND 0x04
#define SSR_MPB  0x02
#define SSR_MPBT 0x01
#define SCR_TIE  0x80
#define SCR_RIE  0x40
#define SCR_TE   0x20
#define SCR_RE   0x10
#define SCR_TEIE 0x04

static uint32_t sci_frame(const cy_sci_t *s)
{
    uint32_t n1 = (uint32_t)s->brr + 1;
    int cks = s->smr & 3;
    if (s->smr & 0x80) return (4u << (2 * cks)) * n1 * 8;              /* clocked sync */
    uint32_t bits = 1 + ((s->smr & 0x40) ? 7 : 8) + ((s->smr & 0x20) ? 1 : 0) + ((s->smr & 0x08) ? 2 : 1);
    return (32u << (2 * cks)) * n1 * bits;
}

static int sci_vec(int n, int which) { return CY_VEC_SCI0 + 4 * n + which; }  /* ERI RXI TXI TEI */

static bool sci_error(const cy_sci_t *s) { return s->ssr & (SSR_ORER | SSR_FER | SSR_PER); }

static void sci_tx_start(cybiko_t *m, int n)
{
    cy_sci_t *s = &m->sci[n];
    s->ssr |= SSR_TDRE;
    s->tsr = s->tdr;
    s->tx_busy = true;
    s->tx_left = sci_frame(s);
    /* Last: a DMA or DTC transfer started by TXI refills TDR at once. */
    if (s->scr & SCR_TIE) cy_irq(m, sci_vec(n, 2));
}

static void sci_rx_start(cybiko_t *m, int n)
{
    cy_sci_t *s = &m->sci[n];
    if (s->rx_busy || !(s->scr & SCR_RE) || !s->rx_count) return;
    s->rx_busy = true;
    s->rx_left = sci_frame(s);
}

static void sci_rx_done(cybiko_t *m, int n)
{
    cy_sci_t *s = &m->sci[n];
    uint8_t b = s->rx_fifo[s->rx_head];
    s->rx_head = (uint8_t)((s->rx_head + 1) % sizeof s->rx_fifo);
    s->rx_count--;
    s->rx_busy = false;
    if (s->ssr & SSR_RDRF) {
        s->ssr |= SSR_ORER;
    } else {
        s->ssr |= SSR_RDRF;
        s->rdr = b;
    }
    if (s->scr & SCR_RIE) cy_irq(m, sci_vec(n, sci_error(s) ? 0 : 1));
}

/* The frame being sent has gone out. */
static void sci_tx_done(cybiko_t *m, int n)
{
    cy_sci_t *s = &m->sci[n];
    s->tx_busy = false;
    bool sync = n < 2 && (s->smr & 0x80);
    /* Text goes out asynchronously; in clocked sync mode SCI1 talks to the
     * cartridge's SD card and SCI0 to its decoder, which is not text. */
    if (n == 1 && !sync) cy_disk_console(m, s->tsr);
    if (n == 2) cy_rf_h8_byte(m, s->tsr);
    if (sync) {
        /* clocked sync: a byte comes back for each one sent */
        uint8_t in = cy_cart_spi(m, n, s->tsr);
        if (s->scr & SCR_RE) {
            if (s->ssr & SSR_RDRF) s->ssr |= SSR_ORER;
            else { s->rdr = in; s->ssr |= SSR_RDRF; }
            if (s->scr & SCR_RIE) cy_irq(m, sci_vec(n, sci_error(s) ? 0 : 1));
        }
    }
    if (m->host.serial_tx && !sync) m->host.serial_tx(m->host.user, n, s->tsr);
    if ((s->scr & SCR_TE) && !(s->ssr & SSR_TDRE)) {
        sci_tx_start(m, n);
    } else {
        s->ssr |= SSR_TEND;
        if (s->scr & SCR_TEIE) cy_irq(m, sci_vec(n, 3));
    }
}

static void sci_advance(cybiko_t *m, uint32_t states)
{
    for (int n = 0; n < 3; n++) {
        cy_sci_t *s = &m->sci[n];
        uint32_t left = states;
        while (s->tx_busy) {
            if (left < s->tx_left) { s->tx_left -= left; break; }
            left -= s->tx_left;
            sci_tx_done(m, n);
        }
        left = states;
        while (s->rx_busy) {
            if (left < s->rx_left) { s->rx_left -= left; break; }
            left -= s->rx_left;
            sci_rx_done(m, n);
            sci_rx_start(m, n);
        }
    }
}

static uint32_t sci_next(const cybiko_t *m)
{
    uint32_t best = NO_EVENT;
    for (int n = 0; n < 3; n++) {
        const cy_sci_t *s = &m->sci[n];
        if (s->tx_busy && s->tx_left < best) best = s->tx_left;
        if (s->rx_busy && s->rx_left < best) best = s->rx_left;
    }
    return best;
}

void cy_sci_kick(cybiko_t *m)
{
    cy_sync(m);
    for (int n = 0; n < 3; n++) sci_rx_start(m, n);
}

static int sci_index(uint32_t a, uint32_t *off)
{
    if (a >= 0xffff78 && a <= 0xffff7e) { *off = a - 0xffff78; return 0; }
    if (a >= 0xffff80 && a <= 0xffff86) { *off = a - 0xffff80; return 1; }
    if (a >= 0xffff88 && a <= 0xffff8e) { *off = a - 0xffff88; return 2; }
    return -1;
}

static bool sci_read(cybiko_t *m, uint32_t a, uint8_t *v)
{
    uint32_t off;
    int n = sci_index(a, &off);
    if (n < 0) return false;
    cy_sci_t *s = &m->sci[n];
    switch (off) {
    case 0: *v = s->smr; break;
    case 1: *v = s->brr; break;
    case 2: *v = s->scr; break;
    case 3: *v = s->tdr; break;
    case 4: {
        s->ssr_read = s->ssr;
        *v = s->ssr;
        /* SSR changes when a frame in flight ends (boot code prints by
         * polling TDRE, the MP3 player polls TEND) */
        uint32_t left = s->tx_busy ? s->tx_left : 0;
        if (s->rx_busy && (!left || s->rx_left < left)) left = s->rx_left;
        poll_read(m, &s->poll, s->ssr, left ? left - 1 : 0);
        break;
    }
    case 5:
        *v = s->rdr;
        if (m->dxc) s->ssr &= (uint8_t)~SSR_RDRF;   /* a DMA/DTC read clears RDRF */
        break;
    default: *v = (uint8_t)(m->scmr[n] | 0xf2); break;     /* SCMR */
    }
    return true;
}

static bool sci_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    uint32_t off;
    int n = sci_index(a, &off);
    if (n < 0) return false;
    cy_sci_t *s = &m->sci[n];
    switch (off) {
    case 0: s->smr = v; break;
    case 1: s->brr = v; break;
    case 2: {
        /* Clocked sync: the byte in flight still comes in when the SCI is
         * switched off. The cartridge's SD driver turns it off from the DMA
         * end interrupt just after the last byte starts, which CyOS's
         * interrupt entry takes longer than that byte to reach. */
        if (s->tx_busy && n < 2 && (s->smr & 0x80) && (s->scr & ~v & (SCR_TE | SCR_RE))) sci_tx_done(m, n);
        uint8_t delta = s->scr ^ v;
        s->scr = v;
        if ((delta & SCR_RE) && !(v & SCR_RE)) s->rx_busy = false;
        if ((delta & SCR_TIE) && (v & SCR_TIE) && (s->ssr & SSR_TDRE)) cy_irq(m, sci_vec(n, 2));
        if ((delta & SCR_TEIE) && (v & SCR_TEIE) && (s->ssr & SSR_TEND)) cy_irq(m, sci_vec(n, 3));
        if ((delta & SCR_RIE) && (v & SCR_RIE) && (s->ssr & SSR_RDRF)) cy_irq(m, sci_vec(n, 1));
        if ((delta & SCR_RIE) && (v & SCR_RIE) && sci_error(s)) cy_irq(m, sci_vec(n, 0));
        sci_rx_start(m, n);
        break;
    }
    case 3:
        s->tdr = v;
        /* A DMA/DTC write clears TDRE itself (the CPU clears it in SSR). */
        if (m->dxc && (s->scr & SCR_TE)) {
            s->ssr &= (uint8_t)~(SSR_TDRE | SSR_TEND);
            if (!s->tx_busy) sci_tx_start(m, n);
        }
        break;
    case 4:
        if ((s->scr & SCR_TE) && (s->ssr & s->ssr_read & SSR_TDRE) && !(v & SSR_TDRE))
            s->ssr &= (uint8_t)~(SSR_TDRE | SSR_TEND);
        s->ssr = (uint8_t)((s->ssr & (~s->ssr_read | v | SSR_TDRE | SSR_TEND | SSR_MPB) & ~SSR_MPBT) | (v & SSR_MPBT));
        s->ssr_read &= s->ssr;
        if (!s->tx_busy && !(s->ssr & SSR_TDRE)) sci_tx_start(m, n);
        sci_rx_start(m, n);
        break;
    case 6: m->scmr[n] = v & 0x0d; break;
    default: break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* A/D converter                                                       */
/* ------------------------------------------------------------------ */

static uint32_t adc_time(const cybiko_t *m, bool first)
{
    bool fast = m->adcsr & 0x08;
    if (m->adcr & 0x08) return first ? (fast ? 134 : 266) : (fast ? 128 : 256);
    return first ? (fast ? 68 : 530) : (fast ? 64 : 512);
}

static void adc_advance(cybiko_t *m, uint32_t states)
{
    while (m->adc_left) {
        if (states < m->adc_left) { m->adc_left -= states; return; }
        states -= m->adc_left;
        m->adc_left = 0;
        int ch = m->adc_ch;
        uint16_t val = ch == 3 ? cy_cart_adc(m, ch) : m->host.adc_read ? m->host.adc_read(m->host.user, ch) : 0;
        m->addr[ch & 3] = val & 0x3ff;
        bool scan = m->adcsr & 0x10;
        int end = m->adcsr & 7;
        if (scan && ch != end) {
            m->adc_ch = (ch + 1) & 7;
            m->adc_left = adc_time(m, false);
            continue;
        }
        m->adcsr |= 0x80;                      /* ADF */
        if (m->adcsr & 0x40) cy_irq(m, CY_VEC_ADI);
        if (scan) {
            m->adc_ch = end & 4;
            m->adc_left = adc_time(m, false);
        } else {
            m->adcsr &= (uint8_t)~0x20;        /* single mode: ADST clears */
        }
    }
}

static bool adc_read(cybiko_t *m, uint32_t a, uint8_t *v)
{
    if (a >= 0xffff90 && a <= 0xffff97) {
        uint16_t r = m->addr[(a - 0xffff90) >> 1];
        if (m->dxc) m->adcsr &= (uint8_t)~0x80;      /* a DMA/DTC read clears ADF */
        *v = (a & 1) ? (uint8_t)(r << 6) : (uint8_t)(r >> 2);
        return true;
    }
    if (a == 0xffff98) { *v = m->adcsr; return true; }
    if (a == 0xffff99) { *v = m->adcr; return true; }
    return false;
}

static bool adc_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    if (a >= 0xffff90 && a <= 0xffff97) return true;
    if (a == 0xffff99) { m->adcr = v; return true; }
    if (a != 0xffff98) return false;
    uint8_t prev = m->adcsr;
    m->adcsr = (uint8_t)((v & 0x7f) | (m->adcsr & v & 0x80));
    if (!(prev & 0x20) && (v & 0x20)) {
        m->adc_ch = (v & 0x10) ? (v & 4) : (v & 7);
        m->adc_left = adc_time(m, true);
    } else if (!(v & 0x20)) {
        m->adc_left = 0;
    }
    return true;
}

/* ------------------------------------------------------------------ */

void cy_periph_reset(cybiko_t *m)
{
    for (int ch = 0; ch < 6; ch++) {
        cy_tpu_ch_t *t = &m->tpu[ch];
        memset(t, 0, sizeof *t);
        memset(t->tgr, 0xff, sizeof t->tgr);
    }
    m->tstr = m->tsyr = 0;
    memset(m->tmr, 0, sizeof m->tmr);
    for (int ch = 0; ch < 2; ch++) m->tmr[ch].tcor[0] = m->tmr[ch].tcor[1] = 0xff;
    m->tmr[1].tcsr = 0x10;
    m->wdt_tcsr = m->wdt_tcnt = m->wdt_rst = 0;
    m->wdt_acc = 0;
    for (int n = 0; n < 3; n++) {
        cy_sci_t *s = &m->sci[n];
        uint8_t fifo[sizeof s->rx_fifo], head = s->rx_head, count = s->rx_count;
        memcpy(fifo, s->rx_fifo, sizeof fifo);
        memset(s, 0, sizeof *s);
        memcpy(s->rx_fifo, fifo, sizeof fifo);   /* bytes still on the wire */
        s->rx_head = head;
        s->rx_count = count;
        s->tdr = 0xff;
        s->ssr = SSR_TDRE | SSR_TEND;
        s->brr = 0xff;
        s->tsr = 0xff;
    }
    m->adcsr = m->adcr = 0;
    memset(m->addr, 0, sizeof m->addr);
    m->adc_left = 0;
    m->adc_ch = 0;
}

/* The timers are brought up to date lazily: when one of their registers is
 * read or written, or when their next event is due. Nothing else sees their
 * counters, and catching all of them up on every I/O access took a large
 * share of the emulator's time. Their next event only moves when a timer
 * register is written, so it is worked out again only then and when it
 * passes. */
static void timers_reschedule(cybiko_t *m)
{
    uint32_t best = tpu_next(m), t;
    if ((t = tmr_next(m)) < best) best = t;
    if ((t = wdt_next(m)) < best) best = t;
    m->timers_due = m->timers_synced + best;
}

void cy_timers_sync(cybiko_t *m, uint64_t now)
{
    if (now <= m->timers_synced) return;
    uint64_t d = now - m->timers_synced;
    m->timers_synced = now;
    while (d) {
        uint32_t step = d > 0x40000000u ? 0x40000000u : (uint32_t)d;
        tpu_advance(m, step);
        tmr_advance(m, step);
        wdt_advance(m, step);
        d -= step;
    }
    timers_reschedule(m);
}

void cy_timers_restart(cybiko_t *m)
{
    m->timers_synced = m->synced;
    timers_reschedule(m);
}

static bool timer_reg(uint32_t a)
{
    return (a >= 0xfffe80 && a < 0xfffeb0) ||       /* TPU3-5 */
           (a >= 0xffffb0 && a < 0xffffc2) ||       /* TMR, WDT, TSTR, TSYR */
           a >= 0xffffd0;                           /* TPU0-2 */
}

void cy_periph_advance(cybiko_t *m, uint32_t states)
{
    if (m->synced >= m->timers_due) cy_timers_sync(m, m->synced);
    sci_advance(m, states);
    adc_advance(m, states);
    cy_rf_advance(m);
}

uint32_t cy_periph_next_event(const cybiko_t *m)
{
    uint64_t due = m->timers_due > m->synced ? m->timers_due - m->synced : 1;
    uint32_t best = due < NO_EVENT ? (uint32_t)due : NO_EVENT, t;
    if ((t = sci_next(m)) < best) best = t;
    if ((t = cy_rf_next(m)) < best) best = t;
    if (m->adc_left && m->adc_left < best) best = m->adc_left;
    return best;
}

bool cy_periph_read(cybiko_t *m, uint32_t a, uint8_t *v)
{
    if (timer_reg(a)) cy_timers_sync(m, m->synced);
    return tpu_read(m, a, v) || tmr_read(m, a, v) || wdt_read(m, a, v) ||
           sci_read(m, a, v) || adc_read(m, a, v);
}

bool cy_periph_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    bool hit;
    if (a >= 0xffffbc && a <= 0xffffbf) return true;     /* WDT: word writes only */
    if (timer_reg(a)) {
        cy_timers_sync(m, m->synced);
        hit = tpu_write(m, a, v) || tmr_write(m, a, v);
        if (hit) timers_reschedule(m);
    } else {
        hit = sci_write(m, a, v) || adc_write(m, a, v);
    }
    /* The next timer event may now be sooner than the slice end. */
    if (hit) h8s_end_slice(&m->cpu);
    return hit;
}

bool cy_periph_write16(cybiko_t *m, uint32_t a, uint16_t v)
{
    if (a < 0xffffbc || a > 0xffffbf) return false;
    cy_timers_sync(m, m->synced);
    if (wdt_write16(m, a, v)) { timers_reschedule(m); h8s_end_slice(&m->cpu); return true; }
    return false;
}
