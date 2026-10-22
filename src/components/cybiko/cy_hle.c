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
 *   LZ decoder loop (boot ROM): the rest of that decompressor, which reads
 *     literals and back-references bit by bit; 44% of all instructions while
 *     a video plays.
 *   serial send loop (CyOS module): sends a buffer on SCI0 (the MP3
 *     decoder's input), waiting for each byte to go out; 18% of instructions
 *     in videos.
 *   wait loop (CyOS module, next to the send loop): spins until an interrupt
 *     handler clears a flag, a tenth of instructions in videos.
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

enum { HLE_SUM, HLE_MUL, HLE_BLIT, HLE_LCD, HLE_STREAM, HLE_FILL, HLE_LZCOPY, HLE_LZ, HLE_SEND, HLE_WAIT, HLE_COUNT };
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

/* The decompressor's set-up and the top of its loop, which reads one bit:
 *   er4 = src, er5 = bytes left, r6 = bit buffer, e6:e3 = window mask
 *   1: if (bit) { len (3990); dist (3ad6); copy } else { r3l = 8 bits (391e);
 *      window[er2++ & mask] = r3l } while (er5 > 0)
 * It calls into the ROM's bit-reading routines and jump tables, which are
 * checked by a hash (lz_check) since the native code follows them. The trap
 * goes on the top of the loop. */
static const uint8_t lz_code[] = {
    0x01, 0x20, 0x6d, 0xf4, 0x01, 0x10, 0x6d, 0xf0, 0x01, 0x00, 0x69, 0x04, 0x01, 0x00, 0x6f, 0x03,
    0x00, 0x04, 0x0a, 0xb4, 0x01, 0x00, 0x6f, 0x75, 0x00, 0x1c, 0x1a, 0xb5, 0x6f, 0x06, 0x00, 0x08,
    0x01, 0x00, 0x6f, 0x73, 0x00, 0x18, 0x0d, 0x3e, 0x0c, 0x66, 0x46, 0x06, 0x6c, 0x4e, 0xf6, 0xff,
    0x1b, 0x05, 0x11, 0x16, 0x45, 0x12,
};
#define LZ_START 0x3be8u

/* bra 2f; 1: mov.b @er1+,r2l; mov.b r2l,@TDR0; bclr #7,@SSR0 (TDRE);
 * 3: mov.b @SSR0,r2l; btst #2,r2l; beq 3b (until TEND);
 * 2: subs #1,er3; cmp.w #0xffff,r3; bne 1b
 * The trap goes on 1. */
static const uint8_t send_code[] = {
    0x58, 0x00, 0x00, 0x1a, 0x6c, 0x1a, 0x6a, 0xaa, 0x00, 0xff, 0xff, 0x7b, 0x6a, 0x38, 0x00, 0xff,
    0xff, 0x7c, 0x72, 0x70, 0x6a, 0x2a, 0x00, 0xff, 0xff, 0x7c, 0x73, 0x2a, 0x47, 0xf6, 0x1b, 0x03,
    0x79, 0x23, 0xff, 0xff, 0x58, 0x60, 0xff, 0xdc,
};
#define SEND_POLL 0x10      /* the wait, from the trap */

/* 1: jsr get_ticks (mov.l @ticks,er0; rts); sub.l er4,er0; cmp.l #999,er0;
 *    bhi 2f; mov.b @error,r2l; bne 2f; mov.b @busy,r2l; bne 1b
 * 2: — waits up to 999 ticks for an interrupt handler to clear busy. The
 * addresses vary (WAIT_ANY). The trap goes on the sub.l. */
static const uint8_t wait_code[] = {
    0x5e, 0x48, 0x8f, 0xd8, 0x1a, 0xc0, 0x7a, 0x20, 0x00, 0x00, 0x03, 0xe7, 0x42, 0x10, 0x6a, 0x2a,
    0x00, 0x4e, 0xba, 0x2a, 0x46, 0x08, 0x6a, 0x2a, 0x00, 0x4e, 0xba, 0x26, 0x46, 0xe2,
};
#define WAIT_ANY (0xeu | 0xfu << 16 | 0xfu << 24)
static bool wait_check(const uint8_t *mem, int32_t at);
static bool lz_check(const uint8_t *mem, int32_t at);

static void page_sum(cybiko_t *m);
static void mul(cybiko_t *m);
static void blit_row(cybiko_t *m);
static void lcd_row(cybiko_t *m);
static void stream_read(cybiko_t *m);
static void fill(cybiko_t *m);
static void lz_copy(cybiko_t *m);
static void lz_decode(cybiko_t *m);
static void send(cybiko_t *m);
static void wait_loop(cybiko_t *m);

/* found late (cy_hle_probe): from the instruction this far into the code */
#define LATE(off) (1 + (off))
#define LATE_NEAR 0xff      /* ... or near another one found late */

static const struct hle {
    const char *name;
    const uint8_t *code;
    uint8_t len, trap;        /* pattern, and where in it the trap goes */
    bool rom;                 /* in the boot ROM, else in the CyOS kernel */
    bool exact;               /* registers, flags and states match exactly */
    void (*run)(cybiko_t *m);
    int16_t exit[2];          /* where the original ends, from the trap; none: returns */
    bool (*check)(const uint8_t *mem, int32_t at);   /* more to check where found */
    uint8_t late;             /* 0, LATE(offset) or LATE_NEAR */
    uint32_t any;             /* bytes of the code (bit n: byte n) that may differ */
    bool spin;                /* waits for an interrupt: checked one round at a time */
} hle[HLE_COUNT] = {
    [HLE_SUM]    = { "page sum", page_sum_code, sizeof page_sum_code, 0, false, false, page_sum, { 0 } },
    [HLE_MUL]    = { "multiply", mul_code, sizeof mul_code, 0, true, false, mul, { 0 } },
    [HLE_BLIT]   = { "blit", blit_code, sizeof blit_code, 0, true, false, blit_row, { 0 } },
    [HLE_LCD]    = { "lcd row", lcd_code, sizeof lcd_code, 8, false, true, lcd_row, { 14 } },
    [HLE_STREAM] = { "stream read", stream_code, sizeof stream_code, 0, false, true, stream_read, { 0x92, 0x48 } },
    [HLE_FILL]   = { "fill", fill_code, sizeof fill_code, 0, false, true, fill, { 0x12 } },
    [HLE_LZCOPY] = { "lz copy", lzcopy_code, sizeof lzcopy_code, 8, true, true, lz_copy, { 0x1e } },
    [HLE_LZ]     = { "lz decode", lz_code, sizeof lz_code, 0x28, true, true, lz_decode, { 0x56 }, lz_check },
    [HLE_SEND]   = { "serial send", send_code, sizeof send_code, 4, false, true, send, { 0x24 }, NULL, LATE(6) },
    [HLE_WAIT]   = { "wait loop", wait_code, sizeof wait_code, 4, false, true, wait_loop, { 0x1a, -4 },
                     wait_check, LATE_NEAR, WAIT_ANY, true },
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

static bool matches(const uint8_t *mem, const struct hle *h)
{
    for (uint32_t i = 0; i < h->len; i++)
        if (mem[i] != h->code[i] && !(i < 32 && (h->any >> i & 1))) return false;
    return true;
}

static void patch(cybiko_t *m, bool rom)
{
    /* Find them all before placing any trap, as some overlap others' checks. */
    uint8_t *mem = rom ? m->rom : m->ram;
    int32_t found[HLE_COUNT];
    for (int i = 0; i < HLE_COUNT; i++) {
        const struct hle *h = &hle[i];
        found[i] = -1;
        if (h->rom != rom || h->late) continue;
        int32_t at = find(mem, rom ? CY_ROM_SIZE : CY_RAM_SIZE, h->code, h->len);
        if (at >= 0 && (!h->check || h->check(mem, at))) found[i] = at + h->trap;
    }
    for (int i = 0; i < HLE_COUNT; i++) {
        const int32_t at = found[i];
        if (at < 0) continue;
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
    m->hle_probed = 0xffffffffu;
    patch(m, true);
}

void cy_hle_patch_kernel(cybiko_t *m)
{
    patch(m, false);
}

/* Code CyOS loads later (its modules) is found when it first runs: the
 * peripherals call this when an instruction writes a register that such
 * code uses (for now: SCI0's TDR), and the code around it is checked once.
 * What is found that way is looked for nearby as well. If a routine was
 * found elsewhere before, that copy gets its word back. */
#ifdef CY_HLE_CHECK
static bool hle_checking(void);
#endif

static void place_late(cybiko_t *m, int i, uint32_t start)
{
    const struct hle *h = &hle[i];
    const uint32_t old = m->hle_pc[i] - 0x400000u;
    if (old < CY_RAM_SIZE - 2 && m->ram[old] == (uint8_t)(CY_HLE_OP >> 8) && m->ram[old + 1] == (uint8_t)CY_HLE_OP)
        memcpy(m->ram + old, h->code + h->trap, 2);
    put_trap(m->ram + start + h->trap);
    m->hle_pc[i] = 0x400000u + start + h->trap;
}

#define NEAR 0x4000u

void cy_hle_probe(cybiko_t *m)
{
    const uint32_t pc = m->cpu.ppc;
#ifdef CY_HLE_CHECK
    if (hle_checking()) return;                 /* the original is running */
#endif
    m->hle_probed = pc;
    for (int i = 0; i < HLE_COUNT; i++) {
        const struct hle *h = &hle[i];
        if (!h->late || h->late == LATE_NEAR) continue;
        const uint32_t start = pc - (h->late - 1u) - 0x400000u;
        if (start >= CY_RAM_SIZE - h->len || !matches(m->ram + start, h)) continue;
        place_late(m, i, start);
        const uint32_t lo = start > NEAR ? start - NEAR : 0;
        const uint32_t hi = start + NEAR < CY_RAM_SIZE ? start + NEAR : CY_RAM_SIZE;
        for (int j = 0; j < HLE_COUNT; j++) {
            const struct hle *n = &hle[j];
            if (n->late != LATE_NEAR) continue;
            for (uint32_t a = lo & ~1u; a + n->len <= hi; a += 2) {
                if (!matches(m->ram + a, n) || (n->check && !n->check(m->ram, (int32_t)a))) continue;
                place_late(m, j, a);
                break;
            }
        }
    }
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
 * interrupt is to be taken (cleared by the checker, which runs them whole).
 * irq_check only says one may be: with none to take, it is cleared, as the
 * core does. */
static bool hle_yield = true;
static bool must_yield(h8s_cpu_t *c)
{
    if (!hle_yield) return false;
    if (c->icount <= 0) return true;
    if (!c->irq_check) return false;
    if (cy_irq_deliverable(c->ctx)) return true;
    c->irq_check = false;
    return false;
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

/* ---- LZ decoder ---- */

/* The ROM code lz_decode() follows, by hash, and the jump tables it uses. */
static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811c9dc5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

static bool lz_check(const uint8_t *mem, int32_t at)
{
    return (uint32_t)at == LZ_START && fnv1a(mem + 0x3874, 0x40e) == 0xc8f7da29u &&
           fnv1a(mem + 0x8e0, 0x50) == 0x880e8bafu;
}

/* Flags of add/sub with carry in, as the core sets them (addx keeps Z set
 * only if it was set and the result is zero). */
static uint8_t addx_flags(uint8_t ccr, uint32_t a, uint32_t b, unsigned carry, int bits, uint32_t *out)
{
    const uint32_t mask = (1u << bits) - 1, sign = 1u << (bits - 1), hmask = (1u << (bits - 4)) - 1;
    a &= mask;
    b &= mask;
    const uint32_t res = a + b + carry, r = res & mask;
    const uint32_t h = (a & hmask) + (b & hmask) + carry;
    uint8_t f = ccr & (uint8_t)~0x2b;
    if (r) f &= (uint8_t)~0x04;
    if ((h >> (bits - 4)) & 1) f |= 0x20;
    if (r & sign) f |= 0x08;
    if (~(a ^ b) & (a ^ r) & sign) f |= 0x02;
    if ((res >> bits) & 1) f |= 0x01;
    *out = r;
    return f;
}

#define R3L(r) ((uint8_t)(r)[3])
#define SET_R3L(r, v) ((r)[3] = ((r)[3] & ~0xffu) | (uint8_t)(v))
#define SET_R0L(r, v) ((r)[0] = ((r)[0] & ~0xffu) | (uint8_t)(v))
#define SET_R0(r, v) ((r)[0] = ((r)[0] & 0xffff0000u) | (uint16_t)(v))

/* jsr @aa:24 / rts, with the return address really pushed */
static void lz_call(h8s_cpu_t *c, uint32_t ret_to)
{
    words(c, 2);
    c->er[7] -= 4;
    h8s_write32(c, c->er[7], ret_to);
}

static void lz_ret(h8s_cpu_t *c)
{
    words(c, 1);
    h8s_read32(c, c->er[7]);
    c->er[7] += 4;
    c->icount -= 1;
}

/* mov.b r6h,r6h; bne 1f; mov.b @er4+,r6l; mov.b #0xff,r6h; subs #1,er5;
 * 1: shlr.w r6 — the next input bit, in C. r6h counts the bits left in r6l. */
static unsigned lz_bit(h8s_cpu_t *c)
{
    uint32_t *r = c->er;
    words(c, 2);
    if (r[6] & 0xff00) c->icount -= 1;
    else {
        words(c, 3);
        const uint8_t b = h8s_read8(c, r[4]);
        r[4] += 1;
        c->icount -= 1;
        r[6] = (r[6] & 0xffff0000u) | 0xff00u | b;
        r[5] -= 1;
    }
    words(c, 1);
    const unsigned bit = r[6] & 1;
    const uint32_t v = (r[6] & 0xffff) >> 1;
    r[6] = (r[6] & 0xffff0000u) | v;
    c->ccr = (uint8_t)((c->ccr & 0xf0) | (v ? 0 : 0x04) | bit);
    return bit;
}

/* The two unrolled readers: n bits into er0 (rotxl.l, entered at
 * 0x391c - 14n) or into r3l (rotxl.b, at 0x398e - 14n), then rts. */
static void lz_bits32(h8s_cpu_t *c, int n)
{
    uint32_t *r = c->er;
    while (n--) {
        const unsigned in = lz_bit(c);
        words(c, 1);
        const uint32_t msb = r[0] >> 31;
        r[0] = r[0] << 1 | in;
        c->ccr = (uint8_t)((c->ccr & 0xf0) | (r[0] & 0x80000000u ? 0x08 : 0) | (r[0] ? 0 : 0x04) | msb);
    }
    lz_ret(c);
}

static void lz_bits8(h8s_cpu_t *c, int n)
{
    uint32_t *r = c->er;
    while (n--) {
        const unsigned in = lz_bit(c);
        words(c, 1);
        const uint8_t v = R3L(r), nv = (uint8_t)(v << 1 | in);
        SET_R3L(r, nv);
        c->ccr = (uint8_t)((c->ccr & 0xf0) | (nv & 0x80 ? 0x08 : 0) | (nv ? 0 : 0x04) | (v >> 7));
    }
    lz_ret(c);
}

#define CALL32(ret_to, entry) (lz_call(c, ret_to), lz_bits32(c, (0x391c - (entry)) / 14))
#define CALL8(ret_to, entry) (lz_call(c, ret_to), lz_bits8(c, (0x398e - (entry)) / 14))

/* bcs: taken when C (the bit just read) is set */
static unsigned lz_bcs(h8s_cpu_t *c, unsigned bit)
{
    words(c, 1);
    if (bit) c->icount -= 1;
    return bit;
}

/* sub.l er0,er0 / sub.w r3,r3: zero, flags Z only */
static void lz_clear(h8s_cpu_t *c, int reg, bool word)
{
    words(c, 1);
    c->er[reg] = word ? c->er[reg] & 0xffff0000u : 0;
    c->ccr = (uint8_t)((c->ccr & 0xd0) | 0x04);
}

/* mov.b #v,r3l (or r0l) */
static void lz_movb(h8s_cpu_t *c, int reg, uint8_t v)
{
    words(c, 1);
    c->er[reg] = (c->er[reg] & ~0xffu) | v;
    c->ccr = nzv_flags(c->ccr, v, 0x80);
}

/* addx #v,rNl: v + the bit just read */
static void lz_addx(h8s_cpu_t *c, int reg, uint8_t v)
{
    uint32_t out;
    words(c, 1);
    c->ccr = addx_flags(c->ccr, (uint8_t)c->er[reg], v, c->ccr & 1, 8, &out);
    c->er[reg] = (c->er[reg] & ~0xffu) | out;
}

/* add.b #v,rNl */
static void lz_addb(h8s_cpu_t *c, int reg, uint8_t v)
{
    words(c, 1);
    const uint8_t a = (uint8_t)c->er[reg];
    c->ccr = arith_flags(c->ccr, a, v, 8, false);
    c->er[reg] = (c->er[reg] & ~0xffu) | (uint8_t)(a + v);
}

/* add.w #v,r0 */
static void lz_addw0(h8s_cpu_t *c, uint16_t v)
{
    words(c, 2);
    const uint16_t a = (uint16_t)c->er[0];
    c->ccr = arith_flags(c->ccr, a, v, 16, false);
    SET_R0(c->er, a + v);
}

/* sub.l er0,er0; jsr <3 or 2 bits>; shll.l #2,er0; mov.l @(table,er0),er0;
 * jmp @er0 — returns the handler's address. */
static uint32_t lz_dispatch(h8s_cpu_t *c, uint32_t ret_to, uint32_t reader, uint32_t table)
{
    uint32_t *r = c->er;
    lz_clear(c, 0, false);
    CALL32(ret_to, reader);
    words(c, 1);
    const uint32_t v = r[0];
    r[0] = v << 2;
    c->ccr = (uint8_t)((c->ccr & 0xf0) | (r[0] & 0x80000000u ? 0x08 : 0) | (r[0] ? 0 : 0x04) |
                       ((v >> 30) & 1));
    words(c, 5);
    r[0] = h8s_read32(c, table + r[0]);
    c->ccr = nzv_flags(c->ccr, r[0], 0x80000000u);
    words(c, 1);
    return r[0] & AMASK;
}

/* The length (3990): r3 = 0, then r3l from a prefix code; r3h = 1 marks the
 * longest. Ends with its rts. */
static void lz_length(h8s_cpu_t *c)
{
    lz_clear(c, 3, true);
    if (!lz_bcs(c, lz_bit(c))) { lz_bit(c); lz_addx(c, 3, 2); lz_ret(c); return; }
    if (!lz_bcs(c, lz_bit(c))) { lz_bit(c); lz_addx(c, 3, 4); lz_ret(c); return; }
    if (!lz_bcs(c, lz_bit(c))) {
        if (!lz_bcs(c, lz_bit(c))) { lz_movb(c, 3, 6); lz_ret(c); return; }
        lz_bit(c); lz_addx(c, 3, 8); lz_ret(c); return;
    }
    if (!lz_bcs(c, lz_bit(c))) {
        if (!lz_bcs(c, lz_bit(c))) { lz_movb(c, 3, 7); lz_ret(c); return; }
        lz_bit(c); lz_addx(c, 3, 0xa); lz_ret(c); return;
    }
    switch (lz_dispatch(c, 0x3a34, 0x38f2, 0x8e0)) {
    case 0x3a42: lz_movb(c, 3, 0xc); break;
    case 0x3a46: lz_movb(c, 3, 0xd); break;
    case 0x3a4a: CALL8(0x3a4e, 0x3972); lz_addb(c, 3, 0x14); break;
    case 0x3a52: lz_bit(c); lz_addx(c, 3, 0x10); break;
    case 0x3a62: CALL8(0x3a66, 0x3964); lz_addb(c, 3, 0x18); break;
    case 0x3a6a: lz_movb(c, 3, 0xe); break;
    case 0x3a6e:
        if (!lz_bcs(c, lz_bit(c))) { CALL8(0x3a80, 0x3948); lz_addb(c, 3, 0x20); }
        else { lz_bit(c); lz_addx(c, 3, 0x12); }
        break;
    default: /* 0x3a94 */
        if (!lz_bcs(c, lz_bit(c))) { lz_movb(c, 3, 0xf); break; }
        if (!lz_bcs(c, lz_bit(c))) { CALL8(0x3ab8, 0x393a); lz_addb(c, 3, 0x40); break; }
        if (!lz_bcs(c, lz_bit(c))) { CALL8(0x3ace, 0x392c); lz_addb(c, 3, 0x80); break; }
        words(c, 1);                                /* mov.b #1,r3h */
        c->er[3] = (c->er[3] & ~0xff00u) | 0x100u;
        c->ccr = nzv_flags(c->ccr, 1, 0x80);
        break;
    }
    lz_ret(c);
}

/* The distance (3ad6) into er0, from a prefix code. Ends with its rts. */
static void lz_distance(h8s_cpu_t *c)
{
    switch (lz_dispatch(c, 0x3adc, 0x38f2, 0x900)) {
    case 0x3aea: lz_clear(c, 0, false); CALL32(0x3af0, 0x3890); lz_addw0(c, 0x400); break;
    case 0x3af6: lz_clear(c, 0, false); CALL32(0x3afc, 0x389e); lz_addw0(c, 0x200); break;
    case 0x3b02: lz_clear(c, 0, false); CALL32(0x3b08, 0x38ac); lz_addw0(c, 0x100); break;
    case 0x3b0e: lz_clear(c, 0, false); CALL32(0x3b14, 0x38c8); lz_addb(c, 0, 0x40); break;
    case 0x3b18: lz_clear(c, 0, false); CALL32(0x3b1e, 0x38ba); lz_addb(c, 0, 0x80); break;
    case 0x3b22:
        lz_clear(c, 0, false);
        if (!lz_bcs(c, lz_bit(c))) { CALL32(0x3b36, 0x3882); lz_addw0(c, 0x800); }
        else { CALL32(0x3b40, 0x3874); lz_addw0(c, 0x1000); }
        break;
    case 0x3b46:
        lz_clear(c, 0, false);
        if (!lz_bcs(c, lz_bit(c))) { CALL32(0x3b5a, 0x38d6); lz_addb(c, 0, 0x20); }
        else { CALL32(0x3b62, 0x38e4); lz_addb(c, 0, 0x10); }
        break;
    default: /* 0x3b66 */
        switch (lz_dispatch(c, 0x3b6c, 0x3900, 0x920)) {
        case 0x3b7a:                                /* jmp to the 1-bit reader */
            lz_clear(c, 0, false);
            words(c, 2);
            lz_bits32(c, 1);
            return;
        case 0x3b80: lz_clear(c, 0, false); CALL32(0x3b86, 0x3900); lz_addb(c, 0, 6); break;
        case 0x3b8a:
            lz_clear(c, 0, false);
            if (!lz_bcs(c, lz_bit(c))) { lz_bit(c); lz_addx(c, 0, 4); }
            else lz_movb(c, 0, 2);
            break;
        default: /* 0x3bae */
            lz_clear(c, 0, false);
            if (!lz_bcs(c, lz_bit(c))) { CALL32(0x3bc2, 0x3900); lz_addb(c, 0, 0xc); break; }
            if (!lz_bcs(c, lz_bit(c))) { lz_movb(c, 0, 3); break; }
            lz_bit(c);
            lz_addx(c, 0, 0xa);
            break;
        }
        break;
    }
    lz_ret(c);
}

static void lz_decode(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t *r = c->er;
    const uint32_t mask = (r[3] & 0xffff0000u) | r[6] >> 16;
    trap_word(c);
    for (;;) {
        if (!lz_bcs(c, lz_bit(c))) {
            /* a literal: 8 bits into r3l, stored at er2++ in the window */
            CALL8(0x3c22, 0x391e);
            words(c, 7);
            const uint32_t p = r[2] & mask;
            r[0] = p + r[1];
            c->ccr = arith_flags(c->ccr, p, r[1], 32, false);
            h8s_write8(c, r[0], R3L(r));
            c->ccr = nzv_flags(c->ccr, R3L(r), 0x80);
            r[2] += 1;
            c->icount -= 1;                         /* bra taken */
        } else {
            /* a back-reference: length, distance, then the copy loop */
            lz_call(c, 0x3c34);
            lz_length(c);
            lz_call(c, 0x3c38);
            lz_distance(c);
            words(c, 4);
            h8s_write32(c, r[7], r[5]);
            const uint16_t r0 = (uint16_t)r[0], r3 = (uint16_t)r[3];
            SET_R0(r, r0 + r3);
            r[3] = (r[3] & 0xffff0000u) | (uint16_t)(r3 - 1);
            for (;;) {
                r[5] = ((r[2] - r[0]) & mask) + r[1];
                const uint8_t b = h8s_read8(c, r[5]);
                r[5] = (r[2] & mask) + r[1];
                h8s_write8(c, r[5], b);
                r[2] += 1;
                const uint8_t n = R3L(r);
                r[3] = (r[3] & 0xffff0000u) | 0x0100u | (uint8_t)(n - 1);
                words(c, 15);
                if (!n) {
                    c->ccr = arith_flags(c->ccr, n, 1, 8, true);
                    break;
                }
                c->icount -= 1;
            }
            words(c, 2);
            r[5] = h8s_read32(c, r[7]);
        }
        /* mov.l er5,er5; bgt 1b */
        words(c, 2);
        c->ccr = nzv_flags(c->ccr, r[5], 0x80000000u);
        if ((int32_t)r[5] <= 0) {
            c->pc = (c->ppc + 0x56) & AMASK;
            return;
        }
        c->icount -= 1;
        if (must_yield(c)) { c->pc = c->ppc; return; }
    }
}

/* ---- serial send ---- */

/* The peripherals see each access at the time, and from the instruction
 * (c->ppc, which their polling skips go by), that the original makes it. */
static void send(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t *r = c->er;
    const uint32_t at = c->ppc;
    trap_word(c);
    for (;;) {
        words(c, 1);
        c->icount -= 1;
        const uint8_t v = h8s_read8(c, r[1]);
        r[1] += 1;
        words(c, 3);
        c->ppc = at + 2;
        h8s_write8(c, 0xffff7bu, v);
        words(c, 4);
        c->ppc = at + 8;
        const uint8_t ssr = h8s_read8(c, 0xffff7cu);
        h8s_write8(c, 0xffff7cu, ssr & 0x7f);
        uint8_t s;
        for (;;) {
            words(c, 3);
            c->ppc = at + SEND_POLL;
            s = h8s_read8(c, 0xffff7cu);
            c->ppc = at;
            words(c, 2);
            if (s & 0x04) break;
            c->icount -= 1;
            if (must_yield(c)) {
                /* back in the wait loop, after its beq */
                r[2] = (r[2] & ~0xffu) | s;
                c->ccr = (uint8_t)(nzv_flags(c->ccr, s, 0x80) | 0x04);
                c->pc = (at + SEND_POLL) & AMASK;
                return;
            }
        }
        r[2] = (r[2] & ~0xffu) | s;
        r[3] -= 1;
        words(c, 5);
        c->ccr = arith_flags(c->ccr, r[3], 0xffff, 16, true);
        if ((uint16_t)r[3] == 0xffff) {
            c->pc = (at + 0x24) & AMASK;
            return;
        }
        c->icount -= 1;
        if (must_yield(c)) { c->pc = at; return; }
    }
}

/* ---- wait loop ---- */

static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | be24(p + 1); }

/* get_ticks must be mov.l @aa:32,er0; rts */
static bool wait_check(const uint8_t *mem, int32_t at)
{
    const uint32_t fn = be24(mem + at + 1) - 0x400000u;
    if (fn >= CY_RAM_SIZE - 10) return false;
    const uint8_t *f = mem + fn;
    return f[0] == 0x01 && f[1] == 0x00 && f[2] == 0x6b && f[3] == 0x20 && f[8] == 0x54 && f[9] == 0x70;
}

static void words_at(h8s_cpu_t *c, uint32_t pc, int32_t n)
{
    c->icount -= n * c->cost16[(pc >> 16) & 0xff];
}

/* Nothing the loop reads changes until an interrupt is taken, and none can
 * be raised while it runs, so after one whole round the rest of the slice
 * goes by in rounds just like it. */
static void wait_loop(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
    uint32_t *r = c->er;
    const uint32_t at = c->ppc;
    const uint8_t *code = c->rpage[(at >> 16) & 0xff] + (at & 0xffff) - 4;
    const uint32_t fn = be24(code + 1), err = be32(code + 16), busy = be32(code + 24);
    const uint32_t ticks = (uint32_t)peek16(c, fn + 4) << 16 | peek16(c, fn + 6);
    trap_word(c);
    for (;;) {
        const int32_t before = c->icount;
        words(c, 4);
        const uint32_t d = r[0] - r[4];
        c->ccr = arith_flags(c->ccr, r[0], r[4], 32, true);
        r[0] = d;
        c->ccr = arith_flags(c->ccr, d, 999, 32, true);
        words(c, 1);
        if (!(c->ccr & 0x05)) {                     /* bhi: timed out */
            c->icount -= 1;
            c->pc = (at + 0x1a) & AMASK;
            return;
        }
        words(c, 3);
        uint8_t v = h8s_read8(c, err);
        r[2] = (r[2] & ~0xffu) | v;
        c->ccr = nzv_flags(c->ccr, v, 0x80);
        words(c, 1);
        if (v) {
            c->icount -= 1;
            c->pc = (at + 0x1a) & AMASK;
            return;
        }
        words(c, 3);
        v = h8s_read8(c, busy);
        r[2] = (r[2] & ~0xffu) | v;
        c->ccr = nzv_flags(c->ccr, v, 0x80);
        words(c, 1);
        if (!v) {
            c->pc = (at + 0x1a) & AMASK;
            return;
        }
        c->icount -= 1;
        if (!hle_yield) {                           /* the checker: one round */
            c->pc = (at - 4) & AMASK;
            return;
        }
        /* jsr get_ticks */
        words(c, 2);
        r[7] -= 4;
        h8s_write32(c, r[7], at);
        words_at(c, fn, 4);
        r[0] = h8s_read32(c, ticks);
        c->ccr = nzv_flags(c->ccr, r[0], 0x80000000u);
        words_at(c, fn, 1);
        h8s_read32(c, r[7]);
        r[7] += 4;
        c->icount -= 1;
        if (must_yield(c)) { c->pc = at; return; }
        if (c->icount > 0) {
            const int32_t round = before - c->icount;
            c->icount -= (c->icount + round - 1) / round * round;
            c->pc = at;
            return;
        }
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

static bool hle_checking(void) { return chk.active; }

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
    c->cycles = outer_cycles + (uint64_t)(outer_slice - outer_icount);     /* the time stays */
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
