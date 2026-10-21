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
 *     to unscramble data. */
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

void cy_hle_patch_rom(cybiko_t *m)
{
    int32_t at = find(m->rom, CY_ROM_SIZE, mul_code, sizeof mul_code);
    m->hle_mul_pc = at < 0 ? 0xffffffffu : (uint32_t)at;
    if (at >= 0) {
        put_trap(m->rom + at);
        put_trap(m->rom + CY_ROM_SIZE + at);    /* the mirror */
    }
}

void cy_hle_patch_kernel(cybiko_t *m)
{
    int32_t at = find(m->ram, CY_RAM_SIZE, page_sum_code, sizeof page_sum_code);
    if (at < 0) return;
    put_trap(m->ram + at);
    m->hle_sum_pc = 0x400000u + (uint32_t)at;
}

static uint8_t peek8(h8s_cpu_t *c, uint32_t a)
{
    a &= AMASK;
    const uint8_t *p = c->rpage[a >> 16];
    return p ? p[a & 0xffff] : h8s_read8(c, a);
}

static void ret(h8s_cpu_t *c)
{
    c->pc = h8s_read32(c, c->er[7]) & AMASK;
    c->er[7] += 4;
}

bool cy_hle(cybiko_t *m)
{
    h8s_cpu_t *c = &m->cpu;
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
