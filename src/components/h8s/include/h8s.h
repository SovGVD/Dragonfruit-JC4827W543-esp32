#pragma once
/* Hitachi H8S/2000 CPU interpreter (advanced mode, 24-bit address space).
 *
 * Portable C with no platform dependencies, like the m68k core. Instruction
 * encodings and flag behaviour follow the H8S/2600 & H8S/2000 Programming
 * Manual, cross-checked against MAME's H8 core (BSD-3-Clause, Olivier
 * Galibert), whose instruction table is the reference for the opcode masks
 * used here. The H8S/2600 MAC instructions are not part of the H8S/2000 and
 * are treated as illegal.
 *
 * Memory is a table of 64 KB pages. A page with a host pointer is read (and,
 * if writable, written) directly; anything else goes through the machine's
 * slow-path callbacks. All accesses are big-endian and word/longword
 * accesses ignore address bit 0, as on the real bus. */
#include <stdint.h>
#include <stdbool.h>

#define H8S_PAGES 256

/* CCR bits */
#define H8S_CCR_I  0x80
#define H8S_CCR_UI 0x40
#define H8S_CCR_H  0x20
#define H8S_CCR_U  0x10
#define H8S_CCR_N  0x08
#define H8S_CCR_Z  0x04
#define H8S_CCR_V  0x02
#define H8S_CCR_C  0x01
/* EXR bits; bits 6..3 always read as 1 */
#define H8S_EXR_T  0x80
#define H8S_EXR_NC 0x78
#define H8S_EXR_I  0x07

/* Vector numbers fixed by the core */
#define H8S_VEC_RESET 0
#define H8S_VEC_TRACE 5
#define H8S_VEC_NMI   7
#define H8S_VEC_TRAPA 8

typedef struct h8s_cpu h8s_cpu_t;

typedef struct {
    uint8_t  (*read8)(void *ctx, uint32_t addr);
    uint16_t (*read16)(void *ctx, uint32_t addr);   /* addr is even */
    void     (*write8)(void *ctx, uint32_t addr, uint8_t v);
    void     (*write16)(void *ctx, uint32_t addr, uint16_t v);
    /* Return the vector of the highest-priority interrupt the CPU should take
     * now, given its CCR/EXR (cpu->ccr, cpu->exr) and interrupt control mode,
     * or 0 for none, with its priority level (0-7, 8 for NMI) in *level. The
     * machine acknowledges the source (clears edge latches) when it returns
     * a vector. */
    int      (*irq_ack)(void *ctx, h8s_cpu_t *cpu, int *level);
    /* Optional: called for an undefined opcode. */
    void     (*illegal)(void *ctx, h8s_cpu_t *cpu, uint16_t op);
} h8s_bus_t;

struct h8s_cpu {
    uint32_t er[8];
    uint32_t pc;          /* 24-bit */
    uint8_t  ccr, exr;
    /* Interrupt control mode 2 (SYSCR INTM1=1): EXR masks by priority and is
     * stacked on exception entry. Mode 0 otherwise. The machine keeps this in
     * sync with SYSCR. */
    bool     intm2;
    bool     sleeping;    /* SLEEP executed; woken by any accepted interrupt */
    /* Set by the machine whenever its pending interrupt set may have changed;
     * the core also sets it when CCR/EXR change. Cleared by the core when
     * irq_ack reports nothing to take. */
    volatile bool irq_check;

    int32_t  icount;      /* states left in the current run slice */
    int32_t  slice;       /* length of the current run slice */
    uint64_t cycles;      /* total states executed */
    uint32_t ppc;         /* address of the instruction being executed */

    uint8_t *rpage[H8S_PAGES];
    uint8_t *wpage[H8S_PAGES];
    /* States one byte / one word access to each page takes (1 after
     * h8s_init); the machine sets them from its bus controller. */
    uint8_t cost8[H8S_PAGES], cost16[H8S_PAGES];
    /* Host memory for [fine_base, fine_base + fine_size), checked only when
     * the page table sends an access to the slow path. It serves on-chip
     * RAM that shares its 64 KB page with I/O registers. fine_size 0 (the
     * default) turns it off. fine_base and fine_size must be even. */
    uint32_t fine_base, fine_size;
    uint8_t *fine_mem;
    /* Instruction fetch cache: the 64 KB page code last ran from, its host
     * memory and word cost. h8s_map and h8s_set_cost empty it. */
    uint32_t fetch_page;
    const uint8_t *fetch_host;
    int32_t fetch_cost;
    h8s_bus_t bus;
    void *ctx;
};

/* Clears registers and page tables. Map memory and set bus/ctx afterwards,
 * then call h8s_reset. */
void h8s_init(h8s_cpu_t *c);
/* Reset exception: loads PC from vector 0, sets I, EXR = I2..0 = 7. */
void h8s_reset(h8s_cpu_t *c);
/* Map [base, base+size) (64 KB aligned, size a multiple of 64 KB) to host
 * memory of host_size bytes (a multiple of 64 KB); the host block repeats
 * across the window when it is smaller. `writable` false sends writes to the
 * slow path. `host` NULL sends both directions to the slow path. */
void h8s_map(h8s_cpu_t *c, uint32_t base, uint32_t size, uint8_t *host,
             uint32_t host_size, bool writable);
/* Set the states a byte and a word access to [base, base+size) take (64 KB
 * aligned). Instruction fetches are word accesses. */
void h8s_set_cost(h8s_cpu_t *c, uint32_t base, uint32_t size, uint8_t byte_states,
                  uint8_t word_states);
/* Execute until `states` have elapsed, the CPU sleeps (the rest of the
 * slice then counts as idle time) or h8s_end_slice is called. Returns the
 * number of states that elapsed. */
int32_t h8s_run(h8s_cpu_t *c, int32_t states);
/* Take exception `vector` now (used for NMI-style events by the machine and
 * internally for interrupts and TRAPA). */
void h8s_exception(h8s_cpu_t *c, int vector);

/* States executed so far, including the part of the slice in progress, so
 * peripherals can catch up from inside a bus callback. */
static inline uint64_t h8s_time(const h8s_cpu_t *c)
{
    return c->cycles + (uint64_t)(int64_t)(c->slice - c->icount);
}
/* Make h8s_run return after the current instruction, counting only the
 * states really executed (for a machine whose next event moved closer). */
static inline void h8s_end_slice(h8s_cpu_t *c)
{
    c->slice -= c->icount;
    c->icount = 0;
}
/* Make the slice end at most `states` from now. */
static inline void h8s_trim_slice(h8s_cpu_t *c, uint32_t states)
{
    if (c->icount > 0 && (uint32_t)c->icount > states) {
        c->slice -= c->icount - (int32_t)states;
        c->icount = (int32_t)states;
    }
}

/* Memory helpers for machine code and tests (go through the page tables). */
uint8_t  h8s_read8(h8s_cpu_t *c, uint32_t a);
uint16_t h8s_read16(h8s_cpu_t *c, uint32_t a);
uint32_t h8s_read32(h8s_cpu_t *c, uint32_t a);
void     h8s_write8(h8s_cpu_t *c, uint32_t a, uint8_t v);
void     h8s_write16(h8s_cpu_t *c, uint32_t a, uint16_t v);
void     h8s_write32(h8s_cpu_t *c, uint32_t a, uint32_t v);
