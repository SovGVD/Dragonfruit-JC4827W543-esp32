#pragma once
/* jit_execmem.h — chip-specific executable-memory backend for the JIT cache.
 *
 * Isolates the parts that vary by SoC memory topology (not by 68k->native ISA):
 * how CPU-executable RAM is allocated, how a word is written into instruction
 * space, and how the i-cache is synced. The classic ESP32 implementation
 * (jit_execmem_esp32.c) handles the SRAM0-vs-D/IRAM(SRAM1) split; a RISC-V part
 * (e.g. ESP32-P4) would supply a flat-SRAM + fence.i implementation instead. */

#include <stdint.h>
#include <stddef.h>

/* A reserved block of CPU-executable memory. base==NULL on failure. */
typedef struct { void *base; int size; } jit_exec_region_t;

/* Allocate the executable JIT cache. Tries for the full design size, shrinking
 * toward `min_bytes` if that much contiguous executable memory isn't free;
 * base==NULL if even `min_bytes` can't be satisfied. */
jit_exec_region_t jit_exec_alloc(size_t min_bytes);

/* Write one 32-bit instruction word at an instruction-space address, handling any
 * chip-specific data/instruction aliasing. */
void jit_exec_write_word(uint32_t instr_addr, uint32_t w);

/* Make freshly written code visible to instruction fetch (isync / fence.i). */
void jit_exec_icache_sync(void);
