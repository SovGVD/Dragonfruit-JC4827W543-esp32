/* Differential test of the H8S core against the GNU simulator (sim/h8300 from
 * binutils-gdb, H8S mode). Each case loads one random instruction drawn from
 * the opcode patterns, random registers and CCR/EXR, steps both CPUs once and
 * compares registers, PC and every byte our core wrote.
 *
 * Build: see README.md in this directory. Usage: h8s_diff [cases] [seed] */
#include "h8s.h"
extern unsigned h8s_odd_accesses;
#include "sim/sim.h"
#include "sim/callback.h"
#include "sim/sim-h8300.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include "patterns.h"

#define MEM_SIZE (1u << 24)
#define CODE 0x1000u

static uint8_t *mem;          /* our core's memory, all accesses via slow path */
static uint32_t writes[64];
static int nwrites;
static SIM_DESC sd;

static void log_write(uint32_t a)
{
    if (nwrites < 64) writes[nwrites++] = a;
}
/* The sim treats 0xff0000-0xffffff (on-chip RAM and I/O) its own way. */
static bool touched_io;
static uint8_t r8(void *ctx, uint32_t a) { (void)ctx; touched_io |= (a & 0xffffff) >= 0xff0000; return mem[a & 0xffffff]; }
static uint16_t r16(void *ctx, uint32_t a) { (void)ctx; a &= 0xfffffe; touched_io |= a >= 0xff0000; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void w8(void *ctx, uint32_t a, uint8_t v) { (void)ctx; a &= 0xffffff; touched_io |= a >= 0xff0000; mem[a] = v; log_write(a); }
static void w16(void *ctx, uint32_t a, uint16_t v)
{
    (void)ctx; a &= 0xfffffe; touched_io |= a >= 0xff0000;
    mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v;
    log_write(a); log_write(a + 1);
}
static int no_irq(void *ctx, h8s_cpu_t *c, int *level) { (void)ctx; (void)c; (void)level; return 0; }

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

static uint32_t hexval(const char *s, int from, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        char ch = s[from + i];
        v = v << 4 | (uint32_t)(ch <= '9' ? ch - '0' : ch - 'a' + 10);
    }
    return v;
}

static void sim_set(int reg, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    sim_store_register(sd, reg, b, 4);
}
static uint32_t sim_get(int reg)
{
    uint8_t b[4] = { 0 };
    sim_fetch_register(sd, reg, b, 4);
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

int main(int argc, char **argv)
{
    long cases = argc > 1 ? atol(argv[1]) : 100000;
    if (argc > 2) rng ^= strtoull(argv[2], NULL, 0) * 0x9e3779b97f4a7c15ull;
    const char *only = argc > 3 ? argv[3] : NULL;

    char *sargv[] = { "h8s_diff", "--h8300s", NULL };
    sd = sim_open(SIM_OPEN_STANDALONE, &default_callback, NULL, sargv);
    if (!sd) { fprintf(stderr, "sim_open failed\n"); return 2; }
    sim_create_inferior(sd, NULL, NULL, NULL);

    mem = malloc(MEM_SIZE);
    for (uint32_t i = 0; i < MEM_SIZE; i++) mem[i] = (uint8_t)rnd();
    for (uint32_t off = 0; off < MEM_SIZE; off += 1u << 20) sim_write(sd, off, mem + off, 1u << 20);

    h8s_cpu_t cpu;
    h8s_init(&cpu);
    cpu.bus = (h8s_bus_t){ r8, r16, w8, w16, no_irq, NULL };

    const int npat = (int)(sizeof patterns / sizeof patterns[0]);
    long fails = 0, run = 0, skipped = 0;
    for (long t = 0; t < cases && fails < 30; t++) {
        int p = (int)(rnd() % (unsigned)npat);
        if (only && !strstr(patterns[p].name, only)) { t--; continue; }
        /* Not comparable with the GNU sim: it models on-chip I/O at @aa:8,
         * uses its own exception frame (rte), decodes ldm/stm the H8SX way,
         * and stores a different low byte for stc.w. Unit tests cover these. */
        static const char *const skip[] = { "abs8,", ",abs8", "rte ", "ldm.l", "stm.l", "stc.w",
                                            "stc exr", "eepmov", "divx", "ldc.w" };
        bool skipit = false;
        for (unsigned k = 0; k < sizeof skip / sizeof skip[0]; k++)
            if (strstr(patterns[p].name, skip[k])) skipit = true;
        if (skipit) { t--; continue; }
        const char *v = patterns[p].val, *m = patterns[p].mask;
        int nw = (int)strlen(v) / 4;
        uint8_t code[16];
        for (int i = 0; i < 8; i++) {
            uint16_t w = (uint16_t)rnd();
            if (i < nw) {
                uint16_t pv = (uint16_t)hexval(v, i * 4, 4), pm = (uint16_t)hexval(m, i * 4, 4);
                w = (uint16_t)((w & ~pm) | pv);
            }
            code[i * 2] = (uint8_t)(w >> 8);
            code[i * 2 + 1] = (uint8_t)w;
        }
        /* The GNU sim turns jsr to 0xc5..0xcc into host syscalls. */
        if (code[0] == 0x5f || code[0] == 0x5e || code[0] == 0x5b) {
            if (code[1] >= 0xc4 && code[1] <= 0xcd) continue;
        }
        if (code[0] == 0x5e && code[1] == 0 && code[2] == 0) continue;
        /* Rs,@-ERd with Rs inside ERd: the core stores the value before the
         * decrement (as MAME does), the sim the value after. */
        if (strstr(patterns[p].name, "pr32h")) {
            uint8_t b = code[0] == 0x01 ? code[3] : code[1];
            if (((b >> 4) & 7) == (b & 7)) continue;
        }
        /* The sim ignores H in DAA/DAS adjustments. */
        bool decimal = strstr(patterns[p].name, "daa") || strstr(patterns[p].name, "das");
        memcpy(mem + CODE, code, sizeof code);
        sim_write(sd, CODE, code, sizeof code);

        /* The GNU sim indexes its memory with unmasked 32-bit addresses in
         * places, so instructions that touch memory get 24-bit pointers. */
        const char *nm = patterns[p].name;
        bool memop = strstr(nm, "r32i") || strstr(nm, "r32p") || strstr(nm, "pr32") ||
                     strstr(nm, "r32d") || strstr(nm, "sp") || strstr(nm, "abs") ||
                     strstr(nm, "rel") || strstr(nm, "jsr") || strstr(nm, "rts") ||
                     strstr(nm, "rte") || strstr(nm, "eepmov") || strstr(nm, "tas");
        uint32_t regs[8];
        for (int i = 0; i < 8; i++) {
            /* Keep pointers mostly away from the code. */
            regs[i] = rnd();
            if (memop) regs[i] = (regs[i] & 0x7ffffe) | 0x100000;
            if ((regs[i] & 0xfff000) == (CODE & 0xfff000)) regs[i] ^= 0x100000;
        }
        uint8_t ccr = (uint8_t)rnd(), exr = (uint8_t)(rnd() | H8S_EXR_NC);
        if (decimal) ccr &= (uint8_t)~(H8S_CCR_H | H8S_CCR_C);   /* sim mishandles both */
        for (int i = 0; i < 8; i++) { cpu.er[i] = regs[i]; sim_set(SIM_H8300_R0_REGNUM + i, regs[i]); }
        cpu.pc = CODE; cpu.ccr = ccr; cpu.exr = exr; cpu.intm2 = true; cpu.sleeping = false;
        sim_set(SIM_H8300_PC_REGNUM, CODE);
        sim_set(SIM_H8300_CCR_REGNUM, ccr);
        sim_set(SIM_H8300_EXR_REGNUM, exr);
        cpu.exr = (uint8_t)sim_get(SIM_H8300_EXR_REGNUM) | H8S_EXR_NC;
        if ((uint8_t)sim_get(SIM_H8300_CCR_REGNUM) != ccr) { fprintf(stderr, "ccr store mismatch\n"); }

        nwrites = 0;
        h8s_odd_accesses = 0;
        touched_io = false;
        cpu.irq_check = false;
        cpu.icount = 1;
        /* one instruction */
        h8s_run(&cpu, 1);
        sim_resume(sd, 1, 0);
        enum sim_stop reason; int sig;
        sim_stop_reason(sd, &reason, &sig);
        run++;
        if (h8s_odd_accesses || touched_io) {
            /* Odd word address (the real bus, and our core, drop bit 0) or
             * on-chip I/O: not comparable. The sim may have written somewhere
             * else, so take its whole memory image. */
            for (uint32_t off = 0; off < MEM_SIZE; off += 1u << 20) sim_read(sd, off, mem + off, 1u << 20);
            skipped++;
            continue;
        }

        bool bad = false;
        char why[4096]; int wl = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t s = sim_get(SIM_H8300_R0_REGNUM + i);
            if (s != cpu.er[i]) { bad = true; wl += snprintf(why + wl, sizeof why - wl, " ER%d ours %08X sim %08X;", i, cpu.er[i], s); }
        }
        uint32_t spc = sim_get(SIM_H8300_PC_REGNUM) & 0xffffff;
        if (spc != cpu.pc) { bad = true; wl += snprintf(why + wl, sizeof why - wl, " PC ours %06X sim %06X;", cpu.pc, spc); }
        /* The GNU sim never computes the half-carry flag; H is checked by the
         * unit tests instead. */
        uint8_t sccr = (uint8_t)sim_get(SIM_H8300_CCR_REGNUM);
        /* DAA/DAS leave H and V undefined; the sim also drops the decimal
         * carry that the manual's tables keep, so only N/Z are compared. */
        uint8_t cmask = (uint8_t)~H8S_CCR_H;
        if (strstr(nm, "daa") || strstr(nm, "das")) cmask = H8S_CCR_N | H8S_CCR_Z;
        /* The sim never sets V for SHAL #2 and computes INC/DEC V as an
         * unsigned carry; the core follows the manual (and MAME). */
        if ((strstr(nm, "shal") && strstr(nm, "two")) || strstr(nm, "inc.") || strstr(nm, "dec."))
            cmask &= (uint8_t)~H8S_CCR_V;
        /* Sim bugs: ADDX/SUBX set Z on a zero result (the manual keeps Z),
         * SUBX folds the carry into V, NEG misses V for 0x80. */
        if (strstr(nm, "addx") || strstr(nm, "subx")) cmask &= (uint8_t)~(H8S_CCR_Z | H8S_CCR_V);
        if (strstr(nm, "neg")) cmask &= (uint8_t)~H8S_CCR_V;
        if ((sccr ^ cpu.ccr) & cmask) { bad = true; wl += snprintf(why + wl, sizeof why - wl, " CCR ours %02X sim %02X (in %02X);", cpu.ccr, sccr, ccr); }
        /* The sim rewrites EXR from internal state on every step, so EXR is
         * left to the unit tests too. */
        for (int i = 0; i < nwrites && wl < 3000; i++) {
            uint8_t s;
            sim_read(sd, writes[i], &s, 1);
            if (s != mem[writes[i]]) {
                bad = true;
                wl += snprintf(why + wl, sizeof why - wl, " MEM %06X ours %02X sim %02X;", writes[i], mem[writes[i]], s);
                mem[writes[i]] = s;
            }
        }
        if (reason != sim_stopped || (sig != 5 && sig != 0)) {
            /* The sim rejected the instruction (an encoding it lacks, or an
             * odd branch target): nothing to compare. */
            for (int i = 0; i < nwrites; i++) sim_write(sd, writes[i], mem + writes[i], 1);
            skipped++;
            continue;
        }
        if (bad) {
            fails++;
            printf("FAIL %-24s code", patterns[p].name);
            for (int i = 0; i < 10; i++) printf(" %02X", code[i]);
            printf(" |%s\n", why);
            /* resync the sim's view of memory we wrote */
            for (int i = 0; i < nwrites; i++) sim_write(sd, writes[i], mem + writes[i], 1);
        }
    }
    printf("%ld cases, %ld skipped by the sim, %ld failures\n", run, skipped, fails);
    return fails ? 1 : 0;
}
