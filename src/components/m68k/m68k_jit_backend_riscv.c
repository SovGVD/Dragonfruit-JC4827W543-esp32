/* m68k_jit_backend_riscv.c — placeholder RISC-V code-generation backend.
 *
 * No native code generation yet. backend_has_codegen() returns false, so the
 * architecture-neutral JIT core (m68k_jit_core.c) skips executable-cache setup
 * and runs the pure interpreter for everything — behaviourally identical to
 * g_m68k_accel_mode == 0. This unit exists so a CONFIG_IDF_TARGET_ARCH_RISCV
 * build (e.g. ESP32-P4) links and boots interpreter-only; a real 68k->RISC-V
 * translator would replace these stubs with an instruction emitter + jit_emit_op,
 * mirroring m68k_jit_backend_xtensa.c, and pair with a real jit_execmem_*.c.
 */

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
