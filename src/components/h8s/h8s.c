/* H8S/2000 interpreter. See h8s.h for scope and references.
 *
 * Decoding follows the first opcode byte, then the second where the manual
 * packs several instructions into one first byte. Comments name each case by
 * its opcode pattern (MAME h8.lst value/mask) so a case can be checked against
 * the table. Timing is approximate: each instruction word fetched and each
 * data access costs what the machine set for that page (h8s_set_cost, from
 * its bus widths and wait states), plus the manual's internal states for
 * multiply/divide. The machine only needs states to pace its timers. */
#include "h8s.h"
#include <string.h>

#define F_I H8S_CCR_I
#define F_H H8S_CCR_H
#define F_N H8S_CCR_N
#define F_Z H8S_CCR_Z
#define F_V H8S_CCR_V
#define F_C H8S_CCR_C

#define AMASK 0xffffffu

#ifdef H8S_TEST_HOOKS
/* Counts word/longword accesses at odd addresses, which the bus forces even.
 * The differential tester skips those cases (the GNU sim does not). */
unsigned h8s_odd_accesses;
#define NOTE_ODD(a) (h8s_odd_accesses += (a) & 1)
#else
#define NOTE_ODD(a) ((void)0)
#endif

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define INLINE static inline __attribute__((always_inline))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define INLINE static inline
#endif

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

INLINE uint8_t rd8(h8s_cpu_t *c, uint32_t a)
{
    a &= AMASK;
    c->icount -= c->cost8[a >> 16];
    const uint8_t *p = c->rpage[a >> 16];
    if (LIKELY(p != NULL)) return p[a & 0xffff];
    if (a - c->fine_base < c->fine_size) return c->fine_mem[a - c->fine_base];
    return c->bus.read8(c->ctx, a);
}

INLINE uint16_t rd16(h8s_cpu_t *c, uint32_t a)
{
    NOTE_ODD(a);
    a &= AMASK & ~1u;
    c->icount -= c->cost16[a >> 16];
    const uint8_t *p = c->rpage[a >> 16];
    if (LIKELY(p != NULL)) {
        p += a & 0xffff;
        return (uint16_t)(p[0] << 8 | p[1]);
    }
    if (a - c->fine_base < c->fine_size) {
        p = c->fine_mem + (a - c->fine_base);
        return (uint16_t)(p[0] << 8 | p[1]);
    }
    return c->bus.read16(c->ctx, a);
}

INLINE uint32_t rd32(h8s_cpu_t *c, uint32_t a)
{
    return (uint32_t)rd16(c, a) << 16 | rd16(c, a + 2);
}

INLINE void wr8(h8s_cpu_t *c, uint32_t a, uint8_t v)
{
    a &= AMASK;
    c->icount -= c->cost8[a >> 16];
    uint8_t *p = c->wpage[a >> 16];
    if (LIKELY(p != NULL)) p[a & 0xffff] = v;
    else if (a - c->fine_base < c->fine_size) c->fine_mem[a - c->fine_base] = v;
    else c->bus.write8(c->ctx, a, v);
}

INLINE void wr16(h8s_cpu_t *c, uint32_t a, uint16_t v)
{
    NOTE_ODD(a);
    a &= AMASK & ~1u;
    c->icount -= c->cost16[a >> 16];
    uint8_t *p = c->wpage[a >> 16];
    if (LIKELY(p != NULL)) {
        p += a & 0xffff;
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else if (a - c->fine_base < c->fine_size) {
        p = c->fine_mem + (a - c->fine_base);
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        c->bus.write16(c->ctx, a, v);
    }
}

INLINE void wr32(h8s_cpu_t *c, uint32_t a, uint32_t v)
{
    wr16(c, a, (uint16_t)(v >> 16));
    wr16(c, a + 2, (uint16_t)v);
}

#define FETCH_NONE 0xff000000u   /* no 24-bit PC is within 64 KB above it */

static uint16_t fetch_slow(h8s_cpu_t *c)
{
    uint32_t pc = c->pc & AMASK, page = pc >> 16;
    if (c->rpage[page]) {
        c->fetch_page = pc & 0xff0000u;
        c->fetch_host = c->rpage[page];
        c->fetch_cost = c->cost16[page];
    }
    uint16_t v = rd16(c, pc);
    c->pc = (pc + 2) & AMASK;
    return v;
}

/* Code runs from directly mapped memory nearly always, so the page it is in
 * is kept at hand. Leaving the page (by a branch or by running off its end)
 * goes through fetch_slow, which also wraps the PC at 16 MB. */
INLINE uint16_t fetch(h8s_cpu_t *c)
{
    uint32_t off = c->pc - c->fetch_page;
    if (LIKELY(off < 0x10000)) {
        NOTE_ODD(off);
        const uint8_t *p = c->fetch_host + (off & ~1u);
        c->icount -= c->fetch_cost;
        c->pc += 2;
        return (uint16_t)(p[0] << 8 | p[1]);
    }
    return fetch_slow(c);
}

uint8_t  h8s_read8(h8s_cpu_t *c, uint32_t a)  { return rd8(c, a); }
uint16_t h8s_read16(h8s_cpu_t *c, uint32_t a) { return rd16(c, a); }
uint32_t h8s_read32(h8s_cpu_t *c, uint32_t a) { return rd32(c, a); }
void h8s_write8(h8s_cpu_t *c, uint32_t a, uint8_t v)   { wr8(c, a, v); }
void h8s_write16(h8s_cpu_t *c, uint32_t a, uint16_t v) { wr16(c, a, v); }
void h8s_write32(h8s_cpu_t *c, uint32_t a, uint32_t v) { wr32(c, a, v); }

/* ------------------------------------------------------------------ */
/* Registers: r8 0-7 = R0H-R7H, 8-15 = R0L-R7L; r16 0-7 = R0-R7,      */
/* 8-15 = E0-E7; r32 0-7 = ER0-ER7.                                   */
/* ------------------------------------------------------------------ */

INLINE uint8_t r8(h8s_cpu_t *c, unsigned n)
{
    uint32_t v = c->er[n & 7];
    return (uint8_t)(n & 8 ? v : v >> 8);
}

INLINE void w8(h8s_cpu_t *c, unsigned n, uint8_t v)
{
    uint32_t *r = &c->er[n & 7];
    if (n & 8) *r = (*r & ~0xffu) | v;
    else *r = (*r & ~0xff00u) | (uint32_t)v << 8;
}

INLINE uint16_t r16(h8s_cpu_t *c, unsigned n)
{
    uint32_t v = c->er[n & 7];
    return (uint16_t)(n & 8 ? v >> 16 : v);
}

INLINE void w16(h8s_cpu_t *c, unsigned n, uint16_t v)
{
    uint32_t *r = &c->er[n & 7];
    if (n & 8) *r = (*r & 0xffffu) | (uint32_t)v << 16;
    else *r = (*r & 0xffff0000u) | v;
}

#define R32(n) (c->er[(n) & 7])

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

INLINE void nzv8(h8s_cpu_t *c, uint8_t v)
{
    c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z | F_V)) | (v ? 0 : F_Z) | (v & 0x80 ? F_N : 0));
}
INLINE void nzv16(h8s_cpu_t *c, uint16_t v)
{
    c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z | F_V)) | (v ? 0 : F_Z) | (v & 0x8000 ? F_N : 0));
}
INLINE void nzv32(h8s_cpu_t *c, uint32_t v)
{
    c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z | F_V)) | (v ? 0 : F_Z) | (v & 0x80000000u ? F_N : 0));
}

/* Generic add/sub with H/N/Z/V/C. `bits` is 8/16/32, `hbit` the half-carry
 * bit (3/11/27). carry_in for ADDX/SUBX; `zsticky` keeps Z set only if it was
 * already set (ADDX/SUBX). */
INLINE uint32_t addsub(h8s_cpu_t *c, uint32_t a, uint32_t b, int bits, bool sub,
                       unsigned carry_in, bool zsticky)
{
    const uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    const uint32_t sign = 1u << (bits - 1);
    const uint32_t hmask = (1u << (bits - 4)) - 1;
    uint64_t res;
    unsigned h;
    if (sub) {
        res = (uint64_t)(a & mask) - (b & mask) - carry_in;
        h = (((a & hmask) - (b & hmask) - carry_in) >> (bits - 4)) & 1;
    } else {
        res = (uint64_t)(a & mask) + (b & mask) + carry_in;
        h = (((a & hmask) + (b & hmask) + carry_in) >> (bits - 4)) & 1;
    }
    uint32_t r = (uint32_t)res & mask;
    uint8_t f = c->ccr & ~(F_H | F_N | F_V | F_C);
    if (zsticky) { if (r) f &= ~F_Z; }
    else f = (uint8_t)((f & ~F_Z) | (r ? 0 : F_Z));
    if (h) f |= F_H;
    if (r & sign) f |= F_N;
    uint32_t ovf = sub ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r);
    if (ovf & sign) f |= F_V;
    if ((res >> bits) & 1) f |= F_C;
    c->ccr = f;
    return r;
}

#define ADD8(a, b)  ((uint8_t)addsub(c, a, b, 8, false, 0, false))
#define ADD16(a, b) ((uint16_t)addsub(c, a, b, 16, false, 0, false))
#define ADD32(a, b) (addsub(c, a, b, 32, false, 0, false))
#define SUB8(a, b)  ((uint8_t)addsub(c, a, b, 8, true, 0, false))
#define SUB16(a, b) ((uint16_t)addsub(c, a, b, 16, true, 0, false))
#define SUB32(a, b) (addsub(c, a, b, 32, true, 0, false))

/* INC/DEC: N, Z, V only. */
INLINE uint32_t incdec(h8s_cpu_t *c, uint32_t a, uint32_t b, int bits, bool dec)
{
    const uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    const uint32_t sign = 1u << (bits - 1);
    uint32_t r = (dec ? a - b : a + b) & mask;
    uint32_t ovf = dec ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r);
    c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z | F_V)) | (r ? 0 : F_Z) |
                       (r & sign ? F_N : 0) | (ovf & sign ? F_V : 0));
    return r;
}

/* Shift/rotate group (opcodes 10-13). kind: 0 shll, 1 shal, 2 shlr, 3 shar,
 * 4 rotxl, 5 rotl, 6 rotxr, 7 rotr. Shifts by 1 or 2. */
INLINE uint32_t shift(h8s_cpu_t *c, uint32_t v, int bits, int kind, int count)
{
    const uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    const uint32_t sign = 1u << (bits - 1);
    uint8_t f = c->ccr & ~(F_N | F_Z | F_V | F_C);
    unsigned carry = c->ccr & F_C;
    bool ovf = false;
    v &= mask;
    for (int i = 0; i < count; i++) {
        uint32_t msb = v & sign, lsb = v & 1;
        switch (kind) {
        case 0: v = (v << 1) & mask; carry = msb != 0; break;
        case 1:
            v = (v << 1) & mask;
            carry = msb != 0;
            if ((v & sign) != msb) ovf = true;
            break;
        case 2: v >>= 1; carry = lsb; break;
        case 3: v = (v >> 1) | msb; carry = lsb; break;
        case 4: v = ((v << 1) & mask) | carry; carry = msb != 0; break;
        case 5: v = ((v << 1) & mask) | (msb ? 1 : 0); carry = msb != 0; break;
        case 6: v = (v >> 1) | (carry ? sign : 0); carry = lsb; break;
        case 7: v = (v >> 1) | (lsb ? sign : 0); carry = lsb; break;
        }
    }
    if (!v) f |= F_Z;
    if (v & sign) f |= F_N;
    if (ovf) f |= F_V;
    if (carry) f |= F_C;
    c->ccr = f;
    return v;
}

/* Branch condition, cc = low nibble of the Bcc opcode. */
INLINE bool cond(h8s_cpu_t *c, unsigned cc)
{
    const uint8_t f = c->ccr;
    const bool C = f & F_C, Z = f & F_Z, N = f & F_N, V = f & F_V;
    switch (cc & 15) {
    case 0x0: return true;
    case 0x1: return false;
    case 0x2: return !(C || Z);
    case 0x3: return C || Z;
    case 0x4: return !C;
    case 0x5: return C;
    case 0x6: return !Z;
    case 0x7: return Z;
    case 0x8: return !V;
    case 0x9: return V;
    case 0xa: return !N;
    case 0xb: return N;
    case 0xc: return N == V;
    case 0xd: return N != V;
    case 0xe: return !Z && N == V;
    default:  return Z || N != V;
    }
}

static inline uint32_t sx8(uint32_t v)  { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
static inline uint32_t abs16(uint16_t v) { return sx16(v) & AMASK; }

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

static void push16(h8s_cpu_t *c, uint16_t v)
{
    R32(7) -= 2;
    wr16(c, R32(7), v);
}

static void enter_exception(h8s_cpu_t *c, int vector)
{
    /* Stack: PC low word, then CCR with PC bits 23..16, then EXR in mode 2. */
    push16(c, (uint16_t)c->pc);
    push16(c, (uint16_t)(c->ccr << 8 | ((c->pc >> 16) & 0xff)));
    if (c->intm2) push16(c, (uint16_t)(c->exr << 8));
    c->pc = rd32(c, (uint32_t)vector * 4) & AMASK;
    c->icount -= 2;
    c->sleeping = false;
    c->irq_check = true;
}

void h8s_exception(h8s_cpu_t *c, int vector)
{
    enter_exception(c, vector);
    c->ccr |= F_I;
    if (c->intm2) c->exr &= ~H8S_EXR_T;
}

/* Interrupt entry: mode 0 masks with CCR.I; mode 2 raises the EXR mask to
 * the accepted priority (NMI = 8 masks at 7) and leaves CCR.I alone. */
static void take_interrupt(h8s_cpu_t *c, int vector, int level)
{
    enter_exception(c, vector);
    if (c->intm2) c->exr = (uint8_t)(H8S_EXR_NC | (level > 7 ? 7 : level));
    else c->ccr |= F_I;
}

void h8s_init(h8s_cpu_t *c)
{
    memset(c, 0, sizeof *c);
    memset(c->cost8, 1, sizeof c->cost8);
    memset(c->cost16, 1, sizeof c->cost16);
    c->fetch_page = FETCH_NONE;
}

void h8s_set_cost(h8s_cpu_t *c, uint32_t base, uint32_t size, uint8_t byte_states,
                  uint8_t word_states)
{
    for (uint32_t off = 0; off < size; off += 0x10000) {
        uint32_t page = ((base + off) & AMASK) >> 16;
        c->cost8[page] = byte_states;
        c->cost16[page] = word_states;
    }
    c->fetch_page = FETCH_NONE;
}

void h8s_reset(h8s_cpu_t *c)
{
    c->ccr = (uint8_t)(c->ccr | F_I);
    c->exr = H8S_EXR_NC | H8S_EXR_I;
    c->intm2 = false;
    c->sleeping = false;
    c->pc = rd32(c, 0) & AMASK;
    c->irq_check = true;
}

void h8s_map(h8s_cpu_t *c, uint32_t base, uint32_t size, uint8_t *host,
             uint32_t host_size, bool writable)
{
    for (uint32_t off = 0; off < size; off += 0x10000) {
        uint32_t page = ((base + off) & AMASK) >> 16;
        uint8_t *p = host ? host + (host_size ? off % host_size : off) : NULL;
        c->rpage[page] = p;
        c->wpage[page] = writable ? p : NULL;
    }
    c->fetch_page = FETCH_NONE;
}

/* ------------------------------------------------------------------ */
/* Effective addresses for the MOV/LDC/STC memory family.             */
/* `sz` is the access size in bytes for post-increment/pre-decrement.  */
/* ------------------------------------------------------------------ */

static void illegal(h8s_cpu_t *c, uint16_t op)
{
    if (c->bus.illegal) c->bus.illegal(c->ctx, c, op);
    /* Real hardware takes no illegal-instruction exception on the H8S; we
     * treat it as a NOP so a stray opcode does not wedge the emulator. */
}

/* Bit-manipulation operations shared by the register and memory forms.
 * `sub` selects the operation (second byte high nibble/bit 7 of the bit
 * opcode: 60-63 register-number forms, 67/70-77 immediate forms). Returns
 * true when the operand is modified and must be written back. */
enum { B_SET, B_NOT, B_CLR, B_TST, B_OR, B_IOR, B_XOR, B_IXOR, B_AND, B_IAND,
       B_LD, B_ILD, B_ST, B_IST };

INLINE bool bitop(h8s_cpu_t *c, int op, unsigned bit, uint8_t *v)
{
    const uint8_t m = (uint8_t)(1u << (bit & 7));
    const bool b = *v & m;
    switch (op) {
    case B_SET:  *v |= m; return true;
    case B_NOT:  *v ^= m; return true;
    case B_CLR:  *v &= (uint8_t)~m; return true;
    case B_TST:  c->ccr = (uint8_t)(b ? c->ccr & ~F_Z : c->ccr | F_Z); return false;
    case B_OR:   if (b) c->ccr |= F_C; return false;
    case B_IOR:  if (!b) c->ccr |= F_C; return false;
    case B_XOR:  if (b) c->ccr ^= F_C; return false;
    case B_IXOR: if (!b) c->ccr ^= F_C; return false;
    case B_AND:  if (!b) c->ccr &= ~F_C; return false;
    case B_IAND: if (b) c->ccr &= ~F_C; return false;
    case B_LD:   c->ccr = (uint8_t)(b ? c->ccr | F_C : c->ccr & ~F_C); return false;
    case B_ILD:  c->ccr = (uint8_t)(!b ? c->ccr | F_C : c->ccr & ~F_C); return false;
    case B_ST:   if (c->ccr & F_C) *v |= m; else *v &= (uint8_t)~m; return true;
    case B_IST:  if (c->ccr & F_C) *v &= (uint8_t)~m; else *v |= m; return true;
    }
    return false;
}

/* Decode the operation of a bit instruction from its operation word
 * (the 60xx-77xx word, which for memory forms is the second word). Returns
 * -1 if not a valid bit operation. *bit receives the bit number source:
 * register number (for 60-63) is resolved by the caller. */
static int bit_decode(uint16_t w, bool *reg_form)
{
    const unsigned hi = w >> 8, b7 = (w >> 7) & 1;
    *reg_form = false;
    switch (hi) {
    case 0x60: *reg_form = true; return B_SET;
    case 0x61: *reg_form = true; return B_NOT;
    case 0x62: *reg_form = true; return B_CLR;
    case 0x63: *reg_form = true; return B_TST;
    case 0x67: return b7 ? B_IST : B_ST;
    case 0x70: return b7 ? -1 : B_SET;
    case 0x71: return b7 ? -1 : B_NOT;
    case 0x72: return b7 ? -1 : B_CLR;
    case 0x73: return b7 ? -1 : B_TST;
    case 0x74: return b7 ? B_IOR : B_OR;
    case 0x75: return b7 ? B_IXOR : B_XOR;
    case 0x76: return b7 ? B_IAND : B_AND;
    case 0x77: return b7 ? B_ILD : B_LD;
    }
    return -1;
}

/* Memory-operand bit instruction: `ea` already computed, `w` is the
 * operation word. Test-type opcodes (7c/7e, 6a1x/6a3x) accept only the
 * non-modifying operations; modifying ones (7d/7f, 6a18/6a38) only the
 * modifying operations, matching the real encoding space. */
static void bit_mem(h8s_cpu_t *c, uint32_t ea, uint16_t w, bool modify_form)
{
    bool reg_form;
    int op = bit_decode(w, &reg_form);
    if (op < 0) { illegal(c, w); return; }
    bool modifies = op == B_SET || op == B_NOT || op == B_CLR || op == B_ST || op == B_IST;
    if (modifies != modify_form) { illegal(c, w); return; }
    unsigned bit = reg_form ? r8(c, (w >> 4) & 15) : (w >> 4) & 7;
    uint8_t v = rd8(c, ea);
    if (bitop(c, op, bit, &v)) wr8(c, ea, v);
}

/* ------------------------------------------------------------------ */
/* 0100 / 0140 / 0141 prefixed memory moves (MOV.L, LDC.W, STC.W).     */
/* `kind` 0 = MOV.L, 1 = CCR, 2 = EXR. Returns false on a bad encoding. */
/* ------------------------------------------------------------------ */

static bool prefixed_move(h8s_cpu_t *c, int kind, uint16_t w)
{
    const unsigned rs = (w >> 4) & 7, rd = w & 7;
    const bool store = w & 0x80;
    const int sz = kind == 0 ? 4 : 2;
    uint32_t ea;
    switch (w >> 8) {
    case 0x69: ea = R32(rs); break;                                   /* @ERn */
    case 0x6b:                                                        /* @aa */
        if ((w & 0x70) == 0x00) ea = abs16(fetch(c));
        else if ((w & 0x70) == 0x20) { ea = (uint32_t)fetch(c) << 16; ea = (ea | fetch(c)) & AMASK; }
        else return false;
        break;
    case 0x6d:                                                        /* @ERn+ / @-ERn */
        if (store) { R32(rs) -= sz; ea = R32(rs); }
        else { ea = R32(rs); R32(rs) += sz; }
        c->icount--;
        break;
    case 0x6f: ea = R32(rs) + sx16(fetch(c)); break;                  /* @(d:16,ERn) */
    case 0x78: {                                                      /* @(d:32,ERn) */
        const unsigned base = (w >> 4) & 7;
        uint16_t w2 = fetch(c);
        if ((w2 & 0xff70) != 0x6b20) return false;
        uint32_t d = (uint32_t)fetch(c) << 16;
        d |= fetch(c);
        ea = R32(base) + d;
        if (kind == 0) {
            /* mov.l @(d:32,ERs),ERd: 0100 78s0 6b2d dddd dddd */
            if (w2 & 0x80) { uint32_t v = R32(w2 & 7); nzv32(c, v); wr32(c, ea, v); }
            else { uint32_t v = rd32(c, ea); nzv32(c, v); R32(w2 & 7) = v; }
        } else {
            if (w2 & 0x80) {
                uint8_t v = kind == 1 ? c->ccr : c->exr;
                wr16(c, ea, (uint16_t)(v << 8 | v));
            } else {
                uint8_t v = (uint8_t)(rd16(c, ea) >> 8);
                if (kind == 1) c->ccr = v; else c->exr = v | H8S_EXR_NC;
                c->irq_check = c->irq_hold = true;
            }
        }
        return true;
    }
    default: return false;
    }
    if (kind == 0) {
        if (store) {
            uint32_t v = R32(rd);
            if ((w >> 8) == 0x6d && rd == rs) v += 4;   /* value before predecrement */
            nzv32(c, v);
            wr32(c, ea, v);
        } else {
            uint32_t v = rd32(c, ea);
            nzv32(c, v);
            R32(rd) = v;
        }
    } else if (store) {
        uint8_t v = kind == 1 ? c->ccr : c->exr;
        wr16(c, ea, (uint16_t)(v << 8 | v));
    } else {
        uint8_t v = (uint8_t)(rd16(c, ea) >> 8);
        if (kind == 1) c->ccr = v; else c->exr = v | H8S_EXR_NC;
        c->irq_check = c->irq_hold = true;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Execute one instruction                                             */
/* ------------------------------------------------------------------ */

#ifdef H8S_PROFILE
__attribute__((noinline)) static void execute(h8s_cpu_t *c)
#else
INLINE void execute(h8s_cpu_t *c)
#endif
{
    c->ppc = c->pc;
    const uint16_t op = fetch(c);
    const unsigned hi = op >> 8, lo = op & 0xff;
    const unsigned n_hi = (op >> 4) & 15, n_lo = op & 15;

    switch (hi) {
    case 0x00:                                       /* 0000 nop */
        if (lo) illegal(c, op);
        return;

    case 0x01: {
        switch (lo) {
        case 0x00: {                                 /* 0100 mov.l prefix */
            uint16_t w = fetch(c);
            if (w & 0x08 && (w >> 8) != 0x78) { illegal(c, op); return; }
            /* The register indirect forms, most of them, without the call. */
            const unsigned mode = w >> 8;
            if (mode == 0x69 || mode == 0x6d || mode == 0x6f) {
                const unsigned rs = (w >> 4) & 7, rd = w & 7;
                const bool store = w & 0x80;
                uint32_t ea;
                if (mode == 0x69) {
                    ea = R32(rs);
                } else if (mode == 0x6f) {
                    ea = R32(rs) + sx16(fetch(c));
                } else {
                    if (store) { R32(rs) -= 4; ea = R32(rs); }
                    else { ea = R32(rs); R32(rs) += 4; }
                    c->icount--;
                }
                if (store) {
                    uint32_t v = R32(rd);
                    if (mode == 0x6d && rd == rs) v += 4;   /* value before predecrement */
                    nzv32(c, v);
                    wr32(c, ea, v);
                } else {
                    uint32_t v = rd32(c, ea);
                    nzv32(c, v);
                    R32(rd) = v;
                }
                return;
            }
            if (!prefixed_move(c, 0, w)) illegal(c, op);
            return;
        }
        case 0x10: case 0x20: case 0x30: {           /* 01x0 ldm.l / stm.l */
            uint16_t w = fetch(c);
            const int count = (lo >> 4) + 1;
            c->icount--;
            if ((w & 0xfff8) == 0x6d70) {            /* ldm.l @sp+,(ERn-count+1 .. ERn) */
                unsigned top = w & 7;
                for (int i = 0; i < count; i++) {
                    uint32_t v = rd32(c, R32(7));
                    R32(7) += 4;
                    if (((top - i) & 7) != 7) R32(top - i) = v;
                }
                return;
            }
            if ((w & 0xfff8) == 0x6df0) {            /* stm.l (ERn .. ERn+count-1),@-sp */
                unsigned first = w & 7;
                for (int i = 0; i < count; i++) {
                    uint32_t v = R32(first + i);
                    R32(7) -= 4;
                    wr32(c, R32(7), v);
                }
                return;
            }
            illegal(c, op);
            return;
        }
        case 0x40: {                                 /* 0140 ldc/stc.w ccr */
            uint16_t w = fetch(c);
            if (!prefixed_move(c, 1, w)) illegal(c, op);
            return;
        }
        case 0x41: {                                 /* 0141 exr forms */
            uint16_t w = fetch(c);
            switch (w >> 8) {
            case 0x04: c->exr |= (uint8_t)w; break;                       /* orc #,exr */
            case 0x05: c->exr ^= (uint8_t)w; break;                       /* xorc #,exr */
            case 0x06: c->exr &= (uint8_t)w; break;                       /* andc #,exr */
            case 0x07: c->exr = (uint8_t)w; break;                        /* ldc #,exr */
            default:
                if (!prefixed_move(c, 2, w)) illegal(c, op);
                return;
            }
            c->exr |= H8S_EXR_NC;
            c->irq_check = true;
            return;
        }
        case 0x80:                                   /* 0180 sleep */
            c->sleeping = true;
            c->irq_check = true;
            return;
        case 0xc0: {                                 /* 01c0 mulxs */
            uint16_t w = fetch(c);
            const unsigned rs = (w >> 4) & 15, rd = w & 15;
            if ((w & 0xff00) == 0x5000) {            /* mulxs.b Rs8,Rd16 */
                uint16_t v = (uint16_t)((int8_t)r16(c, rd) * (int8_t)r8(c, rs));
                c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z)) | (v ? 0 : F_Z) | (v & 0x8000 ? F_N : 0));
                w16(c, rd, v);
                c->icount -= 11;
                return;
            }
            if ((w & 0xff08) == 0x5200) {            /* mulxs.w Rs16,ERd */
                uint32_t v = (uint32_t)((int32_t)(int16_t)R32(rd) * (int16_t)r16(c, rs));
                c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z)) | (v ? 0 : F_Z) | (v & 0x80000000u ? F_N : 0));
                R32(rd) = v;
                c->icount -= 19;
                return;
            }
            illegal(c, op);
            return;
        }
        case 0xd0: {                                 /* 01d0 divxs */
            uint16_t w = fetch(c);
            const unsigned rs = (w >> 4) & 15, rd = w & 15;
            if ((w & 0xff00) == 0x5100) {            /* divxs.b Rs8,Rd16 */
                int32_t p = (int16_t)r16(c, rd), d = (int8_t)r8(c, rs);
                c->icount -= 11;
                c->ccr &= ~(F_N | F_Z);
                if (!d) { c->ccr |= F_Z; return; }
                int32_t q = p / d, r = p % d;    /* C truncates toward zero, as the H8S */
                if (q < 0) c->ccr |= F_N;
                w16(c, rd, (uint16_t)((q & 0xff) | (r & 0xff) << 8));
                return;
            }
            if ((w & 0xff08) == 0x5300) {            /* divxs.w Rs16,ERd */
                int32_t p = (int32_t)R32(rd), d = (int16_t)r16(c, rs);
                c->icount -= 19;
                c->ccr &= ~(F_N | F_Z);
                if (!d) { c->ccr |= F_Z; return; }
                int64_t q = (int64_t)p / d, r = (int64_t)p % d;
                if (q < 0) c->ccr |= F_N;
                R32(rd) = (uint32_t)((q & 0xffff) | (r & 0xffff) << 16);
                return;
            }
            illegal(c, op);
            return;
        }
        case 0xe0: {                                 /* 01e0 7b0c tas @ERd */
            uint16_t w = fetch(c);
            if ((w & 0xff8f) != 0x7b0c) { illegal(c, op); return; }
            uint32_t ea = R32((w >> 4) & 7);
            uint8_t v = rd8(c, ea);
            nzv8(c, v);
            wr8(c, ea, v | 0x80);
            return;
        }
        case 0xf0: {                                 /* 01f0 or/xor/and.l */
            uint16_t w = fetch(c);
            if (w & 0x88) { illegal(c, op); return; }
            const unsigned rs = (w >> 4) & 7, rd = w & 7;
            uint32_t v;
            switch (w >> 8) {
            case 0x64: v = R32(rd) | R32(rs); break;
            case 0x65: v = R32(rd) ^ R32(rs); break;
            case 0x66: v = R32(rd) & R32(rs); break;
            default: illegal(c, op); return;
            }
            nzv32(c, v);
            R32(rd) = v;
            return;
        }
        default:
            illegal(c, op);
            return;
        }
    }

    case 0x02:                                       /* 020x stc ccr / 021x stc exr */
        if (n_hi == 0) w8(c, n_lo, c->ccr);
        else if (n_hi == 1) w8(c, n_lo, c->exr);
        else illegal(c, op);
        return;
    case 0x03:                                       /* 030x ldc ccr / 031x ldc exr */
        if (n_hi == 0) c->ccr = r8(c, n_lo);
        else if (n_hi == 1) c->exr = r8(c, n_lo) | H8S_EXR_NC;
        else { illegal(c, op); return; }
        c->irq_check = c->irq_hold = true;
        return;
    case 0x04: c->ccr |= (uint8_t)lo; c->irq_check = c->irq_hold = true; return;    /* orc */
    case 0x05: c->ccr ^= (uint8_t)lo; c->irq_check = c->irq_hold = true; return;    /* xorc */
    case 0x06: c->ccr &= (uint8_t)lo; c->irq_check = c->irq_hold = true; return;    /* andc */
    case 0x07: c->ccr = (uint8_t)lo; c->irq_check = c->irq_hold = true; return;     /* ldc #,ccr */

    case 0x08: w8(c, n_lo, ADD8(r8(c, n_lo), r8(c, n_hi))); return;   /* add.b */
    case 0x09: w16(c, n_lo, ADD16(r16(c, n_lo), r16(c, n_hi))); return; /* add.w */
    case 0x0a:
        if (n_hi == 0) w8(c, n_lo, (uint8_t)incdec(c, r8(c, n_lo), 1, 8, false)); /* inc.b */
        else if ((op & 0x88) == 0x80) R32(n_lo) = ADD32(R32(n_lo), R32(n_hi));    /* add.l */
        else illegal(c, op);
        return;
    case 0x0b:
        switch (n_hi) {
        case 0x0: if (op & 8) break; R32(n_lo) += 1; return;                      /* adds #1 */
        case 0x8: if (op & 8) break; R32(n_lo) += 2; return;                      /* adds #2 */
        case 0x9: if (op & 8) break; R32(n_lo) += 4; return;                      /* adds #4 */
        case 0x5: w16(c, n_lo, (uint16_t)incdec(c, r16(c, n_lo), 1, 16, false)); return;
        case 0xd: w16(c, n_lo, (uint16_t)incdec(c, r16(c, n_lo), 2, 16, false)); return;
        case 0x7: if (op & 8) break; R32(n_lo) = incdec(c, R32(n_lo), 1, 32, false); return;
        case 0xf: if (op & 8) break; R32(n_lo) = incdec(c, R32(n_lo), 2, 32, false); return;
        }
        illegal(c, op);
        return;
    case 0x0c: { uint8_t v = r8(c, n_hi); nzv8(c, v); w8(c, n_lo, v); return; }    /* mov.b */
    case 0x0d: { uint16_t v = r16(c, n_hi); nzv16(c, v); w16(c, n_lo, v); return; } /* mov.w */
    case 0x0e:                                                                       /* addx */
        w8(c, n_lo, (uint8_t)addsub(c, r8(c, n_lo), r8(c, n_hi), 8, false, c->ccr & F_C, true));
        return;
    case 0x0f:
        if (n_hi == 0) {                                                             /* daa */
            uint8_t v = r8(c, n_lo), adj = 0;
            if (c->ccr & F_C) {
                if (c->ccr & F_H) { if ((v & 0xf0) <= 0x30 && (v & 0x0f) <= 3) adj = 0x66; }
                else if ((v & 0xf0) <= 0x20) adj = (v & 0x0f) <= 9 ? 0x60 : 0x66;
            } else if (c->ccr & F_H) {
                if ((v & 0x0f) <= 3) adj = (v & 0xf0) <= 0x90 ? 0x06 : 0x66;
            } else if ((v & 0x0f) <= 9) {
                adj = (v & 0xf0) <= 0x90 ? 0x00 : 0x60;
            } else {
                adj = (v & 0xf0) <= 0x80 ? 0x06 : 0x66;
            }
            /* A decimal carry in stays a carry out. */
            uint8_t carry = c->ccr & F_C;
            w8(c, n_lo, ADD8(v, adj));
            c->ccr |= carry;
        } else if ((op & 0x88) == 0x80) {                                            /* mov.l */
            uint32_t v = R32(n_hi);
            nzv32(c, v);
            R32(n_lo) = v;
        } else {
            illegal(c, op);
        }
        return;

    case 0x10: case 0x11: case 0x12: case 0x13: {    /* shifts and rotates */
        /* n_hi: bit 3 selects the second operation of the opcode (shal, shar,
         * rotl, rotr), bit 2 the by-two form, bits 1..0 the size (0 b, 1 w, 3 l). */
        const int kind = (int)(hi - 0x10) * 2 + (n_hi >> 3);
        const int count = n_hi & 4 ? 2 : 1;
        switch (n_hi & 3) {
        case 0: w8(c, n_lo, (uint8_t)shift(c, r8(c, n_lo), 8, kind, count)); return;
        case 1: w16(c, n_lo, (uint16_t)shift(c, r16(c, n_lo), 16, kind, count)); return;
        case 3: if (op & 8) break; R32(n_lo) = shift(c, R32(n_lo), 32, kind, count); return;
        }
        illegal(c, op);
        return;
    }

    case 0x14: { uint8_t v = r8(c, n_lo) | r8(c, n_hi); nzv8(c, v); w8(c, n_lo, v); return; }
    case 0x15: { uint8_t v = r8(c, n_lo) ^ r8(c, n_hi); nzv8(c, v); w8(c, n_lo, v); return; }
    case 0x16: { uint8_t v = r8(c, n_lo) & r8(c, n_hi); nzv8(c, v); w8(c, n_lo, v); return; }
    case 0x17:
        switch (n_hi) {
        case 0x0: { uint8_t v = (uint8_t)~r8(c, n_lo); nzv8(c, v); w8(c, n_lo, v); return; }
        case 0x1: { uint16_t v = (uint16_t)~r16(c, n_lo); nzv16(c, v); w16(c, n_lo, v); return; }
        case 0x3: if (op & 8) break; { uint32_t v = ~R32(n_lo); nzv32(c, v); R32(n_lo) = v; return; }
        case 0x5: { uint16_t v = r16(c, n_lo) & 0xff; nzv16(c, v); w16(c, n_lo, v); return; }
        case 0x7: if (op & 8) break; { uint32_t v = R32(n_lo) & 0xffff; nzv32(c, v); R32(n_lo) = v; return; }
        case 0x8: w8(c, n_lo, SUB8(0, r8(c, n_lo))); return;
        case 0x9: w16(c, n_lo, SUB16(0, r16(c, n_lo))); return;
        case 0xb: if (op & 8) break; R32(n_lo) = SUB32(0, R32(n_lo)); return;
        case 0xd: { uint16_t v = (uint16_t)sx8(r16(c, n_lo)); nzv16(c, v); w16(c, n_lo, v); return; }
        case 0xf: if (op & 8) break; { uint32_t v = sx16(R32(n_lo)); nzv32(c, v); R32(n_lo) = v; return; }
        }
        illegal(c, op);
        return;

    case 0x18: w8(c, n_lo, SUB8(r8(c, n_lo), r8(c, n_hi))); return;     /* sub.b */
    case 0x19: w16(c, n_lo, SUB16(r16(c, n_lo), r16(c, n_hi))); return; /* sub.w */
    case 0x1a:
        if (n_hi == 0) w8(c, n_lo, (uint8_t)incdec(c, r8(c, n_lo), 1, 8, true));  /* dec.b */
        else if ((op & 0x88) == 0x80) R32(n_lo) = SUB32(R32(n_lo), R32(n_hi));    /* sub.l */
        else illegal(c, op);
        return;
    case 0x1b:
        switch (n_hi) {
        case 0x0: if (op & 8) break; R32(n_lo) -= 1; return;                      /* subs #1 */
        case 0x8: if (op & 8) break; R32(n_lo) -= 2; return;
        case 0x9: if (op & 8) break; R32(n_lo) -= 4; return;
        case 0x5: w16(c, n_lo, (uint16_t)incdec(c, r16(c, n_lo), 1, 16, true)); return;
        case 0xd: w16(c, n_lo, (uint16_t)incdec(c, r16(c, n_lo), 2, 16, true)); return;
        case 0x7: if (op & 8) break; R32(n_lo) = incdec(c, R32(n_lo), 1, 32, true); return;
        case 0xf: if (op & 8) break; R32(n_lo) = incdec(c, R32(n_lo), 2, 32, true); return;
        }
        illegal(c, op);
        return;
    case 0x1c: (void)SUB8(r8(c, n_lo), r8(c, n_hi)); return;                 /* cmp.b */
    case 0x1d: (void)SUB16(r16(c, n_lo), r16(c, n_hi)); return;              /* cmp.w */
    case 0x1e:                                                         /* subx */
        w8(c, n_lo, (uint8_t)addsub(c, r8(c, n_lo), r8(c, n_hi), 8, true, c->ccr & F_C, true));
        return;
    case 0x1f:
        if (n_hi == 0) {                                               /* das */
            uint8_t v = r8(c, n_lo), adj = 0;
            if (c->ccr & F_C) {
                if (c->ccr & F_H) { if ((v & 0xf0) >= 0x60 && (v & 0x0f) >= 6) adj = 0x9a; }
                else if ((v & 0xf0) >= 0x70 && (v & 0x0f) <= 9) adj = 0xa0;
            } else if ((c->ccr & F_H) && (v & 0xf0) <= 0x80 && (v & 0x0f) >= 6) {
                adj = 0xfa;
            }
            /* DAS keeps C as it was: borrow semantics of the decimal subtract. */
            uint8_t carry = c->ccr & F_C;
            w8(c, n_lo, ADD8(v, adj));
            c->ccr = (uint8_t)((c->ccr & ~F_C) | carry);
        } else if ((op & 0x88) == 0x80) {
            (void)SUB32(R32(n_lo), R32(n_hi));                         /* cmp.l */
        } else {
            illegal(c, op);
        }
        return;

    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: {
        uint8_t v = rd8(c, 0xffff00u | lo);                            /* mov.b @aa:8,Rd */
        nzv8(c, v);
        w8(c, hi & 15, v);
        return;
    }
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: {
        uint8_t v = r8(c, hi & 15);                                    /* mov.b Rs,@aa:8 */
        nzv8(c, v);
        wr8(c, 0xffff00u | lo, v);
        return;
    }

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        if (cond(c, hi)) {                                             /* bcc d:8 */
            c->pc = (c->pc + sx8(lo)) & AMASK;
            c->icount--;
        }
        return;

    case 0x50: {                                                       /* mulxu.b */
        w16(c, n_lo, (uint16_t)((r16(c, n_lo) & 0xff) * r8(c, n_hi)));
        c->icount -= 11;
        return;
    }
    case 0x51: {                                                       /* divxu.b */
        uint16_t p = r16(c, n_lo);
        uint8_t d = r8(c, n_hi);
        c->icount -= 11;
        c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z)) | (d & 0x80 ? F_N : 0) | (d ? 0 : F_Z));
        if (d) w16(c, n_lo, (uint16_t)((p / d) & 0xff) | (uint16_t)((p % d) & 0xff) << 8);
        return;
    }
    case 0x52:                                                         /* mulxu.w */
        if (op & 8) { illegal(c, op); return; }
        R32(n_lo) = (R32(n_lo) & 0xffff) * r16(c, n_hi);
        c->icount -= 19;
        return;
    case 0x53: {                                                       /* divxu.w */
        if (op & 8) { illegal(c, op); return; }
        uint32_t p = R32(n_lo);
        uint16_t d = r16(c, n_hi);
        c->icount -= 19;
        c->ccr = (uint8_t)((c->ccr & ~(F_N | F_Z)) | (d & 0x8000 ? F_N : 0) | (d ? 0 : F_Z));
        if (d) R32(n_lo) = ((p / d) & 0xffff) | ((p % d) & 0xffff) << 16;
        return;
    }
    case 0x54:                                                         /* rts */
        if (lo != 0x70) { illegal(c, op); return; }
        c->pc = rd32(c, R32(7)) & AMASK;
        R32(7) += 4;
        c->icount--;
        return;
    case 0x55: {                                                       /* bsr d:8 */
        uint32_t ret = c->pc;
        R32(7) -= 4;
        wr32(c, R32(7), ret);
        c->pc = (ret + sx8(lo)) & AMASK;
        return;
    }
    case 0x56: {                                                       /* rte */
        if (lo != 0x70) { illegal(c, op); return; }
        uint32_t sp = R32(7);
        if (c->intm2) { c->exr = (uint8_t)(rd16(c, sp) >> 8) | H8S_EXR_NC; sp += 2; }
        uint16_t w = rd16(c, sp);
        c->ccr = (uint8_t)(w >> 8);
        c->pc = ((uint32_t)(w & 0xff) << 16 | rd16(c, sp + 2)) & AMASK;
        R32(7) = sp + 4;
        c->icount--;
        c->irq_check = true;
        return;
    }
    case 0x57:                                                         /* trapa #n */
        if (op & 0xcf) { illegal(c, op); return; }
        h8s_exception(c, H8S_VEC_TRAPA + ((op >> 4) & 3));
        return;
    case 0x58: {                                                       /* bcc d:16 */
        if (op & 0x0f) { illegal(c, op); return; }
        uint16_t d = fetch(c);
        if (cond(c, n_hi)) { c->pc = (c->pc + sx16(d)) & AMASK; c->icount--; }
        return;
    }
    case 0x59:                                                         /* jmp @ERn */
        if (op & 0x8f) { illegal(c, op); return; }
        c->pc = R32(n_hi) & AMASK;
        return;
    case 0x5a: {                                                       /* jmp @aa:24 */
        uint16_t w = fetch(c);
        c->pc = ((uint32_t)lo << 16 | w) & AMASK;
        return;
    }
    case 0x5b:                                                         /* jmp @@aa:8 */
        c->pc = rd32(c, lo) & AMASK;
        c->icount--;
        return;
    case 0x5c: {                                                       /* bsr d:16 */
        if (lo) { illegal(c, op); return; }
        uint16_t d = fetch(c);
        uint32_t ret = c->pc;
        R32(7) -= 4;
        wr32(c, R32(7), ret);
        c->pc = (ret + sx16(d)) & AMASK;
        return;
    }
    case 0x5d: {                                                       /* jsr @ERn */
        if (op & 0x8f) { illegal(c, op); return; }
        uint32_t target = R32(n_hi);
        R32(7) -= 4;
        wr32(c, R32(7), c->pc);
        c->pc = target & AMASK;
        return;
    }
    case 0x5e: {                                                       /* jsr @aa:24 */
        uint16_t w = fetch(c);
        R32(7) -= 4;
        wr32(c, R32(7), c->pc);
        c->pc = ((uint32_t)lo << 16 | w) & AMASK;
        return;
    }
    case 0x5f: {                                                       /* jsr @@aa:8 */
        uint32_t target = rd32(c, lo);
        R32(7) -= 4;
        wr32(c, R32(7), c->pc);
        c->pc = target & AMASK;
        return;
    }

    case 0x60: case 0x61: case 0x62: case 0x63: case 0x67:             /* bit ops on Rd */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: {
        bool reg_form;
        int bop = bit_decode(op, &reg_form);
        if (bop < 0) { illegal(c, op); return; }
        unsigned bit = reg_form ? r8(c, n_hi) : (op >> 4) & 7;
        uint8_t v = r8(c, n_lo);
        if (bitop(c, bop, bit, &v)) w8(c, n_lo, v);
        return;
    }
    case 0x64: { uint16_t v = r16(c, n_lo) | r16(c, n_hi); nzv16(c, v); w16(c, n_lo, v); return; }
    case 0x65: { uint16_t v = r16(c, n_lo) ^ r16(c, n_hi); nzv16(c, v); w16(c, n_lo, v); return; }
    case 0x66: { uint16_t v = r16(c, n_lo) & r16(c, n_hi); nzv16(c, v); w16(c, n_lo, v); return; }

    case 0x68: {                                                       /* mov.b @ERs <-> Rd */
        uint32_t ea = R32(n_hi);
        if (op & 0x80) { uint8_t v = r8(c, n_lo); nzv8(c, v); wr8(c, ea, v); }
        else { uint8_t v = rd8(c, ea); nzv8(c, v); w8(c, n_lo, v); }
        return;
    }
    case 0x69: {                                                       /* mov.w @ERs <-> Rd */
        uint32_t ea = R32(n_hi);
        if (op & 0x80) { uint16_t v = r16(c, n_lo); nzv16(c, v); wr16(c, ea, v); }
        else { uint16_t v = rd16(c, ea); nzv16(c, v); w16(c, n_lo, v); }
        return;
    }
    case 0x6a: {
        switch (n_hi) {
        case 0x0: { uint8_t v = rd8(c, abs16(fetch(c))); nzv8(c, v); w8(c, n_lo, v); return; }
        case 0x8: { uint32_t ea = abs16(fetch(c)); uint8_t v = r8(c, n_lo); nzv8(c, v); wr8(c, ea, v); return; }
        case 0x2: case 0xa: {
            uint32_t ea = (uint32_t)fetch(c) << 16;
            ea = (ea | fetch(c)) & AMASK;
            uint8_t v;
            if (n_hi == 2) { v = rd8(c, ea); nzv8(c, v); w8(c, n_lo, v); }
            else { v = r8(c, n_lo); nzv8(c, v); wr8(c, ea, v); }
            return;
        }
        case 0x4: { uint8_t v = rd8(c, abs16(fetch(c))); nzv8(c, v); w8(c, n_lo, v); c->icount -= 8; return; } /* movfpe */
        case 0xc: { uint32_t ea = abs16(fetch(c)); uint8_t v = r8(c, n_lo); nzv8(c, v); wr8(c, ea, v); c->icount -= 8; return; } /* movtpe */
        case 0x1: case 0x3: {                    /* 6a1x/6a3x bit ops on @aa:16/@aa:32 */
            uint32_t ea;
            if (n_hi == 1) ea = abs16(fetch(c));
            else { ea = (uint32_t)fetch(c) << 16; ea = (ea | fetch(c)) & AMASK; }
            uint16_t w = fetch(c);
            if (n_lo == 0) bit_mem(c, ea, w, false);
            else if (n_lo == 8) bit_mem(c, ea, w, true);
            else illegal(c, op);
            return;
        }
        }
        illegal(c, op);
        return;
    }
    case 0x6b: {                                                       /* mov.w @aa */
        uint32_t ea;
        if ((n_hi & 7) == 0) ea = abs16(fetch(c));
        else if ((n_hi & 7) == 2) { ea = (uint32_t)fetch(c) << 16; ea = (ea | fetch(c)) & AMASK; }
        else { illegal(c, op); return; }
        if (n_hi & 8) { uint16_t v = r16(c, n_lo); nzv16(c, v); wr16(c, ea, v); }
        else { uint16_t v = rd16(c, ea); nzv16(c, v); w16(c, n_lo, v); }
        return;
    }
    case 0x6c: {                                                       /* mov.b @ERs+ / @-ERd */
        const unsigned rn = n_hi & 7;
        c->icount--;
        if (op & 0x80) {
            uint8_t v = r8(c, n_lo);
            R32(rn) -= 1;
            nzv8(c, v);
            wr8(c, R32(rn), v);
        } else {
            uint8_t v = rd8(c, R32(rn));
            R32(rn) += 1;
            nzv8(c, v);
            w8(c, n_lo, v);
        }
        return;
    }
    case 0x6d: {                                                       /* mov.w @ERs+ / @-ERd */
        const unsigned rn = n_hi & 7;
        c->icount--;
        if (op & 0x80) {
            uint16_t v = r16(c, n_lo);
            R32(rn) -= 2;
            nzv16(c, v);
            wr16(c, R32(rn), v);
        } else {
            uint16_t v = rd16(c, R32(rn));
            R32(rn) += 2;
            nzv16(c, v);
            w16(c, n_lo, v);
        }
        return;
    }
    case 0x6e: {                                                       /* mov.b @(d:16,ERs) */
        uint32_t ea = R32(n_hi & 7) + sx16(fetch(c));
        if (op & 0x80) { uint8_t v = r8(c, n_lo); nzv8(c, v); wr8(c, ea, v); }
        else { uint8_t v = rd8(c, ea); nzv8(c, v); w8(c, n_lo, v); }
        return;
    }
    case 0x6f: {                                                       /* mov.w @(d:16,ERs) */
        uint32_t ea = R32(n_hi & 7) + sx16(fetch(c));
        if (op & 0x80) { uint16_t v = r16(c, n_lo); nzv16(c, v); wr16(c, ea, v); }
        else { uint16_t v = rd16(c, ea); nzv16(c, v); w16(c, n_lo, v); }
        return;
    }

    case 0x78: {                                       /* 78r0 6a2x/6b2x mov @(d:32,ERs) */
        if (op & 0x8f) { illegal(c, op); return; }
        uint16_t w = fetch(c);
        uint32_t d = (uint32_t)fetch(c) << 16;
        d |= fetch(c);
        uint32_t ea = R32(n_hi) + d;
        const unsigned rd = w & 15;
        switch (w & 0xfff0) {
        case 0x6a20: { uint8_t v = rd8(c, ea); nzv8(c, v); w8(c, rd, v); return; }
        case 0x6aa0: { uint8_t v = r8(c, rd); nzv8(c, v); wr8(c, ea, v); return; }
        case 0x6b20: { uint16_t v = rd16(c, ea); nzv16(c, v); w16(c, rd, v); return; }
        case 0x6ba0: { uint16_t v = r16(c, rd); nzv16(c, v); wr16(c, ea, v); return; }
        }
        illegal(c, op);
        return;
    }

    case 0x79: {                                                       /* word immediates */
        uint16_t imm = fetch(c);
        uint16_t v = r16(c, n_lo);
        switch (n_hi) {
        case 0x0: nzv16(c, imm); w16(c, n_lo, imm); return;
        case 0x1: w16(c, n_lo, ADD16(v, imm)); return;
        case 0x2: (void)SUB16(v, imm); return;
        case 0x3: w16(c, n_lo, SUB16(v, imm)); return;
        case 0x4: v |= imm; nzv16(c, v); w16(c, n_lo, v); return;
        case 0x5: v ^= imm; nzv16(c, v); w16(c, n_lo, v); return;
        case 0x6: v &= imm; nzv16(c, v); w16(c, n_lo, v); return;
        }
        illegal(c, op);
        return;
    }
    case 0x7a: {                                                       /* long immediates */
        if (op & 8) { illegal(c, op); return; }
        uint32_t imm = (uint32_t)fetch(c) << 16;
        imm |= fetch(c);
        uint32_t v = R32(n_lo);
        switch (n_hi) {
        case 0x0: nzv32(c, imm); R32(n_lo) = imm; return;
        case 0x1: R32(n_lo) = ADD32(v, imm); return;
        case 0x2: (void)SUB32(v, imm); return;
        case 0x3: R32(n_lo) = SUB32(v, imm); return;
        case 0x4: v |= imm; nzv32(c, v); R32(n_lo) = v; return;
        case 0x5: v ^= imm; nzv32(c, v); R32(n_lo) = v; return;
        case 0x6: v &= imm; nzv32(c, v); R32(n_lo) = v; return;
        }
        illegal(c, op);
        return;
    }
    case 0x7b: {                                                       /* eepmov */
        uint16_t w = fetch(c);
        if (w != 0x598f || (lo != 0x5c && lo != 0xd4)) { illegal(c, op); return; }
        if (lo == 0x5c) {                                              /* eepmov.b: count R4L */
            while (r8(c, 4 + 8)) {
                wr8(c, R32(6), rd8(c, R32(5)));
                R32(5)++; R32(6)++;
                w8(c, 4 + 8, (uint8_t)(r8(c, 4 + 8) - 1));
            }
        } else {                                                       /* eepmov.w: count R4 */
            while (r16(c, 4)) {
                wr8(c, R32(6), rd8(c, R32(5)));
                R32(5)++; R32(6)++;
                w16(c, 4, (uint16_t)(r16(c, 4) - 1));
            }
        }
        return;
    }
    case 0x7c: case 0x7d: {                                            /* bit ops on @ERd */
        if (op & 0x8f) { illegal(c, op); return; }
        uint16_t w = fetch(c);
        if (w & 0x0f) { illegal(c, op); return; }
        bit_mem(c, R32(n_hi), w, hi == 0x7d);
        return;
    }
    case 0x7e: case 0x7f: {                                            /* bit ops on @aa:8 */
        uint16_t w = fetch(c);
        if (w & 0x0f) { illegal(c, op); return; }
        bit_mem(c, 0xffff00u | lo, w, hi == 0x7f);
        return;
    }

    default:                                                           /* 8x-fx #imm8,Rd */
        switch (hi >> 4) {
        case 0x8: w8(c, hi & 15, ADD8(r8(c, hi & 15), lo)); return;
        case 0x9: w8(c, hi & 15, (uint8_t)addsub(c, r8(c, hi & 15), lo, 8, false, c->ccr & F_C, true)); return;
        case 0xa: (void)SUB8(r8(c, hi & 15), lo); return;
        case 0xb: w8(c, hi & 15, (uint8_t)addsub(c, r8(c, hi & 15), lo, 8, true, c->ccr & F_C, true)); return;
        case 0xc: { uint8_t v = r8(c, hi & 15) | lo; nzv8(c, v); w8(c, hi & 15, v); return; }
        case 0xd: { uint8_t v = r8(c, hi & 15) ^ lo; nzv8(c, v); w8(c, hi & 15, v); return; }
        case 0xe: { uint8_t v = r8(c, hi & 15) & lo; nzv8(c, v); w8(c, hi & 15, v); return; }
        case 0xf: { nzv8(c, (uint8_t)lo); w8(c, hi & 15, (uint8_t)lo); return; }
        }
        illegal(c, op);
        return;
    }
}

int32_t h8s_run(h8s_cpu_t *c, int32_t states)
{
    c->slice = states;
    c->icount = states;
    while (c->icount > 0) {
        /* SLEEP sets irq_check too, so one test per instruction covers both. */
        if (UNLIKELY(c->irq_check) && LIKELY(!c->irq_hold)) {
            c->irq_check = false;
            int level = 0;
            int vec = c->bus.irq_ack ? c->bus.irq_ack(c->ctx, c, &level) : 0;
            if (vec) {
                take_interrupt(c, vec, level);
                continue;
            }
            if (c->sleeping) {
                /* Nothing to do until the machine raises an interrupt. */
                c->irq_check = true;
                c->icount = 0;
                break;
            }
        }
        c->irq_hold = false;
        execute(c);
    }
    int32_t done = c->slice - c->icount;
    c->cycles += (uint64_t)done;
    c->slice = c->icount = 0;
    return done;
}
