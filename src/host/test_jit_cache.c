/* Standalone self-test for components/m68k/m68k_jit_cache.c — the architecture-
 * neutral JIT block cache (open-addressing hash + hot-block presence bitmap + the
 * generational segmented ring). Pure C, no FreeRTOS and no ISA codegen, so the
 * cache builds and runs on the host (it could not before the JIT was split out of
 * the FreeRTOS-coupled m68k_jit.c).
 *
 * The interesting property is the ring eviction: once the cache wraps, the oldest
 * segment's blocks are dropped and the hash + presence bitmap are rebuilt (via the
 * ping-pong table) from the survivors. A rebuild that lost or misplaced a survivor
 * used to be catchable only on-device; here we force a wrap and assert every
 * survivor still resolves while the evicted block does not.
 */
#include "m68k_jit_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* g_jit_present is defined in m68k.c (the interpreter TU) on device and allocated
 * by the JIT core; the cache module only reads/writes it. Provide it here so the
 * cache links standalone. */
uint8_t *g_jit_present;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void)
{
    /* Tiny cache: JIT_NSEG segments x 256 B, worst-case block 256 B. The "exec"
     * region is just a heap buffer — the cache treats the base as an opaque integer
     * for segment arithmetic and never dereferences it. */
    enum { SEG = 256, EXEC = SEG * JIT_NSEG };
    void *exec = malloc(EXEC);
    g_jit_present = calloc(JIT_PRESENT_BITS / 8, 1);
    CHECK(exec && g_jit_present, "allocation failed");

    CHECK(jit_cache_setup(exec, EXEC, SEG), "jit_cache_setup failed");

    /* Insert one full-segment block per segment, with distinct ROM-ish PCs. A
     * 256 B block exactly fills a segment, so block N lands in segment N; block
     * JIT_NSEG wraps back to segment 0, evicting block 0. */
    uint32_t pc[JIT_NSEG + 1];
    void    *entry[JIT_NSEG + 1];
    for (int i = 0; i <= JIT_NSEG; i++) {
        pc[i] = 0x10C08000u + (uint32_t)i * 0x100u;
        uint32_t base = jit_cache_alloc(SEG);
        CHECK(base != 0, "jit_cache_alloc %d returned 0", i);
        entry[i] = (void *)(uintptr_t)base;
        jit_insert(pc[i], pc[i] + 2, entry[i]);
        CHECK(jit_lookup(pc[i]) != NULL, "block %d not found right after insert", i);
    }

    /* Block 0 was evicted by the wrap; blocks 1..JIT_NSEG survive the rebuild. */
    CHECK(jit_lookup(pc[0]) == NULL, "block 0 should have been evicted");
    for (int i = 1; i <= JIT_NSEG; i++) {
        jit_block_t *b = jit_lookup(pc[i]);
        CHECK(b != NULL, "survivor block %d lost after eviction", i);
        if (b) {
            CHECK(b->start_pc == pc[i],  "block %d start_pc 0x%X != 0x%X", i, b->start_pc, pc[i]);
            CHECK(b->entry    == entry[i], "block %d entry mismatch", i);
            /* Presence bit for a survivor must be set by the rebuild. (We can't
             * assert the evicted block's bit is clear — the hash is lossy and a
             * survivor may collide onto the same bit.) */
            CHECK(jit_present_bit(pc[i] & 0x00FFFFFFu), "block %d presence bit clear", i);
        }
    }
    CHECK(jit_cache_evictions() >= 1,
          "expected >= 1 eviction, got %u", (unsigned)jit_cache_evictions());
    CHECK(jit_cache_block_count() == JIT_NSEG,
          "block count %d != %d after one eviction", jit_cache_block_count(), JIT_NSEG);

    if (failures) { printf("test_jit_cache: %d check(s) FAILED\n", failures); return 1; }
    printf("test_jit_cache: all checks passed\n");
    return 0;
}
