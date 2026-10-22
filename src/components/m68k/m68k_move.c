#include "m68k.h"
#include "bus.h"

/*
 * Groups 1/2/3: MOVE and MOVEA
 *
 * Encoding: 00SS DDDM MMSS SSSS
 *   SS (bits 13-12): 01=byte, 11=word, 10=long (note non-obvious mapping)
 *   DDD (bits 11-9): destination register
 *   MMM (bits 8-6):  destination mode
 *   SSSSSS (bits 5-0): source EA (mode:reg)
 *
 * Destination EA has mode/register REVERSED compared to source.
 */

M68K_AINLINE void do_move(m68k_cpu_t *cpu, uint16_t opcode, int size)
{
    int src_mode = (opcode >> 3) & 7;
    int src_reg  = opcode & 7;
    int dst_reg  = (opcode >> 9) & 7;
    int dst_mode = (opcode >> 6) & 7;

    ea_result_t src = ea_resolve(cpu, src_mode, src_reg, size);
    uint32_t val = ea_read(cpu, &src, size);

    /* MOVEA: destination is An, no flag update, sign-extend word to long */
    if (dst_mode == 1) {
        if (size == SIZE_WORD)
            cpu->regs.a[dst_reg] = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);
        else
            cpu->regs.a[dst_reg] = val;
        cpu->regs.cycles = 4;
        return;
    }

    ea_result_t dst = ea_resolve(cpu, dst_mode, dst_reg, size);
    ea_write(cpu, &dst, val, size);
    m68k_set_flags_move(cpu, val, size);

    cpu->regs.cycles = (dst.type == EA_MEMORY) ? 8 : 4;
    if (src.type == EA_MEMORY) cpu->regs.cycles += 4;
    if (size == SIZE_LONG) cpu->regs.cycles += 4;
}

M68K_AINLINE void m68k_group1(m68k_cpu_t *cpu, uint16_t opcode) { do_move(cpu, opcode, SIZE_BYTE); }
M68K_AINLINE void m68k_group2(m68k_cpu_t *cpu, uint16_t opcode) { do_move(cpu, opcode, SIZE_LONG); }
M68K_AINLINE void m68k_group3(m68k_cpu_t *cpu, uint16_t opcode) { do_move(cpu, opcode, SIZE_WORD); }

/*
 * Group 7: MOVEQ -- MOVE quick
 * Encoding: 0111 RRR0 DDDDDDDD
 *   RRR: destination data register
 *   DDDDDDDD: 8-bit signed immediate, sign-extended to 32 bits
 */
M68K_AINLINE void m68k_group7(m68k_cpu_t *cpu, uint16_t opcode)
{
    if (opcode & 0x0100) {
        m68k_illegal(cpu);
        return;
    }
    int reg = (opcode >> 9) & 7;
    int8_t data = (int8_t)(opcode & 0xFF);
    uint32_t val = (uint32_t)(int32_t)data;
    cpu->regs.d[reg] = val;
    m68k_set_flags_move(cpu, val, SIZE_LONG);
    cpu->regs.cycles = 4;
}

/*
 * Group 4 (partial): MOVEM, LEA, PEA, EXG, SWAP, LINK, UNLK, MOVE USP
 * These are mixed into the "miscellaneous" group 4 along with CLR/NEG/NOT/etc.
 * The group4 handler in m68k_alu.c dispatches here for specific opcodes.
 */

void m68k_op_movem(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dir = (opcode >> 10) & 1;   /* 0=reg-to-mem, 1=mem-to-reg */
    int size = (opcode & 0x0040) ? SIZE_LONG : SIZE_WORD;
    int inc = (size == SIZE_LONG) ? 4 : 2;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    uint16_t mask = m68k_fetch16(cpu);
    int count = 0;

    if (dir == 0) {
        /* Register to memory */
        if (ea_mode == 4) {
            /* -(An): register mask is reversed, predecrement */
            uint32_t addr = cpu->regs.a[ea_reg];
            for (int i = 15; i >= 0; i--) {
                if (mask & (1 << (15 - i))) {
                    addr -= inc;
                    uint32_t val = (i < 8) ? cpu->regs.d[i] : cpu->regs.a[i - 8];
                    if (size == SIZE_WORD)
                        bus_write16(addr, (uint16_t)val);
                    else
                        bus_write32(addr, val);
                    count++;
                }
            }
            cpu->regs.a[ea_reg] = addr;
        } else {
            ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
            uint32_t addr = ea.addr;
            for (int i = 0; i < 16; i++) {
                if (mask & (1 << i)) {
                    uint32_t val = (i < 8) ? cpu->regs.d[i] : cpu->regs.a[i - 8];
                    if (size == SIZE_WORD)
                        bus_write16(addr, (uint16_t)val);
                    else
                        bus_write32(addr, val);
                    addr += inc;
                    count++;
                }
            }
        }
    } else {
        /* Memory to register */
        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
        uint32_t addr = ea.addr;
        for (int i = 0; i < 16; i++) {
            if (mask & (1 << i)) {
                uint32_t val;
                if (size == SIZE_WORD)
                    val = (uint32_t)(int32_t)(int16_t)bus_read16(addr);
                else
                    val = bus_read32(addr);
                if (i < 8)
                    cpu->regs.d[i] = val;
                else
                    cpu->regs.a[i - 8] = val;
                addr += inc;
                count++;
            }
        }
        if (ea_mode == 3) /* (An)+ */
            cpu->regs.a[ea_reg] = addr;
    }

    cpu->regs.cycles = 12 + count * ((size == SIZE_LONG) ? 8 : 4);
}

void m68k_op_lea(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dst_reg = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    cpu->regs.a[dst_reg] = ea.addr;
    cpu->regs.cycles = (ea_mode == 5 || ea_mode == 6) ? 8 : 4;
}

void m68k_op_pea(m68k_cpu_t *cpu, uint16_t opcode)
{
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    cpu->regs.a[7] -= 4;
    bus_write32(cpu->regs.a[7], ea.addr);
    cpu->regs.cycles = 12;
}

void m68k_op_swap(m68k_cpu_t *cpu, uint16_t opcode)
{
    int reg = opcode & 7;
    uint32_t val = cpu->regs.d[reg];
    val = (val >> 16) | (val << 16);
    cpu->regs.d[reg] = val;
    m68k_set_flags_move(cpu, val, SIZE_LONG);
    cpu->regs.cycles = 4;
}

void m68k_op_exg(m68k_cpu_t *cpu, uint16_t opcode)
{
    int rx = (opcode >> 9) & 7;
    int ry = opcode & 7;
    int mode = (opcode >> 3) & 0x1F;
    uint32_t tmp;

    switch (mode) {
    case 0x08: /* Dx,Dy */
        tmp = cpu->regs.d[rx];
        cpu->regs.d[rx] = cpu->regs.d[ry];
        cpu->regs.d[ry] = tmp;
        break;
    case 0x09: /* Ax,Ay */
        tmp = cpu->regs.a[rx];
        cpu->regs.a[rx] = cpu->regs.a[ry];
        cpu->regs.a[ry] = tmp;
        break;
    case 0x11: /* Dx,Ay */
        tmp = cpu->regs.d[rx];
        cpu->regs.d[rx] = cpu->regs.a[ry];
        cpu->regs.a[ry] = tmp;
        break;
    default:
        m68k_illegal(cpu);
        return;
    }
    cpu->regs.cycles = 6;
}

void m68k_op_link(m68k_cpu_t *cpu, uint16_t opcode)
{
    int reg = opcode & 7;
    int16_t disp = (int16_t)m68k_fetch16(cpu);

    cpu->regs.a[7] -= 4;
    bus_write32(cpu->regs.a[7], cpu->regs.a[reg]);
    cpu->regs.a[reg] = cpu->regs.a[7];
    cpu->regs.a[7] += (int32_t)disp;
    cpu->regs.cycles = 16;
}

void m68k_op_unlk(m68k_cpu_t *cpu, uint16_t opcode)
{
    int reg = opcode & 7;
    cpu->regs.a[7] = cpu->regs.a[reg];
    cpu->regs.a[reg] = bus_read32(cpu->regs.a[7]);
    cpu->regs.a[7] += 4;
    cpu->regs.cycles = 12;
}

void m68k_op_movep(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dreg = (opcode >> 9) & 7;
    int areg = opcode & 7;
    int mode = (opcode >> 6) & 7;
    int16_t disp = (int16_t)m68k_fetch16(cpu);
    uint32_t addr = cpu->regs.a[areg] + disp;

    switch (mode) {
    case 4: /* MOVEP.W (d,An),Dn */
        cpu->regs.d[dreg] = (cpu->regs.d[dreg] & 0xFFFF0000) |
                            ((uint32_t)bus_read8(addr) << 8) |
                            bus_read8(addr + 2);
        break;
    case 5: /* MOVEP.L (d,An),Dn */
        cpu->regs.d[dreg] = ((uint32_t)bus_read8(addr) << 24) |
                            ((uint32_t)bus_read8(addr + 2) << 16) |
                            ((uint32_t)bus_read8(addr + 4) << 8) |
                            bus_read8(addr + 6);
        break;
    case 6: /* MOVEP.W Dn,(d,An) */
        bus_write8(addr,     (uint8_t)(cpu->regs.d[dreg] >> 8));
        bus_write8(addr + 2, (uint8_t)cpu->regs.d[dreg]);
        break;
    case 7: /* MOVEP.L Dn,(d,An) */
        bus_write8(addr,     (uint8_t)(cpu->regs.d[dreg] >> 24));
        bus_write8(addr + 2, (uint8_t)(cpu->regs.d[dreg] >> 16));
        bus_write8(addr + 4, (uint8_t)(cpu->regs.d[dreg] >> 8));
        bus_write8(addr + 6, (uint8_t)cpu->regs.d[dreg]);
        break;
    }
    cpu->regs.cycles = (mode == 5 || mode == 7) ? 24 : 16;
}
