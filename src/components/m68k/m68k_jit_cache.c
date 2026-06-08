/* m68k_jit_cache.c — architecture-neutral JIT block cache (see m68k_jit_cache.h).
 *
 * Pure C: the block hash, the hot-block presence bitmap (inline in the header),
 * and the generational segmented ring over the executable cache. No FreeRTOS and
 * no ISA codegen — the cpu task is the sole reader/writer. The executable memory
 * itself is allocated and written by jit_execmem; this module only owns the base
 * address as an opaque integer for the ring's address arithmetic. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "m68k_jit_cache.h"
#include "bus.h"

static const char *TAG = "m68kjit";

/* ---- block hash (open addressing) ---- */
#define JIT_HASH_SIZE 8192
#define JIT_HASH_MASK (JIT_HASH_SIZE - 1)
static jit_block_t *g_blocks;       /* active PC->block hash */
static jit_block_t *g_blocks_alt;   /* scratch table for rebuild-on-evict (ping-pong) */
static int          g_block_count;

/* ---- generational segmented ring ----
 * New blocks bump-fill the current segment; when one won't fit, we advance to the
 * next segment (the oldest, since we fill round-robin) and EVICT its contents to
 * make room. This lets the working set migrate as the running code changes.
 * Eviction is demand-driven (only on a compile of a genuinely new hot PC), so a
 * stable screen whose block set has plateaued never evicts. g_code is the cache
 * base (owned by jit_execmem's allocation, stored here as an opaque integer base
 * for segment arithmetic — this module never dereferences it). */
static uint8_t *g_code;
static int      g_code_size;
static int      g_seg_size;      /* bytes per segment (word-aligned) */
static int      g_seg_cur;       /* segment currently being filled */
static int      g_seg_used;      /* bytes used in the current segment */
static uint32_t g_jit_evictions; /* telemetry: segment recycles so far */

jit_block_t *jit_lookup(uint32_t pc)
{
    uint32_t i = ((pc >> 1) * 2654435761u) & JIT_HASH_MASK;
    for (int p = 0; p < JIT_HASH_SIZE; p++) {
        if (!g_blocks[i].used) return NULL;
        if (g_blocks[i].start_pc == pc) return &g_blocks[i];
        i = (i + 1) & JIT_HASH_MASK;
    }
    return NULL;
}

void jit_insert(uint32_t s, uint32_t end, void *entry)
{
    if (g_block_count * 4 >= JIT_HASH_SIZE * 3) return;
    uint32_t i = ((s >> 1) * 2654435761u) & JIT_HASH_MASK;
    while (g_blocks[i].used) i = (i + 1) & JIT_HASH_MASK;
    g_blocks[i].start_pc = s; g_blocks[i].end_pc = end; g_blocks[i].entry = entry;
    g_blocks[i].used = true; g_block_count++;
    jit_present_set(s & BUS_ADDR_MASK);
}

/* Evict every block living in segment `seg`: drop those entries and rebuild the
 * block hash (open addressing tolerates no holes, so we re-insert survivors into
 * the alternate table and swap) plus the present bitmap (hashed bits can't be
 * cleared individually) from the survivors. Runs on the cpu task only — the sole
 * reader/writer of these tables — so there is no concurrent lookup to tear. */
static void jit_evict_segment(int seg)
{
    uint32_t lo = (uint32_t)(uintptr_t)(g_code + seg * g_seg_size);
    uint32_t hi = lo + g_seg_size;
    jit_block_t *src = g_blocks, *dst = g_blocks_alt;
    memset(dst, 0, JIT_HASH_SIZE * sizeof(jit_block_t));
    memset(g_jit_present, 0, JIT_PRESENT_BITS / 8);
    int n = 0;
    for (int i = 0; i < JIT_HASH_SIZE; i++) {
        if (!src[i].used) continue;
        uint32_t ep = (uint32_t)(uintptr_t)src[i].entry;
        if (ep >= lo && ep < hi) continue;              /* in the evicted segment */
        uint32_t j = ((src[i].start_pc >> 1) * 2654435761u) & JIT_HASH_MASK;
        while (dst[j].used) j = (j + 1) & JIT_HASH_MASK;
        dst[j] = src[i];
        jit_present_set(src[i].start_pc & BUS_ADDR_MASK);
        n++;
    }
    g_blocks = dst; g_blocks_alt = src;
    g_block_count = n;
    g_jit_evictions++;
}

/* Reserve `total` bytes of executable cache from the segmented ring, advancing to
 * (and evicting) the next segment when the current one can't fit the block.
 * Returns the instruction-base address, or 0 if a block is too big for a segment
 * (never expected — segments are ~13KB, blocks <2KB). */
uint32_t jit_cache_alloc(int total)
{
    if (total > g_seg_size) return 0;
    if (g_seg_used + total > g_seg_size) {
        g_seg_cur  = (g_seg_cur + 1) % JIT_NSEG;   /* round-robin -> next is oldest */
        g_seg_used = 0;
        jit_evict_segment(g_seg_cur);              /* no-op until the ring first wraps */
    }
    uint32_t base = (uint32_t)(uintptr_t)(g_code + g_seg_cur * g_seg_size + g_seg_used);
    g_seg_used += total;
    return base;
}

bool jit_cache_setup(void *exec_base, int exec_size, int min_seg_bytes)
{
    g_code      = (uint8_t *)exec_base;
    g_code_size = exec_size;
    g_seg_size  = (g_code_size / JIT_NSEG) & ~3;   /* word-aligned segment */
    /* Belt-and-suspenders: jit_exec_alloc already floors the allocation at
     * JIT_NSEG * min_seg_bytes, so this can't trip — but if it ever did, the JIT
     * would silently drop every block too big for a segment, so fail cleanly. */
    if (g_seg_size < min_seg_bytes) {
        ESP_LOGE(TAG, "exec cache %d B -> %d B/seg < %d B worst-case block; JIT disabled",
                 g_code_size, g_seg_size, min_seg_bytes);
        return false;
    }
    g_seg_cur  = 0;
    g_seg_used = 0;
    g_blocks     = heap_caps_calloc(JIT_HASH_SIZE, sizeof(jit_block_t), MALLOC_CAP_SPIRAM);
    g_blocks_alt = heap_caps_calloc(JIT_HASH_SIZE, sizeof(jit_block_t), MALLOC_CAP_SPIRAM);
    if (!g_blocks || !g_blocks_alt) return false;
    ESP_LOGI(TAG, "JIT block cache: %d x %dKB segments", JIT_NSEG, g_seg_size / 1024);
    return true;
}

int      jit_cache_block_count(void) { return g_block_count; }
uint32_t jit_cache_evictions(void)   { return g_jit_evictions; }
