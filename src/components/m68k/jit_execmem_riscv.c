/* jit_execmem_riscv.c — placeholder executable-memory backend for RISC-V targets.
 *
 * No-op. Paired with the no-codegen m68k_jit_backend_riscv.c, the JIT core never
 * allocates or writes the executable cache, so none of this is exercised — it
 * exists only so a CONFIG_IDF_TARGET_ARCH_RISCV build links. A real backend would
 * allocate CPU-executable RAM (flat SRAM on e.g. the ESP32-P4 — no classic-ESP32
 * D/IRAM inversion) and make written code visible to fetch with `fence.i`.
 */

#include <stdint.h>
#include <stddef.h>
#include "jit_execmem.h"

jit_exec_region_t jit_exec_alloc(size_t min_bytes)
{
    (void)min_bytes;
    jit_exec_region_t r = { NULL, 0 };
    return r;
}

void jit_exec_write_word(uint32_t instr_addr, uint32_t w)
{
    (void)instr_addr; (void)w;
}

void jit_exec_icache_sync(void)
{
}
