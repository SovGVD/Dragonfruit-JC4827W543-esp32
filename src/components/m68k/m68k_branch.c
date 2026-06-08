#include "m68k.h"
#include "bus.h"

/*
 * Group 6: Bcc / BRA / BSR
 *
 * Encoding: 0110 CCCC DDDDDDDD
 *   CCCC: condition code (0=BRA, 1=BSR, 2-15=Bcc)
 *   DDDDDDDD: 8-bit displacement. If 0, next word is 16-bit displacement.
 */
M68K_AINLINE void m68k_group6(m68k_cpu_t *cpu, uint16_t opcode)
{
    int cc = (opcode >> 8) & 0xF;
    int8_t disp8 = (int8_t)(opcode & 0xFF);
    int32_t disp;
    uint32_t base_pc = cpu->regs.pc; /* PC after opcode word */

    if (disp8 == 0) {
        disp = (int16_t)m68k_fetch16(cpu);
    } else {
        disp = disp8;
    }

    if (cc == 0) {
        /* BRA */
        cpu->regs.pc = base_pc + disp;
        cpu->regs.cycles = 10;
        return;
    }

    if (cc == 1) {
        /* BSR */
        cpu->regs.a[7] -= 4;
        bus_write32(cpu->regs.a[7], cpu->regs.pc);
        cpu->regs.pc = base_pc + disp;
        cpu->regs.cycles = 18;
        return;
    }

    /* Bcc */
    if (m68k_eval_cc(cpu->regs.sr, cc)) {
        cpu->regs.pc = base_pc + disp;
        cpu->regs.cycles = 10;
    } else {
        cpu->regs.cycles = (disp8 == 0) ? 12 : 8;
    }
}

/* JMP and JSR (called from group4 handler) */

M68K_AINLINE void m68k_op_jmp(m68k_cpu_t *cpu, uint16_t opcode)
{
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    cpu->regs.pc = ea.addr;
    cpu->regs.cycles = 8;
}

M68K_AINLINE void m68k_op_jsr(m68k_cpu_t *cpu, uint16_t opcode)
{
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    cpu->regs.a[7] -= 4;
    bus_write32(cpu->regs.a[7], cpu->regs.pc);
    cpu->regs.pc = ea.addr;
    cpu->regs.cycles = 16;
}

M68K_AINLINE void m68k_op_rts(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    cpu->regs.pc = bus_read32(cpu->regs.a[7]);
    cpu->regs.a[7] += 4;
    cpu->regs.cycles = 16;
}

void m68k_op_rte(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    if (!(cpu->regs.sr & SR_S)) {
        m68k_take_exception(cpu, VEC_PRIVILEGE);
        return;
    }

    uint16_t new_sr = bus_read16(cpu->regs.a[7]);
    cpu->regs.a[7] += 2;
    uint32_t new_pc = bus_read32(cpu->regs.a[7]);
    cpu->regs.a[7] += 4;

    /* If transitioning from supervisor to user mode, swap stacks */
    if ((cpu->regs.sr & SR_S) && !(new_sr & SR_S)) {
        cpu->regs.ssp = cpu->regs.a[7];
        cpu->regs.a[7] = cpu->regs.usp;
    }

    cpu->regs.sr = new_sr;
    cpu->regs.pc = new_pc;
    cpu->regs.cycles = 20;
}

void m68k_op_rtr(m68k_cpu_t *cpu, uint16_t opcode)
{
    (void)opcode;
    uint16_t ccr = bus_read16(cpu->regs.a[7]);
    cpu->regs.a[7] += 2;
    cpu->regs.sr = (cpu->regs.sr & 0xFF00) | (ccr & 0x001F);
    cpu->regs.pc = bus_read32(cpu->regs.a[7]);
    cpu->regs.a[7] += 4;
    cpu->regs.cycles = 20;
}
