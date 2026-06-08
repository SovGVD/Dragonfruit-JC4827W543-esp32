#include "m68k.h"
#include "bus.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG_CPU = "m68k";
static int dbg_irq_n = 0;   /* bounded interrupt-dispatch / STOP log counter */

/* Group handlers, dispatched by a direct switch in m68k_execute (a jump table,
 * no indirect CALLX). The hot, small groups are marked M68K_AINLINE so they
 * fold straight into the dispatch loop — eliminating a windowed Xtensa call per
 * instruction. The remaining groups (rare/large: imm-ALU, misc, OR/DIV, A/F
 * line, shifts) stay out-of-line. These are the only references to the group
 * handlers (the old function-pointer table is gone), so the inlined ones can be
 * static with no out-of-line copy emitted. Definitions live in their opcode-
 * group files (compiled into the same unity TU, m68k_all.c). */
M68K_AINLINE void m68k_group1(m68k_cpu_t *cpu, uint16_t opcode);  /* MOVE.B */
M68K_AINLINE void m68k_group2(m68k_cpu_t *cpu, uint16_t opcode);  /* MOVE.L */
M68K_AINLINE void m68k_group3(m68k_cpu_t *cpu, uint16_t opcode);  /* MOVE.W */
M68K_AINLINE void m68k_group5(m68k_cpu_t *cpu, uint16_t opcode);  /* ADDQ/SUBQ/Scc/DBcc */
M68K_AINLINE void m68k_group6(m68k_cpu_t *cpu, uint16_t opcode);  /* Bcc/BRA/BSR */
M68K_AINLINE void m68k_group7(m68k_cpu_t *cpu, uint16_t opcode);  /* MOVEQ */
M68K_AINLINE void m68k_group9(m68k_cpu_t *cpu, uint16_t opcode);  /* SUB/SUBX/SUBA */
M68K_AINLINE void m68k_group_b(m68k_cpu_t *cpu, uint16_t opcode); /* CMP/CMPM/EOR */
M68K_AINLINE void m68k_group_c(m68k_cpu_t *cpu, uint16_t opcode); /* AND/MUL/EXG */
M68K_AINLINE void m68k_group_d(m68k_cpu_t *cpu, uint16_t opcode); /* ADD/ADDX/ADDA */
extern void m68k_group0(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_group4(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_group8(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_group_a(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_group_e(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_group_f(m68k_cpu_t *cpu, uint16_t opcode);

/* Hottest group-4 opcodes (subroutine call/return and jump) get force-inlined
 * into the dispatch switch's case 0x4 so the most frequent control-flow ops
 * skip both the windowed call into m68k_group4 and its long decode cascade. */
M68K_AINLINE void m68k_op_jmp(m68k_cpu_t *cpu, uint16_t opcode);
M68K_AINLINE void m68k_op_jsr(m68k_cpu_t *cpu, uint16_t opcode);
M68K_AINLINE void m68k_op_rts(m68k_cpu_t *cpu, uint16_t opcode);

/* --- Trace ring buffer --- */
#define TRACE_BUF_SIZE 256
typedef struct {
    uint32_t pc;
    uint16_t opcode;
    uint32_t a7;
    uint32_t d0;
} trace_entry_t;

static trace_entry_t trace_buf[TRACE_BUF_SIZE];
static int trace_idx = 0;

/* "UI is up" gate, mirroring CloudpilotEmu's IsUIInitialized(). The auto-installer and
 * pen/key injection wait on this — driving the OS (trap calls, event injection) before the
 * UI subsystem is initialised reenters it mid-init and corrupts its state (a task stack ends
 * up overlapping an AMX system pool, crashing after the work completes).
 *
 * Read it straight from the OS's own low-memory flag rather than watching the instruction
 * stream: Palm OS sets sysMiscFlags bit sysMiscFlagUIInitialized (0x40) the instant
 * UIInitialize (sysTrap 0xA1A6) returns — in this ROM, `ori.w #$40,($270).w` at 0x10C0DE02,
 * the instruction immediately after the trap. A plain RAM read is correct no matter how the
 * code that set it ran (interpreter, JIT native block, or cold burst); the old trap-return
 * watch was not — once the JIT compiled a native block at the trap's return PC, the watch
 * (interpreter-only) never fired and the gate latched shut. sysMiscFlags is the 16-bit word
 * at low-mem 0x270 (0x100 exception-vector table + 0x170 field offset in FixedGlobalsType). */
#define PALM_LOWMEM_SYS_MISC_FLAGS   0x00000270u
#define PALM_SYS_MISC_FLAG_UI_INITED 0x0040u

bool m68k_ui_initialized(void)
{
    return (bus_read16(PALM_LOWMEM_SYS_MISC_FLAGS) & PALM_SYS_MISC_FLAG_UI_INITED) != 0;
}

/* Re-arm the gate on reset. Emulated RAM is preserved across a warm reset (and cold-boot
 * PSRAM is not guaranteed zero), so a stale UIInitialized bit could read "UI up" before the
 * OS re-runs UIInitialize. Clear just that bit — all other sysMiscFlags bits are preserved —
 * and the OS re-sets it once UIInitialize completes again. (This only re-gates pen/key
 * injection during the re-init window; it cannot trigger an install, which is gated
 * separately by the installer's own phase machine.) */
static inline void m68k_ui_gate_rearm(void)
{
    bus_write16(PALM_LOWMEM_SYS_MISC_FLAGS,
                bus_read16(PALM_LOWMEM_SYS_MISC_FLAGS) & ~PALM_SYS_MISC_FLAG_UI_INITED);
}

static void dump_trace(const char *reason)
{
    ESP_LOGE(TAG_CPU, "=== TRACE DUMP: %s (last %d insns) ===", reason, TRACE_BUF_SIZE);
    for (int i = 0; i < TRACE_BUF_SIZE; i++) {
        int idx = (trace_idx + i) % TRACE_BUF_SIZE;
        if (trace_buf[idx].pc == 0 && trace_buf[idx].opcode == 0)
            continue;
        ESP_LOGE(TAG_CPU, "  [%02d] PC=%08lX op=%04X A7=%08lX D0=%08lX",
                 i, (unsigned long)trace_buf[idx].pc,
                 trace_buf[idx].opcode,
                 (unsigned long)trace_buf[idx].a7,
                 (unsigned long)trace_buf[idx].d0);
    }
}

/* ---- Host-invoked ROM subroutine calls (CloudPilot EmSubroutine analog) ----
 * Lets the host call a Palm OS trap handler directly (Palm convention: args
 * pushed right-to-left, result in D0).  Used to inject input via the stable,
 * device-independent EvtEnqueuePenPoint / PenScreenToRaw syscalls instead of
 * emulating each device's digitizer hardware.  The handler runs in a nested
 * single-step loop until it returns to a sentinel address.  Interrupts are
 * masked during the call — this mirrors the real pen ISR, which is the normal
 * caller of EvtEnqueuePenPoint, so the call is reentrancy-safe. */
#define ROM_CALL_SENTINEL 0x00BADD1Eu   /* even; >RAM and <ROM, never real code */

/* Runaway guard: a well-behaved ROM trap returns to the sentinel in far fewer
 * steps than this. If it trips, the handler never returned — see below. */
#define ROM_CALL_MAX_STEPS 4000000

/* Single-step the CPU until PC lands on the sentinel (the handler's terminating
 * RTS) or the runaway guard trips. Returns false on normal completion, true on
 * TIMEOUT. This is the one place this loop lives — every host->ROM call helper
 * below funnels through it, so the cap and the (previously silent) timeout
 * handling stay in a single spot. Stays correct under the JIT: the sentinel is a
 * non-ROM address, so it is never inside/the entry of a compiled block — the
 * handler's RTS to it always ends the block and returns here to test PC. */
static bool rom_run_to_sentinel(m68k_cpu_t *cpu)
{
    for (int g = 0; g < ROM_CALL_MAX_STEPS; g++) {
        if ((cpu->regs.pc & 0x00FFFFFF) == (ROM_CALL_SENTINEL & 0x00FFFFFF))
            return false;
        m68k_execute(cpu, 1);
    }
    ESP_LOGE(TAG_CPU, "rom call timed out: PC=%08lX never reached sentinel",
             (unsigned long)cpu->regs.pc);
    return true;
}

static uint32_t rom_subcall(m68k_cpu_t *cpu, uint32_t addr, uint32_t arg)
{
    uint32_t sp = cpu->regs.a[7];
    sp -= 4; bus_write32(sp, arg);                 /* the single pointer arg   */
    sp -= 4; bus_write32(sp, ROM_CALL_SENTINEL);   /* sentinel return address  */
    cpu->regs.a[7] = sp;
    cpu->regs.pc = addr;
    cpu->regs.stopped = false;
    rom_run_to_sentinel(cpu);
    return cpu->regs.d[0];
}

/* Inject one pen point: build a PointType{Int16 x,y} on the stack, optionally
 * convert screen->raw via PenScreenToRaw, then EvtEnqueuePenPoint.  penUp=true
 * enqueues the (-1,-1) pen-up sentinel (no transform).  All CPU state is saved
 * and restored, so the interrupted program resumes unaffected. */
void m68k_pen_tap(m68k_cpu_t *cpu, uint32_t evtEnqAddr, uint32_t penScreenToRawAddr,
                  int16_t x, int16_t y, bool penUp)
{
    /* Don't inject before the UI subsystem is up (mirrors CloudpilotEmu gating event
     * promotion on IsUIInitialized). EvtEnqueuePenPoint runs the OS's event path; doing
     * it mid-boot reenters code that isn't ready. UIInitialize completes ~1-2 s into
     * boot, long before any real tap, so this never drops a user pen event. */
    if (!m68k_ui_initialized()) {
        static bool warned = false;        /* one-shot: signal early-tap drops without spam */
        if (!warned) { warned = true; ESP_LOGW(TAG_CPU, "pen inject dropped: UI not up yet"); }
        return;
    }

    m68k_regs_t saved = cpu->regs;
    cpu->regs.sr |= 0x2700;                 /* supervisor + mask IRQs (ISR-like) */
    cpu->regs.stopped = false;

    uint32_t ptP = (cpu->regs.a[7] - 4) & ~1u;   /* PointType, kept across calls */
    bus_write16(ptP,     (uint16_t)x);
    bus_write16(ptP + 2, (uint16_t)y);

    if (!penUp && penScreenToRawAddr) {
        cpu->regs.a[7] = ptP;               /* call frames live below ptP */
        rom_subcall(cpu, penScreenToRawAddr, ptP);   /* screen -> raw, in place */
    }
    cpu->regs.a[7] = ptP;
    rom_subcall(cpu, evtEnqAddr, ptP);

    cpu->regs = saved;                      /* full restore */
}

/* Call an arbitrary ROM routine (Palm convention: one 32-bit arg pushed,
 * result returned in D0) as a self-contained subcall.  Interrupts are masked
 * and all CPU state is saved/restored, so the interrupted program resumes
 * unaffected.  For void/no-arg routines pass arg = 0. */
uint32_t m68k_rom_call(m68k_cpu_t *cpu, uint32_t addr, uint32_t arg)
{
    m68k_regs_t saved = cpu->regs;
    cpu->regs.sr |= 0x2700;                 /* supervisor + mask IRQs */
    cpu->regs.stopped = false;
    uint32_t ret = rom_subcall(cpu, addr, arg);
    cpu->regs = saved;                      /* full restore */
    return ret;
}

/* Enqueue a key event via EvtEnqueueKey(WChar ascii, UInt16 keycode,
 * UInt16 modifiers) — Palm trap 0xA12D.  Used to inject the silkscreen
 * "Applications/Home" virtual key (vchrLaunch 0x0108 + commandKeyMask) which has
 * no hardware key on the IIIx.  A thin wrapper over m68k_call_trap with three
 * word args (which pushes them right-to-left so ascii lands at 4(sp), keycode at
 * 6(sp), modifiers at 8(sp)); the mask + full save/restore live there. */
void m68k_key_enqueue(m68k_cpu_t *cpu, uint32_t evtEnqKeyAddr,
                      uint16_t ascii, uint16_t keycode, uint16_t modifiers)
{
    if (!m68k_ui_initialized()) {    /* don't inject before the UI is up (see m68k_pen_tap) */
        static bool warned = false;
        if (!warned) { warned = true; ESP_LOGW(TAG_CPU, "key inject dropped: UI not up yet"); }
        return;
    }
    m68k_arg_t a[] = {
        { ascii, M68K_ARG_W }, { keycode, M68K_ARG_W }, { modifiers, M68K_ARG_W },
    };
    m68k_call_trap(cpu, evtEnqKeyAddr, a, 3, NULL, NULL);
}

/* Generalized host->ROM trap call (see m68k.h).  Pushes the argument list
 * right-to-left (2 bytes for a word arg, 4 for a long/pointer), then the
 * sentinel return address, sets PC to the resolved trap handler, and
 * single-steps (via rom_run_to_sentinel) until it returns to the sentinel.  SP
 * starts even and every push is a multiple of 2, so the stack stays
 * word-aligned.  Returns D0; if a0_out is non-NULL it also reports A0 (Palm's
 * pointer/handle-returning traps — MemPtrNew, DmNewHandle, DmNewRecord/Resource,
 * DmOpenDatabase, MemHandleLock — deliver their result in A0).  If timed_out is
 * non-NULL it reports whether the runaway guard tripped (the handler never
 * returned) so callers driving traps on untrusted input — the installer — can
 * abort cleanly instead of using a garbage result.  Full register save/restore
 * makes caller stack cleanup automatic. */
uint32_t m68k_call_trap(m68k_cpu_t *cpu, uint32_t handler,
                        const m68k_arg_t *args, int nargs,
                        uint32_t *a0_out, bool *timed_out)
{
    m68k_regs_t saved = cpu->regs;
    cpu->regs.sr |= 0x2700;                 /* supervisor + mask IRQs */
    cpu->regs.stopped = false;
    /* IRQs are masked and the caller advances no timer while we single-step the
     * handler, so emulated time effectively pauses for the call's duration. Benign
     * here: it is brief, and RTC time-of-day is host-wall-clock-driven, not tick-
     * derived (see rtc.c) — only the free-running tick count stalls. */

    uint32_t sp = cpu->regs.a[7];
    for (int i = nargs - 1; i >= 0; i--) {
        if (args[i].size == M68K_ARG_L) { sp -= 4; bus_write32(sp, args[i].value); }
        else                            { sp -= 2; bus_write16(sp, (uint16_t)args[i].value); }
    }
    sp -= 4; bus_write32(sp, ROM_CALL_SENTINEL);
    cpu->regs.a[7] = sp;
    cpu->regs.pc   = handler;

    bool to = rom_run_to_sentinel(cpu);

    uint32_t d0 = cpu->regs.d[0];
    uint32_t a0 = cpu->regs.a[0];
    cpu->regs = saved;                      /* full restore (incl. A7) */
    if (a0_out)    *a0_out = a0;
    if (timed_out) *timed_out = to;
    return d0;
}

/* Print the last n executed instruction PCs (true inst_pc, from the host trace
 * ring).  Used to locate peripheral-register accesses precisely. */
void m68k_dump_recent(int n)
{
    if (n > TRACE_BUF_SIZE) n = TRACE_BUF_SIZE;
    for (int i = TRACE_BUF_SIZE - n; i < TRACE_BUF_SIZE; i++) {
        int idx = (trace_idx + i) % TRACE_BUF_SIZE;
        fprintf(stderr, "    trace[-%2d] PC=%08lX op=%04X\n",
                TRACE_BUF_SIZE - i, (unsigned long)trace_buf[idx].pc,
                trace_buf[idx].opcode);
    }
}

void m68k_init(m68k_cpu_t *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->regs.pending_irq = -1;
    cpu->native_dispatch = false;
    m68k_init_decode_table();
}

void m68k_reset(m68k_cpu_t *cpu)
{
    memset(&cpu->regs, 0, sizeof(cpu->regs));
    cpu->regs.pending_irq = -1;
    cpu->regs.a[7] = bus_read32(0x00000000);
    cpu->regs.pc   = bus_read32(0x00000004);
    cpu->regs.sr = SR_S | SR_I2 | SR_I1 | SR_I0;
    cpu->regs.ssp = cpu->regs.a[7];
    m68k_ui_gate_rearm();   /* re-arm the UIInitialize gate (clear the OS UIInitialized bit) */
}

/* Warm ("soft") reset — see m68k.h. Re-runs the 68k reset WITHOUT touching RAM,
 * so storage databases and the ROM's warm-boot signature survive. The reset
 * vector is fetched from ROM (not RAM 0x0/0x4, which the OS reuses after boot);
 * bus_read32 masks to the 24-bit bus, so BUS_ROM_BASE reads ROM file byte 0. */
void m68k_warm_reset(m68k_cpu_t *cpu)
{
    uint32_t ssp = bus_read32(BUS_ROM_BASE);
    uint32_t pc  = bus_read32(BUS_ROM_BASE + 4);
    memset(&cpu->regs, 0, sizeof(cpu->regs));
    cpu->regs.pending_irq = -1;
    cpu->regs.a[7] = ssp;
    cpu->regs.ssp  = ssp;
    cpu->regs.pc   = pc;
    cpu->regs.sr   = SR_S | SR_I2 | SR_I1 | SR_I0;
    m68k_ui_gate_rearm();   /* re-arm the gate; UI re-inits after a warm reset */
}

/* Write a full 16-bit SR value, swapping the active A7 with USP/SSP when the
 * supervisor bit changes (the one piece the JIT can't safely inline). Used by the
 * JIT's ANDI/ORI/EORI #imm,SR codegen so the privileged stack handling stays in
 * one place. Privilege checking is the caller's concern (the ROM runs these in
 * supervisor mode); this just applies the value and SP swap. */
void m68k_jit_set_sr(m68k_cpu_t *cpu, uint32_t value)
{
    uint16_t v = (uint16_t)value;
    if ((cpu->regs.sr & SR_S) && !(v & SR_S)) {
        cpu->regs.ssp = cpu->regs.a[7];
        cpu->regs.a[7] = cpu->regs.usp;
    } else if (!(cpu->regs.sr & SR_S) && (v & SR_S)) {
        cpu->regs.usp = cpu->regs.a[7];
        cpu->regs.a[7] = cpu->regs.ssp;
    }
    cpu->regs.sr = v;
}

void m68k_take_exception(m68k_cpu_t *cpu, int vector)
{
    uint16_t old_sr = cpu->regs.sr;
    if (!(cpu->regs.sr & SR_S)) {
        cpu->regs.usp = cpu->regs.a[7];
        cpu->regs.a[7] = cpu->regs.ssp;
        cpu->regs.sr |= SR_S;
    }
    cpu->regs.sr &= ~SR_T;
    cpu->regs.a[7] -= 4;
    bus_write32(cpu->regs.a[7], cpu->regs.pc);
    cpu->regs.a[7] -= 2;
    bus_write16(cpu->regs.a[7], old_sr);
    cpu->regs.pc = bus_read32((uint32_t)vector * 4);
    cpu->regs.stopped = false;
}

void m68k_address_error(m68k_cpu_t *cpu, uint32_t addr)
{
    (void)addr;
    m68k_take_exception(cpu, VEC_ADDRESS_ERROR);
    cpu->regs.cycles += 50;
}

void m68k_illegal(m68k_cpu_t *cpu)
{
    cpu->regs.pc = cpu->regs.inst_pc;
    static int illegal_count = 0;
    if (illegal_count < 3) {
        ESP_LOGE(TAG_CPU, "ILLEGAL at PC=%08lX op=%04X SR=%04X",
                 (unsigned long)cpu->regs.pc,
                 bus_read16(cpu->regs.pc & 0x00FFFFFF), cpu->regs.sr);
        ESP_LOGE(TAG_CPU, "  D: %08lX %08lX %08lX %08lX A: %08lX %08lX %08lX %08lX",
                 (unsigned long)cpu->regs.d[0], (unsigned long)cpu->regs.d[1],
                 (unsigned long)cpu->regs.d[2], (unsigned long)cpu->regs.d[3],
                 (unsigned long)cpu->regs.a[0], (unsigned long)cpu->regs.a[1],
                 (unsigned long)cpu->regs.a[2], (unsigned long)cpu->regs.a[3]);
        ESP_LOGE(TAG_CPU, "  A4-7: %08lX %08lX %08lX %08lX",
                 (unsigned long)cpu->regs.a[4], (unsigned long)cpu->regs.a[5],
                 (unsigned long)cpu->regs.a[6], (unsigned long)cpu->regs.a[7]);
        if (illegal_count == 0)
            dump_trace("ILLEGAL");
        illegal_count++;
    }
    m68k_take_exception(cpu, VEC_ILLEGAL);
    cpu->regs.cycles += 34;
}

void m68k_set_irq(m68k_cpu_t *cpu, int level)
{
    cpu->regs.pending_irq = level;
}

uint16_t m68k_fetch16(m68k_cpu_t *cpu)
{
    uint16_t val = bus_read16(cpu->regs.pc);
    cpu->regs.pc += 2;
    return val;
}

uint32_t m68k_fetch32(m68k_cpu_t *cpu)
{
    uint32_t val = bus_read32(cpu->regs.pc);
    cpu->regs.pc += 4;
    return val;
}

/* Non-inline bus wrappers the JIT calls for emulated memory access (the inline
 * bus_read/write in bus.h have no callable address). Each inlines the bus
 * fast-path into a standalone function. */
uint32_t jit_bus_read8 (uint32_t a) { return bus_read8(a);  }
uint32_t jit_bus_read16(uint32_t a) { return bus_read16(a); }
uint32_t jit_bus_read32(uint32_t a) { return bus_read32(a); }
void jit_bus_write8 (uint32_t a, uint32_t v) { bus_write8(a, (uint8_t)v);  }
void jit_bus_write16(uint32_t a, uint32_t v) { bus_write16(a, (uint16_t)v); }
void jit_bus_write32(uint32_t a, uint32_t v) { bus_write32(a, v); }

/* --- JIT integration hooks -------------------------------------------------
 * g_m68k_accel_mode selects the execution engine at runtime so the interpreter
 * and the native JIT can be A/B compared. m68k_execute() trampolines to the JIT
 * dispatcher (components/m68k/m68k_jit.c, device only) when enabled, else the
 * interpreter (always-available fallback). */
int g_m68k_accel_mode = 0;   /* 0 = interpreter, 1 = JIT */

/* Interpret exactly one instruction at cpu->regs.pc (the JIT calls this for ops
 * it does not translate natively). Mirrors the interpreter's per-instruction
 * body: same dispatch, same cycle floor. pc must point at the opcode on entry;
 * on return pc has advanced (and may have branched) and cpu->regs.cycles holds
 * the instruction's cost. static inline so both the single-step fallback and the
 * interpreter burst get the fully-inlined dispatch (the M68K_AINLINE group
 * handlers fold in) with no call overhead. */
static inline void interp_one(m68k_cpu_t *cpu)
{
    cpu->regs.cycles = 0;
    cpu->regs.inst_pc = cpu->regs.pc;
    uint16_t opcode = bus_read16(cpu->regs.pc);
    cpu->regs.pc += 2;
    switch (opcode >> 12) {
    case 0x0: m68k_group0(cpu, opcode);  break;
    case 0x1: m68k_group1(cpu, opcode);  break;
    case 0x2: m68k_group2(cpu, opcode);  break;
    case 0x3: m68k_group3(cpu, opcode);  break;
    case 0x4:
        if (opcode == 0x4E75)                 m68k_op_rts(cpu, opcode);
        else if ((opcode & 0xFFC0) == 0x4E80) m68k_op_jsr(cpu, opcode);
        else if ((opcode & 0xFFC0) == 0x4EC0) m68k_op_jmp(cpu, opcode);
        else                                  m68k_group4(cpu, opcode);
        break;
    case 0x5: m68k_group5(cpu, opcode);  break;
    case 0x6: m68k_group6(cpu, opcode);  break;
    case 0x7: m68k_group7(cpu, opcode);  break;
    case 0x8: m68k_group8(cpu, opcode);  break;
    case 0x9: m68k_group9(cpu, opcode);  break;
    case 0xA: m68k_group_a(cpu, opcode); break;
    case 0xB: m68k_group_b(cpu, opcode); break;
    case 0xC: m68k_group_c(cpu, opcode); break;
    case 0xD: m68k_group_d(cpu, opcode); break;
    case 0xE: m68k_group_e(cpu, opcode); break;
    case 0xF: m68k_group_f(cpu, opcode); break;
    }
    if (cpu->regs.cycles == 0)
        cpu->regs.cycles = 4;
}

void m68k_jit_fallback(m68k_cpu_t *cpu) { interp_one(cpu); }

uint8_t *g_jit_present;   /* hot-block presence bitmap; allocated by the JIT */

/* Enter an interrupt exception of `level`. Shared by both dispatch sites — the JIT path
 * (m68k_service_pending) and the pure-interpreter loop (m68k_execute_interp) — so the
 * load-bearing ordering lives in exactly one place. Stack the exception frame, which saves
 * the CURRENT SR, BEFORE raising IPL, so the frame records the interrupted code's own IPL;
 * raising IPL first would stack the new level and the RTE would resume the interrupted code
 * at the interrupt's IPL instead of its own — silently masking lower interrupts for any code
 * that does not re-lower IPL (a tight loop spinning on a timer-driven tick). DragonBall is
 * vectored (vector = (IVR & 0xF8) + level); fall back to the 68000 autovector when no base
 * callback is wired. The caller adds the standard 44-cycle cost. */
static void m68k_enter_interrupt(m68k_cpu_t *cpu, int level)
{
    int vector = cpu->vector_base_callback
                     ? (int)(cpu->vector_base_callback() + level)
                     : (VEC_AUTOVECTOR_BASE + level - 1);
    cpu->regs.irq_count++;
    m68k_take_exception(cpu, vector);
    cpu->regs.sr = (cpu->regs.sr & ~SR_I_MASK) | ((uint16_t)level << 8);
}

/* STOP/interrupt servicing for the JIT dispatcher — the same logic the
 * interpreter runs at the top of its loop, factored out (minus host-only debug
 * logging). Returns true if the caller should `continue` (CPU still stopped, or
 * an interrupt was just taken). */
bool m68k_service_pending(m68k_cpu_t *cpu, int *cyc)
{
    if (cpu->regs.stopped) {
        *cyc += 4;
        if (cpu->tick_callback)
            cpu->tick_callback(4);
        if (cpu->irq_callback) {
            int level = cpu->irq_callback();
            cpu->regs.pending_irq = (level > 0) ? level : 0;
        }
        if (cpu->regs.pending_irq > 0 &&
            cpu->regs.pending_irq > ((cpu->regs.sr & SR_I_MASK) >> 8)) {
            cpu->regs.stopped = false;
            if (cpu->resume_callback) cpu->resume_callback();
        } else if (cpu->wake_callback && cpu->wake_callback()) {
            cpu->regs.stopped = false;
            if (cpu->resume_callback) cpu->resume_callback();
        } else {
            return true;   /* still stopped */
        }
    }
    if (cpu->regs.pending_irq > 0) {
        int mask = (cpu->regs.sr & SR_I_MASK) >> 8;
        if (cpu->regs.pending_irq > mask || cpu->regs.pending_irq == 7) {
            m68k_enter_interrupt(cpu, cpu->regs.pending_irq);
            *cyc += 44;
            return true;
        }
    }
    return false;
}

#ifndef HOST_BUILD
/* True if mpc (24-bit masked PC) might have a compiled native block — a single
 * inlined load+test against the presence bitmap. Only ROM PCs are ever
 * compiled, so the range check also keeps RAM/bootstrap code on the interpreter. */
static inline bool jit_present(uint32_t mpc)
{
    if (mpc < BUS_ROM_LO || mpc >= g_bus_rom_end) return false;
    uint32_t i = JIT_PRESENT_HASH(mpc);
    return (g_jit_present[i >> 3] >> (i & 7)) & 1u;
}

/* Interpreter burst: the same inlined dispatch the pure interpreter runs, but it
 * returns to the JIT dispatcher as soon as it reaches a PC that may have a
 * native block (presence bit set), or after a small op budget (so the dispatcher
 * can sample the PC for compilation), or when the cycle target is met. This is
 * what makes "fallback" run at full interpreter speed instead of paying a
 * cross-TU call + hash probe per op. */
#define JIT_BURST_MAX_OPS 64
int m68k_jit_interp_burst(m68k_cpu_t *cpu, int target_cyc)
{
    int cyc = 0, ops = 0;
    uint32_t last_periph_seq = g_bus_periph_seq;
    while (cyc < target_cyc) {
        /* Inline the common "nothing pending" case (the pure interpreter does
         * too); only call the full servicer when stopped or an IRQ is waiting. */
        if ((cpu->regs.stopped || cpu->regs.pending_irq) &&
            m68k_service_pending(cpu, &cyc)) continue;
        if (jit_present(cpu->regs.pc & BUS_ADDR_MASK)) break;
        interp_one(cpu);
        cyc += cpu->regs.cycles;
        if (g_bus_periph_seq != last_periph_seq && cpu->irq_callback) {
            last_periph_seq = g_bus_periph_seq;
            int level = cpu->irq_callback();
            cpu->regs.pending_irq = (level > 0) ? level : 0;
        }
        if (++ops >= JIT_BURST_MAX_OPS) break;
    }
    return cyc;
}
#endif

/* `cpu` is restrict-qualified: the CPU register file is a separate allocation
 * the emulated RAM/ROM (g_bus_ram/g_bus_rom) never overlaps, so the compiler
 * may keep cpu->regs.* in registers across emulated memory writes rather than
 * reloading after each uint8_t* store. (Measured: no effect on the xtensa-gcc
 * build — the per-instruction floor is raw work + in-order pipeline latency,
 * not aliasing reloads — but the annotation is correct and harmless.) */
void m68k_execute_interp(m68k_cpu_t *M68K_RESTRICT cpu, int target_cycles)
{
    /* Keep the batch cycle counter in a local: handlers never read it, but the
     * compiler can't prove that across the opaque handler calls, so as a struct
     * field it gets reloaded/stored every iteration. A local stays in a
     * register. Written back at the end (nothing outside reads it mid-batch). */
    int cyc = 0;

    /* Pending-interrupt state (db.ipr & ~db.imr) can only change mid-batch as a
     * side effect of an instruction touching a peripheral register; the timer/
     * RTC advance and UART refill happen between batches in the CPU task, which
     * re-sets pending_irq via m68k_set_irq. So instead of an indirect
     * irq_callback() after every instruction, recompute only after instructions
     * that bumped the bus peripheral-access counter — the common RAM/ROM-only
     * instruction skips it entirely. pending_irq is seeded by the caller's
     * m68k_set_irq before entry, so no recompute is needed at the top. */
    uint32_t last_periph_seq = g_bus_periph_seq;

    while (cyc < target_cycles) {
        if (cpu->regs.stopped) {
            cyc += 4;
            if (cpu->tick_callback)
                cpu->tick_callback(4);
            if (cpu->irq_callback) {
                int level = cpu->irq_callback();
                cpu->regs.pending_irq = (level > 0) ? level : 0;
            }
            if (cpu->regs.pending_irq > 0 &&
                cpu->regs.pending_irq > ((cpu->regs.sr & SR_I_MASK) >> 8)) {
                cpu->regs.stopped = false;
                if (cpu->resume_callback) cpu->resume_callback();
                if (dbg_irq_n < 30) { dbg_irq_n++;
                    fprintf(stderr, "[CPU] WAKE(irq) pc=%08lX sr=%04X pend=%d\n",
                            (unsigned long)cpu->regs.pc, cpu->regs.sr, cpu->regs.pending_irq); }
            } else if (cpu->wake_callback && cpu->wake_callback()) {
                cpu->regs.stopped = false;
                if (cpu->resume_callback) cpu->resume_callback();
                if (dbg_irq_n < 30) { dbg_irq_n++;
                    fprintf(stderr, "[CPU] WAKE(iwr) pc=%08lX sr=%04X pend=%d\n",
                            (unsigned long)cpu->regs.pc, cpu->regs.sr, cpu->regs.pending_irq); }
            } else {
                continue;
            }
        }

        if (cpu->regs.pending_irq > 0) {
            int mask = (cpu->regs.sr & SR_I_MASK) >> 8;
            if (cpu->regs.pending_irq > mask || cpu->regs.pending_irq == 7) {
                int level = cpu->regs.pending_irq;
                if (dbg_irq_n < 30) { dbg_irq_n++;
                    extern uint32_t db_int_ivr_raw(void);
                    /* DragonBall is vectored: vector = (IVR & 0xF8) + level; fall back
                     * to the 68000 autovector when no base callback (recomputed here for
                     * the log only — m68k_enter_interrupt derives it the same way). */
                    int vector = cpu->vector_base_callback
                                     ? (int)(cpu->vector_base_callback() + level)
                                     : (VEC_AUTOVECTOR_BASE + level - 1);
                    fprintf(stderr, "[CPU] IRQ lvl=%d pc=%08lX sr=%04X | IVR=%02lX vec=%d->%08lX | vec70->%08lX vec64->%08lX\n",
                            level, (unsigned long)cpu->regs.inst_pc, cpu->regs.sr, (unsigned long)db_int_ivr_raw(),
                            vector, (unsigned long)bus_read32((uint32_t)vector * 4),
                            (unsigned long)bus_read32(70 * 4), (unsigned long)bus_read32(64 * 4)); }
                m68k_enter_interrupt(cpu, level);   /* stack-frame-then-raise-IPL; see helper */
                cyc += 44;
                /* pending_irq is recomputed fresh from irq_callback after every
                 * instruction (see below), so it tracks live hardware state and
                 * is not sticky — a masked/cleared source stops re-dispatching. */
                continue;
            }
        }

        cpu->regs.cycles = 0;
        cpu->regs.inst_pc = cpu->regs.pc;
        uint16_t opcode = bus_read16(cpu->regs.pc);
        cpu->regs.pc += 2;

#ifdef HOST_BUILD
        /* Per-instruction trace ring. Host-only diagnostics; compiled out of the
         * device build (this runs ~16M times/sec). */
        trace_buf[trace_idx].pc = cpu->regs.inst_pc;
        trace_buf[trace_idx].opcode = opcode;
        trace_buf[trace_idx].a7 = cpu->regs.a[7];
        trace_buf[trace_idx].d0 = cpu->regs.d[0];
        trace_idx = (trace_idx + 1) % TRACE_BUF_SIZE;
#endif

        /* Stack-depth + crash guards are per-instruction diagnostics: they
         * catch a runaway emulator early on the host. On the device they are
         * pure hot-path overhead — and a bad PC/stack can't crash the ESP32
         * anyway (every bus access is range-checked and RAM writes stay inside
         * the RAM buffer), it would only wedge the emulation, which the guards
         * do too. So compile them in only for the host build. */
#ifdef HOST_BUILD
        /* Stack depth guard */
        if (cpu->regs.a[7] != 0 && cpu->regs.a[7] < 0x200 && cpu->regs.a[7] < 0x1E0000) {
            static int stack_guard = 0;
            if (stack_guard < 1) {
                ESP_LOGE(TAG_CPU, "STACK OVERFLOW A7=%08lX PC=%08lX op=%04X",
                         (unsigned long)cpu->regs.a[7], (unsigned long)cpu->regs.inst_pc, opcode);
                dump_trace("STACK OVERFLOW");
                stack_guard++;
            }
            cpu->regs.stopped = true;
            break;
        }

        /* Crash guard: execution outside valid code regions */
        {
            uint32_t masked_pc = cpu->regs.inst_pc & 0x00FFFFFF;
            bool valid_pc = false;
            /* RAM code region (bootstrap, exception vectors) */
            if (masked_pc < BUS_RAM_SIZE) valid_pc = true;
            /* ROM region: 0xC00000 - 0xE00000 (BUS_ROM_BASE masked 24-bit, 2MB max) */
            if (masked_pc >= 0xC00000 && masked_pc < 0xE00000) valid_pc = true;
            /* Allow very low addresses (vectors, safe RTE) */
            if (masked_pc < 0xC0) valid_pc = true;

            if (!valid_pc) {
                static int crash_reports = 0;
                if (crash_reports < 3) {
                    ESP_LOGE(TAG_CPU, "CRASH GUARD: bad PC=%08lX (masked=%06lX) op=%04X SR=%04X",
                             (unsigned long)cpu->regs.inst_pc, (unsigned long)masked_pc,
                             opcode, cpu->regs.sr);
                    ESP_LOGE(TAG_CPU, "  D: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                             (unsigned long)cpu->regs.d[0], (unsigned long)cpu->regs.d[1],
                             (unsigned long)cpu->regs.d[2], (unsigned long)cpu->regs.d[3],
                             (unsigned long)cpu->regs.d[4], (unsigned long)cpu->regs.d[5],
                             (unsigned long)cpu->regs.d[6], (unsigned long)cpu->regs.d[7]);
                    ESP_LOGE(TAG_CPU, "  A: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                             (unsigned long)cpu->regs.a[0], (unsigned long)cpu->regs.a[1],
                             (unsigned long)cpu->regs.a[2], (unsigned long)cpu->regs.a[3],
                             (unsigned long)cpu->regs.a[4], (unsigned long)cpu->regs.a[5],
                             (unsigned long)cpu->regs.a[6], (unsigned long)cpu->regs.a[7]);
                    if (crash_reports == 0) dump_trace("CRASH GUARD");
                    crash_reports++;
                }
                cpu->regs.stopped = true;
                break;
            }
        }
#endif /* HOST_BUILD */

        /* Direct switch dispatch on the top nibble (see the group-handler
         * forward decls above for why a switch beats the indirect table).
         * TRAP #15 → group4 → m68k_op_trap → exception vector 47
         * A-line   → group_a → exception vector 10. */
        switch (opcode >> 12) {
        case 0x0: m68k_group0(cpu, opcode);  break;
        case 0x1: m68k_group1(cpu, opcode);  break;
        case 0x2: m68k_group2(cpu, opcode);  break;
        case 0x3: m68k_group3(cpu, opcode);  break;
        case 0x4:
            /* Fast-path call/return/jump (very common); rarer group-4 ops
             * (CLR/NEG/MOVEM/LEA/TRAP/…) still go through m68k_group4. */
            if (opcode == 0x4E75)                 m68k_op_rts(cpu, opcode);
            else if ((opcode & 0xFFC0) == 0x4E80) m68k_op_jsr(cpu, opcode);
            else if ((opcode & 0xFFC0) == 0x4EC0) m68k_op_jmp(cpu, opcode);
            else                                  m68k_group4(cpu, opcode);
            break;
        case 0x5: m68k_group5(cpu, opcode);  break;
        case 0x6: m68k_group6(cpu, opcode);  break;
        case 0x7: m68k_group7(cpu, opcode);  break;
        case 0x8: m68k_group8(cpu, opcode);  break;
        case 0x9: m68k_group9(cpu, opcode);  break;
        case 0xA: m68k_group_a(cpu, opcode); break;
        case 0xB: m68k_group_b(cpu, opcode); break;
        case 0xC: m68k_group_c(cpu, opcode); break;
        case 0xD: m68k_group_d(cpu, opcode); break;
        case 0xE: m68k_group_e(cpu, opcode); break;
        case 0xF: m68k_group_f(cpu, opcode); break;
        }

        if (cpu->regs.cycles == 0)
            cpu->regs.cycles = 4;

        /* Recompute pending interrupt only when the instruction just executed
         * touched a peripheral register (which is the only way it could have
         * changed db.ipr/db.imr). A source that was acked/masked drops back to
         * 0 here and no longer re-dispatches; an RAM/ROM-only instruction skips
         * the recompute entirely. (Timer/RTC/UART changes happen between
         * batches and are folded in by the caller's m68k_set_irq.) */
        if (g_bus_periph_seq != last_periph_seq && cpu->irq_callback) {
            last_periph_seq = g_bus_periph_seq;
            int level = cpu->irq_callback();
            cpu->regs.pending_irq = (level > 0) ? level : 0;
        }

        /* NOTE: do NOT force a PEN interrupt level here.  The pen interrupt
         * must go through the normal IMR-gated path (db_interrupt_pending_level)
         * so that when the ROM masks INT_PEN after the first pen-down it stops
         * re-firing.  Forcing pending_irq=5 every instruction bypassed the mask
         * and re-entered the pen handler while ($253) was set, tripping the
         * ROM's "AMXHardwareEZ.c:443 Pen down int. invalid" fatal error. */

        /* Boot milestone trace — addresses for big ROM at base 0x10C08000.
         * Host-only: this scan (plus the per-address probes below) runs every
         * instruction, so it is compiled out of the device build entirely. */
#ifdef HOST_BUILD
        {
            uint32_t ipc = cpu->regs.inst_pc;

            /* Fatal-error halt loop: dump the call chain that led here. */
            if (ipc == 0x10C1C764) {
                static int fe_n = 0;
                if (fe_n < 1) { fe_n++;
                    fprintf(stderr, "[FATAL] reached halt 0x10C1C764 a7=%08lX\n", (unsigned long)cpu->regs.a[7]);
                    extern void m68k_dump_recent(int n);
                    m68k_dump_recent(60);
                }
            }

            /* Suspend/sleep entry: capture caller reliably AFTER the `link a6`
             * (0x10C0CBCE) so an interrupt taken at the entry instruction can't
             * corrupt the read.  Post-link, the return address lives at 4(a6).
             * Also log the activity-timer globals: $254=currentTick,
             * $152=lastActivity (reset by EvtResetAutoOffTimer). */
            if (ipc == 0x10C0CBCE) {
                static int sleep_n = 0;
                if (sleep_n < 16) { sleep_n++;
                    uint32_t ret  = bus_read32(cpu->regs.a[6] + 4);
                    uint32_t tick = bus_read32(0x254);
                    uint32_t last = bus_read32(0x152);
                    fprintf(stderr, "[SLEEP] #%d caller=%08lX tick($254)=%lu last($152)=%lu idle=%ld\n",
                            sleep_n, (unsigned long)ret,
                            (unsigned long)tick, (unsigned long)last,
                            (long)(tick - last));
                    extern void m68k_dump_recent(int n);
                    m68k_dump_recent(6);
                }
            }

            /* Track InitStage1 re-entries (reset loop detection) */
            if (ipc == 0x10C0836E) {
                static int init_count = 0;
                init_count++;
                ESP_LOGI(TAG_CPU, "InitStage1 entry #%d A7=%08lX D0=%08lX",
                         init_count, (unsigned long)cpu->regs.a[7],
                         (unsigned long)cpu->regs.d[0]);
                if (init_count == 2)
                    dump_trace("RESET LOOP");
            }

            static uint32_t milestones[] = {
                0x10C0823C, /*  0: resetVector entry */
                0x10C08330, /*  1: boot code start */
                0x10C0AF0E, /*  2: HAL init entry */
                0x10C083D0, /*  3: globals setup */
                0x10C083E8, /*  4: dispatch table ptr write */
                0x10C08414, /*  5: TRAP#15 vector setup */
                0x10C0845E, /*  6: JSR MemInit */
                0x10C085AC, /*  7: JSR initCodeOffset1 */
                0x10C085C8, /*  8: JSR initCodeOffset2 */
                0x10C085CE, /*  9: JSR MemCardInit */
                0
            };
            static uint32_t logged = 0;
            for (int i = 0; milestones[i]; i++) {
                if (ipc == milestones[i] && !(logged & (1u << i))) {
                    logged |= (1u << i);
                    ESP_LOGI(TAG_CPU, "MS[%d] PC=%08lX A7=%08lX D0=%08lX D1=%08lX",
                             i, (unsigned long)ipc, (unsigned long)cpu->regs.a[7],
                             (unsigned long)cpu->regs.d[0], (unsigned long)cpu->regs.d[1]);
                }
            }
        }
#endif /* HOST_BUILD */

        cyc += cpu->regs.cycles;
    }
    cpu->regs.cycles_executed = cyc;
}

/* Public entry point: dispatch to the JIT engine when enabled, else interpret.
 * The JIT lives in m68k_jit.c (device only); the host build always interprets. */
void m68k_execute(m68k_cpu_t *M68K_RESTRICT cpu, int target_cycles)
{
#ifndef HOST_BUILD
    extern void m68k_execute_jit(m68k_cpu_t *cpu, int target_cycles);
    if (g_m68k_accel_mode == 1) {
        m68k_execute_jit(cpu, target_cycles);
        return;
    }
#endif
    m68k_execute_interp(cpu, target_cycles);
}

/* --- Size helpers (m68k_size_mask / m68k_sign_bit) are now static inline in
 *     m68k.h so they fold into the handlers. --- */

void m68k_set_flags_move(m68k_cpu_t *cpu, uint32_t result, int size) {
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    result &= mask;
    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
}

void m68k_set_flags_logic(m68k_cpu_t *cpu, uint32_t result, int size) {
    m68k_set_flags_move(cpu, result, size);
}

void m68k_set_flags_add(m68k_cpu_t *cpu, uint32_t src, uint32_t dst,
                         uint32_t result, int size) {
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    src &= mask; dst &= mask; result &= mask;
    cpu->regs.sr &= ~(SR_X | SR_N | SR_Z | SR_V | SR_C);
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    if ((~(src ^ dst) & (result ^ dst)) & sign) cpu->regs.sr |= SR_V;
    uint64_t full = (uint64_t)src + (uint64_t)dst;
    if (full & ((uint64_t)mask + 1)) cpu->regs.sr |= SR_C | SR_X;
}

void m68k_set_flags_sub(m68k_cpu_t *cpu, uint32_t src, uint32_t dst,
                         uint32_t result, int size) {
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    src &= mask; dst &= mask; result &= mask;
    cpu->regs.sr &= ~(SR_X | SR_N | SR_Z | SR_V | SR_C);
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    if (((src ^ dst) & (result ^ dst)) & sign) cpu->regs.sr |= SR_V;
    if (src > dst) cpu->regs.sr |= SR_C | SR_X;
}

void m68k_set_flags_cmp(m68k_cpu_t *cpu, uint32_t src, uint32_t dst,
                         uint32_t result, int size) {
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    src &= mask; dst &= mask; result &= mask;
    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    if (((src ^ dst) & (result ^ dst)) & sign) cpu->regs.sr |= SR_V;
    if (src > dst) cpu->regs.sr |= SR_C;
}
