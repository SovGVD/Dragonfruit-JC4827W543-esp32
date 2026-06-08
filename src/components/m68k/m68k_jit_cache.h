#pragma once
/* m68k_jit_cache.h — architecture-neutral JIT block cache.
 *
 * The PC->native-block hash, the hot-block presence bitmap, and the generational
 * segmented ring that holds compiled blocks in the executable cache. Pure C (no
 * FreeRTOS, no ISA codegen) so it is unit-testable on the host build. The cpu task
 * is the sole reader/writer, so there are no concurrent lookups to tear. */

#include <stdint.h>
#include <stdbool.h>
#include "m68k.h"   /* g_jit_present, JIT_PRESENT_HASH, JIT_PRESENT_BITS */

/* A compiled block: [start_pc, end_pc) -> native entry. */
typedef struct { uint32_t start_pc, end_pc; void *entry; bool used; } jit_block_t;

/* The executable cache is split into JIT_NSEG equal segments: new blocks bump-fill
 * the current one; when it can't fit a block we advance to the next (the oldest,
 * since we fill round-robin) and evict its contents. Exposed so the core can floor
 * the exec-cache allocation at JIT_NSEG * BACKEND_MAX_BLOCK_BYTES. */
#define JIT_NSEG 8

/* Bind the block tables + segmented ring to an executable region obtained from
 * jit_exec_alloc(). `min_seg_bytes` is the worst-case block size each segment must
 * hold (BACKEND_MAX_BLOCK_BYTES). Returns false if the tables can't be allocated
 * or a segment would be too small for a worst-case block. */
bool jit_cache_setup(void *exec_base, int exec_size, int min_seg_bytes);

/* Block hash (open addressing). */
jit_block_t *jit_lookup(uint32_t pc);
void         jit_insert(uint32_t start_pc, uint32_t end_pc, void *entry);

/* Reserve `total` bytes from the segmented ring, advancing to (and evicting) the
 * next segment when the current one can't fit the block. Returns the instruction-
 * space base, or 0 if a block is too big for a segment. Signature matches
 * jit_reserve_fn so it can be handed to backend_finalize. */
uint32_t jit_cache_alloc(int total);

/* Telemetry for the core's periodic JIT readout. */
int      jit_cache_block_count(void);
uint32_t jit_cache_evictions(void);

/* Hot-block presence filter (shared with the interpreter burst in m68k.c via the
 * JIT_PRESENT_HASH in m68k.h). A set bit may be a hash false-positive; jit_lookup
 * confirms. Inline so the per-block check in the dispatcher and the per-op check
 * in the burst stay a single load+test with no cross-TU call. */
static inline void jit_present_set(uint32_t mpc)
{
    if (!g_jit_present) return;
    uint32_t i = JIT_PRESENT_HASH(mpc);
    g_jit_present[i >> 3] |= (1u << (i & 7));
}
static inline bool jit_present_bit(uint32_t mpc)
{
    if (!g_jit_present) return false;
    uint32_t i = JIT_PRESENT_HASH(mpc);
    return (g_jit_present[i >> 3] >> (i & 7)) & 1u;
}
