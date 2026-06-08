/* jit_execmem_esp32.c — executable-memory backend for the classic ESP32 (Xtensa).
 *
 * g_code (returned to the caller as the cache base) is an EXECUTABLE
 * (instruction-bus) address. Two cases the write path must handle:
 *  - SRAM0 IRAM pool (0x4008..0x400A): the instruction address is also the
 *    word-writable data address -> write directly.
 *  - D/IRAM (SRAM1, instruction alias 0x400A..0x400C): word-INVERTED. Code is
 *    written via the DATA alias of each word's instruction address
 *    (esp_ptr_diram_iram_to_dram), which reads forward in instruction space.
 * Using D/IRAM gives a ~100 KB cache (vs ~36 KB SRAM0), big enough to hold the
 * working set without flush-thrash.
 *
 * IRAM tolerates only 32-bit stores, so the finished block is word-copied in (see
 * backend_finalize). g_diram is the only state kept here; the cache base/size and
 * the segmented-ring policy over it live in the neutral m68k_jit_cache.c. */

#include <stdint.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "jit_execmem.h"

static const char *TAG = "m68kjit";

#define JIT_CACHE_BYTES (104 * 1024)    /* design size; shrinks if EXEC heap is tighter */

static bool g_diram;            /* true => write via the inverted data alias */

jit_exec_region_t jit_exec_alloc(size_t min_bytes)
{
    jit_exec_region_t r = { NULL, 0 };

    /* Floor the allocation so every segment can hold one worst-case block; below
     * this the JIT would "succeed" but silently drop every block too big for a
     * segment. Better to fail here and fall back cleanly to the interpreter. */
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
    ESP_LOGI(TAG, "EXEC heap: total free=%u, largest block=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC), (unsigned)largest);
    size_t want = JIT_CACHE_BYTES;
    if (want > largest) want = (largest > min_bytes) ? (largest - 4096) : 0;

    uint8_t *code = NULL;
    while (want >= min_bytes) {
        code = heap_caps_malloc(want, MALLOC_CAP_EXEC);
        if (code && esp_ptr_executable(code)) break;
        if (code) { heap_caps_free(code); code = NULL; }
        want -= 8192;
    }
    if (!code) {
        ESP_LOGE(TAG, "exec cache alloc failed (largest EXEC blk=%u, need >=%u)",
                 (unsigned)largest, (unsigned)min_bytes);
        return r;
    }
    g_diram = esp_ptr_in_diram_iram(code);   /* SRAM1 instruction alias? */
    r.base = code;
    r.size = (int)want;
    ESP_LOGI(TAG, "JIT exec cache: %dKB @ %p (%s)",
             (int)want / 1024, code, g_diram ? "D/IRAM inverted" : "SRAM0 linear");
    return r;
}

void jit_exec_write_word(uint32_t instr_addr, uint32_t w)
{
    volatile uint32_t *p = g_diram
        ? (volatile uint32_t *)esp_ptr_diram_iram_to_dram((void *)(uintptr_t)instr_addr)
        : (volatile uint32_t *)(uintptr_t)instr_addr;
    *p = w;
}

void jit_exec_icache_sync(void)
{
    __asm__ __volatile__("isync");
}
