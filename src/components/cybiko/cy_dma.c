/* H8S/2323 DMA controller and data transfer controller (DTC).
 *
 * DMA controller: two channels, each either two short-address
 * subchannels (A and B) or one full-address channel. Modelled on MAME's
 * h8_dma, with the register bits as in the H8S/2357 hardware manual.
 *
 * The registers live in the plain I/O storage (misc[]); a transfer is set up
 * from them when DMABCR enables it. Auto-request transfers run to the end at
 * once. Transfers started by a peripheral interrupt (A/D, SCI, TPU) move one
 * unit (a byte or word, or a block in block mode) each time that interrupt is
 * raised, and swallow the interrupt when DTA is set.
 *
 * Register map: per channel c, base 0xfffee0 + 16c: MARA (4), IOARA (2),
 * ETCRA (2), MARB (4), IOARB (2), ETCRB (2). DMAWER ff00, DMATCR ff01,
 * DMACR c at ff02 + 2c (A high byte, B low byte), DMABCR ff06 (16 bits). */
#include <string.h>
#include "cy_internal.h"

#define IO_BASE 0xfffe80u

enum { TRIG_NONE = -1, TRIG_AUTO = -2, TRIG_DREQ = -3 };

static uint8_t reg8(const cybiko_t *m, uint32_t a) { return m->misc[a - IO_BASE]; }
static uint16_t reg16(const cybiko_t *m, uint32_t a) { return (uint16_t)(reg8(m, a) << 8 | reg8(m, a + 1)); }
static uint32_t reg24(const cybiko_t *m, uint32_t a) { return ((uint32_t)reg16(m, a) << 16 | reg16(m, a + 2)) & 0xffffff; }
static void set16(cybiko_t *m, uint32_t a, uint16_t v)
{
    m->misc[a - IO_BASE] = (uint8_t)(v >> 8);
    m->misc[a + 1 - IO_BASE] = (uint8_t)v;
}
static void set24(cybiko_t *m, uint32_t a, uint32_t v)
{
    set16(m, a, (uint16_t)(v >> 16 & 0xff));
    set16(m, a + 2, (uint16_t)v);
}

static uint16_t dmabcr(const cybiko_t *m) { return reg16(m, 0xffff06); }
static uint16_t dmacr(const cybiko_t *m, int c) { return reg16(m, 0xffff02 + 2u * c); }
static uint32_t ch_base(int c) { return 0xfffee0u + 16u * c; }

/* Channel and subchannel ids: id = 2c + s, s = 0 for A, 1 for B. */
static uint8_t active_mask(const cybiko_t *m)
{
    uint16_t b = dmabcr(m);
    uint8_t res = 0;
    for (int c = 0; c < 2; c++) {
        if (b >> (14 + c) & 1) {
            /* full address: DTE (A) and DTME (B) both set */
            if (((b >> (4 + 2 * c)) & 3) == 3) res |= (uint8_t)(2 << (2 * c));
        } else {
            if (b >> (4 + 2 * c) & 1) res |= (uint8_t)(1 << (2 * c));
            if (b >> (5 + 2 * c) & 1) res |= (uint8_t)(2 << (2 * c));
        }
    }
    return res;
}

static int sae_trigger(int dtf, bool sub_b)
{
    static const int8_t vec[16] = {
        TRIG_NONE, CY_VEC_ADI, TRIG_DREQ, TRIG_DREQ, 82, 81, 86, 85,
        32, 40, 44, 48, 56, 60, TRIG_NONE, TRIG_NONE,
    };
    int v = vec[dtf & 15];
    return v == TRIG_DREQ && !sub_b ? TRIG_NONE : v;
}

static void start(cybiko_t *m, int id)
{
    int c = id >> 1, s = id & 1;
    cy_dma_sub_t *d = &m->dma[id];
    uint16_t b = dmabcr(m), cr = dmacr(m, c);
    uint32_t base = ch_base(c);
    memset(d, 0, sizeof *d);
    d->fae = b >> (14 + c) & 1;
    d->tend = d->fae ? (b >> (2 * c) & 3) != 0 : (b >> id & 1);
    d->eat = d->fae ? (b >> (8 + 2 * c) & 3) != 0 : (b >> (8 + id) & 1);
    if (d->fae) {
        d->word = cr >> 15 & 1;
        int step = d->word ? 2 : 1;
        d->incs = !(cr >> 13 & 1) ? 0 : (cr >> 14 & 1) ? -step : step;
        d->incd = !(cr >> 5 & 1) ? 0 : (cr >> 6 & 1) ? -step : step;
        d->src = reg24(m, base);
        d->dst = reg24(m, base + 8);
        uint16_t etcra = reg16(m, base + 6);
        if (cr >> 11 & 1) {                       /* block transfer mode */
            d->block = true;
            d->block_dest = !(cr >> 12 & 1);      /* BLKDIR = 0: the destination is the block area */
            d->blen = etcra & 0xff ? etcra & 0xff : 0x100;
            d->count = d->blen;
            d->bcount = reg16(m, base + 14) ? reg16(m, base + 14) : 0x10000;
            d->trigger = sae_trigger(cr & 15, true);
        } else {
            d->count = etcra ? etcra : 0x10000;
            int dtf = cr & 15;
            d->trigger = dtf == 6 || dtf == 7 ? TRIG_AUTO : dtf == 2 || dtf == 3 ? TRIG_DREQ : TRIG_NONE;
        }
    } else {
        uint8_t r = s ? (uint8_t)cr : (uint8_t)(cr >> 8);
        uint32_t sub = base + 8u * s;
        d->word = r >> 7 & 1;
        int step = d->word ? 2 : 1;
        uint32_t mar = reg24(m, sub), ioar = 0xff0000u | reg16(m, sub + 4);
        /* RPE with DTIE is idle mode: MAR stays where it is, otherwise it
         * runs like a normal transfer and ends with the DEND interrupt (an
         * SPI driver clocks out a fixed 0xff this way while it receives). */
        d->idle = (r >> 5 & 1) && d->tend;
        int incm = d->idle ? 0 : r >> 6 & 1 ? -step : step;
        d->mar_dest = r >> 4 & 1;                 /* DTDIR: IOAR to MAR */
        if (d->mar_dest) {
            d->src = ioar; d->dst = mar; d->incd = incm;
        } else {
            d->src = mar; d->dst = ioar; d->incs = incm;
        }
        uint16_t etcr = reg16(m, sub + 6);
        d->repeat = (r >> 5 & 1) && !d->tend;
        if (d->repeat) {
            d->blen = etcr & 0xff ? etcr & 0xff : 0x100;
            d->count = d->blen;
        } else {
            d->count = etcr ? etcr : 0x10000;
        }
        d->trigger = sae_trigger(r & 15, s);
    }
    d->active = true;
    if (d->trigger == TRIG_DREQ || d->trigger == TRIG_NONE) m->dtc_dma_use++;   /* not wired */
}

static void done(cybiko_t *m, int id)
{
    cy_dma_sub_t *d = &m->dma[id];
    int c = id >> 1;
    d->active = false;
    /* DTE is cleared at the end; in full address mode that is the A bit,
     * the B bit (DTME) stays set. */
    int bit = d->fae ? 4 + 2 * c : 4 + id;
    set16(m, 0xffff06, (uint16_t)(dmabcr(m) & ~(1u << bit)));
    if (d->tend) cy_irq(m, 72 + (d->fae ? 2 * c : id));
}

/* Moves one unit; returns true when the transfer has finished. */
static bool step_unit(cybiko_t *m, int id);
static bool step(cybiko_t *m, int id)
{
    m->dxc = true;
    bool fin = step_unit(m, id);
    m->dxc = false;
    return fin;
}

static bool step_unit(cybiko_t *m, int id)
{
    cy_dma_sub_t *d = &m->dma[id];
    int c = id >> 1;
    uint32_t base = ch_base(c);
    uint32_t n = d->block ? d->count : 1;
    for (uint32_t i = 0; i < n; i++) {
        if (d->word) {
            h8s_write16(&m->cpu, d->dst & 0xfffffe, h8s_read16(&m->cpu, d->src & 0xfffffe));
        } else {
            h8s_write8(&m->cpu, d->dst & 0xffffff, h8s_read8(&m->cpu, d->src & 0xffffff));
        }
        d->src = (d->src + (uint32_t)d->incs) & 0xffffff;
        d->dst = (d->dst + (uint32_t)d->incd) & 0xffffff;
        m->cpu.icount -= d->word ? 4 : 2;
    }
    if (d->block) {
        if (d->block_dest) d->dst = reg24(m, base + 8);
        else d->src = reg24(m, base);
        set24(m, base, d->src);
        set24(m, base + 8, d->dst);
        set16(m, base + 14, (uint16_t)--d->bcount);
        return d->bcount == 0;
    }
    if (d->fae) {
        set24(m, base, d->src);
        set24(m, base + 8, d->dst);
        set16(m, base + 6, (uint16_t)--d->count);
        return d->count == 0;
    }
    uint32_t sub = base + 8u * (id & 1);
    bool mar_dest = d->mar_dest;
    if (--d->count == 0 && d->repeat) {
        d->count = d->blen;
        uint32_t mar = reg24(m, sub);
        if (mar_dest) d->dst = mar; else d->src = mar;
        return false;
    }
    if (!d->repeat) {
        set24(m, sub, mar_dest ? d->dst : d->src);
        set16(m, sub + 6, (uint16_t)d->count);
    }
    return d->count == 0;
}

void cy_dma_reset(cybiko_t *m)
{
    memset(m->dma, 0, sizeof m->dma);
    m->dma_waiting = false;
    m->dma_running = false;
    m->dma_again = -1;
}

static void update_vectors(cybiko_t *m)
{
    m->dma_waiting = false;
    for (int id = 0; id < 4; id++)
        if (m->dma[id].active && m->dma[id].trigger >= 0) m->dma_waiting = true;
}

void cy_dma_control(cybiko_t *m)
{
    uint8_t want = active_mask(m);
    for (int id = 0; id < 4; id++) {
        bool on = want >> id & 1;
        if (on && !m->dma[id].active) {
            start(m, id);
            if (m->dma[id].trigger == TRIG_AUTO) {
                while (!step(m, id)) {}
                done(m, id);
            }
        } else if (!on && m->dma[id].active) {
            m->dma[id].active = false;
        } else if (on && !m->dma[id].fae) {
            /* DTIE (and DTA) may be set after DTE; DTIE with RPE turns a
             * running repeat transfer into idle mode. */
            cy_dma_sub_t *d = &m->dma[id];
            uint16_t b = dmabcr(m);
            uint8_t r = id & 1 ? (uint8_t)dmacr(m, id >> 1) : (uint8_t)(dmacr(m, id >> 1) >> 8);
            d->tend = b >> id & 1;
            d->eat = b >> (8 + id) & 1;
            bool idle = (r >> 5 & 1) && d->tend;
            if (idle && !d->idle) {
                d->idle = true;
                d->repeat = false;
                if (d->mar_dest) d->incd = 0; else d->incs = 0;
                /* The count is the whole ETCR again, not the repeat length. */
                uint16_t etcr = reg16(m, ch_base(id >> 1) + 8u * (id & 1) + 6);
                d->count = etcr ? etcr : 0x10000;
            }
        }
    }
    update_vectors(m);
}

bool cy_dma_trigger(cybiko_t *m, int vec)
{
    /* A transfer's own write can raise the interrupt again (an SCI starts
     * sending the byte just written and asks for the next one); run that
     * request after this one has updated its addresses and count. */
    if (m->dma_running) {
        bool eat = false;
        for (int id = 0; id < 4; id++) {
            cy_dma_sub_t *d = &m->dma[id];
            if (d->active && d->trigger == vec) eat |= d->eat;
        }
        if (eat || m->dma_again < 0) m->dma_again = (int8_t)vec;
        return eat;
    }
    bool eaten = false;
    m->dma_running = true;
    for (;;) {
        m->dma_again = -1;
        for (int id = 0; id < 4; id++) {
            cy_dma_sub_t *d = &m->dma[id];
            if (!d->active || d->trigger != vec) continue;
            if (step(m, id)) done(m, id);
            eaten |= d->eat;
        }
        if (m->dma_again < 0) break;
        vec = m->dma_again;
    }
    m->dma_running = false;
    update_vectors(m);
    return eaten;
}

/* ------------------------------------------------------------------ */
/* Data transfer controller                                            */
/* ------------------------------------------------------------------ */

/* DTCER bit of each interrupt vector (MAME h8_dtc vector_to_enable, plus
 * TGI3A-TGI5B which the H8S/2323 manual also lists). The bit for slot n is
 * DTCER[n / 8] bit (7 - n % 8). */
static const int8_t dtc_slot[92] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     0,  1,  2,  3,  4,  5,  6,  7, -1, -1, -1, -1,  9, -1, -1, -1,
    10, 11, 12, 13, -1, -1, -1, -1, 14, 15, -1, -1, 16, 17, -1, -1,
    18, 19, 20, 21, -1, -1, -1, -1, 22, 23, -1, -1, 24, 25, -1, -1,
    28, 29, -1, -1, 30, 31, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 36, 37, -1, -1, 38, 39, -1, -1, 40, 41, -1,
};

/* One register set at `base` in on-chip RAM: MRA|SAR, MRB|DAR, CRA, CRB.
 * Returns true when this was its last transfer (the count ran out, or DISEL
 * asks for the CPU interrupt after every transfer). */
static bool dtc_run(cybiko_t *m, uint32_t base)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t sra = h8s_read32(c, base), dar = h8s_read32(c, base + 4);
    uint32_t cr = h8s_read32(c, base + 8);
    bool word = sra & 0x01000000, dts = sra & 0x02000000;
    int step = word ? 2 : 1;
    int incs = !(sra & 0x80000000) ? 0 : (sra & 0x40000000) ? -step : step;
    int incd = !(sra & 0x20000000) ? 0 : (sra & 0x10000000) ? -step : step;
    uint32_t mode = sra & 0x0c000000;
    int n = 1;
    if (mode == 0x08000000) {                 /* block: CRAL units */
        n = (cr >> 16) & 0xff;
        if (!n) n = 256;
    }
    uint32_t src = sra & 0xffffff, dst = dar & 0xffffff;
    for (int i = 0; i < n; i++) {
        if (word) h8s_write16(c, dst & 0xfffffe, h8s_read16(c, src & 0xfffffe));
        else h8s_write8(c, dst, h8s_read8(c, src));
        src = (src + (uint32_t)incs) & 0xffffff;
        dst = (dst + (uint32_t)incd) & 0xffffff;
        c->icount -= word ? 4 : 2;
    }
    bool done = false;
    switch (mode) {
    case 0x04000000: {                        /* repeat: CRAL counts, CRAH reloads */
        uint32_t left = ((cr >> 16) - 1) & 0xff;
        if (!left) {
            int cnt = (cr >> 24) & 0xff;
            if (!cnt) cnt = 256;
            if (dts) src = (src - (uint32_t)(cnt * incs)) & 0xffffff;
            else dst = (dst - (uint32_t)(cnt * incd)) & 0xffffff;
            left = (cr >> 24) & 0xff;
        }
        cr = (cr & 0xff00ffff) | left << 16;
        break;
    }
    case 0x08000000: {                        /* block: the block area rewinds, CRB counts */
        if (dts) src = (src - (uint32_t)(n * incs)) & 0xffffff;
        else dst = (dst - (uint32_t)(n * incd)) & 0xffffff;
        uint32_t blocks = (cr - 1) & 0xffff;
        cr = (cr & 0xff000000) | ((cr >> 8) & 0x00ff0000) | blocks;
        done = blocks == 0;
        break;
    }
    default:                                  /* normal: CRA counts */
        cr -= 0x00010000;
        done = !(cr & 0xffff0000);
        break;
    }
    h8s_write32(c, base, (sra & 0xff000000) | src);
    h8s_write32(c, base + 4, (dar & 0xff000000) | dst);
    h8s_write32(c, base + 8, cr);
    return done || (dar & 0x40000000);
}

bool cy_dtc_trigger(cybiko_t *m, int vec)
{
    int slot = vec < 92 ? dtc_slot[vec] : -1;
    if (slot < 0) return false;
    uint32_t er = 0xffff30u + (uint32_t)(slot >> 3);
    uint8_t bit = (uint8_t)(0x80 >> (slot & 7));
    if (!(reg8(m, er) & bit)) return false;
    /* The vector table entry at 0x400 + 2 * vector points at the register
     * information in on-chip RAM. Chained sets (CHNE) follow it. */
    uint32_t base = 0xff0000u | h8s_read16(&m->cpu, 0x400u + 2u * (uint32_t)vec);
    bool last = false;
    m->dxc = true;
    for (int chain = 0; chain < 64; chain++) {
        uint32_t dar = h8s_read32(&m->cpu, base + 4);
        bool fin = dtc_run(m, base);
        if (!(dar & 0x80000000)) { last = fin; break; }
        base += 12;
    }
    m->dxc = false;
    if (!last) return true;                   /* swallowed */
    m->misc[er - IO_BASE] &= (uint8_t)~bit;   /* DTCE clears; the CPU gets the interrupt */
    return false;
}
