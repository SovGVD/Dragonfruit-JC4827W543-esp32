#include "m68k.h"
#include "bus.h"

/*
 * Group E: Shifts and Rotates
 *
 * Register form: 1110 CCC D SS T RR RRR
 *   CCC: count register or immediate count (0=8)
 *   D: direction (0=right, 1=left)
 *   SS: size (00=byte, 01=word, 10=long)
 *   T: type (0=immediate count, 1=register count)
 *   RR: operation (00=AS, 01=LS, 10=ROX, 11=RO)
 *   RRR: destination register
 *
 * Memory form: 1110 OOO D 11 SSSSSS
 *   OOO D: selects the operation
 *   SSSSSS: EA (always word-size, shift by 1)
 */

static void do_asr(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);

    if (count == 0) {
        *result_out = val;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    bool last_out = false;
    for (int i = 0; i < count; i++) {
        last_out = (result & 1) != 0;
        uint32_t msb = result & sign;
        result = (result >> 1) | msb;
        result &= mask;
    }

    if (last_out) cpu->regs.sr |= SR_C | SR_X;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    *result_out = result;
}

static void do_asl(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);

    if (count == 0) {
        *result_out = val;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    bool last_out = false;
    bool overflow = false;
    for (int i = 0; i < count; i++) {
        last_out = (result & sign) != 0;
        result = (result << 1) & mask;
        if ((result & sign) != (val & sign))
            overflow = true;
    }

    if (last_out) cpu->regs.sr |= SR_C | SR_X;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    if (overflow) cpu->regs.sr |= SR_V;
    *result_out = result;
}

static void do_lsr(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);

    if (count == 0) {
        *result_out = val;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    bool last_out = false;
    for (int i = 0; i < count; i++) {
        last_out = (result & 1) != 0;
        result >>= 1;
        result &= mask;
    }

    if (last_out) cpu->regs.sr |= SR_C | SR_X;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    *result_out = result;
}

static void do_lsl(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);

    if (count == 0) {
        *result_out = val;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    bool last_out = false;
    for (int i = 0; i < count; i++) {
        last_out = (result & sign) != 0;
        result = (result << 1) & mask;
    }

    if (last_out) cpu->regs.sr |= SR_C | SR_X;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    *result_out = result;
}

static void do_ror(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);

    if (count == 0) {
        *result_out = val;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    for (int i = 0; i < count; i++) {
        bool lsb = (result & 1) != 0;
        result >>= 1;
        if (lsb) result |= sign;
        result &= mask;
    }

    if (result & sign) cpu->regs.sr |= SR_N;
    if (result == 0) cpu->regs.sr |= SR_Z;
    /* C = last bit rotated out (which is now the MSB of result) */
    if (result & sign) cpu->regs.sr |= SR_C;

    *result_out = result;
}

static void do_rol(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C);

    if (count == 0) {
        *result_out = val;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    for (int i = 0; i < count; i++) {
        bool msb = (result & sign) != 0;
        result = ((result << 1) & mask) | (msb ? 1 : 0);
    }

    if (result & sign) cpu->regs.sr |= SR_N;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & 1) cpu->regs.sr |= SR_C;
    *result_out = result;
}

static void do_roxr(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;
    bool x = (cpu->regs.sr & SR_X) != 0;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);

    if (count == 0) {
        *result_out = val;
        if (x) cpu->regs.sr |= SR_C | SR_X;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    for (int i = 0; i < count; i++) {
        bool lsb = (result & 1) != 0;
        result >>= 1;
        if (x) result |= sign;
        result &= mask;
        x = lsb;
    }

    if (x) cpu->regs.sr |= SR_C | SR_X;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    *result_out = result;
}

static void do_roxl(m68k_cpu_t *cpu, uint32_t val, int count, int size, uint32_t *result_out)
{
    uint32_t mask = m68k_size_mask(size);
    uint32_t sign = m68k_sign_bit(size);
    val &= mask;
    bool x = (cpu->regs.sr & SR_X) != 0;

    cpu->regs.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);

    if (count == 0) {
        *result_out = val;
        if (x) cpu->regs.sr |= SR_C | SR_X;
        if (val == 0) cpu->regs.sr |= SR_Z;
        if (val & sign) cpu->regs.sr |= SR_N;
        return;
    }

    uint32_t result = val;
    for (int i = 0; i < count; i++) {
        bool msb = (result & sign) != 0;
        result = ((result << 1) & mask) | (x ? 1 : 0);
        x = msb;
    }

    if (x) cpu->regs.sr |= SR_C | SR_X;
    if (result == 0) cpu->regs.sr |= SR_Z;
    if (result & sign) cpu->regs.sr |= SR_N;
    *result_out = result;
}

void m68k_group_e(m68k_cpu_t *cpu, uint16_t opcode)
{
    int size_bits = (opcode >> 6) & 3;

    /* Memory shift/rotate: size == 3 means memory EA, always word, shift by 1 */
    if (size_bits == 3) {
        int op = (opcode >> 9) & 7;
        int ea_mode = (opcode >> 3) & 7;
        int ea_reg  = opcode & 7;
        ea_result_t ea = m68k_resolve_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
        uint32_t val = m68k_read_ea(cpu, &ea, SIZE_WORD);
        uint32_t result;

        switch (op) {
        case 0: do_asr(cpu, val, 1, SIZE_WORD, &result); break;
        case 1: do_asl(cpu, val, 1, SIZE_WORD, &result); break;
        case 2: do_lsr(cpu, val, 1, SIZE_WORD, &result); break;
        case 3: do_lsl(cpu, val, 1, SIZE_WORD, &result); break;
        case 4: do_roxr(cpu, val, 1, SIZE_WORD, &result); break;
        case 5: do_roxl(cpu, val, 1, SIZE_WORD, &result); break;
        case 6: do_ror(cpu, val, 1, SIZE_WORD, &result); break;
        case 7: do_rol(cpu, val, 1, SIZE_WORD, &result); break;
        default: result = val; break;
        }

        m68k_write_ea(cpu, &ea, result, SIZE_WORD);
        cpu->regs.cycles = 8;
        return;
    }

    /* Register shift/rotate */
    int size = size_bits;
    int dreg = opcode & 7;
    int ir = (opcode >> 5) & 1;  /* 0=immediate count, 1=register count */
    int type = (opcode >> 3) & 3; /* 00=AS, 01=LS, 10=ROX, 11=RO */
    int dir = (opcode >> 8) & 1;  /* 0=right, 1=left */
    int cr = (opcode >> 9) & 7;

    int count;
    if (ir) {
        count = cpu->regs.d[cr] & 63;
    } else {
        count = (cr == 0) ? 8 : cr;
    }

    uint32_t val = cpu->regs.d[dreg] & m68k_size_mask(size);
    uint32_t result;

    switch (type) {
    case 0: /* AS */
        if (dir) do_asl(cpu, val, count, size, &result);
        else     do_asr(cpu, val, count, size, &result);
        break;
    case 1: /* LS */
        if (dir) do_lsl(cpu, val, count, size, &result);
        else     do_lsr(cpu, val, count, size, &result);
        break;
    case 2: /* ROX */
        if (dir) do_roxl(cpu, val, count, size, &result);
        else     do_roxr(cpu, val, count, size, &result);
        break;
    case 3: /* RO */
        if (dir) do_rol(cpu, val, count, size, &result);
        else     do_ror(cpu, val, count, size, &result);
        break;
    default:
        result = val;
        break;
    }

    uint32_t mask = m68k_size_mask(size);
    cpu->regs.d[dreg] = (cpu->regs.d[dreg] & ~mask) | (result & mask);
    cpu->regs.cycles = 6 + 2 * count;
    if (size == SIZE_LONG) cpu->regs.cycles += 2;
}
