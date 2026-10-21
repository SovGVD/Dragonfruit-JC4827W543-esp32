/* H8S core unit tests for behaviour the differential tester cannot check
 * against the GNU simulator: half-carry, decimal adjust, divide, LDM/STM,
 * LDC/STC.W, EEPMOV, exception frames, TRAPA/RTE and interrupt entry in both
 * interrupt control modes. Programs are hand-assembled; each line notes the
 * instruction. */
#include "h8s.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[0x100000];
static h8s_cpu_t cpu;
static int pending_vec, pending_level;

static uint8_t sr8(void *c, uint32_t a) { (void)c; (void)a; return 0xff; }
static uint16_t sr16(void *c, uint32_t a) { (void)c; (void)a; return 0xffff; }
static void sw8(void *c, uint32_t a, uint8_t v) { (void)c; (void)a; (void)v; }
static void sw16(void *c, uint32_t a, uint16_t v) { (void)c; (void)a; (void)v; }

static int ack(void *ctx, h8s_cpu_t *c, int *level)
{
    (void)ctx;
    if (!pending_vec) return 0;
    bool ok = c->intm2 ? pending_level > (c->exr & 7) || pending_level == 8
                       : !(c->ccr & H8S_CCR_I) || pending_level == 8;
    if (!ok) return 0;
    int v = pending_vec;
    *level = pending_level;
    pending_vec = 0;
    return v;
}

static void load(uint32_t at, const uint8_t *code, size_t n)
{
    memcpy(ram + at, code, n);
}

static void setup(uint32_t pc)
{
    h8s_init(&cpu);
    h8s_map(&cpu, 0, 0x100000, ram, 0x100000, true);
    cpu.bus = (h8s_bus_t){ sr8, sr16, sw8, sw16, ack, NULL };
    cpu.pc = pc;
    cpu.er[7] = 0x80000;
    cpu.ccr = 0;
    cpu.exr = H8S_EXR_NC;
    pending_vec = 0;
}

/* Run until PC reaches `stop` (or a step limit). */
static void run_to(uint32_t stop)
{
    for (int i = 0; i < 10000 && cpu.pc != stop; i++) {
        h8s_run(&cpu, 1);
    }
    assert(cpu.pc == stop);
}

#define CODE 0x1000

static void test_half_carry_and_decimal(void)
{
    static const uint8_t p[] = {
        0xf8, 0x0f,             /* mov.b #0x0f,r0l */
        0x88, 0x01,             /* add.b #1,r0l      -> 0x10, H */
    };
    setup(CODE); load(CODE, p, sizeof p); run_to(CODE + sizeof p);
    assert((cpu.er[0] & 0xff) == 0x10);
    assert(cpu.ccr & H8S_CCR_H);

    static const uint8_t w[] = {
        0x79, 0x00, 0x0f, 0xff, /* mov.w #0x0fff,r0 */
        0x79, 0x10, 0x00, 0x01, /* add.w #1,r0       -> 0x1000, H from bit 11 */
    };
    setup(CODE); load(CODE, w, sizeof w); run_to(CODE + sizeof w);
    assert((cpu.er[0] & 0xffff) == 0x1000 && (cpu.ccr & H8S_CCR_H));

    static const uint8_t d[] = {
        0xf8, 0x09,             /* mov.b #9,r0l */
        0x88, 0x01,             /* add.b #1,r0l -> 0x0a */
        0x0f, 0x08,             /* daa r0l      -> 0x10 */
        0xf9, 0x58,             /* mov.b #0x58,r1l */
        0x89, 0x67,             /* add.b #0x67,r1l -> 0xbf */
        0x0f, 0x09,             /* daa r1l      -> 0x25, C (58+67=125) */
    };
    setup(CODE); load(CODE, d, sizeof d); run_to(CODE + sizeof d);
    assert((cpu.er[0] & 0xff) == 0x10);
    assert((cpu.er[1] & 0xff) == 0x25 && (cpu.ccr & H8S_CCR_C));

    static const uint8_t s[] = {
        0xf8, 0x10,             /* mov.b #0x10,r0l */
        0xf9, 0x01,             /* mov.b #1,r1l */
        0x18, 0x98,             /* sub.b r1l,r0l -> 0x0f, H */
        0x1f, 0x08,             /* das r0l       -> 0x09 */
    };
    setup(CODE); load(CODE, s, sizeof s); run_to(CODE + sizeof s);
    assert((cpu.er[0] & 0xff) == 0x09);
}

static void test_multiply_divide(void)
{
    static const uint8_t p[] = {
        0x79, 0x00, 0x00, 0x64, /* mov.w #100,r0 */
        0xf9, 0x07,             /* mov.b #7,r1l */
        0x51, 0x90,             /* divxu.b r1l,r0 -> q 14, r 2 */
        0x79, 0x02, 0xff, 0xec, /* mov.w #-20,r2 */
        0xfb, 0x03,             /* mov.b #3,r3l */
        0x01, 0xd0, 0x51, 0xb2, /* divxs.b r3l,r2 -> q -6, r -2 */
        0x7a, 0x04, 0xff, 0xff, 0xff, 0xec, /* mov.l #-20,er4 */
        0x79, 0x05, 0xff, 0xfd, /* mov.w #-3,r5 */
        0x01, 0xd0, 0x53, 0x54, /* divxs.w r5,er4 -> q 6, r -2 */
        0x79, 0x06, 0x00, 0x07, /* mov.w #7,r6 */
        0x01, 0xc0, 0x52, 0x56, /* mulxs.w r5,er6 -> -21 */
        0xf9, 0x00,             /* mov.b #0,r1l */
        0x51, 0x90,             /* divxu.b r1l,r0 -> divide by zero: Z, r0 kept */
    };
    setup(CODE); load(CODE, p, sizeof p); run_to(CODE + sizeof p);
    assert((cpu.er[0] & 0xffff) == 0x020e);
    assert((cpu.er[2] & 0xffff) == 0xfefa);
    assert(cpu.er[4] == 0xfffe0006);
    assert(cpu.er[6] == 0xffffffeb);
    assert(cpu.ccr & H8S_CCR_Z);
}

static void test_ldm_stm_ldc_stc(void)
{
    static const uint8_t p[] = {
        0x7a, 0x00, 0x11, 0x11, 0x11, 0x11, /* mov.l #0x11111111,er0 */
        0x7a, 0x01, 0x22, 0x22, 0x22, 0x22, /* mov.l #0x22222222,er1 */
        0x01, 0x10, 0x6d, 0xf0,             /* stm.l (er0-er1),@-sp */
        0x01, 0x10, 0x6d, 0x73,             /* ldm.l @sp+,(er2-er3) */
        0x7a, 0x04, 0x00, 0x00, 0x20, 0x00, /* mov.l #0x2000,er4 */
        0x07, 0x0a,                         /* ldc #0x0a,ccr */
        0x01, 0x40, 0x69, 0xc0,             /* stc.w ccr,@er4 */
        0x07, 0x00,                         /* ldc #0,ccr */
        0x01, 0x40, 0x69, 0x40,             /* ldc.w @er4,ccr */
        0x01, 0x41, 0x07, 0x83,             /* ldc #0x83,exr */
        0x01, 0x41, 0x6d, 0xc0,             /* stc.w exr,@-er4 */
    };
    setup(CODE); load(CODE, p, sizeof p); run_to(CODE + sizeof p);
    assert(cpu.er[2] == 0x11111111 && cpu.er[3] == 0x22222222);
    assert(cpu.er[7] == 0x80000);
    assert(ram[0x7fffc] == 0x11 && ram[0x7fff8] == 0x22);   /* er0 pushed first */
    assert(ram[0x2000] == 0x0a && cpu.ccr == 0x0a);
    assert(cpu.exr == (0x83 | H8S_EXR_NC));
    assert(cpu.er[4] == 0x1ffe && ram[0x1ffe] == (0x83 | H8S_EXR_NC));
}

static void test_eepmov_and_calls(void)
{
    static const uint8_t p[] = {
        0x7a, 0x05, 0x00, 0x00, 0x30, 0x00, /* mov.l #0x3000,er5 */
        0x7a, 0x06, 0x00, 0x00, 0x40, 0x00, /* mov.l #0x4000,er6 */
        0xfc, 0x03,                         /* mov.b #3,r4l */
        0x7b, 0x5c, 0x59, 0x8f,             /* eepmov.b */
        0x55, 0x04,                         /* bsr +4 */
        0xf0, 0x55,                         /* mov.b #0x55,r0h (after return) */
        0x40, 0x04,                         /* bra +4 (to end) */
        0xf1, 0x66,                         /* sub: mov.b #0x66,r1h */
        0x54, 0x70,                         /* rts */
    };
    setup(CODE); load(CODE, p, sizeof p);
    ram[0x3000] = 1; ram[0x3001] = 2; ram[0x3002] = 3;
    run_to(CODE + sizeof p);
    assert(ram[0x4000] == 1 && ram[0x4001] == 2 && ram[0x4002] == 3);
    assert(cpu.er[5] == 0x3003 && cpu.er[6] == 0x4003 && (cpu.er[4] & 0xff) == 0);
    assert(((cpu.er[0] >> 8) & 0xff) == 0x55 && ((cpu.er[1] >> 8) & 0xff) == 0x66);
    assert(cpu.er[7] == 0x80000);

    /* jsr @@0x40 through a memory-indirect vector */
    static const uint8_t q[] = { 0x5f, 0x40, 0x00, 0x00 };
    setup(CODE); load(CODE, q, sizeof q);
    ram[0x40] = 0; ram[0x41] = 0; ram[0x42] = 0x20; ram[0x43] = 0x00;
    h8s_run(&cpu, 1);
    assert(cpu.pc == 0x2000);
    assert(ram[0x7fffc] == 0 && ram[0x7fffd] == 0 && ram[0x7fffe] == 0x10 && ram[0x7ffff] == 0x02);
}

static void test_bits_on_memory(void)
{
    static const uint8_t p[] = {
        0x7a, 0x01, 0x00, 0x00, 0x50, 0x00, /* mov.l #0x5000,er1 */
        0x7d, 0x10, 0x70, 0x30,             /* bset #3,@er1 */
        0x7c, 0x10, 0x73, 0x30,             /* btst #3,@er1 -> Z=0 */
        0x7d, 0x10, 0x72, 0x00,             /* bclr #0,@er1 */
        0x6a, 0x18, 0x50, 0x01, 0x70, 0x70, /* bset #7,@0x5001:16 */
    };
    setup(CODE); load(CODE, p, sizeof p);
    ram[0x5000] = 0x01;
    run_to(CODE + sizeof p);
    assert(ram[0x5000] == 0x08);
    assert(ram[0x5001] == 0x80);
    assert(!(cpu.ccr & H8S_CCR_Z));
}

static void test_exceptions(void)
{
    /* Mode 0: TRAPA #2 -> vector 10, frame = CCR:PC(24) in 4 bytes. */
    static const uint8_t p[] = { 0x57, 0x20, 0x00, 0x00 };   /* trapa #2; nop */
    static const uint8_t h[] = { 0x56, 0x70 };               /* handler: rte */
    setup(CODE); load(CODE, p, sizeof p); load(0x2000, h, sizeof h);
    ram[40] = 0; ram[41] = 0; ram[42] = 0x20; ram[43] = 0x00;   /* vector 10 */
    cpu.ccr = 0x05;
    h8s_run(&cpu, 1);
    assert(cpu.pc == 0x2000 && (cpu.ccr & H8S_CCR_I));
    assert(cpu.er[7] == 0x80000 - 4);
    assert(ram[0x7fffc] == 0x05 && ram[0x7fffd] == 0x00 && ram[0x7fffe] == 0x10 && ram[0x7ffff] == 0x02);
    h8s_run(&cpu, 1);
    assert(cpu.pc == CODE + 2 && cpu.ccr == 0x05 && cpu.er[7] == 0x80000);

    /* Mode 0 interrupt: masked by I, taken when I clears. */
    setup(CODE);
    static const uint8_t loop[] = { 0x00, 0x00, 0x06, 0x7f, 0x40, 0xfe }; /* nop; andc #0x7f,ccr; bra . */
    load(CODE, loop, sizeof loop); load(0x2000, h, sizeof h);
    ram[80] = 0; ram[81] = 0; ram[82] = 0x20; ram[83] = 0x00;   /* vector 20 */
    cpu.ccr = H8S_CCR_I;
    pending_vec = 20; pending_level = 1; cpu.irq_check = true;
    h8s_run(&cpu, 1);                      /* nop: masked */
    assert(cpu.pc == CODE + 2 && pending_vec == 20);
    h8s_run(&cpu, 1);                      /* andc */
    h8s_run(&cpu, 1);                      /* interrupt taken before bra */
    assert(cpu.pc == 0x2000 && (cpu.ccr & H8S_CCR_I) && pending_vec == 0);
    h8s_run(&cpu, 1);                      /* rte */
    assert(cpu.pc == CODE + 4 && !(cpu.ccr & H8S_CCR_I));

    /* Mode 2: EXR stacked and raised to the accepted level. */
    setup(CODE);
    load(CODE, loop, sizeof loop); load(0x2000, h, sizeof h);
    ram[80] = 0; ram[81] = 0; ram[82] = 0x20; ram[83] = 0x00;
    cpu.intm2 = true;
    cpu.exr = H8S_EXR_NC | 2;
    pending_vec = 20; pending_level = 2; cpu.irq_check = true;
    h8s_run(&cpu, 1);                      /* level 2 not above mask 2 */
    assert(cpu.pc == CODE + 2);
    pending_level = 5; cpu.irq_check = true;
    h8s_run(&cpu, 1);
    assert(cpu.pc == 0x2000 && (cpu.exr & 7) == 5);
    assert(cpu.er[7] == 0x80000 - 6 && ram[0x7fffa] == (H8S_EXR_NC | 2));
    h8s_run(&cpu, 1);                      /* rte restores EXR */
    assert(cpu.pc == CODE + 2 && (cpu.exr & 7) == 2 && cpu.er[7] == 0x80000);

    /* SLEEP waits for an interrupt. */
    setup(CODE);
    static const uint8_t sl[] = { 0x01, 0x80, 0x00, 0x00 };      /* sleep; nop */
    load(CODE, sl, sizeof sl); load(0x2000, h, sizeof h);
    ram[80] = 0; ram[81] = 0; ram[82] = 0x20; ram[83] = 0x00;
    h8s_run(&cpu, 10);
    assert(cpu.sleeping && cpu.pc == CODE + 2);
    assert(h8s_run(&cpu, 100) == 100 && cpu.sleeping);
    pending_vec = 20; pending_level = 1; cpu.irq_check = true;
    h8s_run(&cpu, 1);
    assert(!cpu.sleeping && cpu.pc == 0x2000);
}

int main(void)
{
    test_half_carry_and_decimal();
    test_multiply_divide();
    test_ldm_stm_ldc_stc();
    test_eepmov_and_calls();
    test_bits_on_memory();
    test_exceptions();
    puts("test_h8s: all passed");
    return 0;
}
