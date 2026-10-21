/* jit_execmem_esp32.c — executable-memory backend for the Xtensa ESP32 and ESP32-S3.
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
 * the segmented-ring policy over it live in the neutral m68k_jit_cache.c.
 *
 * ESP32-S3: all internal SRAM is D/IRAM, mapped linearly (not inverted) on both
 * buses, and heap_caps_malloc(MALLOC_CAP_EXEC) hands back the instruction-bus
 * address. esp_ptr_diram_iram_to_dram() already knows the S3 mapping, so the same
 * write path works unchanged. The exec heap only exists with memory protection
 * (PMS) disabled. The S3 also has less internal RAM to spare than the M5Paper's
 * ESP32 once Wi-Fi is up, so the cache leaves JIT_KEEP_INTERNAL_FREE behind. */

#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "soc/soc.h"
#include "esp_log.h"
#include "jit_execmem.h"

static const char *TAG = "m68kjit";

#ifndef MALLOC_CAP_EXEC
#error "The m68k JIT needs an executable heap: disable CONFIG_ESP_SYSTEM_MEMPROT"
#endif

#define JIT_CACHE_BYTES (104 * 1024)    /* design size; shrinks if EXEC heap is tighter */
/* Internal RAM left for Wi-Fi/lwIP, task stacks, display and SD DMA. */
#define JIT_KEEP_INTERNAL_FREE ((size_t)CONFIG_M68K_JIT_KEEP_INTERNAL_KB * 1024)

#if SOC_DIRAM_INVERTED
#define DIRAM_KIND "D/IRAM inverted"
#else
#define DIRAM_KIND "D/IRAM linear"
#endif

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
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal_free < JIT_KEEP_INTERNAL_FREE + min_bytes) want = 0;
    else if (want > internal_free - JIT_KEEP_INTERNAL_FREE)
        want = (internal_free - JIT_KEEP_INTERNAL_FREE) & ~(size_t)8191;

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
#ifdef M68K_JIT_EXEC_WRITE_DIRECT
    /* QEMU's esp32s3 model keeps the IRAM and DRAM windows as separate memories
     * (no alias), so the bench app's QEMU build writes code through the
     * instruction address itself. Real hardware uses the data alias. */
    g_diram = false;
#endif
    r.base = code;
    r.size = (int)want;
    ESP_LOGI(TAG, "JIT exec cache: %dKB @ %p (%s)",
             (int)want / 1024, code, g_diram ? DIRAM_KIND : "direct");
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
