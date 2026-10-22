#include "m68k.h"
#include "bus.h"

/* A-line trap: every $Axxx opcode triggers Line 1010 exception.
 * This is how Palm OS implements system calls. */
void m68k_group_a(m68k_cpu_t *cpu, uint16_t opcode)
{
    if (cpu->palm_trap_callback && cpu->palm_trap_callback(cpu, opcode)) {
        cpu->regs.cycles = 34;
        return;
    }
    /* 68000 Line-A exception stacks the address OF the faulting instruction,
     * not the instruction after it. PC was already advanced by the fetch. */
    cpu->regs.pc -= 2;
    m68k_take_exception(cpu, VEC_LINE_A);
    cpu->regs.cycles = 34;
}

/* F-line trap: $Fxxx opcodes */
void m68k_group_f(m68k_cpu_t *cpu, uint16_t opcode)
{
    if (m68k_palm_hle_fline(cpu, opcode))   /* native ROM loop (m68k_palm_hle.c) */
        return;
    cpu->regs.pc -= 2;
    m68k_take_exception(cpu, VEC_LINE_F);
    cpu->regs.cycles = 34;
}

void m68k_op_trap(m68k_cpu_t *cpu, uint16_t opcode)
{
    int vector = opcode & 0xF;
    /* Palm's usual ABI is TRAP #15 followed by an Axxx selector word. That
     * word is data read by the ROM dispatcher, not an executed A-line opcode. */
    if (vector == 15 && cpu->palm_trap_callback &&
        cpu->palm_trap_callback(cpu, bus_read16(cpu->regs.pc))) {
        cpu->regs.pc += 2;
        cpu->regs.cycles = 34;
        return;
    }
    if (vector == 15 && m68k_palm_hle_trap15(cpu))   /* native TrapDispatcher */
        return;
    m68k_take_exception(cpu, VEC_TRAP_BASE + vector);
    cpu->regs.cycles = 34;
}

void m68k_op_trapv(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    if (cpu->regs.sr & SR_V) {
        m68k_take_exception(cpu, VEC_TRAPV);
        cpu->regs.cycles = 34;
    } else {
        cpu->regs.cycles = 4;
    }
}

void m68k_op_stop(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    if (!(cpu->regs.sr & SR_S)) {
        m68k_take_exception(cpu, VEC_PRIVILEGE);
        return;
    }
    uint16_t new_sr = m68k_fetch16(cpu);
    m68k_jit_set_sr(cpu, new_sr);   /* swap USP/SSP if STOP's new SR flips the S bit */
    cpu->regs.stopped = true;
    cpu->regs.cycles = 4;
}

void m68k_op_reset_instr(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    if (!(cpu->regs.sr & SR_S)) {
        m68k_take_exception(cpu, VEC_PRIVILEGE);
        return;
    }
    /* RESET asserts the reset line -- in emulation, this is a no-op */
    cpu->regs.cycles = 132;
}

void m68k_op_nop(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    cpu->regs.cycles = 4;
}

void m68k_op_move_usp(m68k_cpu_t *cpu, uint16_t opcode)
{
    if (!(cpu->regs.sr & SR_S)) {
        m68k_take_exception(cpu, VEC_PRIVILEGE);
        return;
    }
    int reg = opcode & 7;
    if (opcode & 0x0008) {
        /* USP -> An */
        cpu->regs.a[reg] = cpu->regs.usp;
    } else {
        /* An -> USP */
        cpu->regs.usp = cpu->regs.a[reg];
    }
    cpu->regs.cycles = 4;
}

void m68k_op_move_sr(m68k_cpu_t *cpu, uint16_t opcode)
{
    /* MOVE <ea>,SR (privileged) */
    if (!(cpu->regs.sr & SR_S)) {
        m68k_take_exception(cpu, VEC_PRIVILEGE);
        return;
    }
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
    uint16_t val = (uint16_t)m68k_read_ea(cpu, &ea, SIZE_WORD);

    /* Check if transitioning from supervisor to user mode */
    if ((cpu->regs.sr & SR_S) && !(val & SR_S)) {
        cpu->regs.ssp = cpu->regs.a[7];
        cpu->regs.a[7] = cpu->regs.usp;
    } else if (!(cpu->regs.sr & SR_S) && (val & SR_S)) {
        cpu->regs.usp = cpu->regs.a[7];
        cpu->regs.a[7] = cpu->regs.ssp;
    }

    cpu->regs.sr = val;
    cpu->regs.cycles = 12;
}

void m68k_op_move_from_sr(m68k_cpu_t *cpu, uint16_t opcode)
{
    /* MOVE SR,<ea> -- not privileged on 68000 */
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
    m68k_write_ea(cpu, &ea, cpu->regs.sr, SIZE_WORD);
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ? 6 : 8;
}

void m68k_op_move_to_ccr(m68k_cpu_t *cpu, uint16_t opcode)
{
    /* MOVE <ea>,CCR -- set only lower byte of SR */
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
    uint16_t val = (uint16_t)m68k_read_ea(cpu, &ea, SIZE_WORD);
    cpu->regs.sr = (cpu->regs.sr & 0xFF00) | (val & 0x001F);
    cpu->regs.cycles = 12;
}
