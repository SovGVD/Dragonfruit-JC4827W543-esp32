#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Force-inline qualifier for the hot opcode handlers so they fold into the
 * m68k_execute dispatch switch instead of costing a windowed Xtensa call per
 * instruction (measured: that call overhead is most of the per-instruction
 * floor). GCC honours always_inline; other compilers (host MSVC) just inline. */
#if defined(__GNUC__)
#define M68K_AINLINE static inline __attribute__((always_inline))
#else
#define M68K_AINLINE static inline
#endif

/* Portable `restrict` (C99 keyword on GCC; MSVC spells it __restrict). */
#if defined(_MSC_VER)
#define M68K_RESTRICT __restrict
#elif defined(__GNUC__)
#define M68K_RESTRICT __restrict__
#else
#define M68K_RESTRICT
#endif

/* --- Status register bits --- */
#define SR_C     0x0001
#define SR_V     0x0002
#define SR_Z     0x0004
#define SR_N     0x0008
#define SR_X     0x0010
#define SR_I0    0x0100
#define SR_I1    0x0200
#define SR_I2    0x0400
#define SR_I_MASK 0x0700
#define SR_S     0x2000
#define SR_T     0x8000

/* --- Operation sizes --- */
#define SIZE_BYTE 0
#define SIZE_WORD 1
#define SIZE_LONG 2

/* --- Exception vectors --- */
#define VEC_RESET_SSP        0
#define VEC_RESET_PC         1
#define VEC_BUS_ERROR        2
#define VEC_ADDRESS_ERROR    3
#define VEC_ILLEGAL          4
#define VEC_ZERO_DIVIDE      5
#define VEC_CHK              6
#define VEC_TRAPV            7
#define VEC_PRIVILEGE        8
#define VEC_TRACE            9
#define VEC_LINE_A           10
#define VEC_LINE_F           11
#define VEC_SPURIOUS         24
#define VEC_AUTOVECTOR_BASE  25  /* levels 1-7 at vectors 25-31 */
#define VEC_TRAP_BASE        32  /* TRAP #0-15 at vectors 32-47 */

/* --- Effective address result --- */
typedef enum {
    EA_DATA_REG,
    EA_ADDR_REG,
    EA_MEMORY,
} ea_type_t;

typedef struct {
    ea_type_t type;
    uint32_t  addr;
    int       reg;
} ea_result_t;

/* --- CPU state --- */
typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint16_t sr;
    uint32_t usp;
    uint32_t ssp;
    uint32_t inst_pc;
    bool     stopped;
    int      pending_irq;
    int      cycles;
    int      cycles_executed;
    uint32_t irq_count;
    int      pen_irq_level;  /* Set by caller when PEN is pending (e.g. 5) */
} m68k_regs_t;

#define PALM_TRAP_TABLE_SIZE 4096

/* Callback to query current highest pending interrupt level.
 * Returns -1 if no interrupt pending, otherwise the level (1-7). */
typedef int (*m68k_irq_callback_t)(void);

/* Callback to advance peripherals (timer, etc.) by N CPU cycles.
 * Called during STOP to keep peripherals running while CPU is halted. */
typedef void (*m68k_tick_callback_t)(uint32_t cycles);

/* Callback to check if the CPU should wake from STOP.  On the 68EZ328,
 * IWR-enabled interrupts can wake the CPU even when masked in IMR. */
typedef bool (*m68k_wake_callback_t)(void);

/* Callback returning the interrupt vector base (IVR & 0xF8).  The DragonBall
 * is a *vectored* interrupt controller: the vector for a level-N interrupt is
 * (IVR & 0xF8) + N, NOT the 68000 autovector.  When unset, the CPU falls back
 * to autovectors. */
typedef uint32_t (*m68k_vector_base_callback_t)(void);

/* Called when the CPU resumes from STOP (any wake). Lets the peripheral model
 * mimic the EZ328 restarting the PLL on wake (clearing DISPLL). */
typedef void (*m68k_resume_callback_t)(void);

typedef struct {
    m68k_regs_t regs;
    uint32_t    trap_table[PALM_TRAP_TABLE_SIZE]; /* native dispatch: trap# 0xA000+i → address */
    bool        native_dispatch;                   /* enable native TRAP#15 / A-line dispatch */
    m68k_irq_callback_t irq_callback;
    m68k_tick_callback_t tick_callback;
    m68k_wake_callback_t wake_callback;
    m68k_vector_base_callback_t vector_base_callback;
    m68k_resume_callback_t resume_callback;
} m68k_cpu_t;

/* --- Public API --- */
void m68k_init(m68k_cpu_t *cpu);
void m68k_reset(m68k_cpu_t *cpu);
void m68k_execute(m68k_cpu_t *M68K_RESTRICT cpu, int target_cycles);

/* Warm ("soft") reset: re-run the 68k reset sequence WITHOUT clearing RAM, so
 * storage databases — and the ROM's storage-RAM signature — survive and the ROM
 * warm-boots (the equivalent of pressing the reset button on real hardware). The
 * reset vector is re-fetched from ROM, not from RAM 0x0/0x4 (the OS overwrites
 * low RAM after boot). The caller must re-init the EZ328 peripherals
 * (dragonball_init) around this and re-poll the IRQ level. The JIT exec cache
 * stays valid (the ROM is unchanged), so no flush is needed. */
void m68k_warm_reset(m68k_cpu_t *cpu);

/* True once the OS has completed the UIInitialize system trap (0xA1A6) during boot,
 * i.e. the UI subsystem is up and it is safe to drive trap calls / inject events.
 * Mirrors CloudpilotEmu's IsUIInitialized(). Reads the OS's own low-memory flag
 * (sysMiscFlags bit sysMiscFlagUIInitialized) directly, so it is correct regardless of
 * how the code that set it executed; re-armed by m68k_reset/m68k_warm_reset. The
 * auto-installer and pen/key injection gate on this — acting earlier reenters the OS
 * mid-initialisation and corrupts its state. */
bool m68k_ui_initialized(void);

/* Inject a pen point by calling the ROM's EvtEnqueuePenPoint trap directly
 * (penScreenToRawAddr optional, 0 to skip the screen->raw transform). */
void m68k_pen_tap(m68k_cpu_t *cpu, uint32_t evtEnqAddr, uint32_t penScreenToRawAddr,
                  int16_t x, int16_t y, bool penUp);

/* Call an arbitrary ROM routine (one 32-bit arg, result in D0) as a
 * self-contained subcall with full CPU-state save/restore. arg=0 for void. */
uint32_t m68k_rom_call(m68k_cpu_t *cpu, uint32_t addr, uint32_t arg);

/* Inject a key event by calling the ROM's EvtEnqueueKey trap directly. */
void m68k_key_enqueue(m68k_cpu_t *cpu, uint32_t evtEnqKeyAddr,
                      uint16_t ascii, uint16_t keycode, uint16_t modifiers);

/* --- Generalized host->ROM trap call (used by the PDB/PRC installer) -------
 * Palm OS traps follow the 68k C convention: args pushed right-to-left, each
 * 16-bit (Char/Boolean/UInt16) value occupying 2 stack bytes and each
 * 32-bit/pointer value 4, result in D0 (and A0 for pointer/handle returns).
 * m68k_call_trap generalizes rom_subcall/m68k_key_enqueue for arbitrary trap
 * signatures: pass args LEFT-TO-RIGHT (natural C order) and it pushes them
 * right-to-left. Self-contained — SR|=0x2700 (supervisor + IRQ mask) and full
 * CPU-state save/restore, so the interrupted program resumes unaffected.
 * a0_out (optional) receives A0 for pointer/handle-returning traps; timed_out
 * (optional) reports whether the runaway guard tripped — i.e. the handler never
 * returned — so callers driving traps on untrusted input can abort cleanly.
 * Assumes the standard stack-based selectorless-trap ABI: all args on the stack
 * (no register args), scalar result in D0/A0 — it cannot drive a trap that takes
 * a register argument or returns a struct by value. */
typedef enum { M68K_ARG_W = 0, M68K_ARG_L = 1 } m68k_arg_size_t;
typedef struct { uint32_t value; m68k_arg_size_t size; } m68k_arg_t;

uint32_t m68k_call_trap(m68k_cpu_t *cpu, uint32_t handler,
                        const m68k_arg_t *args, int nargs,
                        uint32_t *a0_out, bool *timed_out);
void m68k_set_irq(m68k_cpu_t *cpu, int level);
void m68k_load_trap_table(m68k_cpu_t *cpu);

/* --- JIT integration (m68k_jit.c, device only) --- */
extern int g_m68k_accel_mode;            /* 0 = interpreter, 1 = JIT */
/* Set up the JIT cache/tables and start the core-0 async compiler. Call once at
 * boot (after the ROM is loaded) when the JIT is enabled. No-op on host builds. */
void m68k_jit_init(void);
void m68k_execute_interp(m68k_cpu_t *cpu, int target_cycles); /* interp fallback */
void m68k_jit_fallback(m68k_cpu_t *cpu); /* interpret one instruction at pc */
bool m68k_service_pending(m68k_cpu_t *cpu, int *cyc); /* STOP/IRQ servicing */

/* Hot-block presence filter: a direct bitmap (1 hashed bit per PC) the JIT sets
 * when it compiles a block, so the interpreter burst can decide per-op — with a
 * single inlined load+test, no cross-TU call — whether a PC might have native
 * code and hand control back. Shared hash, so m68k.c (check) and m68k_jit.c
 * (set) must agree. A set bit may be a hash false-positive; jit_lookup confirms. */
#define JIT_PRESENT_BITS  (1u << 17)     /* 131072 bits = 16 KB, fits L1 ext cache */
#define JIT_PRESENT_HASH(mpc) ((((mpc) >> 1) * 2654435761u) & (JIT_PRESENT_BITS - 1))
extern uint8_t *g_jit_present;           /* NULL until the JIT initialises */
/* Run the inlined interpreter until it reaches a PC whose presence bit is set
 * (returns so the JIT can run the native block), or a small op budget / the
 * cycle target is hit. Returns cycles executed. Device-only. */
int m68k_jit_interp_burst(m68k_cpu_t *cpu, int target_cyc);

/* --- Internal (used by instruction handlers) --- */
void m68k_take_exception(m68k_cpu_t *cpu, int vector);
void m68k_address_error(m68k_cpu_t *cpu, uint32_t addr);
void m68k_illegal(m68k_cpu_t *cpu);
/* Write a full 16-bit SR, swapping the active A7 with USP/SSP on an S-bit change.
 * The single correct place to apply an SR write; used by the JIT (RTE) and by the
 * privileged SR-writing interpreter ops (ANDI/ORI/EORI #imm,SR, STOP). */
void m68k_jit_set_sr(m68k_cpu_t *cpu, uint32_t value);

uint16_t m68k_fetch16(m68k_cpu_t *cpu);
uint32_t m68k_fetch32(m68k_cpu_t *cpu);

ea_result_t m68k_resolve_ea(m68k_cpu_t *cpu, int mode, int reg, int size);
uint32_t m68k_read_ea(m68k_cpu_t *cpu, ea_result_t *ea, int size);
void m68k_write_ea(m68k_cpu_t *cpu, ea_result_t *ea, uint32_t val, int size);

/* --- CCR helpers ---
 * Hot path: called several times per ALU/MOVE instruction from every opcode
 * group. Defined inline here (rather than out-of-line in m68k.c) so the
 * compiler folds them into the handlers instead of emitting a windowed call
 * per use — on the Xtensa register-window ABI that call overhead is real at
 * ~16M instructions/sec. */
static inline uint32_t m68k_size_mask(int size) {
    static const uint32_t masks[] = { 0xFF, 0xFFFF, 0xFFFFFFFF };
    return masks[size];
}

static inline uint32_t m68k_sign_bit(int size) {
    static const uint32_t bits[] = { 0x80, 0x8000, 0x80000000 };
    return bits[size];
}

/* Evaluate a 68k condition code (0-15) against the status register. Hot: used
 * by Bcc (group 6) and Scc/DBcc (group 5), so inline here alongside the other
 * CCR helpers rather than duplicating the switch per opcode group. */
static inline bool m68k_eval_cc(uint16_t sr, int cc) {
    bool c = (sr & SR_C) != 0;
    bool v = (sr & SR_V) != 0;
    bool z = (sr & SR_Z) != 0;
    bool n = (sr & SR_N) != 0;

    switch (cc) {
    case  0: return true;            /* T */
    case  1: return false;           /* F */
    case  2: return !c && !z;        /* HI */
    case  3: return c || z;          /* LS */
    case  4: return !c;              /* CC */
    case  5: return c;               /* CS */
    case  6: return !z;              /* NE */
    case  7: return z;               /* EQ */
    case  8: return !v;              /* VC */
    case  9: return v;               /* VS */
    case 10: return !n;              /* PL */
    case 11: return n;               /* MI */
    case 12: return n == v;          /* GE */
    case 13: return n != v;          /* LT */
    case 14: return !z && (n == v);  /* GT */
    case 15: return z || (n != v);   /* LE */
    }
    return false;
}

void m68k_set_flags_move(m68k_cpu_t *cpu, uint32_t result, int size);
void m68k_set_flags_add(m68k_cpu_t *cpu, uint32_t src, uint32_t dst, uint32_t result, int size);
void m68k_set_flags_sub(m68k_cpu_t *cpu, uint32_t src, uint32_t dst, uint32_t result, int size);
void m68k_set_flags_logic(m68k_cpu_t *cpu, uint32_t result, int size);
void m68k_set_flags_cmp(m68k_cpu_t *cpu, uint32_t src, uint32_t dst, uint32_t result, int size);

/* --- Decode dispatch --- */
typedef void (*m68k_op_handler_t)(m68k_cpu_t *cpu, uint16_t opcode);
void m68k_init_decode_table(void);
