#pragma once
/* m68k_jit_backend.h — the ISA code-generation backend interface.
 *
 * The architecture-neutral JIT core (m68k_jit_core.c) drives compilation through
 * this interface; the implementation is one translation unit per ISA, selected at
 * build time by CONFIG_IDF_TARGET_ARCH_* (see the m68k component CMakeLists):
 *   - m68k_jit_backend_xtensa.c  — the classic-ESP32 68k->Xtensa translator,
 *   - m68k_jit_backend_riscv.c   — (future) a RISC-V backend; a no-op stub today.
 *
 * The seam is crossed only at compile/install time (never per emulated
 * instruction): the core asks the backend to assemble a basic block's bytes
 * (backend_codegen, pure — reads only the immutable ROM), then to lay them into a
 * core-reserved slot of the executable cache (backend_finalize). Running a native
 * block is a plain function-pointer call into the cache and touches no backend C.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "m68k.h"

/* Per-block codegen sizing. The assembled block (code + literal pool) is carried
 * by value through the core's compile queue, so backend_block_t must be POD with
 * no internal pointers. */
#define JIT_CODEBUF_BYTES 1536          /* worst-case assembled code bytes / block */
#define JIT_MAX_LITS 24                 /* literal-pool depth */
#define JIT_MAX_FIX  64                 /* l32r/branch fixups recorded per block */

/* Worst-case bytes one finished block can occupy in the cache: its code plus its
 * literal pool, word-aligned. The core floors each cache segment at this so a
 * block can always fit (see m68k_jit_cache.c / jit_ensure_ready). */
#define BACKEND_MAX_BLOCK_BYTES (((JIT_CODEBUF_BYTES + JIT_MAX_LITS * 4) + 3) & ~3)

/* An assembled-but-unlinked block. POD (no self-pointers) -> safe to memcpy
 * through a FreeRTOS queue by value. `code` holds the raw instruction bytes; the
 * literal pool + fixup table let backend_finalize patch l32r/branch displacements
 * once the final cache address is known. Opaque to the core, which only routes it
 * between backend_codegen and backend_finalize. */
typedef struct {
    uint8_t  code[JIT_CODEBUF_BYTES];
    int      pos;                       /* bytes of code emitted */
    bool     overflow;                  /* codegen overran a buffer -> discard */
    uint32_t lits[JIT_MAX_LITS];
    int      n_lits;
    struct { int pos; int lit; } fix[JIT_MAX_FIX];
    int      n_fix;
} backend_block_t;

/* Reserve `total_bytes` of executable cache and return the instruction-space base
 * (0 on failure). Supplied by the core (= jit_cache_alloc) and passed into
 * backend_finalize so the backend never depends on the core's cache internals. */
typedef uint32_t (*jit_reserve_fn)(int total_bytes);

/* True if this backend emits native code at all. A no-op backend (e.g. the RISC-V
 * stub) returns false; the core then skips exec-cache setup and runs the pure
 * interpreter for everything (identical to g_m68k_accel_mode == 0). */
bool backend_has_codegen(void);

/* Assemble the basic block starting at `pc` into *blk. PURE: reads only the
 * immutable ROM + compile scratch; touches no executable memory and no tables, so
 * it is safe to run on the off-core async compiler. Sets *end_pc to the block's
 * fall-through PC. Returns false if nothing was emitted (an unsupported leader). */
bool backend_codegen(uint32_t pc, backend_block_t *blk, uint32_t *end_pc);

/* Lay [literal pool][code] into the cache at a base obtained via `reserve`, patch
 * the recorded fixups, write the words (icache-synced), and return the block's
 * entry address (NULL if it couldn't be placed). Runs on the cpu task only. */
void *backend_finalize(backend_block_t *blk, jit_reserve_fn reserve);
