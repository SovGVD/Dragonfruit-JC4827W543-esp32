/* m68k_jit_backend_xtensa.c — 68k -> Xtensa (LX6) code-generation backend.
 *
 * The clean-room 68k->Xtensa basic-block translator: the Xtensa instruction
 * emitter (encodings verified via objdump) and the per-opcode codegen. Implements the
 * m68k_jit_backend.h interface (backend_codegen / backend_finalize). The neutral
 * driver, block cache, and executable-memory mechanics live in m68k_jit_core.c,
 * m68k_jit_cache.c, and jit_execmem_esp32.c respectively.
 *
 * Generated blocks use the windowed ABI (entry/retw.n) so they can be called
 * through a C function pointer. Registers live in the cpu struct (a2 = cpu).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "m68k.h"
#include "bus.h"
#include "m68k_jit_backend.h"
#include "jit_execmem.h"

/* The emitter exposes the full instruction set; v1 codegen uses only a subset. */
#pragma GCC diagnostic ignored "-Wunused-function"

/* ===================== Xtensa instruction emitter ======================
 * Emits into a byte-addressable scratch buffer (DRAM); the finished block is
 * word-copied into the executable cache afterwards (IRAM tolerates only 32-bit
 * stores). All encodings are little-endian instruction words. */

/* JIT_CODEBUF_BYTES / JIT_MAX_LITS / JIT_MAX_FIX are defined in m68k_jit_backend.h
 * (they size both this emitter's scratch struct and the transport backend_block_t). */
typedef struct {
    uint8_t *buf;
    int      pos;
    int      cap;
    bool     overflow;
    /* literal pool: 32-bit constants placed before the code; l32r references
     * are recorded as fixups and patched to real offsets at finalize time. */
    uint32_t lits[JIT_MAX_LITS];
    int      n_lits;
    struct { int pos; int lit; } fix[JIT_MAX_FIX];
    int      n_fix;
} jit_emit_t;

static void e_byte(jit_emit_t *e, uint8_t b)
{
    if (e->pos < e->cap) e->buf[e->pos] = b;
    else                 e->overflow = true;
    e->pos++;
}
static void e16(jit_emit_t *e, uint32_t v) { e_byte(e, v); e_byte(e, v >> 8); }
static void e24(jit_emit_t *e, uint32_t v) { e_byte(e, v); e_byte(e, v >> 8); e_byte(e, v >> 16); }

/* Windowed frame */
static void emit_entry(jit_emit_t *e)      { e24(e, 0x004136); }   /* entry a1, 32 */
static void emit_retw(jit_emit_t *e)       { e16(e, 0xf01d);   }   /* retw.n */

/* Loads/stores against a base register (offsets into the cpu struct etc.) */
static void emit_l32i_n(jit_emit_t *e, int at, int as, int off) { e16(e, (((off >> 2) & 0xF) << 12) | (as << 8) | (at << 4) | 0x8); }
static void emit_s32i_n(jit_emit_t *e, int at, int as, int off) { e16(e, (((off >> 2) & 0xF) << 12) | (as << 8) | (at << 4) | 0x9); }
static void emit_l32i  (jit_emit_t *e, int at, int as, int off) { e24(e, (((off >> 2) & 0xFF) << 16) | (2 << 12) | (as << 8) | (at << 4) | 2); }
static void emit_s32i  (jit_emit_t *e, int at, int as, int off) { e24(e, (((off >> 2) & 0xFF) << 16) | (6 << 12) | (as << 8) | (at << 4) | 2); }
static void emit_l8ui  (jit_emit_t *e, int at, int as, int off) { e24(e, ((off & 0xFF) << 16) | (0 << 12) | (as << 8) | (at << 4) | 2); }
static void emit_s8i   (jit_emit_t *e, int at, int as, int off) { e24(e, ((off & 0xFF) << 16) | (4 << 12) | (as << 8) | (at << 4) | 2); }
static void emit_l16ui (jit_emit_t *e, int at, int as, int off) { e24(e, (((off >> 1) & 0xFF) << 16) | (1 << 12) | (as << 8) | (at << 4) | 2); }  /* off scaled by 2 */
static void emit_s16i  (jit_emit_t *e, int at, int as, int off) { e24(e, (((off >> 1) & 0xFF) << 16) | (5 << 12) | (as << 8) | (at << 4) | 2); }  /* off scaled by 2 */

/* RRR ALU: r = s OP t */
static void emit_rrr(jit_emit_t *e, int op2, int r, int s, int t) { e24(e, (op2 << 20) | (r << 12) | (s << 8) | (t << 4)); }
static void emit_and(jit_emit_t *e, int r, int s, int t) { emit_rrr(e, 0x1, r, s, t); }
static void emit_or (jit_emit_t *e, int r, int s, int t) { emit_rrr(e, 0x2, r, s, t); }
static void emit_xor(jit_emit_t *e, int r, int s, int t) { emit_rrr(e, 0x3, r, s, t); }
static void emit_add(jit_emit_t *e, int r, int s, int t) { emit_rrr(e, 0x8, r, s, t); }
static void emit_sub(jit_emit_t *e, int r, int s, int t) { emit_rrr(e, 0xC, r, s, t); }

static void emit_mov(jit_emit_t *e, int at, int as) { e16(e, (as << 8) | (at << 4) | 0xD); }  /* mov.n */

/* Immediates */
static void emit_addi_n(jit_emit_t *e, int ar, int as, int imm) {       /* imm -1 or 1..15 (addi.n can't encode 0: imm4=0 means -1) */
    int imm4 = (imm == -1) ? 0 : (imm & 0xF);
    e16(e, (ar << 12) | (as << 8) | (imm4 << 4) | 0xB);
}
static void emit_addi (jit_emit_t *e, int at, int as, int imm8) { e24(e, ((imm8 & 0xFF) << 16) | (0xC << 12) | (as << 8) | (at << 4) | 2); }
static void emit_movi (jit_emit_t *e, int at, int imm12) {              /* signed -2048..2047 */
    e24(e, ((imm12 & 0xFF) << 16) | (0xA << 12) | (((imm12 >> 8) & 0xF) << 8) | (at << 4) | 2);
}

/* Extract / shift (zero-extend for byte/word, sign bit for N flag) */
static void emit_extui(jit_emit_t *e, int ar, int as, int shift, int nbits) {
    int op1 = 0x4 | (shift >> 4);
    e24(e, ((nbits - 1) << 20) | (op1 << 16) | (ar << 12) | ((shift & 0xF) << 8) | (as << 4));
}
static void emit_slli(jit_emit_t *e, int ar, int as, int n) {           /* n in 1..31 */
    int sa = 32 - n;
    e24(e, (((sa >> 4) & 1) << 20) | (0x1 << 16) | (ar << 12) | (as << 8) | ((sa & 0xF) << 4));
}

/* l32r at, lit — load 32-bit const from a literal placed BEFORE this insn
 * (4-byte aligned, <=256KB back). lit_addr and the address of this l32r are
 * absolute addresses in the final executable buffer. */
static void emit_l32r(jit_emit_t *e, int at, uint32_t l32r_addr, uint32_t lit_addr) {
    int32_t off = (int32_t)(lit_addr - ((l32r_addr + 3) & ~3u));
    uint32_t imm16 = (off >> 2) & 0xFFFF;
    e24(e, (imm16 << 8) | (at << 4) | 1);
}

/* Conditional branches. off is the signed displacement = target - (insn + 4),
 * computed by the caller from buffer offsets. */
static void emit_beqz(jit_emit_t *e, int as, int off) { e24(e, ((off & 0xFFF) << 12) | (as << 8) | 0x16); }
static void emit_bnez(jit_emit_t *e, int as, int off) { e24(e, ((off & 0xFFF) << 12) | (as << 8) | 0x56); }
static void emit_beq (jit_emit_t *e, int s, int t, int off8) { e24(e, ((off8 & 0xFF) << 16) | (0x1 << 12) | (s << 8) | (t << 4) | 7); }
static void emit_bne (jit_emit_t *e, int s, int t, int off8) { e24(e, ((off8 & 0xFF) << 16) | (0x9 << 12) | (s << 8) | (t << 4) | 7); }

static void emit_callx8(jit_emit_t *e, int as) { e24(e, (as << 8) | 0xE0); }

/* Forward-branch patching: emit the branch with offset 0, remember its position,
 * then patch the displacement to the current emit position once the target is
 * known. target = branch_pos + 4 + off, so off = e->pos - branch_pos - 4. The
 * displacement sits in different bits for the two branch shapes:
 *  - beqz/bnez: 12-bit field at word bits 12..23 (buf[+1] hi nibble, buf[+2]).
 *  - beq/bne:    8-bit field at word bits 16..23 (buf[+2]). */
static void patch_b12(jit_emit_t *e, int pos)
{
    if (pos + 2 >= e->cap) { e->overflow = true; return; }  /* unpatchable -> drop the block */
    int off = e->pos - pos - 4;
    e->buf[pos + 1] = (e->buf[pos + 1] & 0x0F) | ((off & 0xF) << 4);
    e->buf[pos + 2] = (off >> 4) & 0xFF;
}
static void patch_b8(jit_emit_t *e, int pos)
{
    if (pos + 2 >= e->cap) { e->overflow = true; return; }  /* unpatchable -> drop the block */
    int off = e->pos - pos - 4;
    e->buf[pos + 2] = off & 0xFF;
}

/* Load a 32-bit constant into `at` from the block's literal pool (resolved at
 * finalize time). Records a fixup at the placeholder l32r's position. */
static void emit_l32r_lit(jit_emit_t *e, int at, uint32_t value)
{
    /* Once overflowed, the block will be discarded by the caller (finalize checks
     * e->overflow / pass 2 rewinds). Stop emitting so we never leave an unpatched
     * l32r placeholder (which would otherwise load a garbage literal). */
    if (e->overflow) return;
    int idx = -1;
    for (int i = 0; i < e->n_lits; i++) if (e->lits[i] == value) { idx = i; break; }
    if (idx < 0) {
        if (e->n_lits >= JIT_MAX_LITS) { e->overflow = true; return; }
        idx = e->n_lits; e->lits[e->n_lits++] = value;
    }
    if (e->n_fix >= JIT_MAX_FIX) { e->overflow = true; return; }
    e->fix[e->n_fix].pos = e->pos; e->fix[e->n_fix].lit = idx; e->n_fix++;
    e24(e, (at << 4) | 1);   /* l32r placeholder (imm16 patched later) */
}

/* Load an arbitrary 32-bit immediate: movi when it fits its signed 12-bit
 * field, else from the literal pool. */
static void emit_imm(jit_emit_t *e, int at, uint32_t value)
{
    int32_t s = (int32_t)value;
    if (s >= -2048 && s <= 2047) emit_movi(e, at, s);
    else                         emit_l32r_lit(e, at, value);
}

/* ======================= 68k -> Xtensa translator =====================
 *
 * Native codegen for the common ops; the dispatcher single-steps the
 * interpreter (m68k_jit_fallback) for everything else. A block is a run of
 * natively-supported ops; it ends at the first unsupported op (often a branch),
 * which the dispatcher interprets. Registers live in the cpu struct (a2 = cpu).
 *
 * Reg allocation in a block (windowed, after `entry a1,32`): a2=cpu, a3=value,
 * a4=EA address (a2-a7 preserved across call8); a8=call target, a9=temp,
 * a10/a11=call args/result (a8-a15 clobbered by calls). Memory access calls the
 * non-inline jit_bus_* wrappers. Blocks return a compile-time cycle total; the
 * dispatcher advances pc by the block byte length. Native ops here touch no
 * peripherals and no control flow, so per-block IRQ servicing is exact.
 *
 * v2 op set: MOVE.L (src modes 0-5,7.0,7.1,7.4 / dst 0-5,7.0,7.1, incl memory),
 * AND/OR/EOR.L Dn,Dn, MOVEQ, TST.L Dn. N/Z flags; byte/word + arithmetic next.
 */

#define OFF_D(n) ((int)(offsetof(m68k_cpu_t, regs) + 4 * (n)))
#define OFF_A(n) ((int)(offsetof(m68k_cpu_t, regs) + offsetof(m68k_regs_t, a) + 4 * (n)))
#define OFF_SR   ((int)(offsetof(m68k_cpu_t, regs) + offsetof(m68k_regs_t, sr)))
#define OFF_PC   ((int)(offsetof(m68k_cpu_t, regs) + offsetof(m68k_regs_t, pc)))
#define OFF_STOPPED ((int)(offsetof(m68k_cpu_t, regs) + offsetof(m68k_regs_t, stopped)))
#define A_CPU 2

/* Dn/An are accessed via the narrow l32i.n/s32i.n forms, whose (off>>2) field is
 * only 4 bits -> max byte offset 60. OFF_A(7) sits exactly at that limit today; a
 * future m68k_regs_t reorder that pushed it past 60 would silently miscompile every
 * stack op. Fail the build instead. */
_Static_assert(OFF_A(7) <= 60 && OFF_D(7) <= 60,
    "Dn/An struct offsets exceed the l32i.n/s32i.n 4-bit (off>>2) field");

extern uint32_t jit_bus_read8(uint32_t),  jit_bus_read16(uint32_t),  jit_bus_read32(uint32_t);
extern void     jit_bus_write8(uint32_t, uint32_t), jit_bus_write16(uint32_t, uint32_t), jit_bus_write32(uint32_t, uint32_t);

/* The interpreter's eager-flag helpers — reused by arithmetic blocks so the
 * tricky V/C/X computation stays in one verified place. They mask src/dst/result
 * to size and recompute carry, so passing full 32-bit register values is safe. */
extern void m68k_set_flags_add(m68k_cpu_t *, uint32_t src, uint32_t dst, uint32_t result, int size);
extern void m68k_set_flags_sub(m68k_cpu_t *, uint32_t src, uint32_t dst, uint32_t result, int size);
extern void m68k_set_flags_cmp(m68k_cpu_t *, uint32_t src, uint32_t dst, uint32_t result, int size);
extern void m68k_jit_set_sr(m68k_cpu_t *, uint32_t value);   /* full SR write + SP swap */
extern void m68k_group_e(m68k_cpu_t *, uint16_t opcode);     /* interpreter shift/rotate */

/* Dead-flag elimination state. Most computed condition codes are overwritten by
 * a later flag-setting op before any branch reads them — the interpreter has to
 * set them eagerly, but the JIT can skip the dead ones (a real win: it elides
 * the inline flag code AND the arithmetic flag call8s). jit_emit_op classifies
 * each op into g_op_{reads,wr_nzvc,wr_x}; jit_compile runs a backward liveness
 * pass and sets g_suppress_flags before re-emitting each op. The flag-emit
 * helpers honor g_suppress_flags. NZVC and X are tracked separately (X is only
 * written by arithmetic and only consumed by ADDX-class ops, so it must always
 * be materialised at block exit). */
static bool g_suppress_flags;
static bool g_op_reads, g_op_wr_nzvc, g_op_wr_x, g_op_reads_x;

/* SR = (SR & ~0xF) | N | Z from the long result in rr (N=bit31, Z, V=C=0). */
static void emit_flags_nz(jit_emit_t *e, int rr)
{
    g_op_wr_nzvc = true;
    if (g_suppress_flags) return;
    emit_l16ui(e, 8, A_CPU, OFF_SR);
    emit_movi (e, 9, -16);
    emit_and  (e, 8, 8, 9);
    emit_extui(e, 9, rr, 31, 1);
    emit_slli (e, 9, 9, 3);
    emit_or   (e, 8, 8, 9);
    emit_bnez (e, rr, 1);
    emit_addi_n(e, 8, 8, 4);
    emit_s16i (e, 8, A_CPU, OFF_SR);
}

/* SR = (SR & ~0xF) | N | Z from a sized result in rr (low `nbits` significant;
 * V=C=0). size 0=byte(8),1=word(16),2=long(32). */
static void emit_flags_nz_sized(jit_emit_t *e, int rr, int size)
{
    g_op_wr_nzvc = true;
    if (g_suppress_flags) return;
    int nbits = (size == 0) ? 8 : (size == 1) ? 16 : 32;
    emit_l16ui(e, 8, A_CPU, OFF_SR);
    emit_movi (e, 9, -16);
    emit_and  (e, 8, 8, 9);                 /* clear NZVC */
    emit_extui(e, 9, rr, nbits - 1, 1);     /* N = sign bit */
    emit_slli (e, 9, 9, 3);
    emit_or   (e, 8, 8, 9);
    if (size == 2) {
        emit_bnez (e, rr, 1);               /* nonzero -> skip set-Z */
    } else {
        emit_extui(e, 9, rr, 0, nbits);     /* a9 = rr & size-mask */
        emit_bnez (e, 9, 1);
    }
    emit_addi_n(e, 8, 8, 4);                /* Z = bit2 */
    emit_s16i (e, 8, A_CPU, OFF_SR);
}

/* SR = (SR & ~0xF) | Z  (CLR result is always zero: N=V=C=0, Z=1). */
static void emit_flags_clr(jit_emit_t *e)
{
    g_op_wr_nzvc = true;
    if (g_suppress_flags) return;
    emit_l16ui (e, 8, A_CPU, OFF_SR);
    emit_movi  (e, 9, -16);
    emit_and   (e, 8, 8, 9);
    emit_addi_n(e, 8, 8, 4);
    emit_s16i  (e, 8, A_CPU, OFF_SR);
}

/* Set Z = (r == 0), leaving N/V/C/X untouched (the bit ops affect only Z). Not a
 * full NZVC writer, so it never sets g_op_wr_nzvc (that would wrongly let the dead-
 * flag pass treat N/V/C as overwritten) and always emits. Clobbers a8,a9. */
static void emit_set_z(jit_emit_t *e, int r)
{
    emit_l16ui (e, 8, A_CPU, OFF_SR);
    emit_movi  (e, 9, ~SR_Z);           /* -5: clear only the Z bit */
    emit_and   (e, 8, 8, 9);
    emit_bnez  (e, r, 1);               /* r != 0 -> leave Z clear */
    emit_addi_n(e, 8, 8, SR_Z);         /* r == 0 -> Z = 1 */
    emit_s16i  (e, 8, A_CPU, OFF_SR);
}

/* ---- Block-local register allocation (Dn pinned into preserved Xtensa regs) ----
 * Each native block re-loads up to JIT_NPIN of its most-referenced data registers
 * into call-preserved Xtensa regs (a6,a7 — untouched by the bus/flag call8s) at
 * entry and spills the dirty ones at exit, so read-modify-write Dn (loop counters,
 * accumulators) stay in-register instead of hitting the cpu struct each op. The
 * compile is two-pass already (for dead-flag liveness); pass 1 runs with
 * g_jit_counting set and tallies Dn references through the same load/store choke
 * points, jit_compile picks the winners, and pass 2 consults the pin map. An regs
 * and memory EAs are untouched (still cpu-struct backed) — keeping this Dn-only
 * keeps the increment small and the EA/scratch (a3,a4,a5) conventions intact. */
#define JIT_NPIN  2
#define PIN_MIN   2          /* min references before a Dn is worth pinning */
static const int PIN_XREG[JIT_NPIN] = { 6, 7 };
static int  g_pin_dn[JIT_NPIN];     /* Dn (0-7) held in each slot, -1 = unused */
static bool g_pin_dirty[JIT_NPIN];
static int  g_dn_count[8];
static bool g_jit_counting;

static int pin_xreg_for_dn(int n)
{
    if (g_jit_counting) return -1;      /* pass 1: everything stays memory-backed */
    for (int s = 0; s < JIT_NPIN; s++) if (g_pin_dn[s] == n) return PIN_XREG[s];
    return -1;
}
static void pin_mark_dirty(int n)
{
    for (int s = 0; s < JIT_NPIN; s++) if (g_pin_dn[s] == n) { g_pin_dirty[s] = true; return; }
}

/* Coherence around a call into an interpreter helper that reads/writes
 * cpu->regs.d[] directly: spill dirty pinned Dn beforehand so the helper sees
 * current values, then reload them afterwards (the helper may have written one).
 * After the reload the pinned regs match memory, so they are marked clean. */
static void emit_pin_writeback(jit_emit_t *e)
{
    for (int s = 0; s < JIT_NPIN; s++)
        if (g_pin_dn[s] >= 0 && g_pin_dirty[s])
            emit_s32i_n(e, PIN_XREG[s], A_CPU, OFF_D(g_pin_dn[s]));
}
static void emit_pin_reload(jit_emit_t *e)
{
    for (int s = 0; s < JIT_NPIN; s++)
        if (g_pin_dn[s] >= 0) {
            emit_l32i_n(e, PIN_XREG[s], A_CPU, OFF_D(g_pin_dn[s]));
            g_pin_dirty[s] = false;
        }
}

/* Load Dn into Xtensa xreg: mov from its pinned slot if resident, else from the
 * cpu struct. In the counting pass it always loads from memory and tallies. */
static void emit_load_dn(jit_emit_t *e, int xreg, int n)
{
    if (g_jit_counting) g_dn_count[n]++;
    int p = pin_xreg_for_dn(n);
    if (p >= 0) { if (xreg != p) emit_mov(e, xreg, p); }
    else        emit_l32i_n(e, xreg, A_CPU, OFF_D(n));
}

/* Store the low `size` bytes of val_reg into Dn, preserving the upper bits for
 * byte/word (68k sub-register writes are merges). If Dn is pinned the merge is
 * done in-register and the slot marked dirty; else it goes to the cpu struct.
 * Clobbers a8,a9 (memory path) or a9 (pinned path). */
static void emit_write_dn_sized(jit_emit_t *e, int reg, int val_reg, int size)
{
    if (g_jit_counting) g_dn_count[reg]++;
    int p = pin_xreg_for_dn(reg);
    if (p >= 0) {
        if (size == 2) { if (val_reg != p) emit_mov(e, p, val_reg); }
        else {
            int nbits = (size == 0) ? 8 : 16;
            uint32_t keep = (size == 0) ? 0xFFFFFF00u : 0xFFFF0000u;
            emit_imm  (e, 9, keep);
            emit_and  (e, p, p, 9);             /* clear the low byte/word in place */
            emit_extui(e, 9, val_reg, 0, nbits);
            emit_or   (e, p, p, 9);
        }
        pin_mark_dirty(reg);
        return;
    }
    if (size == 2) { emit_s32i_n(e, val_reg, A_CPU, OFF_D(reg)); return; }
    int nbits = (size == 0) ? 8 : 16;
    uint32_t keep = (size == 0) ? 0xFFFFFF00u : 0xFFFF0000u;
    emit_l32i_n(e, 8, A_CPU, OFF_D(reg));
    emit_imm   (e, 9, keep);
    emit_and   (e, 8, 8, 9);                /* old & ~mask */
    emit_extui (e, 9, val_reg, 0, nbits);   /* val & mask */
    emit_or    (e, 8, 8, 9);
    emit_s32i_n(e, 8, A_CPU, OFF_D(reg));
}

/* post-inc / pre-dec stride: size bytes, but byte access through (A7) bumps by
 * 2 to keep the stack pointer even. */
static int ea_inc_sz(int reg, int size)
{
    int n = (size == 0) ? 1 : (size == 1) ? 2 : 4;
    if (size == 0 && reg == 7) n = 2;
    return n;
}

/* Sign-extend the low 16 bits of reg r to 32 bits, in place, via the identity
 * sext16(x) = (x ^ 0x8000) - 0x8000 — avoids needing an arithmetic-shift-right
 * immediate (whose Xtensa encoding isn't in our verified set). Clobbers a8. */
static void emit_sext16(jit_emit_t *e, int r)
{
    emit_extui(e, r, r, 0, 16);     /* x = r & 0xFFFF */
    emit_imm  (e, 8, 0x8000);
    emit_xor  (e, r, r, 8);
    emit_sub  (e, r, r, 8);
}

/* Compute a memory EA address into ar. ext = disp16 (sign-extended) for mode 5,
 * the brief index extension word for mode 6, or the absolute address for 7.0/7.1.
 * Clobbers a8,a9. */
static void emit_ea_addr(jit_emit_t *e, int ar, int mode, int reg, uint32_t ext, int size)
{
    switch (mode) {
    case 2:
        emit_l32i_n(e, ar, A_CPU, OFF_A(reg));
        break;
    case 3:
        emit_l32i_n(e, ar, A_CPU, OFF_A(reg));
        emit_addi(e, 9, ar, ea_inc_sz(reg, size));
        emit_s32i_n(e, 9, A_CPU, OFF_A(reg));
        break;
    case 4:
        emit_l32i_n(e, 9, A_CPU, OFF_A(reg));
        emit_addi(e, 9, 9, -ea_inc_sz(reg, size));
        emit_s32i_n(e, 9, A_CPU, OFF_A(reg));
        emit_mov(e, ar, 9);
        break;
    case 5:
        emit_l32i_n(e, ar, A_CPU, OFF_A(reg));
        emit_imm(e, 9, ext);
        emit_add(e, ar, ar, 9);
        break;
    case 6: {  /* (d8,An,Xn) brief: An + sext8(disp) + (Xn.W sext | Xn.L), scale 1 */
        uint16_t ew = (uint16_t)ext;
        int ix = (ew >> 12) & 7;
        bool ix_addr = (ew >> 15) & 1, ix_long = (ew >> 11) & 1;
        int disp = (int8_t)(ew & 0xFF);
        emit_l32i_n(e, ar, A_CPU, OFF_A(reg));
        if (disp) emit_addi(e, ar, ar, disp);
        if (ix_addr) emit_l32i_n(e, 9, A_CPU, OFF_A(ix));
        else         emit_load_dn(e, 9, ix);        /* pin-aware Dn index */
        if (!ix_long) emit_sext16(e, 9);
        emit_add(e, ar, ar, 9);
        break;
    }
    default:   /* 7.0 abs.W (ext sign-extended) / 7.1 abs.L */
        emit_imm(e, ar, ext);
        break;
    }
}

/* Inlined bus access: the emulated 2 MB RAM is a fixed big-endian byte array
 * (g_bus_ram, set once at boot). The common case — addr < 2 MB — is assembled
 * inline (no call8/window-rotate); ROM/peripheral/wrap addresses fall back to the
 * out-of-line jit_bus_* wrappers (which mask to 24 bits). The RAM test is
 * `(addr >> 21) != 0` via one extui (11 bits — within extui's 16-bit limit; a
 * 24-bit mask is NOT expressible in extui) + bnez. When the test passes, the raw
 * addr is already < 2 MB so it indexes RAM directly with no mask. The fast path
 * skips the slow path via an always-taken `bnez a1` (sp is never 0); both forward
 * branches patch with patch_b12. addr_reg (a4) and val_reg are left intact for the
 * slow call / RMW reuse; a8..a11 are call-clobbered scratch (val captured into a11
 * first so val_reg may itself be a8). */

/* The inlined RAM test below hardcodes the 2 MB bound as `addr >> 21` (extui shift 21),
 * which equals log2(BUS_RAM_SIZE) only by coincidence. Pin them together so a change to
 * BUS_RAM_SIZE (the bus.h comment anticipates a 4 MB ESP32-P4 target) fails the build here
 * instead of silently splitting the interpreter/JIT view of the memory map. If this fires,
 * update the extui shift in emit_mem_read_sized AND emit_mem_write_sized to log2(new size). */
_Static_assert(BUS_RAM_SIZE == (1u << 21),
               "JIT RAM fast-path uses extui shift 21 (2 MB); update emit_mem_*_sized");

static void emit_mem_read_sized(jit_emit_t *e, int dst, int addr_reg, int size)
{
    void *fn = (size == 0) ? (void *)&jit_bus_read8
             : (size == 1) ? (void *)&jit_bus_read16 : (void *)&jit_bus_read32;
    uint32_t rambase = (uint32_t)(uintptr_t)g_bus_ram;
    emit_extui(e, 9, addr_reg, 21, 11);         /* bits 21..31: nonzero iff addr >= 2 MB */
    int b_slow = e->pos; emit_bnez(e, 9, 0);    /* -> slow (ROM/periph/wrap) */
    emit_imm(e, 9, rambase);
    emit_add(e, 9, 9, addr_reg);                /* a9 = &ram[addr] (addr < 2 MB) */
    if (size == 0) {
        emit_l8ui(e, dst, 9, 0);
    } else if (size == 1) {
        emit_l8ui(e, 10, 9, 0); emit_slli(e, 10, 10, 8);
        emit_l8ui(e, 11, 9, 1); emit_or(e, dst, 10, 11);
    } else {
        emit_l8ui(e, 10, 9, 0); emit_slli(e, 10, 10, 24);
        emit_l8ui(e, 11, 9, 1); emit_slli(e, 11, 11, 16); emit_or(e, 10, 10, 11);
        emit_l8ui(e, 11, 9, 2); emit_slli(e, 11, 11, 8);  emit_or(e, 10, 10, 11);
        emit_l8ui(e, 11, 9, 3); emit_or(e, dst, 10, 11);
    }
    int b_done = e->pos; emit_bnez(e, 1, 0);    /* always-taken: skip slow path */
    patch_b12(e, b_slow);
    emit_mov(e, 10, addr_reg);                  /* slow: arg1 = raw addr */
    emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)fn);
    emit_callx8(e, 8);
    emit_mov(e, dst, 10);
    patch_b12(e, b_done);
}
static void emit_mem_write_sized(jit_emit_t *e, int addr_reg, int val_reg, int size)
{
    void *fn = (size == 0) ? (void *)&jit_bus_write8
             : (size == 1) ? (void *)&jit_bus_write16 : (void *)&jit_bus_write32;
    uint32_t rambase = (uint32_t)(uintptr_t)g_bus_ram;
    emit_mov  (e, 11, val_reg);                 /* a11 = value (also slow arg2) */
    emit_extui(e, 9, addr_reg, 21, 11);
    int b_slow = e->pos; emit_bnez(e, 9, 0);
    emit_imm(e, 9, rambase);
    emit_add(e, 9, 9, addr_reg);                /* a9 = &ram[addr] */
    if (size == 0) {
        emit_s8i(e, 11, 9, 0);
    } else if (size == 1) {
        emit_extui(e, 8, 11, 8, 8); emit_s8i(e, 8, 9, 0);
        emit_s8i(e, 11, 9, 1);
    } else {
        emit_extui(e, 8, 11, 24, 8); emit_s8i(e, 8, 9, 0);
        emit_extui(e, 8, 11, 16, 8); emit_s8i(e, 8, 9, 1);
        emit_extui(e, 8, 11, 8, 8);  emit_s8i(e, 8, 9, 2);
        emit_s8i(e, 11, 9, 3);
    }
    int b_done = e->pos; emit_bnez(e, 1, 0);
    patch_b12(e, b_slow);
    emit_mov(e, 10, addr_reg);                  /* slow: arg1 = raw addr (a11 = val) */
    emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)fn);
    emit_callx8(e, 8);
    patch_b12(e, b_done);
}

/* MOVE source EA -> a3 (zero-extended to the size). Returns ext words consumed,
 * or -1 if unsupported. */
static int emit_move_src(jit_emit_t *e, int mode, int reg, uint32_t pc, int size)
{
    switch (mode) {
    case 0: emit_load_dn(e, 3, reg); return 0;
    case 1: emit_l32i_n(e, 3, A_CPU, OFF_A(reg)); return 0;   /* word/long only */
    case 2: emit_ea_addr(e, 4, 2, reg, 0, size); emit_mem_read_sized(e, 3, 4, size); return 0;
    case 3: emit_ea_addr(e, 4, 3, reg, 0, size); emit_mem_read_sized(e, 3, 4, size); return 0;
    case 4: emit_ea_addr(e, 4, 4, reg, 0, size); emit_mem_read_sized(e, 3, 4, size); return 0;
    case 5: { uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
              emit_ea_addr(e, 4, 5, reg, d, size); emit_mem_read_sized(e, 3, 4, size); return 1; }
    case 6: { uint16_t ew = bus_read16((pc + 2) & BUS_ADDR_MASK);
              emit_ea_addr(e, 4, 6, reg, ew, size); emit_mem_read_sized(e, 3, 4, size); return 1; }
    case 7:
        if (reg == 0) { uint32_t a = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
                        emit_ea_addr(e, 4, 7, 0, a, size); emit_mem_read_sized(e, 3, 4, size); return 1; }
        if (reg == 1) { uint32_t a = bus_read32((pc + 2) & BUS_ADDR_MASK);
                        emit_ea_addr(e, 4, 7, 1, a, size); emit_mem_read_sized(e, 3, 4, size); return 2; }
        if (reg == 4) {   /* immediate: byte/word in one ext word, long in two */
            if (size == 2) { emit_imm(e, 3, bus_read32((pc + 2) & BUS_ADDR_MASK)); return 2; }
            uint32_t w = bus_read16((pc + 2) & BUS_ADDR_MASK);
            emit_imm(e, 3, (size == 0) ? (w & 0xFF) : (w & 0xFFFF)); return 1;
        }
        return -1;
    }
    return -1;
}

/* MOVE a3 -> dest EA (sized). Returns ext words consumed, or -1. */
static int emit_move_dst(jit_emit_t *e, int mode, int reg, uint32_t pc, int srcext, int size)
{
    int o = 2 + srcext * 2;
    switch (mode) {
    case 0: emit_write_dn_sized(e, reg, 3, size); return 0;
    case 1: if (size == 1) emit_sext16(e, 3);   /* MOVEA.W sign-extends source to 32 */
            emit_s32i_n(e, 3, A_CPU, OFF_A(reg)); return 0;
    case 2: emit_ea_addr(e, 4, 2, reg, 0, size); emit_mem_write_sized(e, 4, 3, size); return 0;
    case 3: emit_ea_addr(e, 4, 3, reg, 0, size); emit_mem_write_sized(e, 4, 3, size); return 0;
    case 4: emit_ea_addr(e, 4, 4, reg, 0, size); emit_mem_write_sized(e, 4, 3, size); return 0;
    case 5: { uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + o) & BUS_ADDR_MASK);
              emit_ea_addr(e, 4, 5, reg, d, size); emit_mem_write_sized(e, 4, 3, size); return 1; }
    case 6: { uint16_t ew = bus_read16((pc + o) & BUS_ADDR_MASK);
              emit_ea_addr(e, 4, 6, reg, ew, size); emit_mem_write_sized(e, 4, 3, size); return 1; }
    case 7:
        if (reg == 0) { uint32_t a = (uint32_t)(int32_t)(int16_t)bus_read16((pc + o) & BUS_ADDR_MASK);
                        emit_ea_addr(e, 4, 7, 0, a, size); emit_mem_write_sized(e, 4, 3, size); return 1; }
        if (reg == 1) { uint32_t a = bus_read32((pc + o) & BUS_ADDR_MASK);
                        emit_ea_addr(e, 4, 7, 1, a, size); emit_mem_write_sized(e, 4, 3, size); return 2; }
        return -1;
    }
    return -1;
}

static bool ea_is_mem(int mode, int reg) { return mode >= 2 && !(mode == 7 && reg == 4); }

/* Evaluate 68k condition cc (2..15) into a9 = 1 (true/taken) or 0. Loads SR into
 * a8 first. Uses a8,a9,a10,a11 (block-enders; no live values needed after). The
 * flag bits in SR low byte: C=0, V=1, Z=2, N=3 (XNZVC layout). */
static void emit_eval_cc(jit_emit_t *e, int cc)
{
    emit_l16ui(e, 8, A_CPU, OFF_SR);
    switch (cc) {
    case 2:  /* HI: !c & !z */
        emit_extui(e, 9, 8, 0, 1); emit_extui(e, 10, 8, 2, 1); emit_or(e, 9, 9, 10);
        emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11); break;
    case 3:  /* LS: c | z */
        emit_extui(e, 9, 8, 0, 1); emit_extui(e, 10, 8, 2, 1); emit_or(e, 9, 9, 10); break;
    case 4:  /* CC/HS: !c */
        emit_extui(e, 9, 8, 0, 1); emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11); break;
    case 5:  /* CS/LO: c */
        emit_extui(e, 9, 8, 0, 1); break;
    case 6:  /* NE: !z */
        emit_extui(e, 9, 8, 2, 1); emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11); break;
    case 7:  /* EQ: z */
        emit_extui(e, 9, 8, 2, 1); break;
    case 8:  /* VC: !v */
        emit_extui(e, 9, 8, 1, 1); emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11); break;
    case 9:  /* VS: v */
        emit_extui(e, 9, 8, 1, 1); break;
    case 10: /* PL: !n */
        emit_extui(e, 9, 8, 3, 1); emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11); break;
    case 11: /* MI: n */
        emit_extui(e, 9, 8, 3, 1); break;
    case 12: /* GE: n == v */
        emit_extui(e, 9, 8, 3, 1); emit_extui(e, 10, 8, 1, 1); emit_xor(e, 9, 9, 10);
        emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11); break;
    case 13: /* LT: n != v */
        emit_extui(e, 9, 8, 3, 1); emit_extui(e, 10, 8, 1, 1); emit_xor(e, 9, 9, 10); break;
    case 14: /* GT: !z & (n == v) */
        emit_extui(e, 9, 8, 3, 1); emit_extui(e, 10, 8, 1, 1); emit_xor(e, 9, 9, 10);
        emit_movi(e, 11, 1); emit_xor(e, 9, 9, 11);          /* a9 = (n==v) */
        emit_extui(e, 10, 8, 2, 1); emit_xor(e, 10, 10, 11); /* a10 = !z */
        emit_and(e, 9, 9, 10); break;
    case 15: /* LE: z | (n != v) */
        emit_extui(e, 9, 8, 3, 1); emit_extui(e, 10, 8, 1, 1); emit_xor(e, 9, 9, 10);
        emit_extui(e, 10, 8, 2, 1); emit_or(e, 9, 9, 10); break;
    default: emit_movi(e, 9, 1); break;
    }
}

/* Call a m68k_set_flags_* helper: fn(cpu, src, dst, result, size). Windowed
 * call8 places args in a10..a14 (the callee sees them as a2..a6). src/dst/result
 * must be in a2..a7 (preserved across the call); we use a3/a4/a5. */
static void emit_call_flags(jit_emit_t *e, void *fn, int src_r, int dst_r, int result_r, int size)
{
    g_op_wr_nzvc = true;   /* g_op_wr_x is set by the caller (add/sub vs cmp) */
    if (g_suppress_flags) return;
    emit_mov (e, 10, A_CPU);
    emit_mov (e, 11, src_r);
    emit_mov (e, 12, dst_r);
    emit_mov (e, 13, result_r);
    emit_movi(e, 14, size);
    emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)fn);
    emit_callx8(e, 8);
}

/* Emit native code for the op at pc. Returns length in words (sets *cycles),
 * or 0 if unsupported. *ends_block is set true for control-flow ops that write
 * cpu->regs.pc themselves (the block must stop after them). */
static int jit_emit_op(jit_emit_t *e, uint32_t pc, int *cycles, bool *ends_block)
{
    uint16_t op = bus_read16(pc & BUS_ADDR_MASK);
    int top = op >> 12;
    *ends_block = false;
    g_op_reads = g_op_wr_nzvc = g_op_wr_x = g_op_reads_x = false;

    /* MOVE.B/.W/.L : groups 1/3/2 (top nibble 1=byte, 3=word, 2=long). */
    if (top == 1 || top == 2 || top == 3) {
        int size = (top == 1) ? 0 : (top == 2) ? 2 : 1;
        int sm = (op >> 3) & 7, sr = op & 7;
        int dm = (op >> 6) & 7, dr = (op >> 9) & 7;
        if (size == 0 && sm == 1)  return 0;   /* byte An source illegal */
        if (dm == 1 && size == 0)  return 0;   /* MOVEA.B doesn't exist */
        if (dm == 7 && dr > 1)     return 0;   /* dest abs only 7.0/7.1 */
        int sext = emit_move_src(e, sm, sr, pc, size);
        if (sext < 0) return 0;
        int dext = emit_move_dst(e, dm, dr, pc, sext, size);
        if (dext < 0) return 0;
        if (dm != 1) emit_flags_nz_sized(e, 3, size);
        int c = (dm == 1) ? 4
              : (ea_is_mem(dm, dr) ? 8 : 4) + (ea_is_mem(sm, sr) ? 4 : 0) + 4;
        *cycles = c;
        return 1 + sext + dext;
    }
    /* MOVEQ #imm,Dn */
    if ((op & 0xF100) == 0x7000) {
        emit_movi(e, 3, (int8_t)(op & 0xFF));
        emit_write_dn_sized(e, (op >> 9) & 7, 3, SIZE_LONG);
        emit_flags_nz(e, 3);
        *cycles = 4; return 1;
    }
    /* AND/OR.{B,W,L} <ea>,Dn  (data direction, opmode 0ss) — any readable EA. */
    if (((op & 0xF000) == 0xC000 || (op & 0xF000) == 0x8000) && ((op >> 6) & 7) <= 2) {
        bool isand = (op & 0xF000) == 0xC000;
        int size = (op >> 6) & 3, dn = (op >> 9) & 7, sm = (op >> 3) & 7, sr = op & 7;
        if (sm == 1) return 0;                       /* An not a valid AND/OR source */
        int sext = emit_move_src(e, sm, sr, pc, size);   /* src -> a3 */
        if (sext < 0) return 0;
        emit_mov(e, 5, 3);                           /* a5 = src */
        emit_load_dn(e, 4, dn);                      /* a4 = Dn */
        if (isand) emit_and(e, 3, 4, 5); else emit_or(e, 3, 4, 5);
        emit_write_dn_sized(e, dn, 3, size);
        emit_flags_nz_sized(e, 3, size);
        *cycles = (size == 2 ? 6 : 4) + (ea_is_mem(sm, sr) ? 4 : 0);
        return 1 + sext;
    }
    /* EOR.{B,W,L} Dn,Dn  (register direct; opmode 1ss, dst mode 0). */
    if ((op & 0xF100) == 0xB100 && ((op >> 6) & 3) != 3 && ((op >> 3) & 7) == 0) {
        int size = (op >> 6) & 3, sd = (op >> 9) & 7, dr = op & 7;
        emit_load_dn(e, 5, sd);                      /* a5 = src Dn */
        emit_load_dn(e, 4, dr);                      /* a4 = dst Dn */
        emit_xor(e, 3, 4, 5);
        emit_write_dn_sized(e, dr, 3, size);
        emit_flags_nz_sized(e, 3, size);
        *cycles = (size == 2) ? 8 : 4; return 1;
    }
    /* Shifts/rotates (group E), register form only (size != 3). Delegated to the
     * verified interpreter handler m68k_group_e: the flag math (C = last bit out,
     * V for ASL, X through-carry) is intricate and it touches no pc for the
     * register form. It accesses cpu->regs.d[] directly, so pinned Dn are synced
     * around the call. Memory-form shifts (size==3) fetch EA words via pc, so they
     * stay on the interpreter. ROXL/ROXR read X — flagged so liveness keeps the
     * feeding X write; all forms write NZVC+X. */
    if (top == 0xE && ((op >> 6) & 3) != 3) {
        g_op_wr_nzvc = true; g_op_wr_x = true;
        if (((op >> 3) & 3) == 2) g_op_reads_x = true;   /* ROXL/ROXR */
        emit_pin_writeback(e);
        emit_mov(e, 10, A_CPU);
        emit_imm(e, 11, op);
        emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)&m68k_group_e);
        emit_callx8(e, 8);
        emit_pin_reload(e);
        int cr = (op >> 9) & 7;
        int count = ((op >> 5) & 1) ? 4 : (cr ? cr : 8);    /* reg-count: estimate */
        *cycles = 6 + 2 * count + (((op >> 6) & 3) == 2 ? 2 : 0);
        return 1;
    }
    /* TST.B/.W/.L <ea>  (Dn or readable memory; An is 68020+, excluded). Loads the
     * operand via the shared MOVE source path and sets N/Z (V=C=0). */
    if ((op & 0xFF00) == 0x4A00 && ((op >> 6) & 3) != 3) {
        int size = (op >> 6) & 3, sm = (op >> 3) & 7, sr = op & 7;
        if (sm == 1) return 0;                       /* TST An not on 68000 */
        if (size == 0 && sm == 1) return 0;
        int sext = emit_move_src(e, sm, sr, pc, size);   /* operand -> a3 */
        if (sext < 0) return 0;
        emit_flags_nz_sized(e, 3, size);
        *cycles = 4 + (ea_is_mem(sm, sr) ? 4 : 0);
        return 1 + sext;
    }
    /* CLR.B/.W/.L (Dn or alterable memory) */
    if ((op & 0xFF00) == 0x4200 && ((op >> 6) & 3) != 3) {
        int size = (op >> 6) & 3, dm = (op >> 3) & 7, dr = op & 7;
        if (dm == 1) return 0;                 /* An not allowed */
        if (dm == 7 && dr > 1) return 0;
        emit_movi(e, 3, 0);
        int dext = emit_move_dst(e, dm, dr, pc, 0, size);
        if (dext < 0) return 0;
        emit_flags_clr(e);
        *cycles = ea_is_mem(dm, dr) ? 8 : 4;
        return 1 + dext;
    }
    /* MOVE SR,<ea>  (0100 0000 11 mmmrrr) — word; not privileged on the 68000.
     * No flags affected. */
    if ((op & 0xFFC0) == 0x40C0) {
        int dm = (op >> 3) & 7, dr = op & 7;
        if (dm == 1) return 0;                       /* An not allowed */
        if (dm == 7 && dr > 1) return 0;
        /* Reads the live SR (incl. NZVC + X), so the dead-flag pass must keep any
         * feeding flag writer alive — otherwise it could be suppressed as "dead"
         * by a later NZVC/X overwriter within this same block. */
        g_op_reads = g_op_reads_x = true;
        emit_l16ui(e, 3, A_CPU, OFF_SR);             /* a3 = SR (word) */
        int dext = emit_move_dst(e, dm, dr, pc, 0, SIZE_WORD);
        if (dext < 0) return 0;
        *cycles = ea_is_mem(dm, dr) ? 8 : 6;
        return 1 + dext;
    }
    /* LEA <ea>,An  (0100 AAA1 11 mmmrrr) — address into An, no flags. */
    if ((op & 0xF1C0) == 0x41C0) {
        int An = (op >> 9) & 7, sm = (op >> 3) & 7, sr = op & 7;
        switch (sm) {
        case 2:   /* (An) */
            emit_l32i_n(e, 4, A_CPU, OFF_A(sr));
            emit_s32i_n(e, 4, A_CPU, OFF_A(An));
            *cycles = 4; return 1;
        case 5: { /* (d16,An) */
            uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
            emit_ea_addr(e, 4, 5, sr, d, 2);
            emit_s32i_n(e, 4, A_CPU, OFF_A(An));
            *cycles = 8; return 2; }
        case 7:
            if (sr == 0 || sr == 1) {
                uint32_t a = (sr == 0)
                    ? (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK)
                    : bus_read32((pc + 2) & BUS_ADDR_MASK);
                emit_imm(e, 4, a);
                emit_s32i_n(e, 4, A_CPU, OFF_A(An));
                *cycles = (sr == 0) ? 8 : 12; return (sr == 0) ? 2 : 3;
            }
            if (sr == 2) {   /* (d16,PC) — resolved at compile time */
                uint32_t a = (pc + 2) + (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
                emit_imm(e, 4, a);
                emit_s32i_n(e, 4, A_CPU, OFF_A(An));
                *cycles = 8; return 2;
            }
            return 0;
        default: return 0;
        }
    }
    /* DBcc Dn,disp16 (0101 cccc 11001 rrr) — the decrement-and-branch loop
     * primitive. If cond true: exit (no decrement). Else decrement Dn's low word;
     * if that did not pass -1, branch to the displacement, else exit. This is the
     * back-edge of counted loops, executed every iteration, so keeping it native
     * keeps whole loops native. Block-ender; both internal branches patch to the
     * single pc-store at the end. */
    if ((op & 0xF0F8) == 0x50C8) {
        int cc = (op >> 8) & 0xF, dn = op & 7;
        int16_t disp = (int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
        uint32_t target = (pc + 2) + (uint32_t)(int32_t)disp;   /* full PC (matches interp; bus masks on access) */
        uint32_t fall   = pc + 4;
        emit_imm(e, 3, fall);                       /* default pc = fall (loop exit) */
        if (cc == 0) {                              /* DBT: cond always true -> exit */
            emit_s32i(e, 3, A_CPU, OFF_PC);
            *cycles = 12; *ends_block = true; return 2;
        }
        g_op_reads = (cc != 1);                     /* reads NZVC unless DBRA/DBF */
        if (cc == 1) emit_movi(e, 9, 0);            /* DBRA: condition is always false */
        else         emit_eval_cc(e, cc);           /* a9 = cond (1=true) */
        int b_condtrue = e->pos; emit_bnez(e, 9, 0);   /* cond true -> exit (keep fall) */
        emit_load_dn(e, 4, dn);                     /* a4 = Dn */
        emit_extui (e, 5, 4, 0, 16);                /* a5 = Dn.W */
        emit_addi  (e, 5, 5, -1);
        emit_extui (e, 5, 5, 0, 16);                /* a5 = (Dn.W - 1) & 0xFFFF */
        emit_imm   (e, 9, 0xFFFF0000);
        emit_and   (e, 4, 4, 9);
        emit_or    (e, 4, 4, 5);                    /* a4 = (Dn & ~0xFFFF) | new low */
        emit_write_dn_sized(e, dn, 4, SIZE_LONG);
        emit_imm   (e, 9, 0xFFFF);
        int b_exh = e->pos; emit_beq(e, 5, 9, 0);   /* low word == -1 -> exhausted, exit */
        emit_imm   (e, 3, target);                  /* not exhausted -> take the branch */
        patch_b12(e, b_condtrue);                   /* both exits land on the pc-store */
        patch_b8 (e, b_exh);
        emit_s32i(e, 3, A_CPU, OFF_PC);
        *cycles = 10; *ends_block = true; return 2;
    }
    /* ADDQ/SUBQ #d,An  (0101 ddd S ss 001 rrr) — full 32-bit address arith on
     * An regardless of size, no flags. size==3 is Scc/DBcc, not ADDQ/SUBQ. */
    if ((op & 0xF000) == 0x5000 && ((op >> 3) & 7) == 1 && ((op >> 6) & 3) != 3) {
        int data = (op >> 9) & 7; if (data == 0) data = 8;
        int sub = (op >> 8) & 1, An = op & 7;
        emit_l32i_n(e, 3, A_CPU, OFF_A(An));
        emit_addi(e, 3, 3, sub ? -data : data);
        emit_s32i_n(e, 3, A_CPU, OFF_A(An));
        *cycles = 8; return 1;
    }
    /* ADDQ/SUBQ #d,Dn  (0101 ddd S ss 000 rrr) — full flags. */
    if ((op & 0xF000) == 0x5000 && ((op >> 3) & 7) == 0 && ((op >> 6) & 3) != 3) {
        int data = (op >> 9) & 7; if (data == 0) data = 8;
        int sub = (op >> 8) & 1, size = (op >> 6) & 3, dn = op & 7;
        emit_load_dn(e, 4, dn);                      /* a4 = dst */
        emit_movi (e, 5, data);                      /* a5 = src */
        if (sub) emit_sub(e, 3, 4, 5); else emit_add(e, 3, 4, 5);  /* a3 = result */
        emit_write_dn_sized(e, dn, 3, size);
        g_op_wr_x = true;
        emit_call_flags(e, sub ? (void *)&m68k_set_flags_sub : (void *)&m68k_set_flags_add,
                        5, 4, 3, size);
        *cycles = (size == 2) ? 8 : 4; return 1;
    }
    /* ADDQ/SUBQ #d,<ea>  (alterable memory) — read-modify-write with full flags.
     * The dominant hot-loop op (a byte countdown in memory); covering it lets the
     * loop block extend past the decrement and chain through its branch. EA address
     * is computed once into a4 (preserved across the two bus calls); the result is
     * recomputed for the flag helper only when the flags are live. */
    if ((op & 0xF000) == 0x5000 && ((op >> 3) & 7) >= 2 && ((op >> 6) & 3) != 3) {
        int data = (op >> 9) & 7; if (data == 0) data = 8;
        int sub = (op >> 8) & 1, size = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        int ext = 0;
        switch (mode) {
        case 2: case 3: case 4: emit_ea_addr(e, 4, mode, reg, 0, size); break;
        case 6: { uint16_t ew = bus_read16((pc + 2) & BUS_ADDR_MASK);
                  emit_ea_addr(e, 4, 6, reg, ew, size); ext = 1; break; }
        case 5: { uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
                  emit_ea_addr(e, 4, 5, reg, d, size); ext = 1; break; }
        case 7:
            if (reg == 0) { uint32_t a = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
                            emit_ea_addr(e, 4, 7, 0, a, size); ext = 1; }
            else if (reg == 1) { uint32_t a = bus_read32((pc + 2) & BUS_ADDR_MASK);
                                 emit_ea_addr(e, 4, 7, 1, a, size); ext = 2; }
            else return 0;
            break;
        default: return 0;
        }
        emit_mem_read_sized(e, 3, 4, size);              /* a3 = dst (orig) */
        emit_movi(e, 5, data);                           /* a5 = src */
        if (sub) emit_sub(e, 8, 3, 5); else emit_add(e, 8, 3, 5);   /* a8 = result */
        emit_mem_write_sized(e, 4, 8, size);             /* mem[a4] = result */
        if (!g_suppress_flags) {                         /* recompute result for flags */
            if (sub) emit_sub(e, 4, 3, 5); else emit_add(e, 4, 3, 5);
        }
        g_op_wr_x = true;
        emit_call_flags(e, sub ? (void *)&m68k_set_flags_sub : (void *)&m68k_set_flags_add,
                        5, 3, 4, size);
        *cycles = (size == 2 ? 12 : 8); return 1 + ext;
    }
    /* ADD/SUB.{B,W,L} <ea>,Dn  (data direction, opmode 0ss; any readable EA). The
     * Dn,<ea> direction (opmode 1ss) is ADD-to-memory / ADDX, left to the interp. */
    if (((op & 0xF000) == 0xD000 || (op & 0xF000) == 0x9000) && ((op >> 6) & 7) <= 2) {
        bool isadd = (op & 0xF000) == 0xD000;
        int size = (op >> 6) & 3, dn = (op >> 9) & 7, sm = (op >> 3) & 7, sr = op & 7;
        if (size == 0 && sm == 1) return 0;          /* byte An source illegal */
        int sext = emit_move_src(e, sm, sr, pc, size);   /* src -> a3 */
        if (sext < 0) return 0;
        emit_mov(e, 5, 3);                           /* a5 = src */
        emit_load_dn(e, 4, dn);                      /* a4 = dst */
        if (isadd) emit_add(e, 3, 4, 5); else emit_sub(e, 3, 4, 5);
        emit_write_dn_sized(e, dn, 3, size);
        g_op_wr_x = true;
        emit_call_flags(e, isadd ? (void *)&m68k_set_flags_add : (void *)&m68k_set_flags_sub,
                        5, 4, 3, size);
        *cycles = (size == 2 ? 6 : 4) + (ea_is_mem(sm, sr) ? 4 : 0);
        return 1 + sext;
    }
    /* CMP.B/.W/.L <ea>,Dn  (1011 ddd 0ss eeeeee) — flags only, no writeback. */
    if ((op & 0xF100) == 0xB000 && ((op >> 6) & 3) != 3) {
        int size = (op >> 6) & 3, dn = (op >> 9) & 7;
        int sm = (op >> 3) & 7, srr = op & 7;
        if (size == 0 && sm == 1) return 0;          /* byte An source illegal */
        int sext = emit_move_src(e, sm, srr, pc, size);   /* src -> a3 */
        if (sext < 0) return 0;
        emit_mov(e, 5, 3);                           /* a5 = src */
        emit_load_dn(e, 4, dn);                       /* a4 = dst (Dn) */
        emit_sub(e, 3, 4, 5);                        /* a3 = dst - src */
        emit_call_flags(e, (void *)&m68k_set_flags_cmp, 5, 4, 3, size);
        *cycles = (size == 2 ? 6 : 4) + (ea_is_mem(sm, srr) ? 4 : 0);
        return 1 + sext;
    }
    /* ADDA/SUBA/CMPA.L <ea>,An  (opmode 111). ADDA/SUBA write An (no flags);
     * CMPA does a long compare into the flags. Long only — the word forms need the
     * source sign-extended to 32 bits, which we don't emit yet. */
    if ((op & 0xF1C0) == 0xD1C0 || (op & 0xF1C0) == 0x91C0 || (op & 0xF1C0) == 0xB1C0) {
        int An = (op >> 9) & 7, sm = (op >> 3) & 7, srr = op & 7;
        bool iscmp = (op & 0xF1C0) == 0xB1C0;
        bool issub = (op & 0xF1C0) == 0x91C0;
        int sext = emit_move_src(e, sm, srr, pc, SIZE_LONG);   /* src -> a3 */
        if (sext < 0) return 0;
        emit_mov(e, 5, 3);                            /* a5 = src */
        emit_l32i_n(e, 4, A_CPU, OFF_A(An));          /* a4 = An */
        if (iscmp || issub) emit_sub(e, 3, 4, 5); else emit_add(e, 3, 4, 5);
        if (iscmp) emit_call_flags(e, (void *)&m68k_set_flags_cmp, 5, 4, 3, SIZE_LONG);
        else       emit_s32i_n(e, 3, A_CPU, OFF_A(An));
        *cycles = 8 + (ea_is_mem(sm, srr) ? 4 : 0);
        return 1 + sext;
    }
    /* Static bit ops BTST/BCHG/BCLR/BSET #imm,<ea> (0000 1000 tt mmmrrr; bit number
     * in the next word). Dn dst is 32-bit (bit mod 32), memory dst is byte (bit mod
     * 8). Only Z is affected (Z = !bit). Dynamic (Dn-numbered) forms need a variable
     * shift and stay on the interpreter. */
    if ((op & 0xFF00) == 0x0800) {
        int tt = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
        if (mode == 1) return 0;                       /* An not allowed */
        uint16_t bn = bus_read16((pc + 2) & BUS_ADDR_MASK);
        if (mode == 0) {                               /* Dn, 32-bit */
            uint32_t mask = 1u << (bn & 31);
            emit_load_dn(e, 3, reg);                   /* a3 = Dn */
            emit_imm    (e, 4, mask);
            emit_and    (e, 5, 3, 4);                  /* a5 = Dn & mask */
            emit_set_z  (e, 5);
            if (tt != 0) {
                if      (tt == 1) emit_xor(e, 3, 3, 4);                 /* BCHG */
                else if (tt == 2) { emit_imm(e, 9, ~mask); emit_and(e, 3, 3, 9); } /* BCLR */
                else              emit_or(e, 3, 3, 4);                  /* BSET */
                emit_write_dn_sized(e, reg, 3, SIZE_LONG);
            }
            *cycles = (tt == 0) ? 6 : 8; return 2;
        }
        uint32_t mask = 1u << (bn & 7);                /* memory, byte */
        int ext = 0;
        switch (mode) {
        case 2: case 3: case 4: emit_ea_addr(e, 4, mode, reg, 0, SIZE_BYTE); break;
        case 5: { uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 4) & BUS_ADDR_MASK);
                  emit_ea_addr(e, 4, 5, reg, d, SIZE_BYTE); ext = 1; break; }
        case 6: { uint16_t ew = bus_read16((pc + 4) & BUS_ADDR_MASK);
                  emit_ea_addr(e, 4, 6, reg, ew, SIZE_BYTE); ext = 1; break; }
        case 7:
            if (reg == 0) { uint32_t a = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 4) & BUS_ADDR_MASK);
                            emit_ea_addr(e, 4, 7, 0, a, SIZE_BYTE); ext = 1; }
            else if (reg == 1) { uint32_t a = bus_read32((pc + 4) & BUS_ADDR_MASK);
                                 emit_ea_addr(e, 4, 7, 1, a, SIZE_BYTE); ext = 2; }
            else return 0;
            break;
        default: return 0;
        }
        emit_mem_read_sized(e, 3, 4, SIZE_BYTE);       /* a3 = byte (a4 = addr) */
        emit_imm  (e, 5, mask);
        /* Z = !(byte & mask), computed inline with only a8/a9 scratch so the
         * byte (a3), addr (a4) and mask (a5) survive for the read-modify-write. */
        emit_l16ui (e, 8, A_CPU, OFF_SR);
        emit_movi  (e, 9, ~SR_Z);
        emit_and   (e, 8, 8, 9);                       /* clear Z */
        emit_and   (e, 9, 3, 5);                       /* a9 = byte & mask */
        emit_bnez  (e, 9, 1);
        emit_addi_n(e, 8, 8, SR_Z);
        emit_s16i  (e, 8, A_CPU, OFF_SR);
        if (tt != 0) {
            if      (tt == 1) emit_xor(e, 3, 3, 5);
            else if (tt == 2) { emit_imm(e, 9, ~mask); emit_and(e, 3, 3, 9); }
            else              emit_or(e, 3, 3, 5);
            emit_mem_write_sized(e, 4, 3, SIZE_BYTE);
        }
        *cycles = (tt == 0) ? 8 : 12; return 2 + ext;
    }
    /* ORI/ANDI/EORI #imm,CCR (byte) / #imm,SR (word).  The SR (word) forms are
     * privileged and touch the IPL mask + supervisor bit, so they route through
     * m68k_jit_set_sr (the single setter that swaps USP/SSP on an S change) and end
     * the block — the dispatcher then re-evaluates pending interrupts and chains to
     * the next block via the written pc, like a branch. This matches the
     * interpreter, which now applies the SR write through the SAME setter (so both
     * engines swap identically); the ROM runs these in supervisor mode, where the
     * interpreter's user-mode privilege trap is unreachable, so omitting it here is
     * safe. CCR (byte) forms touch only the low 5 bits (XNZVC) and stay inline. */
    if (op == 0x003C || op == 0x007C || op == 0x023C ||
        op == 0x027C || op == 0x0A3C || op == 0x0A7C) {
        int fam = op & 0x0F00;                    /* 0x000 ORI, 0x200 ANDI, 0xA00 EORI */
        bool is_sr = (op & 0xFF) == 0x7C;
        uint16_t imm = bus_read16((pc + 2) & BUS_ADDR_MASK);
        /* Reads the live SR/CCR (incl. NZVC + X) and re-merges it, so the dead-flag
         * pass must keep any feeding flag writer alive (the CCR form is not a
         * block-ender, so a later overwriter could otherwise suppress it). */
        g_op_reads = g_op_reads_x = true;
        emit_l16ui(e, 3, A_CPU, OFF_SR);          /* a3 = SR */
        emit_imm  (e, 5, is_sr ? imm : (imm & 0xFF));
        if      (fam == 0x000) emit_or (e, 3, 3, 5);
        else if (fam == 0x200) emit_and(e, 3, 3, 5);
        else                   emit_xor(e, 3, 3, 5);
        if (is_sr) {
            emit_mov(e, 10, A_CPU);
            emit_mov(e, 11, 3);
            emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)&m68k_jit_set_sr);
            emit_callx8(e, 8);
            emit_imm  (e, 3, pc + 4);
            emit_s32i (e, 3, A_CPU, OFF_PC);
            *ends_block = true;
        } else {
            /* CCR write affects only bits 0-4 (XNZVC); bits 5-7 read as 0 (match the
             * interpreter's `imm & 0x1F` / `imm | 0xFFE0` masking). */
            emit_l16ui(e, 8, A_CPU, OFF_SR);
            emit_imm  (e, 9, 0xFFE0);
            emit_and  (e, 8, 8, 9);               /* keep SR bits 5-15 */
            emit_extui(e, 9, 3, 0, 5);            /* new CCR = bits 0-4 only */
            emit_or   (e, 8, 8, 9);
            emit_s16i (e, 8, A_CPU, OFF_SR);
        }
        *cycles = (is_sr || (op & 0xFF) == 0x3C) ? 20 : 16;
        return 2;
    }
    /* Immediate ALU #imm,Dn: ORI/ANDI/SUBI/ADDI/EORI/CMPI (0000 oooo ss 000 rrr). */
    {
        int fam = op & 0xFF00;
        if ((fam == 0x0000 || fam == 0x0200 || fam == 0x0400 ||
             fam == 0x0600 || fam == 0x0A00 || fam == 0x0C00) &&
            ((op >> 3) & 7) == 0 && ((op >> 6) & 3) != 3) {
            int size = (op >> 6) & 3, dn = op & 7;
            uint32_t imm; int iext;
            if (size == 2) { imm = bus_read32((pc + 2) & BUS_ADDR_MASK); iext = 2; }
            else { uint32_t w = bus_read16((pc + 2) & BUS_ADDR_MASK);
                   imm = (size == 0) ? (w & 0xFF) : (w & 0xFFFF); iext = 1; }
            bool cmp = (fam == 0x0C00);
            emit_load_dn(e, 4, dn);                  /* a4 = dst */
            emit_imm  (e, 5, imm);                   /* a5 = src */
            if      (fam == 0x0000) emit_or (e, 3, 4, 5);
            else if (fam == 0x0200) emit_and(e, 3, 4, 5);
            else if (fam == 0x0A00) emit_xor(e, 3, 4, 5);
            else if (fam == 0x0600) emit_add(e, 3, 4, 5);
            else                    emit_sub(e, 3, 4, 5);   /* SUBI / CMPI */
            if (!cmp) emit_write_dn_sized(e, dn, 3, size);
            if (fam == 0x0000 || fam == 0x0200 || fam == 0x0A00)
                emit_flags_nz_sized(e, 3, size);             /* logical: N/Z, V=C=0 */
            else if (fam == 0x0600)
                { g_op_wr_x = true; emit_call_flags(e, (void *)&m68k_set_flags_add, 5, 4, 3, size); }
            else if (fam == 0x0400)
                { g_op_wr_x = true; emit_call_flags(e, (void *)&m68k_set_flags_sub, 5, 4, 3, size); }
            else
                emit_call_flags(e, (void *)&m68k_set_flags_cmp, 5, 4, 3, size);
            *cycles = (size == 2) ? 16 : 8;
            return 1 + iext;
        }
    }
    /* SWAP Dn (0100 1000 0100 0rrr) — swap register halves; N/Z on 32-bit. */
    if ((op & 0xFFF8) == 0x4840) {
        int dn = op & 7;
        emit_load_dn(e, 3, dn);
        emit_extui (e, 9, 3, 16, 16);       /* a9 = Dn >> 16 */
        emit_slli  (e, 3, 3, 16);           /* a3 = Dn << 16 */
        emit_or    (e, 3, 3, 9);
        emit_write_dn_sized(e, dn, 3, SIZE_LONG);
        emit_flags_nz(e, 3);
        *cycles = 4; return 1;
    }
    /* MOVEM.L regs,-(An) / (An)+,regs — subroutine register save/restore. Only the
     * predecrement-store and postincrement-load forms (the ones the ROM prologue/
     * epilogue use) and long size (word load would need sign-extension). The mask
     * is a compile-time ext word, so the transfer unrolls into straight-line code;
     * the pass-2 overflow rewind keeps a too-long MOVEM all-or-nothing. */
    if ((op & 0xFB80) == 0x4880 && (op & 0x40)) {
        int dr = (op >> 10) & 1, mode = (op >> 3) & 7, rg = op & 7;
        uint16_t mask = bus_read16((pc + 2) & BUS_ADDR_MASK);
        int nreg = 0; for (int i = 0; i < 16; i++) if (mask & (1 << i)) nreg++;
        if (dr == 0 && mode == 4) {                  /* regs -> -(An) (mask bit0=A7) */
            emit_l32i_n(e, 4, A_CPU, OFF_A(rg));      /* a4 = An (running address) */
            for (int i = 0; i < 16; i++) {
                if (!(mask & (1 << i))) continue;
                emit_addi(e, 4, 4, -4);              /* predecrement */
                if (i < 8) emit_l32i_n(e, 3, A_CPU, OFF_A(7 - i));   /* An(7-i) */
                else       emit_load_dn(e, 3, 15 - i);              /* Dn(15-i) */
                emit_mem_write_sized(e, 4, 3, SIZE_LONG);
            }
            emit_s32i_n(e, 4, A_CPU, OFF_A(rg));
            *cycles = 8 + 8 * nreg; return 2;
        }
        if (dr == 1 && mode == 3) {                  /* (An)+ -> regs (mask bit0=D0) */
            if (mask & (1 << (rg + 8))) return 0;    /* base in list: writeback races load */
            emit_l32i_n(e, 4, A_CPU, OFF_A(rg));      /* a4 = An */
            for (int i = 0; i < 16; i++) {
                if (!(mask & (1 << i))) continue;
                emit_mem_read_sized(e, 3, 4, SIZE_LONG);
                if (i < 8) emit_write_dn_sized(e, i, 3, SIZE_LONG); /* Dn(i) (pin-aware) */
                else       emit_s32i_n(e, 3, A_CPU, OFF_A(i - 8));  /* An(i-8) */
                emit_addi(e, 4, 4, 4);              /* postincrement */
            }
            emit_s32i_n(e, 4, A_CPU, OFF_A(rg));
            *cycles = 12 + 8 * nreg; return 2;
        }
        return 0;
    }
    /* LINK An,#disp16 (0100 1110 0101 0rrr) — set up a stack frame. */
    if ((op & 0xFFF8) == 0x4E50 && (op & 7) != 7) {
        int An = op & 7;
        int16_t d = (int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
        emit_l32i_n(e, 4, A_CPU, OFF_A(7));
        emit_addi  (e, 4, 4, -4);
        emit_s32i_n(e, 4, A_CPU, OFF_A(7));         /* SP -= 4 */
        emit_l32i_n(e, 5, A_CPU, OFF_A(An));
        emit_mem_write_sized(e, 4, 5, SIZE_LONG);   /* push old An */
        emit_s32i_n(e, 4, A_CPU, OFF_A(An));        /* An = SP */
        emit_imm   (e, 9, (uint32_t)(int32_t)d);
        emit_add   (e, 4, 4, 9);
        emit_s32i_n(e, 4, A_CPU, OFF_A(7));         /* SP += disp */
        *cycles = 16; return 2;
    }
    /* UNLK An (0100 1110 0101 1rrr) — tear down a stack frame. */
    if ((op & 0xFFF8) == 0x4E58 && (op & 7) != 7) {
        int An = op & 7;
        emit_l32i_n(e, 4, A_CPU, OFF_A(An));        /* a4 = An (frame ptr) */
        emit_mem_read_sized(e, 3, 4, SIZE_LONG);    /* a3 = mem[An] */
        emit_addi  (e, 4, 4, 4);
        emit_s32i_n(e, 4, A_CPU, OFF_A(7));         /* SP = An + 4 */
        emit_s32i_n(e, 3, A_CPU, OFF_A(An));        /* An = popped */
        *cycles = 12; return 1;
    }
    /* BRA / BSR / Bcc  (group 6: 0110 cccc dddddddd). All resolve pc to a
     * compile-time target/fall-through, so they end the block. */
    if (top == 6) {
        int cc = (op >> 8) & 0xF;
        int8_t disp8 = (int8_t)(op & 0xFF);
        int insn_len, disp;
        if (disp8 == 0) { disp = (int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK); insn_len = 4; }
        else            { disp = disp8; insn_len = 2; }
        uint32_t base   = pc + 2;                       /* full PC (matches interp; bus masks on access) */
        uint32_t target = base + (uint32_t)disp;        /* BSR pushes `fall` as the return addr -> must be full */
        uint32_t fall   = pc + insn_len;
        if (cc == 1) {                         /* BSR: push return, jump */
            emit_l32i_n(e, 4, A_CPU, OFF_A(7));
            emit_addi(e, 4, 4, -4);
            emit_s32i_n(e, 4, A_CPU, OFF_A(7));
            emit_imm(e, 3, fall);
            emit_mem_write_sized(e, 4, 3, SIZE_LONG);
            emit_imm(e, 3, target);
            emit_s32i(e, 3, A_CPU, OFF_PC);
            *cycles = 18;
        } else if (cc == 0) {                  /* BRA: unconditional */
            emit_imm(e, 3, target);
            emit_s32i(e, 3, A_CPU, OFF_PC);
            *cycles = 10;
        } else {                               /* Bcc: target if taken else fall */
            g_op_reads = true;                 /* reads NZVC */
            emit_imm(e, 3, fall);
            emit_eval_cc(e, cc);
            emit_beqz(e, 9, 2);                /* not taken -> keep fall (skip 3B imm) */
            emit_imm(e, 3, target);
            emit_s32i(e, 3, A_CPU, OFF_PC);
            *cycles = 10;                      /* compile-time fixed; taken cost */
        }
        *ends_block = true;
        return insn_len / 2;
    }
    /* JSR <ea>  (0100 1110 10 mmmrrr) — push return, jump to EA. */
    if ((op & 0xFFC0) == 0x4E80) {
        int sm = (op >> 3) & 7, srr = op & 7;
        int ilen;
        switch (sm) {
        case 2: emit_ea_addr(e, 5, 2, srr, 0, SIZE_LONG); ilen = 2; break;       /* (An) */
        case 5: { uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
                  emit_ea_addr(e, 5, 5, srr, d, SIZE_LONG); ilen = 4; break; }   /* (d16,An) */
        case 7:
            if (srr == 0)      { emit_imm(e, 5, (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK)); ilen = 4; }
            else if (srr == 1) { emit_imm(e, 5, bus_read32((pc + 2) & BUS_ADDR_MASK)); ilen = 6; }
            else if (srr == 2) { emit_imm(e, 5, (pc + 2) + (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK)); ilen = 4; }
            else return 0;
            break;
        default: return 0;
        }
        uint32_t fall = pc + ilen;                  /* full PC — pushed as the return addr */
        emit_l32i_n(e, 4, A_CPU, OFF_A(7));
        emit_addi(e, 4, 4, -4);
        emit_s32i_n(e, 4, A_CPU, OFF_A(7));
        emit_imm(e, 3, fall);
        emit_mem_write_sized(e, 4, 3, SIZE_LONG);   /* preserves a5 (target) */
        emit_s32i(e, 5, A_CPU, OFF_PC);
        *cycles = 16; *ends_block = true; return ilen / 2;
    }
    /* RTS (0x4E75) — pop return PC off the stack. */
    if (op == 0x4E75) {
        emit_l32i_n(e, 4, A_CPU, OFF_A(7));
        emit_mem_read_sized(e, 3, 4, SIZE_LONG);    /* a3 = mem[a7] */
        emit_addi(e, 4, 4, 4);
        emit_s32i_n(e, 4, A_CPU, OFF_A(7));
        emit_s32i(e, 3, A_CPU, OFF_PC);
        *cycles = 16; *ends_block = true; return 1;
    }
    /* NOP (0x4E71) — no effect; keep the block flowing (not a block-ender). */
    if (op == 0x4E71) { *cycles = 4; return 1; }
    /* STOP #imm (0x4E72) — load SR (privileged, via m68k_jit_set_sr so an S change
     * swaps USP/SSP — same setter the interpreter now uses) and halt until an
     * interrupt; the dispatcher's pending-interrupt service handles the wait.
     * Block-ender. */
    if (op == 0x4E72) {
        uint16_t imm = bus_read16((pc + 2) & BUS_ADDR_MASK);
        emit_mov (e, 10, A_CPU);
        emit_imm (e, 11, imm);
        emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)&m68k_jit_set_sr);
        emit_callx8(e, 8);
        emit_movi(e, 9, 1);
        emit_s8i (e, 9, A_CPU, OFF_STOPPED);        /* cpu->regs.stopped = true */
        emit_imm (e, 3, pc + 4);
        emit_s32i(e, 3, A_CPU, OFF_PC);
        *cycles = 4; *ends_block = true; return 2;
    }
    /* RTE (0x4E73) — return from exception: pop SR (word) then PC (long) off the
     * supervisor stack and apply SR via m68k_jit_set_sr (USP/SSP swap on an S
     * transition). The ROM's timer ISR ends in RTE every tick, so this is hot.
     * Block-ender: the dispatcher re-evaluates pending interrupts afterwards. */
    if (op == 0x4E73) {
        emit_l32i_n(e, 4, A_CPU, OFF_A(7));         /* a4 = A7 (SSP) */
        emit_mem_read_sized(e, 3, 4, SIZE_WORD);    /* a3 = popped SR */
        emit_addi(e, 4, 4, 2);
        emit_mem_read_sized(e, 5, 4, SIZE_LONG);    /* a5 = popped PC */
        emit_addi(e, 4, 4, 4);
        emit_s32i_n(e, 4, A_CPU, OFF_A(7));         /* A7 += 6 (still SSP) */
        emit_s32i (e, 5, A_CPU, OFF_PC);            /* pc = popped */
        emit_mov  (e, 10, A_CPU);                   /* apply SR (+ SP swap) */
        emit_mov  (e, 11, 3);
        emit_l32r_lit(e, 8, (uint32_t)(uintptr_t)&m68k_jit_set_sr);
        emit_callx8(e, 8);
        *cycles = 20; *ends_block = true; return 1;
    }
    /* JMP <ea>  (0100 1110 11 mmmrrr) — set pc to EA, no push. */
    if ((op & 0xFFC0) == 0x4EC0) {
        int sm = (op >> 3) & 7, srr = op & 7;
        int ilen;
        switch (sm) {
        case 2: emit_ea_addr(e, 5, 2, srr, 0, SIZE_LONG); ilen = 2; break;
        case 5: { uint32_t d = (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK);
                  emit_ea_addr(e, 5, 5, srr, d, SIZE_LONG); ilen = 4; break; }
        case 7:
            if (srr == 0)      { emit_imm(e, 5, (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK)); ilen = 4; }
            else if (srr == 1) { emit_imm(e, 5, bus_read32((pc + 2) & BUS_ADDR_MASK)); ilen = 6; }
            else if (srr == 2) { emit_imm(e, 5, (pc + 2) + (uint32_t)(int32_t)(int16_t)bus_read16((pc + 2) & BUS_ADDR_MASK)); ilen = 4; }
            else return 0;
            break;
        default: return 0;
        }
        emit_s32i(e, 5, A_CPU, OFF_PC);
        *cycles = 8; *ends_block = true; return ilen / 2;
    }
    return 0;
}

/* Lay out [literal pool][code] at a core-reserved cache base, patch the recorded
 * l32r/branch fixups, write the words via the execmem backend, and icache-sync.
 * `reserve` is the core's segmented-ring allocator (jit_cache_alloc); passing it
 * in keeps this backend independent of the core's cache internals. Runs on the
 * cpu task only (the sole owner of the executable cache). */
void *backend_finalize(backend_block_t *blk, jit_reserve_fn reserve)
{
    if (blk->overflow) return NULL;
    int litbytes = blk->n_lits * 4;
    int total = (litbytes + blk->pos + 3) & ~3;
    uint32_t base_instr = reserve(total);           /* literals (evicts a segment if needed) */
    if (!base_instr) return NULL;
    uint32_t entry_instr = base_instr + litbytes;                        /* code */
    for (int i = 0; i < blk->n_fix; i++) {
        uint32_t la = entry_instr + blk->fix[i].pos;
        uint32_t li = base_instr + blk->fix[i].lit * 4;
        int32_t off = (int32_t)(li - ((la + 3) & ~3u));
        uint32_t imm16 = (off >> 2) & 0xFFFF;
        blk->code[blk->fix[i].pos + 1] = imm16 & 0xFF;
        blk->code[blk->fix[i].pos + 2] = (imm16 >> 8) & 0xFF;
    }
    for (int i = 0; i < blk->n_lits; i++)
        jit_exec_write_word(base_instr + i * 4, blk->lits[i]);
    int words = (blk->pos + 3) / 4;
    for (int i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) { int idx = i * 4 + b; if (idx < blk->pos) w |= (uint32_t)blk->code[idx] << (b * 8); }
        jit_exec_write_word(entry_instr + i * 4, w);
    }
    jit_exec_icache_sync();
    return (void *)(uintptr_t)entry_instr;
}

#define JIT_MAX_BLOCK_OPS 32
/* Codegen — PURE w.r.t. shared state: it only reads the immutable ROM and uses the
 * compile-scratch globals, assembling the block into *blk. It touches NO executable
 * cache and NO block/present tables, so it is safe to run on the off-core async
 * compiler. The address-dependent finalize + publish stay on the cpu task. Returns
 * false if nothing emitted. (Implements backend_codegen from m68k_jit_backend.h.) */
bool backend_codegen(uint32_t pc, backend_block_t *blk, uint32_t *end_pc)
{
    /* Pass 1: walk the block (emit to a throwaway buffer) to capture each op's
     * length, cycle cost, control-flow flag, and flag-effect classification. */
    uint32_t opc[JIT_MAX_BLOCK_OPS];
    bool     ord[JIT_MAX_BLOCK_OPS], onz[JIT_MAX_BLOCK_OPS], oxx[JIT_MAX_BLOCK_OPS];
    bool     orx[JIT_MAX_BLOCK_OPS];   /* reads X (ROXL/ROXR via helper) */
    int n = 0; uint32_t cur = pc; bool ended_by_cf = false;
    for (int i = 0; i < 8; i++) g_dn_count[i] = 0;
    for (int s = 0; s < JIT_NPIN; s++) { g_pin_dn[s] = -1; g_pin_dirty[s] = false; }
    {
        uint8_t sb[512];
        g_jit_counting = true;        /* tally Dn refs through the load/store helpers */
        while (n < JIT_MAX_BLOCK_OPS) {
            jit_emit_t pe = { .buf = sb, .cap = sizeof(sb) };
            g_suppress_flags = false;
            int cyc = 0; bool ends = false;
            int len = jit_emit_op(&pe, cur, &cyc, &ends);
            if (len == 0) break;   /* unsupported op ends the block; interpreter handles it */
            opc[n] = cur;   /* pass 2 re-emits, so only the PC + flag classification persist */
            ord[n] = g_op_reads; onz[n] = g_op_wr_nzvc; oxx[n] = g_op_wr_x; orx[n] = g_op_reads_x;
            cur += len * 2; n++;
            if (ends) { ended_by_cf = true; break; }
        }
        g_jit_counting = false;
    }
    if (n == 0) return false;

    /* Pick the JIT_NPIN most-referenced data registers (>= PIN_MIN refs) to pin
     * into a6/a7 for the block. Greedy by count; ties resolve to the lower Dn. */
    for (int s = 0; s < JIT_NPIN; s++) {
        int best = -1, bestc = PIN_MIN - 1;
        for (int d = 0; d < 8; d++) {
            bool taken = false;
            for (int t = 0; t < s; t++) if (g_pin_dn[t] == d) taken = true;
            if (!taken && g_dn_count[d] > bestc) { bestc = g_dn_count[d]; best = d; }
        }
        g_pin_dn[s] = best;
    }

    /* Backward liveness: an op's flag write is dead if a later op overwrites the
     * same flags before any op reads them. NZVC and X are both live at block exit
     * (the next block / a fallback op may read them), so the last writer of each
     * always emits. MUST process each op's DEFs (writes) before its USEs (reads):
     * an op that both reads and writes a flag (e.g. ROXL/ROXR read+write X) needs
     * its def killed first, then its read to revive the flag for the PRIOR writer
     * that feeds it. Doing reads first would let the same op's write clear the
     * flag, wrongly suppressing the producer it depends on. */
    bool sup[JIT_MAX_BLOCK_OPS];
    bool live_nzvc = true, live_x = true;
    for (int i = n - 1; i >= 0; i--) {
        bool need = false;
        if (onz[i]) { if (live_nzvc) need = true; live_nzvc = false; }   /* def: needed iff live downstream, then kill */
        if (oxx[i]) { if (live_x)    need = true; live_x = false; }
        if (ord[i]) live_nzvc = true;                 /* use: a prior NZVC writer is needed */
        if (orx[i]) live_x = true;                     /* use: a prior X writer is needed */
        sup[i] = !need;
    }

    /* Pass 2: emit for real, suppressing dead flag computations. */
    jit_emit_t e = { .buf = blk->code, .cap = (int)sizeof(blk->code) };
    emit_entry(&e);
    for (int s = 0; s < JIT_NPIN; s++)              /* load pinned Dn into a6/a7 */
        if (g_pin_dn[s] >= 0) emit_l32i_n(&e, PIN_XREG[s], A_CPU, OFF_D(g_pin_dn[s]));
    int total = 0, emitted = 0;
    for (int i = 0; i < n; i++) {
        int before = e.pos, nfix = e.n_fix, nlit = e.n_lits, cyc = 0; bool ends = false;
        g_suppress_flags = sup[i];
        int len = jit_emit_op(&e, opc[i], &cyc, &ends);
        if (len == 0 || e.overflow) { e.pos = before; e.n_fix = nfix; e.n_lits = nlit; break; }
        total += cyc; emitted++;
    }
    if (emitted == 0) return false;
    /* If pass 2 stopped short of the planned extent (buffer overflow), the block
     * is a straight-line prefix; its fall-through pc is the next op's pc. */
    if (emitted < n) { ended_by_cf = false; cur = opc[emitted]; }

    /* Spill dirty pinned Dn back to the cpu struct. MUST precede emit_movi(A_CPU,..)
     * which clobbers a2 (the cpu pointer) with the return value. Block-enders only
     * write pc, so the pinned values are still authoritative here. */
    for (int s = 0; s < JIT_NPIN; s++)
        if (g_pin_dn[s] >= 0 && g_pin_dirty[s])
            emit_s32i_n(&e, PIN_XREG[s], A_CPU, OFF_D(g_pin_dn[s]));

    if (!ended_by_cf) {
        emit_imm(&e, 3, cur);
        emit_s32i(&e, 3, A_CPU, OFF_PC);
    }
    emit_imm(&e, A_CPU, total);   /* range-safe: a long high-cost block can exceed movi's 12-bit field */
    emit_retw(&e);
    /* Serialize the emit state into the POD transport block (no self-pointers, so
     * it survives the by-value queue copy to the cpu task). The code bytes are
     * already in blk->code (e.buf pointed there). */
    blk->pos = e.pos; blk->overflow = e.overflow;
    blk->n_lits = e.n_lits;
    memcpy(blk->lits, e.lits, (size_t)e.n_lits * sizeof(blk->lits[0]));
    blk->n_fix = e.n_fix;
    memcpy(blk->fix, e.fix, (size_t)e.n_fix * sizeof(blk->fix[0]));
    *end_pc = cur;
    return true;
}

/* This backend emits native code. (A no-op backend — e.g. a RISC-V stub before its
 * codegen is written — returns false here; the core then skips exec-cache setup and
 * runs the pure interpreter for everything, exactly as if the JIT were disabled.) */
bool backend_has_codegen(void)
{
    return true;
}
