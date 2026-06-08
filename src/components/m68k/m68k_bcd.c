#include "m68k.h"
#include "bus.h"

/*
 * BCD (Binary-Coded Decimal) instructions: ABCD, SBCD, NBCD
 *
 * These are rarely used by Palm OS but must be implemented for correctness.
 */

void m68k_op_abcd(m68k_cpu_t *cpu, uint16_t opcode)
{
    int rx = (opcode >> 9) & 7;
    int ry = opcode & 7;
    int rm = (opcode >> 3) & 1;
    uint8_t src, dst;
    uint32_t x = (cpu->regs.sr & SR_X) ? 1 : 0;

    if (rm == 0) {
        /* Dn mode */
        src = (uint8_t)cpu->regs.d[ry];
        dst = (uint8_t)cpu->regs.d[rx];
    } else {
        /* -(An) mode */
        cpu->regs.a[ry] -= 1;
        cpu->regs.a[rx] -= 1;
        src = bus_read8(cpu->regs.a[ry]);
        dst = bus_read8(cpu->regs.a[rx]);
    }

    uint32_t low  = (dst & 0x0F) + (src & 0x0F) + x;
    uint32_t high = (dst & 0xF0) + (src & 0xF0);

    if (low > 9) {
        low += 6;
    }
    high += low;
    if (high > 0x99) {
        high += 0x60;
    }

    uint8_t result = (uint8_t)high;

    cpu->regs.sr &= ~(SR_C | SR_X);
    if (high > 0x99)
        cpu->regs.sr |= SR_C | SR_X;
    if (result != 0)
        cpu->regs.sr &= ~SR_Z;

    if (rm == 0) {
        cpu->regs.d[rx] = (cpu->regs.d[rx] & 0xFFFFFF00) | result;
    } else {
        bus_write8(cpu->regs.a[rx], result);
    }

    cpu->regs.cycles = (rm == 0) ? 6 : 18;
}

void m68k_op_sbcd(m68k_cpu_t *cpu, uint16_t opcode)
{
    int rx = (opcode >> 9) & 7;
    int ry = opcode & 7;
    int rm = (opcode >> 3) & 1;
    uint8_t src, dst;
    uint32_t x = (cpu->regs.sr & SR_X) ? 1 : 0;

    if (rm == 0) {
        src = (uint8_t)cpu->regs.d[ry];
        dst = (uint8_t)cpu->regs.d[rx];
    } else {
        cpu->regs.a[ry] -= 1;
        cpu->regs.a[rx] -= 1;
        src = bus_read8(cpu->regs.a[ry]);
        dst = bus_read8(cpu->regs.a[rx]);
    }

    int32_t low  = (dst & 0x0F) - (src & 0x0F) - x;
    int32_t high = (dst & 0xF0) - (src & 0xF0);

    if (low < 0) {
        low += 10;
        high -= 0x10;
    }
    if (high < 0) {
        high += 0xA0;
        cpu->regs.sr |= SR_C | SR_X;
    } else {
        cpu->regs.sr &= ~(SR_C | SR_X);
    }

    uint8_t result = (uint8_t)((high & 0xF0) | (low & 0x0F));

    if (result != 0)
        cpu->regs.sr &= ~SR_Z;

    if (rm == 0) {
        cpu->regs.d[rx] = (cpu->regs.d[rx] & 0xFFFFFF00) | result;
    } else {
        bus_write8(cpu->regs.a[rx], result);
    }

    cpu->regs.cycles = (rm == 0) ? 6 : 18;
}

void m68k_op_nbcd(m68k_cpu_t *cpu, uint16_t opcode)
{
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
    uint8_t dst = (uint8_t)m68k_read_ea(cpu, &ea, SIZE_BYTE);
    uint32_t x = (cpu->regs.sr & SR_X) ? 1 : 0;

    int32_t low  = 0 - (dst & 0x0F) - x;
    int32_t high = 0 - (dst & 0xF0);

    if (low < 0) {
        low += 10;
        high -= 0x10;
    }
    if (high < 0) {
        high += 0xA0;
        cpu->regs.sr |= SR_C | SR_X;
    } else {
        cpu->regs.sr &= ~(SR_C | SR_X);
    }

    uint8_t result = (uint8_t)((high & 0xF0) | (low & 0x0F));

    if (result != 0)
        cpu->regs.sr &= ~SR_Z;

    m68k_write_ea(cpu, &ea, result, SIZE_BYTE);
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ? 6 : 8;
}
