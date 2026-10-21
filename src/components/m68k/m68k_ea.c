#include "m68k.h"
#include "bus.h"

/* ea_resolve / ea_read / ea_write are the force-inlined forms, used by the hot
 * opcode handlers that fold into the dispatch loop (MOVE, ADD/SUB/CMP, ADDQ,
 * JSR/JMP...). There GCC would otherwise keep them as calls: each MOVE paid up
 * to four windowed calls and returned its ea_result_t through memory. Colder
 * handlers call the out-of-line m68k_* wrappers at the end of this file, which
 * keeps the IRAM footprint of the interpreter in check. */

static int ea_increment_size(int reg, int size)
{
    if (size == SIZE_BYTE)
        return (reg == 7) ? 2 : 1;  /* A7 stays word-aligned */
    if (size == SIZE_WORD) return 2;
    return 4;
}

M68K_AINLINE ea_result_t ea_resolve(m68k_cpu_t *cpu, int mode, int reg, int size)
{
    ea_result_t ea = { .type = EA_MEMORY, .addr = 0, .reg = 0 };

    switch (mode) {
    case 0: /* Dn */
        ea.type = EA_DATA_REG;
        ea.reg = reg;
        return ea;

    case 1: /* An */
        ea.type = EA_ADDR_REG;
        ea.reg = reg;
        return ea;

    case 2: /* (An) */
        ea.addr = cpu->regs.a[reg];
        return ea;

    case 3: /* (An)+ */
        ea.addr = cpu->regs.a[reg];
        cpu->regs.a[reg] += ea_increment_size(reg, size);
        return ea;

    case 4: /* -(An) */
        cpu->regs.a[reg] -= ea_increment_size(reg, size);
        ea.addr = cpu->regs.a[reg];
        return ea;

    case 5: { /* (d16,An) */
        int16_t disp = (int16_t)m68k_fetch16(cpu);
        ea.addr = cpu->regs.a[reg] + disp;
        return ea;
    }

    case 6: { /* (d8,An,Xn) */
        uint16_t ext = m68k_fetch16(cpu);
        int8_t disp = (int8_t)(ext & 0xFF);
        int xreg = (ext >> 12) & 7;
        int32_t xval;
        if (ext & 0x8000)
            xval = (int32_t)cpu->regs.a[xreg];
        else
            xval = (int32_t)cpu->regs.d[xreg];
        if (!(ext & 0x0800))
            xval = (int16_t)(xval & 0xFFFF); /* word-size index */
        ea.addr = cpu->regs.a[reg] + disp + xval;
        return ea;
    }

    case 7:
        switch (reg) {
        case 0: { /* (xxx).W */
            int16_t addr = (int16_t)m68k_fetch16(cpu);
            ea.addr = (uint32_t)(int32_t)addr;
            return ea;
        }
        case 1: /* (xxx).L */
            ea.addr = m68k_fetch32(cpu);
            return ea;

        case 2: { /* (d16,PC) */
            uint32_t base_pc = cpu->regs.pc;
            int16_t disp = (int16_t)m68k_fetch16(cpu);
            ea.addr = base_pc + disp;
            return ea;
        }
        case 3: { /* (d8,PC,Xn) */
            uint32_t base_pc = cpu->regs.pc;
            uint16_t ext = m68k_fetch16(cpu);
            int8_t disp = (int8_t)(ext & 0xFF);
            int xreg = (ext >> 12) & 7;
            int32_t xval;
            if (ext & 0x8000)
                xval = (int32_t)cpu->regs.a[xreg];
            else
                xval = (int32_t)cpu->regs.d[xreg];
            if (!(ext & 0x0800))
                xval = (int16_t)(xval & 0xFFFF);
            ea.addr = base_pc + disp + xval;
            return ea;
        }
        case 4: { /* #imm */
            ea.type = EA_MEMORY;
            ea.addr = cpu->regs.pc;
            if (size == SIZE_BYTE) {
                ea.addr++; /* byte value is in low byte of word (big-endian) */
                cpu->regs.pc += 2;
            } else if (size == SIZE_WORD)
                cpu->regs.pc += 2;
            else
                cpu->regs.pc += 4;
            return ea;
        }
        }
        break;
    }

    ea.addr = 0;
    return ea;
}

M68K_AINLINE uint32_t ea_read(m68k_cpu_t *cpu, ea_result_t *ea, int size)
{
    switch (ea->type) {
    case EA_DATA_REG:
        return cpu->regs.d[ea->reg] & m68k_size_mask(size);
    case EA_ADDR_REG:
        if (size == SIZE_BYTE)
            return cpu->regs.a[ea->reg] & 0xFF;
        if (size == SIZE_WORD)
            return cpu->regs.a[ea->reg] & 0xFFFF;
        return cpu->regs.a[ea->reg];
    case EA_MEMORY:
        if (size == SIZE_BYTE)
            return bus_read8(ea->addr);
        if (size == SIZE_WORD)
            return bus_read16(ea->addr);
        return bus_read32(ea->addr);
    }
    return 0;
}

M68K_AINLINE void ea_write(m68k_cpu_t *cpu, ea_result_t *ea, uint32_t val, int size)
{
    switch (ea->type) {
    case EA_DATA_REG: {
        uint32_t mask = m68k_size_mask(size);
        cpu->regs.d[ea->reg] = (cpu->regs.d[ea->reg] & ~mask) | (val & mask);
        return;
    }
    case EA_ADDR_REG:
        if (size == SIZE_WORD)
            cpu->regs.a[ea->reg] = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);
        else
            cpu->regs.a[ea->reg] = val;
        return;
    case EA_MEMORY:
        if (size == SIZE_BYTE)
            bus_write8(ea->addr, (uint8_t)val);
        else if (size == SIZE_WORD)
            bus_write16(ea->addr, (uint16_t)val);
        else
            bus_write32(ea->addr, val);
        return;
    }
}

ea_result_t m68k_resolve_ea(m68k_cpu_t *cpu, int mode, int reg, int size)
{
    return ea_resolve(cpu, mode, reg, size);
}

uint32_t m68k_read_ea(m68k_cpu_t *cpu, ea_result_t *ea, int size)
{
    return ea_read(cpu, ea, size);
}

void m68k_write_ea(m68k_cpu_t *cpu, ea_result_t *ea, uint32_t val, int size)
{
    ea_write(cpu, ea, val, size);
}
