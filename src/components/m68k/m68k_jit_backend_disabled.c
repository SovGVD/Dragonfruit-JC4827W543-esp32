/* No code generation on targets without a validated native backend. */
#include <stdint.h>
#include <stdbool.h>
#include "m68k_jit_backend.h"

bool backend_has_codegen(void)
{
    return false;   /* no codegen -> core stays on the interpreter */
}

bool backend_codegen(uint32_t pc, backend_block_t *blk, uint32_t *end_pc)
{
    (void)pc; (void)blk; (void)end_pc;
    return false;   /* never produces a block, so the core never compiles or installs */
}

void *backend_finalize(backend_block_t *blk, jit_reserve_fn reserve)
{
    (void)blk; (void)reserve;
    return NULL;     /* unreachable: only blocks codegen produced are finalized */
}
