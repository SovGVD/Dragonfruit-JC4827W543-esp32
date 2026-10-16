/* No executable cache on targets without a validated native backend. */
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
