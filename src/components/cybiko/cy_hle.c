/* Native stand-ins for a few hot CyOS and boot ROM routines.
 *
 * The first instruction word of each is replaced by CY_HLE_OP, an opcode
 * the H8S does not define. When the CPU reaches it, the core reports an
 * illegal instruction and cy_hle() runs the code in C, charges the states
 * the original would have taken (so emulated time is unchanged) and goes on
 * where the original would. Each piece is found by matching its code, so
 * other ROM or CyOS versions simply run unpatched.
 *
 *   page sum (CyOS 1.5.08 kernel): the RAM disk checks every page with it
 *     at start-up, about half of all instructions until the desktop.
 *   multiply (boot ROM 1.5): a shift-and-add loop the loader uses to
 *     unscramble data.
 *   row blit (boot ROM): draws one row of a 2-bit bitmap into a screen
 *     buffer. Games spend most of their time here.
 *   LCD row loop (kernel): sends one 40-byte row to the LCD controller,
 *     about a fifth of all instructions while the screen moves.
 *   stream read loop (kernel): copies bytes out of an open file's 8 KB
 *     buffer, a quarter of all instructions while a video plays.
 *   fill loop (kernel): memset.
 *   LZ copy loop (boot ROM): the back-reference copy of the decompressor
 *     videos and compressed files go through.
 *
 * Loops run in C until they end, or until the slice ends or an interrupt
 * is pending; then they stop at the top of an iteration, where the trap
 * brings them back after the interrupt. Everything but the blit leaves
 * registers, flags, memory and states exactly as the original does. The PC
 * runner built with -DCY_HLE_CHECK runs calls both ways and compares them
 * (see the end of this file). */
#include <string.h>
#include "cy_internal.h"

#define AMASK 0xffffffu

enum { HLE_SUM, HLE_MUL, HLE_BLIT, HLE_LCD, HLE_STREAM, HLE_FILL, HLE_LZCOPY, HLE_COUNT };
_Static_assert(HLE_COUNT <= CY_HLE_SLOTS, "cybiko_t.hle_pc too small");

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

/* 1: er0 = f + 0x10; er1 = *er0 (pos); er2 = f + (pos & 0x1fff);
 *    r2l = @(0x14,er2); er3 = @(0x10,sp) (dst); *er3++ = r2l; @(0x10,sp) = er3;
 *    *er0 = ++er1; er6++; er5 = @(0x14,sp); if (er6 == er5) goto done;
 *    er2 = @(0xc,f) (end); if (er1 < er2) goto 1b; (refill)
 * with f = er4, a file handle with its buffer at +0x14. */
static const uint8_t stream_code[] = {
    0x0f, 0xc0, 0x7a, 0x10, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x69, 0x01, 0x0f, 0x95, 0x7a, 0x65,
    0x00, 0x00, 0x1f, 0xff, 0x0f, 0xc2, 0x0a, 0xd2, 0x6e, 0x2a, 0x00, 0x14, 0x01, 0x00, 0x6f, 0x73,
    0x00, 0x10, 0x68, 0xba, 0x0b, 0x03, 0x01, 0x00, 0x6f, 0xf3, 0x00, 0x10, 0x0b, 0x01, 0x01, 0x00,
    0x69, 0x81, 0x0b, 0x06, 0x01, 0x00, 0x6f, 0x75, 0x00, 0x14, 0x1f, 0xd6, 0x47, 0x54, 0x01, 0x00,
    0x6f, 0x42, 0x00, 0x0c, 0x1f, 0xa1, 0x4d, 0xb8,
};

/* 1: r3 = r0; exts.l er3; er2 = er5 + er3; *er2 = r1l; er0++; er4--;
 *    if (er4) goto 1b */
static const uint8_t fill_code[] = {
    0x0d, 0x03, 0x17, 0xf3, 0x0f, 0xd2, 0x0a, 0xb2, 0x68, 0xa9, 0x0b, 0x00, 0x1b, 0x04, 0x0f, 0xc4,
    0x46, 0xee,
};

/* mov.l er5,@sp; add.w r3,r0; dec.w #1,r3;
 * 1: er5 = ((er2 - er0) & mask) + er1; r3h = *er5;
 *    er5 = (er2 & mask) + er1; *er5 = r3h; er2++; r3l -= 1; if no borrow goto 1b
 * with mask = e3:e6, a ring buffer at er1. The trap goes on the loop. */
static const uint8_t lzcopy_code[] = {
    0x01, 0x00, 0x69, 0xf5, 0x09, 0x30, 0x1b, 0x53, 0x0f, 0xa5, 0x1a, 0x85, 0x66, 0xe5, 0x66, 0xbd,
    0x0a, 0x95, 0x68, 0x53, 0x0f, 0xa5, 0x66, 0xe5, 0x66, 0xbd, 0x0a, 0x95, 0x68, 0xd3, 0x0b, 0x02,
    0xf3, 0x01, 0x18, 0x3b, 0x44, 0xe2,
};

static void page_sum(cybiko_t *m);
static void mul(cybiko_t *m);
static void blit_row(cybiko_t *m);
static void lcd_row(cybiko_t *m);
static void stream_read(cybiko_t *m);
static void fill(cybiko_t *m);
static void lz_copy(cybiko_t *m);

static const struct hle {
    const char *name;
    const uint8_t *code;
    uint8_t len, trap;        /* pattern, and where in it the trap goes */
    bool rom;                 /* in the boot ROM, else in the CyOS kernel */
    bool exact;               /* registers, flags and states match exactly */
    void (*run)(cybiko_t *m);
    int16_t exit[2];          /* where the original ends, from the trap; none: returns */
} hle[HLE_COUNT] = {
    [HLE_SUM]    = { "page sum", page_sum_code, sizeof page_sum_code, 0, false, false, page_sum, { 0 } },
    [HLE_MUL]    = { "multiply", mul_code, sizeof mul_code, 0, true, false, mul, { 0 } },
    [HLE_BLIT]   = { "blit", blit_code, sizeof blit_code, 0, true, false, blit_row, { 0 } },
    [HLE_LCD]    = { "lcd row", lcd_code, sizeof lcd_code, 8, false, true, lcd_row, { 14 } },
    [HLE_STREAM] = { "stream read", stream_code, sizeof stream_code, 0, false, true, stream_read, { 0x92, 0x48 } },
    [HLE_FILL]   = { "fill", fill_code, sizeof fill_code, 0, false, true, fill, { 0x12 } },
    [HLE_LZCOPY] = { "lz copy", lzcopy_code, sizeof lzcopy_code, 8, true, true, lz_copy, { 0x1e } },
};

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

static void patch(cybiko_t *m, bool rom)
{
    for (int i = 0; i < HLE_COUNT; i++) {
        const struct hle *h = &hle[i];
        if (h->rom != rom) continue;
        int32_t at = find(rom ? m->rom : m->ram, rom ? CY_ROM_SIZE : CY_RAM_SIZE, h->code, h->len);
        if (at < 0) continue;
        at += h->trap;
        if (rom) {
            put_trap(m->rom + at);
            put_trap(m->rom + CY_ROM_SIZE + at);    /* the mirror */
            m->hle_pc[i] = (uint32_t)at;
        } else {
            put_trap(m->ram + at);
            m->hle_pc[i] = 0x400000u + (uint32_t)at;
        }
    }
}

void cy_hle_patch_rom(cybiko_t *m)
{
    memset(m->hle_pc, 0xff, sizeof m->hle_pc);
    patch(m, true);
}

void cy_hle_patch_kernel(cybiko_t *m)
{
    patch(m, false);
}

/* Memory access for the native routines that charge their states as a
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

/* The exact routines charge each instruction word they stand for (all in
 * the trap's page) and do their data accesses through h8s_read/h8s_write,
 * which charge those, so states come out as the interpreter's. */
static void words(h8s_cpu_t *c, int32_t n)
{
    c->icount -= n * c->cost16[(c->ppc >> 16) & 0xff];
}

/* A loop's first word was fetched as the trap. */
static void trap_word(h8s_cpu_t *c)
{
    words(c, -1);
}

/* Loops stop at the top of an iteration once the slice is used up or an
 * interrupt may be pending (cleared by the checker, which runs them whole). */
static bool hle_yield = true;
static bool must_yield(const h8s_cpu_t *c)
{
    return hle_yield && (c->icount <= 0 || c->irq_check);
}

/* Flags of a - b (cmp/sub) or a + b, as the core sets them. */
static uint8_t arith_flags(uint8_t ccr, uint32_t a, uint32_t b, int bits, bool sub)
{
    const uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1, sign = 1u << (bits - 1);
    const uint32_t hmask = (1u << (bits - 4)) - 1;
    a &= mask;
    b &= mask;
    uint64_t res = sub ? (uint64_t)a - b : (uint64_t)a + b;
    uint32_t h = sub ? (a & hmask) - (b & hmask) : (a & hmask) + (b & hmask);
    uint32_t r = (uint32_t)res & mask;
    uint32_t ovf = sub ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r);
    ccr &= (uint8_t)~0x2f;
    if ((h >> (bits - 4)) & 1) ccr |= 0x20;
    if (r & sign) ccr |= 0x08;
    if (!r) ccr |= 0x04;
    if (ovf & sign) ccr |= 0x02;
    if ((res >> bits) & 1) ccr |= 0x01;
    return ccr;
}

/* Flags of a move or logic result: N and Z, V cleared, C kept. */
static uint8_t nzv_flags(uint8_t ccr, uint32_t v, uint32_t sign)
{
    return (uint8_t)((ccr & ~0x0e) | (v & sign ? 0x08 : 0) | (v & (sign | (sign - 1)) ? 0 : 0x04));
}

/* ------------------------------------------------------------------ */
/* Page sum and multiply                                               */
/* ------------------------------------------------------------------ */

static void page_sum(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
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
}

static void mul(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
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


#ifdef CY_HLE_CHECK
static bool hle_check(cybiko_t *m, int which);
static bool hle_check_exit(cybiko_t *m);
#endif

/* ------------------------------------------------------------------ */
/* Kernel and ROM loops                                                */
/* ------------------------------------------------------------------ */

static void lcd_row(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    /* Per byte: 6 words, the post-increment's internal state and the taken
     * branch (all but the last time). The trap word stood for the sub.w. */
    uint8_t v = 0;
    for (int i = 0; i < 40; i++) {
        v = h8s_read8(c, c->er[5]);
        c->er[5] += 1;
        h8s_write8(c, c->er[0], v);
    }
    words(c, 40 * 6);
    c->icount -= 40 + 39;
    c->er[2] = (c->er[2] & ~0xffu) | v;
    c->er[3] = (c->er[3] & 0xffff0000u) | 0x28;
    c->ccr &= (uint8_t)~0x2f;                   /* cmp.w #0x27 of 0x28: all clear */
    c->pc = (c->ppc + 14) & AMASK;
}

static void stream_read(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t *r = c->er;
    const uint32_t sp = r[7];
    trap_word(c);
    for (;;) {
        r[0] = r[4] + 0x10;
        r[1] = h8s_read32(c, r[0]);
        r[5] = r[1] & 0x1fff;
        r[2] = r[4] + r[5];
        const uint8_t b = h8s_read8(c, r[2] + 0x14);
        r[2] = (r[2] & ~0xffu) | b;
        r[3] = h8s_read32(c, sp + 0x10);
        h8s_write8(c, r[3], b);
        r[3] += 1;
        h8s_write32(c, sp + 0x10, r[3]);
        r[1] += 1;
        h8s_write32(c, r[0], r[1]);
        r[6] += 1;
        r[5] = h8s_read32(c, sp + 0x14);
        c->ccr = arith_flags(c->ccr, r[6], r[5], 32, true);
        if (r[6] == r[5]) {                     /* count reached */
            words(c, 31);
            c->icount -= 1;
            c->pc = (c->ppc + 0x92) & AMASK;
            return;
        }
        r[2] = h8s_read32(c, r[4] + 0xc);
        c->ccr = arith_flags(c->ccr, r[1], r[2], 32, true);
        words(c, 36);
        if (!(((c->ccr >> 3) ^ (c->ccr >> 1)) & 1)) {   /* pos >= end: refill */
            c->pc = (c->ppc + 0x48) & AMASK;
            return;
        }
        c->icount -= 1;
        if (must_yield(c)) { c->pc = c->ppc; return; }
    }
}

static void fill(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t *r = c->er;
    trap_word(c);
    for (;;) {
        r[3] = (uint32_t)(int32_t)(int16_t)r[0];
        const uint32_t base = r[5];
        r[2] = base + r[3];
        h8s_write8(c, r[2], (uint8_t)r[1]);
        r[0] += 1;
        r[4] -= 1;
        words(c, 9);
        if (!r[4]) {
            /* H and C from the add.l, N Z V from mov.l er4,er4 */
            c->ccr = nzv_flags(arith_flags(c->ccr, base, r[3], 32, false), 0, 0x80000000u);
            c->pc = (c->ppc + 0x12) & AMASK;
            return;
        }
        c->icount -= 1;
        if (must_yield(c)) {
            c->ccr = nzv_flags(arith_flags(c->ccr, base, r[3], 32, false), r[4], 0x80000000u);
            c->pc = c->ppc;
            return;
        }
    }
}

static void lz_copy(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t *r = c->er;
    const uint32_t mask = (r[3] & 0xffff0000u) | r[6] >> 16;
    trap_word(c);
    for (;;) {
        r[5] = ((r[2] - r[0]) & mask) + r[1];
        const uint8_t b = h8s_read8(c, r[5]);
        r[5] = (r[2] & mask) + r[1];
        h8s_write8(c, r[5], b);
        r[2] += 1;
        const uint8_t n = (uint8_t)r[3];
        r[3] = (r[3] & 0xffff0000u) | 0x0100u | (uint8_t)(n - 1);
        c->ccr = arith_flags(c->ccr, n, 1, 8, true);
        words(c, 15);
        if (!n) {                               /* borrow: done */
            c->pc = (c->ppc + 0x1e) & AMASK;
            return;
        }
        c->icount -= 1;
        if (must_yield(c)) { c->pc = c->ppc; return; }
    }
}

bool cy_hle(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    const uint32_t pc = c->ppc < 0x10000u ? c->ppc & ~CY_ROM_SIZE : c->ppc;   /* ROM mirror */
    for (int i = 0; i < HLE_COUNT; i++) {
        if (pc != m->hle_pc[i]) continue;
#ifdef CY_HLE_CHECK
        if (hle_check(m, i)) return true;
#endif
        hle[i].run(m);
        return true;
    }
#ifdef CY_HLE_CHECK
    return hle_check_exit(m);
#else
    return false;
#endif
}

#ifdef CY_HLE_CHECK
/* Debug check (PC runner built with -DCY_HLE_CHECK). Every Nth call of each
 * routine (CY_HLE_CHECK_EVERY, default 200) runs both ways: the native
 * version first, with its result put aside; then the machine goes back to
 * the trap and runs the original on the spot, with interrupts held off and
 * traps on its exits (or on the return address), and the two results are
 * compared:
 * registers, flags and states for the exact routines, and all of RAM and
 * on-chip RAM (for the blit, only above the stack pointer). Differences go
 * to stderr as HLEBAD lines; cy_hle_check_report() prints a summary. */
#include <stdio.h>
#include <stdlib.h>

static struct {
    int every, fuzz;
    uint32_t calls[HLE_COUNT], checked[HLE_COUNT], bad[HLE_COUNT];
    int64_t states_native[HLE_COUNT], states_orig[HLE_COUNT];
    bool active;
    int which;
    uint32_t entry, exit_pc[2], ret_sp;
    uint8_t exit_word[2][2];
    uint64_t t0;
    int32_t native_states;
    cybiko_t want, start;
    uint8_t *want_ram, *want_fine, *save_ram, *save_fine;
    uint8_t ccr_i, exr;
} chk;

static uint8_t *code_at(cybiko_t *m, uint32_t pc)
{
    return m->cpu.rpage[(pc >> 16) & 0xff] + (pc & 0xffff);
}

static bool hle_check(cybiko_t *m, int which)
{
    h8s_cpu_t *c = &m->cpu;
    if (!chk.every) {
        const char *e = getenv("CY_HLE_CHECK_EVERY");
        chk.every = e ? atoi(e) : 200;
        if (chk.every < 1) chk.every = 1;
        chk.want_ram = malloc(CY_RAM_SIZE);
        chk.save_ram = malloc(CY_RAM_SIZE);
        chk.want_fine = malloc(c->fine_size);
        chk.save_fine = malloc(c->fine_size);
    }
    if (chk.active) return false;                     /* nested: run natively */
    if (!chk.fuzz && chk.calls[which]++ % (uint32_t)chk.every) return false;
    const struct hle *h = &hle[which];

    /* the native run, put aside */
    chk.start = *m;
    memcpy(chk.save_ram, m->ram, CY_RAM_SIZE);
    memcpy(chk.save_fine, c->fine_mem, c->fine_size);
    const uint64_t t = h8s_time(c);
    hle_yield = false;
    h->run(m);
    hle_yield = true;
    chk.native_states = (int32_t)(h8s_time(c) - t);
    chk.want = *m;
    memcpy(chk.want_ram, m->ram, CY_RAM_SIZE);
    memcpy(chk.want_fine, c->fine_mem, c->fine_size);
    *m = chk.start;
    memcpy(m->ram, chk.save_ram, CY_RAM_SIZE);
    memcpy(c->fine_mem, chk.save_fine, c->fine_size);

    /* the original: put its first word back, trap its exits */
    const uint32_t at = c->ppc;
    const uint8_t *orig = h->code + h->trap;
    uint8_t *p = code_at(m, at);
    p[0] = orig[0];
    p[1] = orig[1];
    for (int i = 0; i < 2; i++) chk.exit_pc[i] = 0xffffffffu;
    chk.ret_sp = 0xffffffffu;
    if (!h->exit[0]) {
        chk.exit_pc[0] = ((uint32_t)peek16(c, c->er[7]) << 16 | peek16(c, c->er[7] + 2)) & AMASK;
        chk.ret_sp = c->er[7] + 4;
    } else {
        for (int i = 0; i < 2 && h->exit[i]; i++) chk.exit_pc[i] = (at + (uint32_t)h->exit[i]) & AMASK;
    }
    for (int i = 0; i < 2; i++) {
        if (chk.exit_pc[i] == 0xffffffffu) continue;
        uint8_t *q = code_at(m, chk.exit_pc[i]);
        chk.exit_word[i][0] = q[0];
        chk.exit_word[i][1] = q[1];
        put_trap(q);
    }
    chk.ccr_i = c->ccr & 0x80;
    chk.exr = c->exr;
    c->ccr |= 0x80;
    c->exr |= 7;
    c->pc = at;
    chk.entry = at;
    chk.which = which;
    chk.active = true;
    chk.checked[which]++;

    /* Run it right here, outside the machine's slices, so that no timer,
     * DMA or DTC moves meanwhile; then account its states to the slice in
     * progress as if the trap had taken them. */
    const int32_t outer_icount = c->icount + c->cost16[(at >> 16) & 0xff];   /* fetched again */
    const int32_t outer_slice = c->slice;
    const uint64_t outer_cycles = c->cycles;
    c->slice = c->icount = 0;
    chk.t0 = c->cycles;
    int64_t used = 0;
    for (int guard = 0; chk.active && guard < 1000; guard++) used += h8s_run(c, 100000);
    if (chk.active) {
        fprintf(stderr, "HLEBAD %s: the original did not reach an exit\n", h->name);
        chk.active = false;
    }
    c->cycles = outer_cycles;
    c->slice = outer_slice;
    c->icount = outer_icount - (int32_t)used;
    return true;
}

static void report(int which, const char *what, uint32_t a, uint32_t want, uint32_t got)
{
    if (++chk.bad[which] <= 40)
        fprintf(stderr, "HLEBAD %s: %s %06x: native %08x original %08x\n", hle[which].name, what,
                (unsigned)a, (unsigned)want, (unsigned)got);
}

static bool hle_check_exit(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    if (!chk.active) return false;
    int e = c->ppc == chk.exit_pc[0] ? 0 : c->ppc == chk.exit_pc[1] ? 1 : -1;
    if (e < 0 || (chk.ret_sp != 0xffffffffu && c->er[7] != chk.ret_sp)) return false;
    const int which = chk.which;
    const struct hle *h = &hle[which];
    for (int i = 0; i < 2; i++) {
        if (chk.exit_pc[i] == 0xffffffffu) continue;
        uint8_t *q = code_at(m, chk.exit_pc[i]);
        q[0] = chk.exit_word[i][0];
        q[1] = chk.exit_word[i][1];
    }
    put_trap(code_at(m, chk.entry));
    c->icount += c->cost16[(c->ppc >> 16) & 0xff];
    c->pc = c->ppc;
    const int32_t orig_states = (int32_t)(h8s_time(c) - chk.t0);
    h8s_end_slice(c);
    c->ccr = (uint8_t)((c->ccr & 0x7f) | chk.ccr_i);
    c->exr = chk.exr;
    chk.active = false;

    const h8s_cpu_t *w = &chk.want.cpu;
    const int32_t native_states = chk.native_states + c->cost16[(m->hle_pc[which] >> 16) & 0xff];
    chk.states_native[which] += native_states;
    chk.states_orig[which] += orig_states;
    if (w->pc != c->pc) report(which, "pc", 0, w->pc, c->pc);
    for (int i = h->exact ? 0 : 4; i < 8; i++)
        if (w->er[i] != c->er[i]) report(which, "er", (uint32_t)i, w->er[i], c->er[i]);
    if (h->exact && w->ccr != c->ccr) report(which, "ccr", 0, w->ccr, c->ccr);
    if (h->exact && native_states != orig_states) report(which, "states", 0, (uint32_t)native_states, (uint32_t)orig_states);
    uint32_t live = h->exact ? 0 : c->er[7] - 0x400000u;   /* the blit leaves dead locals below sp */
    if (live > CY_RAM_SIZE) live = 0;
    for (uint32_t i = live; i < CY_RAM_SIZE && memcmp(chk.want_ram + live, m->ram + live, CY_RAM_SIZE - live); i++) {
        if (chk.want_ram[i] != m->ram[i]) {
            report(which, "ram", 0x400000u + i, chk.want_ram[i], m->ram[i]);
            break;
        }
    }
    for (uint32_t i = 0; i < c->fine_size && memcmp(chk.want_fine, c->fine_mem, c->fine_size); i++) {
        if (chk.want_fine[i] != c->fine_mem[i]) {
            report(which, "on-chip ram", c->fine_base + i, chk.want_fine[i], c->fine_mem[i]);
            break;
        }
    }
    if (memcmp(&chk.want.lcd, &m->lcd, sizeof m->lcd)) report(which, "lcd", 0, 0, 0);
    return true;
}

void cy_hle_check_report(void)
{
    for (int i = 0; i < HLE_COUNT; i++) {
        if (!chk.checked[i]) continue;
        fprintf(stderr, "HLE %-12s %8u calls, %6u checked, %u differ, states native/original %.3f\n",
                hle[i].name, chk.calls[i], chk.checked[i], chk.bad[i],
                chk.states_orig[i] ? (double)chk.states_native[i] / (double)chk.states_orig[i] : 0.0);
    }
}

/* Random calls of the blit, each checked as above. Run it on a machine that
 * has booted (the ROM builds its tables at start-up). Uses RAM at 5f0000. */
uint32_t cy_hle_blit_fuzz(cybiko_t *m, int n, uint32_t seed)
{
    h8s_cpu_t *c = &m->cpu;
    if (m->hle_pc[HLE_BLIT] == 0xffffffffu) return 0;
    const h8s_cpu_t saved = *c;
    const uint32_t base = 0x5f0000u, ret_at = base, table = base + 0x100, src0 = base + 0x1000,
                   dst0 = base + 0x2000, stack = base + 0xff00;
    const uint32_t bad0 = chk.bad[HLE_BLIT];
    chk.fuzz = 1;
#define RND() (seed = seed * 1103515245u + 12345u, seed >> 8)
    for (int k = 0; k < n; k++) {
        for (uint32_t i = 0; i < 0x300; i++) poke8(c, src0 + i, (uint8_t)RND());
        for (uint32_t i = 0; i < 0x100; i++) poke8(c, dst0 + i, (uint8_t)RND());
        for (uint32_t i = 0; i < 0x100; i++) poke8(c, table + i, (uint8_t)RND());
        poke8(c, ret_at, 0x00); poke8(c, ret_at + 1, 0x00);        /* nop */
        const uint16_t w = (uint16_t)(RND() % 8 == 0 ? (RND() % 12) - 4 : 1 + RND() % 200);
        const uint16_t dx = (uint16_t)(RND() % 4), mode = (uint16_t)(RND() % 3);
        const uint8_t mirror = (uint8_t)(RND() & 1);
        const uint32_t slots[6] = { ret_at, dx, w, mode, table, mirror };
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 4; j++) poke8(c, stack + (uint32_t)(i * 4 + j), (uint8_t)(slots[i] >> (24 - 8 * j)));
        c->er[7] = stack;
        c->er[0] = src0 + 0x100 + RND() % 0x80;
        c->er[1] = RND() % 4;
        c->er[2] = dst0 + 0x10 + RND() % 0x40;
        c->ccr |= 0x80;
        c->exr |= 7;
        c->pc = m->hle_pc[HLE_BLIT];
        c->sleeping = false;
        for (int guard = 0; c->pc != ret_at + 2 && guard < 1000000; guard++) h8s_run(c, 1);
    }
#undef RND
    chk.fuzz = 0;
    *c = saved;
    return chk.bad[HLE_BLIT] - bad0;
}
#endif
