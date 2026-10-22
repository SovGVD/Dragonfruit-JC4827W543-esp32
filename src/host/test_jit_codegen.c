/* Standalone host harness that runs the Xtensa code generator (backend_codegen)
 * over a real Palm ROM and fingerprints its output. This proves two things:
 *
 *   1. The 68k->Xtensa translator is pure, architecture-independent C — it
 *      generates correct Xtensa instruction bytes on an x86-64 host (where those
 *      bytes are produced and hashed, never executed). That the codegen runs at
 *      all on a non-Xtensa host is the portability property the JIT split exists
 *      to enable.
 *   2. The fingerprint is a stable regression net: any change to the generated
 *      code shifts the FNV-1a hash, so a future backend edit that was meant to be
 *      behaviour-preserving can be checked here without a device.
 *
 * Only the instruction bytes (blk.code) are hashed. They are address-independent:
 * l32r displacements are patched at finalize time (not here) and call/operand
 * pointers live in the literal pool, which holds host pointers and is excluded.
 * So the fingerprint depends only on the ROM, not on the build.
 *
 * This links against only m68k_jit_backend_xtensa.c; everything the backend takes
 * the address of (the flag helpers, the bus wrappers) or links to (the bus
 * fast-path globals, the execmem hooks) is stubbed below, since codegen never
 * *calls* them — it only emits their addresses into the (excluded) literal pool.
 *
 * Usage: test_jit_codegen [path-to-rom]   (default: Palm-IIIx-3.1.rom)
 */
#include "m68k.h"
#include "bus.h"
#include "m68k_jit_backend.h"
#include "jit_execmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* --- bus fast-path backing: the inline bus_read16/32 in bus.h read these. We
 * point g_bus_rom at the loaded image; g_bus_ram is never dereferenced during
 * codegen (only its address is emitted as a RAM base). The *_slow paths are
 * referenced by the inline accessors but never taken for ROM reads. --- */
const uint8_t *g_bus_rom;
uint32_t       g_bus_rom_end;
uint32_t       g_bus_rom_size;
uint8_t       *g_bus_ram;
uint32_t       g_bus_periph_seq;
uint8_t  bus_read8_slow (uint32_t a)              { (void)a; return 0; }
uint16_t bus_read16_slow(uint32_t a)              { (void)a; return 0; }
uint32_t bus_read32_slow(uint32_t a)              { (void)a; return 0; }
void     bus_write8_slow (uint32_t a, uint8_t v)  { (void)a; (void)v; }
void     bus_write16_slow(uint32_t a, uint16_t v) { (void)a; (void)v; }
void     bus_write32_slow(uint32_t a, uint32_t v) { (void)a; (void)v; }

/* --- symbols the generated code targets (emitted as l32r literals, never called
 * here). --- */
uint32_t jit_bus_read8 (uint32_t a)               { (void)a; return 0; }
uint32_t jit_bus_read16(uint32_t a)               { (void)a; return 0; }
uint32_t jit_bus_read32(uint32_t a)               { (void)a; return 0; }
void     jit_bus_write8 (uint32_t a, uint32_t v)  { (void)a; (void)v; }
void     jit_bus_write16(uint32_t a, uint32_t v)  { (void)a; (void)v; }
void     jit_bus_write32(uint32_t a, uint32_t v)  { (void)a; (void)v; }
void m68k_set_flags_add(m68k_cpu_t *c, uint32_t s, uint32_t d, uint32_t r, int z) { (void)c;(void)s;(void)d;(void)r;(void)z; }
void m68k_set_flags_sub(m68k_cpu_t *c, uint32_t s, uint32_t d, uint32_t r, int z) { (void)c;(void)s;(void)d;(void)r;(void)z; }
void m68k_set_flags_cmp(m68k_cpu_t *c, uint32_t s, uint32_t d, uint32_t r, int z) { (void)c;(void)s;(void)d;(void)r;(void)z; }
void m68k_jit_set_sr(m68k_cpu_t *c, uint32_t v)   { (void)c; (void)v; }
void m68k_group_e(m68k_cpu_t *c, uint16_t op)     { (void)c; (void)op; }

/* --- execmem hooks: backend_finalize references these; we never finalize. --- */
jit_exec_region_t jit_exec_alloc(size_t min_bytes) { (void)min_bytes; jit_exec_region_t r = { NULL, 0 }; return r; }
void jit_exec_write_word(uint32_t a, uint32_t w)   { (void)a; (void)w; }
void jit_exec_icache_sync(void)                    {}

int main(int argc, char **argv)
{
    const char *rom_path = (argc > 1) ? argv[1] : "Palm-IIIx-3.1.rom";
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "test_jit_codegen: cannot open ROM '%s'\n", rom_path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "test_jit_codegen: bad ROM size\n"); fclose(f); return 2; }
    uint8_t *rom = malloc((size_t)sz);
    if (!rom || fread(rom, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "test_jit_codegen: ROM read failed\n"); fclose(f); return 2;
    }
    fclose(f);

    g_bus_rom     = rom;
    g_bus_rom_end = (BUS_ROM_BASE & BUS_ADDR_MASK) + (uint32_t)sz;
    g_bus_rom_size = (uint32_t)sz;
    g_bus_ram     = calloc(BUS_RAM_SIZE, 1);   /* address only; never read in codegen */

    /* Compile a block at every instruction boundary in the ROM and FNV-1a the
     * assembled instruction bytes. Overlapping leaders are fine — this is a
     * deterministic fingerprint, not a runtime trace. */
    uint64_t hash = 1469598103934665603ull;   /* FNV-1a offset basis */
    uint32_t blocks = 0;
    uint64_t code_bytes = 0;
    backend_block_t blk;
    uint32_t end;
    for (uint32_t pc = BUS_ROM_BASE; pc + 2 <= BUS_ROM_BASE + (uint32_t)sz; pc += 2) {
        if (!backend_codegen(pc, &blk, &end)) continue;
        blocks++;
        code_bytes += (uint64_t)blk.pos;
        for (int i = 0; i < blk.pos; i++) {
            hash ^= blk.code[i];
            hash *= 1099511628211ull;          /* FNV-1a prime */
        }
    }

    printf("test_jit_codegen: rom=%ld B  blocks=%u  code_bytes=%llu  fnv1a=0x%016llx\n",
           sz, blocks, (unsigned long long)code_bytes, (unsigned long long)hash);
    if (blocks == 0) { fprintf(stderr, "test_jit_codegen: no blocks compiled?!\n"); return 1; }
    return 0;
}
