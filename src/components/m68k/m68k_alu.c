#include "m68k.h"
#include "bus.h"

/* Forward declarations for group4 sub-handlers in m68k_move.c / m68k_system.c */
extern void m68k_op_movem(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_lea(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_pea(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_swap(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_exg(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_link(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_unlk(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_movep(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_trap(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_move_usp(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_move_sr(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_move_to_ccr(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_move_from_sr(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_stop(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_rte(m68k_cpu_t *cpu, uint16_t opcode);
M68K_AINLINE void m68k_op_rts(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_nop(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_reset_instr(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_trapv(m68k_cpu_t *cpu, uint16_t opcode);
extern void m68k_op_rtr(m68k_cpu_t *cpu, uint16_t opcode);
M68K_AINLINE void m68k_op_jmp(m68k_cpu_t *cpu, uint16_t opcode);
M68K_AINLINE void m68k_op_jsr(m68k_cpu_t *cpu, uint16_t opcode);

/* --- Helpers --- */

static int decode_size_67(uint16_t opcode)
{
    switch ((opcode >> 6) & 3) {
    case 0: return SIZE_BYTE;
    case 1: return SIZE_WORD;
    case 2: return SIZE_LONG;
    default: return -1;
    }
}

/* ================================================================
 * Group 0: Immediate ALU + bit manipulation + MOVEP
 * ================================================================ */
void m68k_group0(m68k_cpu_t *cpu, uint16_t opcode)
{
    /* Bit 8 set = dynamic bit ops (register specified) or MOVEP */
    if (opcode & 0x0100) {
        int ea_mode = (opcode >> 3) & 7;

        /* MOVEP: ea_mode == 1 (address register) */
        if (ea_mode == 1) {
            m68k_op_movep(cpu, opcode);
            return;
        }

        /* Dynamic bit operations: BTST/BCHG/BCLR/BSET with Dn */
        int bit_reg = (opcode >> 9) & 7;
        int type = (opcode >> 6) & 3;
        int ea_reg = opcode & 7;

        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
        int size = (ea.type == EA_DATA_REG) ? SIZE_LONG : SIZE_BYTE;
        uint32_t val = m68k_read_ea(cpu, &ea, size);
        int bit = cpu->regs.d[bit_reg] & ((size == SIZE_LONG) ? 31 : 7);

        cpu->regs.sr &= ~SR_Z;
        if (!(val & (1u << bit)))
            cpu->regs.sr |= SR_Z;

        switch (type) {
        case 0: /* BTST - test only */
            break;
        case 1: /* BCHG */
            val ^= (1u << bit);
            m68k_write_ea(cpu, &ea, val, size);
            break;
        case 2: /* BCLR */
            val &= ~(1u << bit);
            m68k_write_ea(cpu, &ea, val, size);
            break;
        case 3: /* BSET */
            val |= (1u << bit);
            m68k_write_ea(cpu, &ea, val, size);
            break;
        }
        cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
        return;
    }

    /* Bit 8 clear: static bit ops or immediate ALU */
    int sub_op = (opcode >> 9) & 7;

    if (sub_op == 4) {
        /* Static BTST/BCHG/BCLR/BSET with immediate bit number */
        int type = (opcode >> 6) & 3;
        uint16_t bit_word = m68k_fetch16(cpu);
        int ea_mode = (opcode >> 3) & 7;
        int ea_reg = opcode & 7;

        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
        int size = (ea.type == EA_DATA_REG) ? SIZE_LONG : SIZE_BYTE;
        uint32_t val = m68k_read_ea(cpu, &ea, size);
        int bit = bit_word & ((size == SIZE_LONG) ? 31 : 7);

        cpu->regs.sr &= ~SR_Z;
        if (!(val & (1u << bit)))
            cpu->regs.sr |= SR_Z;

        switch (type) {
        case 0: break;
        case 1: val ^= (1u << bit); m68k_write_ea(cpu, &ea, val, size); break;
        case 2: val &= ~(1u << bit); m68k_write_ea(cpu, &ea, val, size); break;
        case 3: val |= (1u << bit); m68k_write_ea(cpu, &ea, val, size); break;
        }
        cpu->regs.cycles = (size == SIZE_LONG) ? 12 : 8;
        return;
    }

    /* Immediate ALU: ORI, ANDI, SUBI, ADDI, EORI, CMPI */
    int size = decode_size_67(opcode);
    if (size < 0) { m68k_illegal(cpu); return; }

    uint32_t imm;
    if (size == SIZE_BYTE)
        imm = m68k_fetch16(cpu) & 0xFF;
    else if (size == SIZE_WORD)
        imm = m68k_fetch16(cpu);
    else
        imm = m68k_fetch32(cpu);

    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    /* Special case: ORI/ANDI/EORI to CCR or SR */
    if (ea_mode == 7 && ea_reg == 4) {
        /* #imm to CCR (byte) or SR (word) */
        if (size == SIZE_BYTE) {
            switch (sub_op) {
            case 0: cpu->regs.sr |= (imm & 0x1F); break;  /* ORI to CCR */
            case 1: cpu->regs.sr &= (imm | 0xFFE0); break; /* ANDI to CCR */
            case 5: cpu->regs.sr ^= (imm & 0x1F); break;  /* EORI to CCR */
            }
        } else if (size == SIZE_WORD) {
            /* Word form writes the whole SR and is privileged. Apply through
             * m68k_jit_set_sr so a change to the S bit swaps USP/SSP; in user
             * mode the 68000 takes a privilege violation, same as STOP/RESET. */
            if (cpu->regs.sr & SR_S) {
                uint16_t newsr = cpu->regs.sr;
                switch (sub_op) {
                case 0: newsr |= imm; break;   /* ORI to SR */
                case 1: newsr &= imm; break;   /* ANDI to SR */
                case 5: newsr ^= imm; break;   /* EORI to SR */
                }
                m68k_jit_set_sr(cpu, newsr);
            } else {
                m68k_take_exception(cpu, VEC_PRIVILEGE);
                return;
            }
        }
        cpu->regs.cycles = 20;
        return;
    }

    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
    uint32_t dst = m68k_read_ea(cpu, &ea, size);
    uint32_t result;

    switch (sub_op) {
    case 0: /* ORI */
        result = dst | imm;
        m68k_write_ea(cpu, &ea, result, size);
        m68k_set_flags_logic(cpu, result, size);
        break;
    case 1: /* ANDI */
        result = dst & imm;
        m68k_write_ea(cpu, &ea, result, size);
        m68k_set_flags_logic(cpu, result, size);
        break;
    case 2: /* SUBI */
        result = dst - imm;
        m68k_write_ea(cpu, &ea, result, size);
        m68k_set_flags_sub(cpu, imm, dst, result, size);
        break;
    case 3: /* ADDI */
        result = dst + imm;
        m68k_write_ea(cpu, &ea, result, size);
        m68k_set_flags_add(cpu, imm, dst, result, size);
        break;
    case 5: /* EORI */
        result = dst ^ imm;
        m68k_write_ea(cpu, &ea, result, size);
        m68k_set_flags_logic(cpu, result, size);
        break;
    case 6: /* CMPI */
        result = dst - imm;
        m68k_set_flags_cmp(cpu, imm, dst, result, size);
        break;
    default:
        m68k_illegal(cpu);
        return;
    }

    cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
        ((size == SIZE_LONG) ? 16 : 8) :
        ((size == SIZE_LONG) ? 20 : 12);
}

/* ================================================================
 * Group 4: Miscellaneous
 * ================================================================ */

static void do_clr(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size = decode_size_67(opcode);
    if (size < 0) { m68k_illegal(cpu); return; }
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
    m68k_write_ea(cpu, &ea, 0, size);
    cpu->regs.sr &= ~(SR_N | SR_V | SR_C);
    cpu->regs.sr |= SR_Z;
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ? 4 :
                       ((size == SIZE_LONG) ? 12 : 8);
}

static void do_neg(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size = decode_size_67(opcode);
    if (size < 0) { m68k_illegal(cpu); return; }
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
    uint32_t dst = m68k_read_ea(cpu, &ea, size);
    uint32_t result = 0 - dst;
    m68k_write_ea(cpu, &ea, result, size);
    m68k_set_flags_sub(cpu, dst, 0, result, size);
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
        ((size == SIZE_LONG) ? 6 : 4) :
        ((size == SIZE_LONG) ? 12 : 8);
}

static void do_negx(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size = decode_size_67(opcode);
    if (size < 0) { m68k_illegal(cpu); return; }
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
    uint32_t dst = m68k_read_ea(cpu, &ea, size);
    uint32_t x = (cpu->regs.sr & SR_X) ? 1 : 0;
    uint32_t result = 0 - dst - x;
    m68k_write_ea(cpu, &ea, result, size);

    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    result &= mask;
    dst &= mask;
    cpu->regs.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (result & sign) cpu->regs.sr |= SR_N;
    if (result != 0) cpu->regs.sr &= ~SR_Z; /* Z is only cleared, never set */
    if ((result & dst) & sign) cpu->regs.sr |= SR_V;
    if (dst != 0 || x) cpu->regs.sr |= SR_C | SR_X;
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
        ((size == SIZE_LONG) ? 6 : 4) :
        ((size == SIZE_LONG) ? 12 : 8);
}

static void do_not(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size = decode_size_67(opcode);
    if (size < 0) { m68k_illegal(cpu); return; }
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = m68k_read_ea(cpu, &ea, size);
    uint32_t result = ~val;
    m68k_write_ea(cpu, &ea, result, size);
    m68k_set_flags_logic(cpu, result, size);
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
        ((size == SIZE_LONG) ? 6 : 4) :
        ((size == SIZE_LONG) ? 12 : 8);
}

static void do_tst(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size = decode_size_67(opcode);
    if (size < 0) { m68k_illegal(cpu); return; }
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = m68k_read_ea(cpu, &ea, size);
    m68k_set_flags_move(cpu, val, size);
    cpu->regs.cycles = 4;
}

static void do_ext(m68k_cpu_t *cpu, uint16_t opcode)
{
    int reg = opcode & 7;
    int mode = (opcode >> 6) & 7;

    if (mode == 2) {
        /* EXT.W: sign-extend byte to word */
        int8_t b = (int8_t)(cpu->regs.d[reg] & 0xFF);
        cpu->regs.d[reg] = (cpu->regs.d[reg] & 0xFFFF0000) | ((uint16_t)(int16_t)b & 0xFFFF);
        m68k_set_flags_move(cpu, cpu->regs.d[reg], SIZE_WORD);
    } else if (mode == 3) {
        /* EXT.L: sign-extend word to long */
        int16_t w = (int16_t)(cpu->regs.d[reg] & 0xFFFF);
        cpu->regs.d[reg] = (uint32_t)(int32_t)w;
        m68k_set_flags_move(cpu, cpu->regs.d[reg], SIZE_LONG);
    } else {
        m68k_illegal(cpu);
        return;
    }
    cpu->regs.cycles = 4;
}

static void do_tas(m68k_cpu_t *cpu, uint16_t opcode)
{
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
    uint32_t val = m68k_read_ea(cpu, &ea, SIZE_BYTE);
    m68k_set_flags_move(cpu, val, SIZE_BYTE);
    val |= 0x80;
    m68k_write_ea(cpu, &ea, val, SIZE_BYTE);
    cpu->regs.cycles = (ea.type == EA_DATA_REG) ? 4 : 14;
}

static void do_chk(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dreg = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;
    ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
    int16_t bound = (int16_t)m68k_read_ea(cpu, &ea, SIZE_WORD);
    int16_t val = (int16_t)(cpu->regs.d[dreg] & 0xFFFF);

    if (val < 0) {
        cpu->regs.sr |= SR_N;
        m68k_take_exception(cpu, VEC_CHK);
    } else if (val > bound) {
        cpu->regs.sr &= ~SR_N;
        m68k_take_exception(cpu, VEC_CHK);
    }
    cpu->regs.cycles = 10;
}

void m68k_group4(m68k_cpu_t *cpu, uint16_t opcode)
{
    int bits_11_8 = (opcode >> 8) & 0xF;
    int bits_7_6  = (opcode >> 6) & 3;

    /* EXT: 0100 100x 1x00 0RRR — must check before MOVEM (overlapping at mode 000) */
    if ((opcode & 0xFEB8) == 0x4880) {
        do_ext(cpu, opcode);
        return;
    }

    /* MOVEM: $4880-$4CFF (mode != 000 since EXT already handled) */
    if ((opcode & 0xFB80) == 0x4880 && bits_7_6 >= 2) {
        m68k_op_movem(cpu, opcode);
        return;
    }

    /* LEA: 0100 RRR1 11SS SSSS */
    if ((opcode & 0xF1C0) == 0x41C0) {
        m68k_op_lea(cpu, opcode);
        return;
    }

    /* CHK: 0100 RRR1 10SS SSSS */
    if ((opcode & 0xF1C0) == 0x4180) {
        do_chk(cpu, opcode);
        return;
    }

    /* SWAP: 0100 1000 0100 0RRR — must check before PEA (overlapping pattern) */
    if ((opcode & 0xFFF8) == 0x4840) {
        m68k_op_swap(cpu, opcode);
        return;
    }

    /* PEA: 0100 1000 01MM MRRR (mode != 000, i.e., not data register) */
    if ((opcode & 0xFFC0) == 0x4840) {
        m68k_op_pea(cpu, opcode);
        return;
    }

    switch (opcode & 0xFFF8) {
    case 0x4E40: case 0x4E48: /* TRAP #vector (0x4E40-0x4E4F) */
        m68k_op_trap(cpu, opcode);
        return;
    case 0x4E50: /* LINK (0x4E50-0x4E57) */
        m68k_op_link(cpu, opcode);
        return;
    case 0x4E58: /* UNLK (0x4E58-0x4E5F) */
        m68k_op_unlk(cpu, opcode);
        return;
    case 0x4E60: case 0x4E68: /* MOVE USP (0x4E60-0x4E6F) */
        m68k_op_move_usp(cpu, opcode);
        return;
    }

    switch (opcode) {
    case 0x4E70: m68k_op_reset_instr(cpu, opcode); return;
    case 0x4E71: m68k_op_nop(cpu, opcode); return;
    case 0x4E72: m68k_op_stop(cpu, opcode); return;
    case 0x4E73: m68k_op_rte(cpu, opcode); return;
    case 0x4E75: m68k_op_rts(cpu, opcode); return;
    case 0x4E76: m68k_op_trapv(cpu, opcode); return;
    case 0x4E77: m68k_op_rtr(cpu, opcode); return;
    }

    /* JMP: 0100 1110 11SS SSSS */
    if ((opcode & 0xFFC0) == 0x4EC0) {
        m68k_op_jmp(cpu, opcode);
        return;
    }

    /* JSR: 0100 1110 10SS SSSS */
    if ((opcode & 0xFFC0) == 0x4E80) {
        m68k_op_jsr(cpu, opcode);
        return;
    }

    /* MOVE from SR: 0100 0000 11SS SSSS */
    if ((opcode & 0xFFC0) == 0x40C0) {
        m68k_op_move_from_sr(cpu, opcode);
        return;
    }

    /* MOVE to CCR: 0100 0100 11SS SSSS */
    if ((opcode & 0xFFC0) == 0x44C0) {
        m68k_op_move_to_ccr(cpu, opcode);
        return;
    }

    /* MOVE to SR: 0100 0110 11SS SSSS */
    if ((opcode & 0xFFC0) == 0x46C0) {
        m68k_op_move_sr(cpu, opcode);
        return;
    }

    /* TAS: 0100 1010 11SS SSSS */
    if ((opcode & 0xFFC0) == 0x4AC0) {
        do_tas(cpu, opcode);
        return;
    }

    /* NBCD: 0100 1000 00SS SSSS — implemented in m68k_bcd.c */
    if ((opcode & 0xFFC0) == 0x4800) {
        extern void m68k_op_nbcd(m68k_cpu_t *cpu, uint16_t opcode);
        m68k_op_nbcd(cpu, opcode);
        return;
    }

    /* CLR/NEG/NEGX/NOT/TST */
    switch (bits_11_8) {
    case 0x0: do_negx(cpu, opcode); return;  /* 0100 0000 */
    case 0x2: do_clr(cpu, opcode); return;   /* 0100 0010 */
    case 0x4: do_neg(cpu, opcode); return;   /* 0100 0100 */
    case 0x6: do_not(cpu, opcode); return;   /* 0100 0110 */
    case 0xA: do_tst(cpu, opcode); return;   /* 0100 1010 */
    }

    /* Catch the actual 0100 000x patterns */
    switch ((opcode >> 6) & 0x3F) {
    case 0x00: case 0x01: case 0x02: do_negx(cpu, opcode); return;
    case 0x08: case 0x09: case 0x0A: do_clr(cpu, opcode); return;
    case 0x10: case 0x11: case 0x12: do_neg(cpu, opcode); return;
    case 0x18: case 0x19: case 0x1A: do_not(cpu, opcode); return;
    case 0x28: case 0x29: case 0x2A: do_tst(cpu, opcode); return;
    }

    m68k_illegal(cpu);
}

/* ================================================================
 * Group 5: ADDQ / SUBQ / Scc / DBcc
 * ================================================================ */

M68K_AINLINE void m68k_group5(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size_bits = (opcode >> 6) & 3;

    /* Scc and DBcc: size field == 3 */
    if (size_bits == 3) {
        int cc = (opcode >> 8) & 0xF;
        int ea_mode = (opcode >> 3) & 7;
        int ea_reg  = opcode & 7;

        if (ea_mode == 1) {
            /* DBcc Dn,displacement */
            int16_t disp = (int16_t)m68k_fetch16(cpu);
            if (!m68k_eval_cc(cpu->regs.sr, cc)) {
                int16_t counter = (int16_t)(cpu->regs.d[ea_reg] & 0xFFFF) - 1;
                cpu->regs.d[ea_reg] = (cpu->regs.d[ea_reg] & 0xFFFF0000) |
                                      ((uint16_t)counter);
                if (counter != -1) {
                    cpu->regs.pc = cpu->regs.pc - 2 + disp;
                    cpu->regs.cycles = 10;
                } else {
                    cpu->regs.cycles = 14;
                }
            } else {
                cpu->regs.cycles = 12;
            }
            return;
        }

        /* Scc */
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, SIZE_BYTE);
        uint8_t val = m68k_eval_cc(cpu->regs.sr, cc) ? 0xFF : 0x00;
        ea_write(cpu, &ea, val, SIZE_BYTE);
        cpu->regs.cycles = (ea.type == EA_DATA_REG) ? 4 : 8;
        return;
    }

    /* ADDQ / SUBQ */
    int size = size_bits; /* 0=byte, 1=word, 2=long */
    int data = (opcode >> 9) & 7;
    if (data == 0) data = 8;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    if (opcode & 0x0100) {
        /* SUBQ */
        if (ea_mode == 1) {
            cpu->regs.a[ea_reg] -= data;
            cpu->regs.cycles = 8;
        } else {
            ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
            uint32_t dst = ea_read(cpu, &ea, size);
            uint32_t result = dst - data;
            ea_write(cpu, &ea, result, size);
            m68k_set_flags_sub(cpu, data, dst, result, size);
            cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
                ((size == SIZE_LONG) ? 8 : 4) :
                ((size == SIZE_LONG) ? 12 : 8);
        }
    } else {
        /* ADDQ */
        if (ea_mode == 1) {
            cpu->regs.a[ea_reg] += data;
            cpu->regs.cycles = 8;
        } else {
            ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
            uint32_t dst = ea_read(cpu, &ea, size);
            uint32_t result = dst + data;
            ea_write(cpu, &ea, result, size);
            m68k_set_flags_add(cpu, data, dst, result, size);
            cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
                ((size == SIZE_LONG) ? 8 : 4) :
                ((size == SIZE_LONG) ? 12 : 8);
        }
    }
}

/* ================================================================
 * Group 8: OR / DIVU / DIVS / SBCD
 * ================================================================ */
void m68k_group8(m68k_cpu_t *cpu, uint16_t opcode)
{
    int opmode = (opcode >> 6) & 7;

    /* SBCD: opmode 4, bits 5-4 = 00 */
    if (opmode == 4 && ((opcode >> 3) & 7) <= 1) {
        extern void m68k_op_sbcd(m68k_cpu_t *cpu, uint16_t opcode);
        m68k_op_sbcd(cpu, opcode);
        return;
    }

    /* DIVU: opmode == 3 */
    if (opmode == 3) {
        int dreg = (opcode >> 9) & 7;
        int ea_mode = (opcode >> 3) & 7;
        int ea_reg  = opcode & 7;
        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
        uint16_t divisor = (uint16_t)m68k_read_ea(cpu, &ea, SIZE_WORD);

        if (divisor == 0) {
            m68k_take_exception(cpu, VEC_ZERO_DIVIDE);
            cpu->regs.cycles = 38;
            return;
        }

        uint32_t dividend = cpu->regs.d[dreg];
        uint32_t quotient = dividend / divisor;
        uint16_t remainder = dividend % divisor;

        cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
        if (quotient > 0xFFFF) {
            cpu->regs.sr |= SR_V;
        } else {
            cpu->regs.d[dreg] = ((uint32_t)remainder << 16) | (quotient & 0xFFFF);
            if (quotient == 0) cpu->regs.sr |= SR_Z;
            if (quotient & 0x8000) cpu->regs.sr |= SR_N;
        }
        cpu->regs.cycles = 140;
        return;
    }

    /* DIVS: opmode == 7 */
    if (opmode == 7) {
        int dreg = (opcode >> 9) & 7;
        int ea_mode = (opcode >> 3) & 7;
        int ea_reg  = opcode & 7;
        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
        int16_t divisor = (int16_t)m68k_read_ea(cpu, &ea, SIZE_WORD);

        if (divisor == 0) {
            m68k_take_exception(cpu, VEC_ZERO_DIVIDE);
            cpu->regs.cycles = 38;
            return;
        }

        int32_t dividend = (int32_t)cpu->regs.d[dreg];
        int32_t quotient = dividend / divisor;
        int16_t remainder = dividend % divisor;

        cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
        if (quotient > 32767 || quotient < -32768) {
            cpu->regs.sr |= SR_V;
        } else {
            cpu->regs.d[dreg] = ((uint32_t)(uint16_t)remainder << 16) |
                                ((uint16_t)quotient);
            if ((int16_t)quotient == 0) cpu->regs.sr |= SR_Z;
            if ((int16_t)quotient < 0) cpu->regs.sr |= SR_N;
        }
        cpu->regs.cycles = 158;
        return;
    }

    /* OR */
    int dreg = (opcode >> 9) & 7;
    int size = opmode & 3;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    if (opmode < 3) {
        /* OR <ea>,Dn */
        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
        uint32_t src = m68k_read_ea(cpu, &ea, size);
        uint32_t dst = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = src | dst;
        cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~m68k_size_mask(size)) |
                            (result & m68k_size_mask(size));
        m68k_set_flags_logic(cpu, result, size);
    } else {
        /* OR Dn,<ea> */
        size = opmode - 4;
        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, size);
        uint32_t dst = m68k_read_ea(cpu, &ea, size);
        uint32_t src = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = src | dst;
        m68k_write_ea(cpu, &ea, result, size);
        m68k_set_flags_logic(cpu, result, size);
    }

    cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
}

/* ================================================================
 * Group 9: SUB / SUBX / SUBA
 * ================================================================ */
M68K_AINLINE void m68k_group9(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dreg = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    /* SUBA: opmode 3 (word) or 7 (long) */
    if (opmode == 3 || opmode == 7) {
        int size = (opmode == 3) ? SIZE_WORD : SIZE_LONG;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        if (size == SIZE_WORD)
            src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
        cpu->regs.a[dreg] -= src;
        cpu->regs.cycles = 8;
        return;
    }

    /* SUBX: ea_mode 0 (Dx,Dy) or 1 (-(Ax),-(Ay)) when opmode 4/5/6 */
    if ((opmode == 4 || opmode == 5 || opmode == 6) &&
        (ea_mode == 0 || ea_mode == 1)) {
        int size = opmode - 4;
        uint32_t x = (cpu->regs.sr & SR_X) ? 1 : 0;
        uint32_t src, dst, result;

        if (ea_mode == 0) {
            src = cpu->regs.d[ea_reg] & m68k_size_mask(size);
            dst = cpu->regs.d[dreg] & m68k_size_mask(size);
            result = dst - src - x;
            cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~m68k_size_mask(size)) |
                                (result & m68k_size_mask(size));
        } else {
            int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
            cpu->regs.a[ea_reg] -= inc;
            cpu->regs.a[dreg] -= inc;
            if (size == SIZE_BYTE) {
                src = bus_read8(cpu->regs.a[ea_reg]);
                dst = bus_read8(cpu->regs.a[dreg]);
            } else if (size == SIZE_WORD) {
                src = bus_read16(cpu->regs.a[ea_reg]);
                dst = bus_read16(cpu->regs.a[dreg]);
            } else {
                src = bus_read32(cpu->regs.a[ea_reg]);
                dst = bus_read32(cpu->regs.a[dreg]);
            }
            result = dst - src - x;
            if (size == SIZE_BYTE)
                bus_write8(cpu->regs.a[dreg], (uint8_t)result);
            else if (size == SIZE_WORD)
                bus_write16(cpu->regs.a[dreg], (uint16_t)result);
            else
                bus_write32(cpu->regs.a[dreg], result);
        }

        uint32_t mask = m68k_size_mask(size);
        uint32_t sign = m68k_sign_bit(size);
        src &= mask; dst &= mask; result &= mask;
        cpu->regs.sr &= ~(SR_N | SR_V | SR_C | SR_X);
        if (result & sign) cpu->regs.sr |= SR_N;
        if (result != 0) cpu->regs.sr &= ~SR_Z;
        if (((src ^ dst) & (result ^ dst)) & sign) cpu->regs.sr |= SR_V;
        if (src + x > dst) cpu->regs.sr |= SR_C | SR_X;
        cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
        return;
    }

    /* SUB <ea>,Dn (opmode 0/1/2) or SUB Dn,<ea> (opmode 4/5/6) */
    int size;
    if (opmode < 3) {
        size = opmode;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        uint32_t dst = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = dst - src;
        cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~m68k_size_mask(size)) |
                            (result & m68k_size_mask(size));
        m68k_set_flags_sub(cpu, src, dst, result, size);
    } else {
        size = opmode - 4;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t dst = ea_read(cpu, &ea, size);
        uint32_t src = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = dst - src;
        ea_write(cpu, &ea, result, size);
        m68k_set_flags_sub(cpu, src, dst, result, size);
    }

    cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
}

/* ================================================================
 * Group B: CMP / CMPA / CMPM / EOR
 * ================================================================ */
M68K_AINLINE void m68k_group_b(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dreg = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    /* CMPA: opmode 3 (word) or 7 (long) */
    if (opmode == 3 || opmode == 7) {
        int size = (opmode == 3) ? SIZE_WORD : SIZE_LONG;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        if (size == SIZE_WORD)
            src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
        uint32_t dst = cpu->regs.a[dreg];
        uint32_t result = dst - src;
        m68k_set_flags_cmp(cpu, src, dst, result, SIZE_LONG);
        cpu->regs.cycles = 6;
        return;
    }

    /* EOR Dn,<ea>: opmode 4/5/6. CMPM (which shares opmode 4/5/6) is encoded
     * with ea_mode == 1, so excluding that here splits the two apart — the
     * ea_mode == 1 case is handled by the CMPM block below. */
    if (opmode >= 4 && opmode <= 6 && ea_mode != 1) {
        int size = opmode - 4;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t dst = ea_read(cpu, &ea, size);
        uint32_t src = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = dst ^ src;
        ea_write(cpu, &ea, result, size);
        m68k_set_flags_logic(cpu, result, size);
        cpu->regs.cycles = (ea.type == EA_DATA_REG) ?
            ((size == SIZE_LONG) ? 8 : 4) :
            ((size == SIZE_LONG) ? 12 : 8);
        return;
    }

    /* CMPM: opmode 4/5/6 with ea_mode == 1 (address register) */
    if (opmode >= 4 && ea_mode == 1) {
        int size = opmode - 4;
        int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
        uint32_t src_addr = cpu->regs.a[ea_reg];
        cpu->regs.a[ea_reg] += inc;
        uint32_t dst_addr = cpu->regs.a[dreg];
        cpu->regs.a[dreg] += inc;

        uint32_t src, dst;
        if (size == SIZE_BYTE) {
            src = bus_read8(src_addr);
            dst = bus_read8(dst_addr);
        } else if (size == SIZE_WORD) {
            src = bus_read16(src_addr);
            dst = bus_read16(dst_addr);
        } else {
            src = bus_read32(src_addr);
            dst = bus_read32(dst_addr);
        }
        uint32_t result = dst - src;
        m68k_set_flags_cmp(cpu, src, dst, result, size);
        cpu->regs.cycles = (size == SIZE_LONG) ? 20 : 12;
        return;
    }

    /* CMP <ea>,Dn: opmode 0/1/2 */
    if (opmode < 3) {
        int size = opmode;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        uint32_t dst = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = dst - src;
        m68k_set_flags_cmp(cpu, src, dst, result, size);
        cpu->regs.cycles = (size == SIZE_LONG) ? 6 : 4;
        return;
    }

    m68k_illegal(cpu);
}

/* ================================================================
 * Group C: AND / MULU / MULS / ABCD / EXG
 * ================================================================ */
M68K_AINLINE void m68k_group_c(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dreg = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    /* ABCD */
    if (opmode == 4 && (ea_mode == 0 || ea_mode == 1)) {
        extern void m68k_op_abcd(m68k_cpu_t *cpu, uint16_t opcode);
        m68k_op_abcd(cpu, opcode);
        return;
    }

    /* EXG */
    if (opmode == 5 && ea_mode == 0) { /* EXG Dx,Dy: opmode=5 ea_mode=0 -> mode 01000 */
        m68k_op_exg(cpu, opcode);
        return;
    }
    if (opmode == 5 && ea_mode == 1) { /* EXG Ax,Ay */
        m68k_op_exg(cpu, opcode);
        return;
    }
    if (opmode == 6 && ea_mode == 1) { /* EXG Dx,Ay */
        m68k_op_exg(cpu, opcode);
        return;
    }

    /* MULU: opmode == 3 */
    if (opmode == 3) {
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, SIZE_WORD);
        uint16_t src = (uint16_t)ea_read(cpu, &ea, SIZE_WORD);
        uint16_t dst = (uint16_t)(cpu->regs.d[dreg] & 0xFFFF);
        uint32_t result = (uint32_t)src * (uint32_t)dst;
        cpu->regs.d[dreg] = result;
        cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
        if (result == 0) cpu->regs.sr |= SR_Z;
        if (result & 0x80000000) cpu->regs.sr |= SR_N;
        cpu->regs.cycles = 70;
        return;
    }

    /* MULS: opmode == 7 */
    if (opmode == 7) {
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, SIZE_WORD);
        int16_t src = (int16_t)ea_read(cpu, &ea, SIZE_WORD);
        int16_t dst = (int16_t)(cpu->regs.d[dreg] & 0xFFFF);
        int32_t result = (int32_t)src * (int32_t)dst;
        cpu->regs.d[dreg] = (uint32_t)result;
        cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
        if (result == 0) cpu->regs.sr |= SR_Z;
        if (result < 0) cpu->regs.sr |= SR_N;
        cpu->regs.cycles = 70;
        return;
    }

    /* AND */
    int size;
    if (opmode < 3) {
        size = opmode;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        uint32_t dst = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = src & dst;
        cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~m68k_size_mask(size)) |
                            (result & m68k_size_mask(size));
        m68k_set_flags_logic(cpu, result, size);
    } else {
        size = opmode - 4;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t dst = ea_read(cpu, &ea, size);
        uint32_t src = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = src & dst;
        ea_write(cpu, &ea, result, size);
        m68k_set_flags_logic(cpu, result, size);
    }
    cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
}

/* ================================================================
 * Group D: ADD / ADDX / ADDA
 * ================================================================ */
M68K_AINLINE void m68k_group_d(m68k_cpu_t *cpu, uint16_t opcode)
{
    int dreg = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg  = opcode & 7;

    /* ADDA: opmode 3 (word) or 7 (long) */
    if (opmode == 3 || opmode == 7) {
        int size = (opmode == 3) ? SIZE_WORD : SIZE_LONG;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        if (size == SIZE_WORD)
            src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
        cpu->regs.a[dreg] += src;
        cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 8;
        return;
    }

    /* ADDX */
    if ((opmode == 4 || opmode == 5 || opmode == 6) &&
        (ea_mode == 0 || ea_mode == 1)) {
        int size = opmode - 4;
        uint32_t x = (cpu->regs.sr & SR_X) ? 1 : 0;
        uint32_t src, dst, result;

        if (ea_mode == 0) {
            src = cpu->regs.d[ea_reg] & m68k_size_mask(size);
            dst = cpu->regs.d[dreg] & m68k_size_mask(size);
            result = dst + src + x;
            cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~m68k_size_mask(size)) |
                                (result & m68k_size_mask(size));
        } else {
            int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
            cpu->regs.a[ea_reg] -= inc;
            cpu->regs.a[dreg] -= inc;
            if (size == SIZE_BYTE) {
                src = bus_read8(cpu->regs.a[ea_reg]);
                dst = bus_read8(cpu->regs.a[dreg]);
            } else if (size == SIZE_WORD) {
                src = bus_read16(cpu->regs.a[ea_reg]);
                dst = bus_read16(cpu->regs.a[dreg]);
            } else {
                src = bus_read32(cpu->regs.a[ea_reg]);
                dst = bus_read32(cpu->regs.a[dreg]);
            }
            result = dst + src + x;
            if (size == SIZE_BYTE)
                bus_write8(cpu->regs.a[dreg], (uint8_t)result);
            else if (size == SIZE_WORD)
                bus_write16(cpu->regs.a[dreg], (uint16_t)result);
            else
                bus_write32(cpu->regs.a[dreg], result);
        }

        uint32_t mask = m68k_size_mask(size);
        uint32_t sign = m68k_sign_bit(size);
        src &= mask; dst &= mask; result &= mask;
        cpu->regs.sr &= ~(SR_N | SR_V | SR_C | SR_X);
        if (result & sign) cpu->regs.sr |= SR_N;
        if (result != 0) cpu->regs.sr &= ~SR_Z;
        if ((~(src ^ dst) & (result ^ dst)) & sign) cpu->regs.sr |= SR_V;
        uint64_t full = (uint64_t)(src) + (uint64_t)(dst) + x;
        if (full & ((uint64_t)mask + 1))
            cpu->regs.sr |= SR_C | SR_X;
        cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
        return;
    }

    /* ADD <ea>,Dn (opmode 0/1/2) or ADD Dn,<ea> (opmode 4/5/6) */
    int size;
    if (opmode < 3) {
        size = opmode;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t src = ea_read(cpu, &ea, size);
        uint32_t dst = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = dst + src;
        cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~m68k_size_mask(size)) |
                            (result & m68k_size_mask(size));
        m68k_set_flags_add(cpu, src, dst, result, size);
    } else {
        size = opmode - 4;
        ea_result_t ea = ea_resolve(cpu, ea_mode, ea_reg, size);
        uint32_t dst = ea_read(cpu, &ea, size);
        uint32_t src = cpu->regs.d[dreg] & m68k_size_mask(size);
        uint32_t result = dst + src;
        ea_write(cpu, &ea, result, size);
        m68k_set_flags_add(cpu, src, dst, result, size);
    }
    cpu->regs.cycles = (size == SIZE_LONG) ? 8 : 4;
}
