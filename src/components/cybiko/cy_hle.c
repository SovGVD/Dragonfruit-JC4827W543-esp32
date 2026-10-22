/* Native stand-ins for a few hot CyOS and boot ROM routines.
 *
 * The routine's first instruction word is replaced by CY_HLE_OP, an opcode
 * the H8S does not define. When the CPU reaches it, the core reports an
 * illegal instruction and cy_hle() runs the routine in C, charges the states
 * the original loop would have taken (so emulated time is unchanged) and
 * returns to the caller. Each routine is found by matching its code, so
 * other ROM or CyOS versions simply run unpatched.
 *
 *   ROM page checksum (CyOS 1.5.08 kernel): the RAM disk checks every page
 *     with it at start-up, about half of all instructions until the desktop.
 *   ROM 32-bit multiply (boot ROM 1.5): a shift-and-add loop the loader uses
 *     to unscramble data.
 *   ROM row blit (boot ROM 1.5): draws one row of a 2-bit bitmap into a
 *     screen buffer. Games spend most of their time here.
 *   LCD row loop (CyOS 1.5.08 kernel): sends one 40-byte row to the LCD
 *     controller, about a fifth of all instructions while the screen moves.
 *
 * The blit is checked against the original code by building the PC runner
 * with -DCY_HLE_CHECK (see the end of this file). */
#include <string.h>
#include "cy_internal.h"

#define AMASK 0xffffffu

/* uint32 sum(const uint8 *p (er0), int32 len (er1)): for each byte
 *   v = ((v ^ b ^ i) << 1) | (bit 16 of that);  */
static const uint8_t page_sum_code[] = {
    0x01, 0x10, 0x6d, 0xf4, 0x0f, 0x82, 0x1a, 0x80, 0x1a, 0xb3, 0x1f, 0x90, 0x4c, 0x28,
    0x0f, 0xa4, 0x6c, 0x4a, 0xf2, 0x00, 0x17, 0x72, 0x01, 0xf0, 0x65, 0x20, 0x01, 0xf0,
    0x65, 0x30, 0x10, 0x30, 0x0f, 0x82, 0x0d, 0xa2, 0x19, 0xaa, 0xea, 0x01, 0xe2, 0x00,
    0x17, 0x72, 0x01, 0xf0, 0x64, 0x20, 0x0b, 0x03, 0x1f, 0x93, 0x4d, 0xda, 0x01, 0x10,
    0x6d, 0x75, 0x54, 0x70,
};
/* States per byte of the loop above (19 words fetched from DRAM, one byte
 * read, post-increment, branch taken) and for entry and exit. */
#define PAGE_SUM_BYTE 62
#define PAGE_SUM_FIXED 40

/* uint32 mul(er0, er1): er2 = 0; while (er0) { if (er0 & 1) er2 += er1;
 *   er0 >>= 1; er1 <<= 1; } er0 = er2. */
static const uint8_t mul_code[] = {
    0x1a, 0xa2, 0x0f, 0x80, 0x47, 0x0c, 0x77, 0x08, 0x44, 0x02, 0x0a, 0x92,
    0x11, 0x30, 0x10, 0x31, 0x40, 0xf0, 0x0f, 0xa0, 0x54, 0x70,
};
#define MUL_STEP 10     /* states per bit, all from on-chip ROM */
#define MUL_FIXED 6

/* void blit_row(const uint8 *src (er0), int sx (r1), uint8 *dst (er2),
 *   int dx, int w, int mode, const uint8 *table, char mirror) — the start of
 * a compiled C routine; blit_row() below follows it step by step. */
static const uint8_t blit_code[] = {
    0x7a, 0x37, 0x00, 0x00, 0x00, 0x18, 0x01, 0x20, 0x6d, 0xf4, 0x01, 0x00, 0x6f, 0xf0, 0x00, 0x20,
    0x0f, 0xa6, 0x6f, 0x73, 0x00, 0x2e, 0x58, 0xf0, 0x08, 0x36, 0x6f, 0x74, 0x00, 0x2a, 0x6f, 0xf4,
    0x00, 0x0e, 0x0d, 0x45, 0x09, 0x35, 0x6f, 0xf5, 0x00, 0x0e, 0x6f, 0x72, 0x00, 0x0e, 0x1b, 0x02,
    0x0d, 0x20, 0x0d, 0x22, 0x4c, 0x06, 0x6f, 0x70, 0x00, 0x0e, 0x0b, 0x80, 0xe8, 0xfc, 0x19, 0x02,
};

/* mov.b #4,r2l; mov.b r2l,@0x100000 (LCD index = data); sub.w r3,r3;
 * 1: mov.b @er5+,r2l; mov.b r2l,@er0; adds #1,er3; cmp.w #0x27,r3; bls 1b
 * The trap goes on the sub.w. */
static const uint8_t lcd_code[] = {
    0xfa, 0x04, 0x6a, 0xaa, 0x00, 0x10, 0x00, 0x00, 0x19, 0x33, 0x6c, 0x5a, 0x68, 0x8a,
    0x0b, 0x03, 0x79, 0x23, 0x00, 0x27, 0x43, 0xf4,
};
#define LCD_TRAP 8

static void put_trap(uint8_t *p)
{
    p[0] = (uint8_t)(CY_HLE_OP >> 8);
    p[1] = (uint8_t)CY_HLE_OP;
}

/* Find code in [mem, mem+len) on an even offset; returns the offset or -1. */
static int32_t find(const uint8_t *mem, uint32_t len, const uint8_t *code, uint32_t n)
{
    for (uint32_t i = 0; i + n <= len; i += 2)
        if (mem[i] == code[0] && memcmp(mem + i, code, n) == 0) return (int32_t)i;
    return -1;
}

static uint32_t patch_rom(cybiko_t *m, const uint8_t *code, uint32_t n)
{
    int32_t at = find(m->rom, CY_ROM_SIZE, code, n);
    if (at < 0) return 0xffffffffu;
    put_trap(m->rom + at);
    put_trap(m->rom + CY_ROM_SIZE + at);    /* the mirror */
    return (uint32_t)at;
}

void cy_hle_patch_rom(cybiko_t *m)
{
    m->hle_mul_pc = patch_rom(m, mul_code, sizeof mul_code);
    m->hle_blit_pc = patch_rom(m, blit_code, sizeof blit_code);
}

void cy_hle_patch_kernel(cybiko_t *m)
{
    int32_t at = find(m->ram, CY_RAM_SIZE, page_sum_code, sizeof page_sum_code);
    if (at >= 0) {
        put_trap(m->ram + at);
        m->hle_sum_pc = 0x400000u + (uint32_t)at;
    }
    at = find(m->ram, CY_RAM_SIZE, lcd_code, sizeof lcd_code);
    if (at >= 0) {
        put_trap(m->ram + at + LCD_TRAP);
        m->hle_lcd_pc = 0x400000u + (uint32_t)at + LCD_TRAP;
    }
}

/* Is pc the ROM routine at `at` (or its mirror)? */
static bool rom_pc(uint32_t pc, uint32_t at)
{
    return pc < 0x10000u && (pc & ~CY_ROM_SIZE) == at;
}

/* Memory access for the native routines, which charge their states as a
 * whole: no per-access states for directly mapped memory and on-chip RAM. */
static uint8_t peek8(h8s_cpu_t *c, uint32_t a)
{
    a &= AMASK;
    const uint8_t *p = c->rpage[a >> 16];
    if (p) return p[a & 0xffff];
    if (a - c->fine_base < c->fine_size) return c->fine_mem[a - c->fine_base];
    return h8s_read8(c, a);
}

static void poke8(h8s_cpu_t *c, uint32_t a, uint8_t v)
{
    a &= AMASK;
    uint8_t *p = c->wpage[a >> 16];
    if (p) p[a & 0xffff] = v;
    else if (a - c->fine_base < c->fine_size) c->fine_mem[a - c->fine_base] = v;
    else h8s_write8(c, a, v);
}

static uint16_t peek16(h8s_cpu_t *c, uint32_t a)
{
    return (uint16_t)(peek8(c, a) << 8 | peek8(c, a + 1));
}

static void ret(h8s_cpu_t *c)
{
    c->pc = h8s_read32(c, c->er[7]) & AMASK;
    c->er[7] += 4;
}

/* ------------------------------------------------------------------ */
/* Row blit                                                            */
/* ------------------------------------------------------------------ */

/* Bitmaps are 2 bits a pixel, 4 pixels a byte, leftmost pixel in the top
 * bits. Tables the boot ROM builds in on-chip RAM: */
#define T_START  0xffed04u   /* [dx]: mask of pixels dx..3 */
#define T_END    0xffed08u   /* [(dx+w-1)%4]: mask of pixels 0..n */
#define T_MIRROR 0xfff1feu   /* [b]: b with its 4 pixels in reverse order */

typedef struct {
    h8s_cpu_t *c;
    uint32_t dst, table;
    uint16_t mode;
} blit_t;

/* Write v under mask mk: mode 0 copies, mode 1 copies only the pixels that
 * table[v] marks (transparency), any other mode XORs. */
static void blit_put(blit_t *b, uint8_t v, uint8_t mk)
{
    h8s_cpu_t *c = b->c;
    uint8_t d = peek8(c, b->dst);
    if (b->mode == 0) d = (uint8_t)((d & ~mk) | (v & mk));
    else if (b->mode == 1) {
        uint8_t t = peek8(c, b->table + v) & mk;
        d = (uint8_t)((d & ~t) | (v & t));
    } else d = (uint8_t)((d & ~mk) | ((v ^ d) & mk));
    poke8(c, b->dst, d);
    b->dst++;
}

/* The ROM's shift loop: shift a word left by the low byte of n, if that is
 * positive as a signed byte. */
static uint16_t blit_shl(uint16_t v, uint16_t n)
{
    int k = (int8_t)(uint8_t)n;
    if (k <= 0) return v;
    return k >= 16 ? 0 : (uint16_t)(v << k);
}

/* States the original takes: a fixed part plus so many a pixel, by path
 * (source aligned with the destination or shifted) and mode. Fitted against
 * the interpreter with CY_HLE_CHECK; within about 5% for most calls. */
static const uint8_t blit_fixed[2] = { 75, 86 };            /* x4 */
static const uint8_t blit_pixel[2][3] = { { 12, 26, 13 }, { 28, 43, 28 } };

static void blit_row(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    const uint32_t sp = c->er[7];               /* return address at sp */
    uint32_t src = c->er[0];
    uint16_t sx = (uint16_t)c->er[1];
    blit_t b = { c, c->er[2], (uint32_t)peek16(c, sp + 0x10) << 16 | peek16(c, sp + 0x12),
                 peek16(c, sp + 0x0e) };
    const uint16_t dx = peek16(c, sp + 0x06), w = peek16(c, sp + 0x0a);
    const uint8_t mirror = peek8(c, sp + 0x17);
    uint8_t r0l = (uint8_t)c->er[0];
    bool shifted = false;
#define SRC_BYTE() (mirror ? peek8(c, T_MIRROR + peek8(c, src)) : peek8(c, src))
#define NEXT_SRC() (src += mirror ? 0xffffffffu : 1u)

    if ((int16_t)w > 0) {
        /* (dx + w - 1) % 4 the way C rounds it, sign included */
        const uint16_t last = (uint16_t)(dx + w - 1);
        const uint16_t down = (uint16_t)(((int16_t)last >= 0 ? last : (uint16_t)(dx + w + 2)) & 0xfffc);
        const uint8_t end = peek8(c, T_END + (uint32_t)(int32_t)(int16_t)(uint16_t)(last - down));
        uint16_t mask = peek8(c, T_START + (uint32_t)(int32_t)(int16_t)dx);
        if (mirror) sx = (uint16_t)(3 - sx);
        uint16_t bits = (uint16_t)(w << 1);
        const uint16_t sb = (uint16_t)(sx << 1), db = (uint16_t)(dx << 1);
        poke8(c, sp + 0x06, (uint8_t)(db >> 8));    /* the ROM keeps these */
        poke8(c, sp + 0x07, (uint8_t)db);           /* in its argument slots */

        if (sb == db) {
            r0l = SRC_BYTE();
            bits = (uint16_t)(bits - 8 + db);
            if ((int16_t)bits > 0) {
                blit_put(&b, r0l, (uint8_t)mask);
                mask = 0xffff;
                for (;;) {
                    NEXT_SRC();
                    r0l = SRC_BYTE();
                    bits = (uint16_t)(bits - 8);
                    if ((int16_t)bits <= 0) break;
                    blit_put(&b, r0l, 0xff);
                }
            }
        } else {
            uint16_t l, r, e = 0;  /* left and right shifts, shifted source word */
            shifted = true;
            if ((int16_t)sb >= (int16_t)db) {
                l = (uint16_t)(sb - db);
                r = (uint16_t)(8 - l);
                r0l = (uint8_t)blit_shl(SRC_BYTE(), l);
                bits = (uint16_t)(bits - 8 + sb);
                if ((int16_t)bits > 0) {
                    NEXT_SRC();
                    e = blit_shl(SRC_BYTE(), l);
                    r0l |= (uint8_t)(e >> 8);
                    bits = (uint16_t)(bits - l);
                }
            } else {
                r = (uint16_t)(db - sb);
                l = (uint16_t)(8 - r);
                e = blit_shl(SRC_BYTE(), l);
                r0l = (uint8_t)(e >> 8);
                bits = (uint16_t)(bits - 8 + db);
            }
            if ((int16_t)bits > 0) {
                blit_put(&b, r0l, (uint8_t)mask);
                mask = 0xffff;
                for (;;) {
                    r0l = (uint8_t)e;
                    bits = (uint16_t)(bits - r);
                    if ((int16_t)bits <= 0) break;
                    NEXT_SRC();
                    e = blit_shl(SRC_BYTE(), l);
                    r0l |= (uint8_t)(e >> 8);
                    bits = (uint16_t)(bits - l);
                    if ((int16_t)bits <= 0) break;
                    blit_put(&b, r0l, 0xff);
                }
            }
        }
        /* the last byte, under the end mask */
        blit_put(&b, r0l, (uint8_t)(mask & end));
        poke8(c, sp + 0x0a, (uint8_t)(bits >> 8));
        poke8(c, sp + 0x0b, (uint8_t)bits);
    }
#undef SRC_BYTE
#undef NEXT_SRC
    c->er[0] = (c->er[0] & ~0xffu) | r0l;
    c->icount -= (int16_t)w <= 0 ? 30 :
        blit_fixed[shifted] * 4 + (int32_t)w * blit_pixel[shifted][b.mode > 2 ? 2 : b.mode];
    ret(c);
}

/* ------------------------------------------------------------------ */
/* LCD row loop                                                        */
/* ------------------------------------------------------------------ */

static void lcd_row(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    /* Per byte: 6 instruction words, the post-increment's internal state and
     * the taken branch (all but the last time); the data accesses charge
     * their own states. The trap word stood for the sub.w. */
    const int32_t words = c->cost16[(c->ppc >> 16) & 0xff];
    uint8_t v = 0;
    for (int i = 0; i < 40; i++) {
        v = h8s_read8(c, c->er[5]);
        c->er[5] += 1;
        h8s_write8(c, c->er[0], v);
    }
    c->icount -= 40 * (6 * words + 1) + 39;
    c->er[2] = (c->er[2] & ~0xffu) | v;
    c->er[3] = (c->er[3] & 0xffff0000u) | 0x28;
    c->ccr &= (uint8_t)~0x2f;                   /* cmp.w #0x27 of 0x28: all clear */
    c->pc = (c->ppc + 14) & AMASK;
}

#ifdef CY_HLE_CHECK
static bool blit_check(cybiko_t *m);
static bool blit_check_return(cybiko_t *m);
#endif

bool cy_hle(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    if (c->ppc == m->hle_lcd_pc) {
        lcd_row(m);
        return true;
    }
    if (rom_pc(c->ppc, m->hle_blit_pc)) {
#ifdef CY_HLE_CHECK
        if (blit_check(m)) return true;
#endif
        blit_row(m);
        return true;
    }
#ifdef CY_HLE_CHECK
    if (blit_check_return(m)) return true;
#endif
    if (c->ppc == m->hle_sum_pc) {
        uint32_t p = c->er[0], v = 0, bit = 0;
        int32_t len = (int32_t)c->er[1];
        for (int32_t i = 0; i < len; i++) {
            v = (v ^ peek8(c, p + (uint32_t)i) ^ (uint32_t)i) << 1;
            bit = (v >> 16) & 1;
            v |= bit;
        }
        /* Registers and flags as the loop leaves them (er4/er5 are restored). */
        c->er[2] = len > 0 ? bit : p;
        c->er[3] = len > 0 ? (uint32_t)len : 0;
        uint32_t r = len > 0 ? 0 : 0u - (uint32_t)len;   /* the last cmp.l */
        c->ccr = (uint8_t)((c->ccr & 0xf0) | (r ? 0 : 0x04) | (r & 0x80000000u ? 0x08 : 0) |
                           (len < 0 ? 0x01 : 0) | ((uint32_t)len == 0x80000000u ? 0x02 : 0));
        c->er[0] = v;
        c->icount -= PAGE_SUM_FIXED + (len > 0 ? len * PAGE_SUM_BYTE : 0);
        ret(c);
        return true;
    }
    if (c->ppc == m->hle_mul_pc) {
        uint32_t a = c->er[0], b = c->er[1], r = 0;
        int steps = 0;
        while (a) {
            if (a & 1) r += b;
            a >>= 1;
            b <<= 1;
            steps++;
        }
        c->er[0] = c->er[2] = r;
        c->er[1] = b;
        c->ccr = (uint8_t)((c->ccr & 0xf1) | (r ? 0 : 0x04) | (r & 0x80000000u ? 0x08 : 0));
        c->icount -= MUL_FIXED + steps * MUL_STEP;
        ret(c);
        return true;
    }
    return false;
}

#ifdef CY_HLE_CHECK
/* Debug check of blit_row (PC runner built with -DCY_HLE_CHECK): every call
 * runs both ways. The native version runs first and its result is put
 * aside; then the original runs from the same state, with a trap on the
 * return address, and the bytes around the destination, the argument slots
 * and the states taken are compared. Lines go to stderr:
 *   BLITCHK shifted mode mirror width original_states native_states */
#include <stdio.h>

#define CHK_WIN 112
#define CHK_ARGS 20
static struct {
    bool active;
    uint32_t ret_pc, sp, dst;
    uint8_t ret_word[2], want[CHK_WIN], want_args[CHK_ARGS];
    uint64_t t0;
    int32_t want_states;
    unsigned mode, mirror, w, sx, dx;
    uint32_t calls, bad;
} chk;

static bool blit_check(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    const uint32_t sp = c->er[7], dst = c->er[2] - 8;
    const int16_t w = (int16_t)peek16(c, sp + 0x0a);
    if (chk.active || w > 4 * (CHK_WIN - 24)) return false;
    uint8_t win[CHK_WIN], args[CHK_ARGS];
    for (int i = 0; i < CHK_WIN; i++) win[i] = peek8(c, dst + (uint32_t)i);
    for (int i = 0; i < CHK_ARGS; i++) args[i] = peek8(c, sp + 4 + (uint32_t)i);
    chk.mode = peek16(c, sp + 0x0e);
    chk.mirror = peek8(c, sp + 0x17);
    chk.w = (uint16_t)w;
    chk.sx = (uint16_t)c->er[1];
    chk.dx = peek16(c, sp + 0x06);
    const h8s_cpu_t saved = *c;
    blit_row(m);
    chk.want_states = saved.icount - c->icount;
    for (int i = 0; i < CHK_WIN; i++) { chk.want[i] = peek8(c, dst + (uint32_t)i); poke8(c, dst + (uint32_t)i, win[i]); }
    for (int i = 0; i < CHK_ARGS; i++) { chk.want_args[i] = peek8(c, sp + 4 + (uint32_t)i); poke8(c, sp + 4 + (uint32_t)i, args[i]); }
    *c = saved;
    chk.t0 = h8s_time(c);
    chk.dst = dst;
    chk.sp = sp + 4;
    chk.ret_pc = (uint32_t)peek16(c, sp) << 16 | peek16(c, sp + 2);
    chk.ret_pc &= AMASK;
    uint8_t *p = c->rpage[chk.ret_pc >> 16] + (chk.ret_pc & 0xffff);
    chk.ret_word[0] = p[0];
    chk.ret_word[1] = p[1];
    put_trap(p);
    chk.active = true;
    /* the original's first instruction, sub.l #0x18,er7 (3 words) */
    c->er[7] -= 0x18;
    c->icount -= 2 * c->cost16[0];
    c->pc = (c->ppc + 6) & AMASK;
    return true;
}

static bool blit_check_return(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    if (!chk.active || c->ppc != chk.ret_pc) return false;
    uint8_t *p = c->rpage[chk.ret_pc >> 16] + (chk.ret_pc & 0xffff);
    p[0] = chk.ret_word[0];
    p[1] = chk.ret_word[1];
    c->icount += c->cost16[chk.ret_pc >> 16];   /* it is fetched again */
    c->pc = c->ppc;
    chk.active = false;
    int32_t got = (int32_t)(h8s_time(c) - chk.t0);
    int diff = -1;
    for (int i = 0; i < CHK_WIN && diff < 0; i++) if (peek8(c, chk.dst + (uint32_t)i) != chk.want[i]) diff = i;
    for (int i = 0; i < CHK_ARGS && diff < 0; i++) if (peek8(c, chk.sp + (uint32_t)i) != chk.want_args[i]) diff = 1000 + i;
    chk.calls++;
    if (c->er[7] != chk.sp) fprintf(stderr, "BLITCHK sp mismatch %06x %06x\n", (unsigned)c->er[7], (unsigned)chk.sp);
    if (diff >= 0 && ++chk.bad <= 30) {
        fprintf(stderr, "BLITBAD call %u at %s+%d: mode %u mirror %u w %u sx %u dx %u\n  orig:", chk.calls,
                diff >= 1000 ? "args" : "dst-8", diff % 1000, chk.mode, chk.mirror, chk.w, chk.sx, chk.dx);
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", peek8(c, chk.dst + (uint32_t)i));
        fprintf(stderr, "\n  hle: ");
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", chk.want[i]);
        fprintf(stderr, "\n");
    }
    unsigned sb = (chk.mirror ? 3 - chk.sx : chk.sx) & 0xffff;
    fprintf(stderr, "BLITCHK %d %u %u %u %d %d\n", sb != chk.dx, chk.mode, chk.mirror, chk.w,
            got, chk.want_states);
    return true;
}

/* Random calls of the blit, each checked as above. Run it on a machine that
 * has booted (the ROM builds its tables at start-up). Uses RAM at 5f0000. */
uint32_t cy_hle_blit_fuzz(cybiko_t *m, int n, uint32_t seed)
{
    h8s_cpu_t *c = &m->cpu;
    if (m->hle_blit_pc == 0xffffffffu) return 0;
    const h8s_cpu_t saved = *c;
    const uint32_t base = 0x5f0000u, ret_at = base, table = base + 0x100, src0 = base + 0x1000,
                   dst0 = base + 0x2000, stack = base + 0xff00;
    const uint32_t bad0 = chk.bad;
#define RND() (seed = seed * 1103515245u + 12345u, seed >> 8)
    for (int k = 0; k < n; k++) {
        for (uint32_t i = 0; i < 0x300; i++) poke8(c, src0 + i, (uint8_t)RND());
        for (uint32_t i = 0; i < 0x100; i++) poke8(c, dst0 + i, (uint8_t)RND());
        for (uint32_t i = 0; i < 0x100; i++) poke8(c, table + i, (uint8_t)RND());
        poke8(c, ret_at, 0x00); poke8(c, ret_at + 1, 0x00);        /* nop */
        uint32_t sp = stack;
        const uint16_t w = (uint16_t)(RND() % 8 == 0 ? (RND() % 12) - 4 : 1 + RND() % 200);
        const uint16_t dx = (uint16_t)(RND() % 4), mode = (uint16_t)(RND() % 3);
        const uint8_t mirror = (uint8_t)(RND() & 1);
        const uint32_t slots[6] = { ret_at, dx, w, mode, table, mirror };
        for (int i = 0; i < 6; i++) {
            for (int j = 0; j < 4; j++) poke8(c, sp + (uint32_t)(i * 4 + j), (uint8_t)(slots[i] >> (24 - 8 * j)));
        }
        c->er[7] = sp;
        c->er[0] = src0 + 0x100 + RND() % 0x80;
        c->er[1] = RND() % 4;
        c->er[2] = dst0 + 0x10 + RND() % 0x40;
        c->ccr |= 0x80;
        c->exr |= 7;
        c->pc = m->hle_blit_pc;
        c->sleeping = false;
        for (int guard = 0; c->pc != ret_at + 2 && guard < 1000000; guard++) h8s_run(c, 1);
    }
#undef RND
    *c = saved;
    return chk.bad - bad0;
}
#endif
