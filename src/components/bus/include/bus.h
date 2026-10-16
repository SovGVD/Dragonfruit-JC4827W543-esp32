#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BUS_ADDR_MASK     0x00FFFFFF  /* 24-bit address bus */

#define BUS_RAM_BASE      0x00000000
/* 2 MB. The Palm IIIx actually has 4 MB, but the classic ESP32 (M5Paper) memory-maps
 * only 4 MB of PSRAM for direct access, and the ~2 MB ROM already shares that window;
 * 4 MB here leaves no room for the ROM and boot-loops. Getting to a real 4 MB needs the
 * ROM moved out of PSRAM (e.g. mmap'd from flash), which would free the window for RAM. */
#define BUS_RAM_SIZE      0x00200000

#define BUS_ROM_BASE      0x10C08000  /* big ROM starts at bigROMOffset (0x8000) */
#define BUS_ROM_MAX_SIZE  0x00200000  /* 2 MB max */

#define BUS_PERIPH_BASE   0x00FFF000  /* $FFFFF000 masked to 24 bits */
#define BUS_PERIPH_SIZE   0x00001000  /* 4 KB */

/* Palm IIIc Epson SED1375 external color LCD controller mapped on CSB1.
 * The low 24 bits of $1F000000 are $000000, so this window must be decoded
 * before applying BUS_ADDR_MASK to avoid aliasing onto low system RAM. */
#define BUS_SED1375_BASE      0x1F000000u
#define BUS_SED1375_SIZE      0x00020000u  /* 128 KB CSB1 decode window */
#define BUS_SED1375_VRAM_SIZE 0x00014000u  /* 80 KB embedded video SRAM */
#define BUS_SED1375_REG_BASE  0x0001FFE0u  /* 32-byte control/LUT register block */

#define BUS_ROM_LO        (BUS_ROM_BASE & BUS_ADDR_MASK)  /* 0x00C08000 */

void bus_init(const uint8_t *rom_data, size_t rom_size);
uint8_t *bus_ram_ptr(void);
uint8_t *bus_vram_ptr(void);

/* Called when sysDispatchTableP is written — populates table from ROM trap data */
void bus_on_dispatch_table_set(uint32_t table_base);

/* --- Memory access ---------------------------------------------------------
 * The 68k hot loop fetches an opcode and touches operands on nearly every
 * instruction, so the common RAM/ROM cases are inlined here as direct,
 * big-endian pointer accesses (no out-of-line call, no range-check cascade).
 * Only the rare peripheral / address-space-wraparound cases fall through to
 * the out-of-line *_slow handlers in bus.c.  g_bus_ram/g_bus_rom and the ROM
 * window bounds are published by bus_init(). */
extern uint8_t       *g_bus_ram;
extern uint8_t       *g_bus_vram;
extern const uint8_t *g_bus_rom;
extern uint32_t       g_bus_rom_end;   /* BUS_ROM_LO + rom_size */

/* Bumped on every access to a DragonBall peripheral register (the only thing
 * a running instruction can do to change pending-interrupt state mid-batch).
 * The 68k core watches this to skip its per-instruction interrupt recompute on
 * the overwhelming majority of instructions that touch only RAM/ROM. */
extern uint32_t       g_bus_periph_seq;

uint8_t  bus_read8_slow(uint32_t addr);
uint16_t bus_read16_slow(uint32_t addr);
uint32_t bus_read32_slow(uint32_t addr);
void     bus_write8_slow(uint32_t addr, uint8_t val);
void     bus_write16_slow(uint32_t addr, uint16_t val);
void     bus_write32_slow(uint32_t addr, uint32_t val);

/* True if `addr` resolves into the loaded ROM image. Used to sanity-check a trap
 * handler / vector read out of emulated RAM before jumping to it (the Palm trap
 * dispatch table stores ROM addresses; a not-yet-populated slot reads as RAM or
 * zero). Masks to the 24-bit bus and bounds-checks against the *actual* loaded
 * size (g_bus_rom_end, published by bus_init) — exactly like the bus_read* fast
 * path below — so it stays correct for a ROM of any supported size instead of
 * assuming a fixed 2 MB window. Keep this the single source of truth for "is it
 * in ROM?" rather than hardcoding the window bounds at each call site. */
static inline bool bus_addr_in_rom(uint32_t addr)
{
    addr &= BUS_ADDR_MASK;
    return addr >= BUS_ROM_LO && addr < g_bus_rom_end;
}

static inline uint8_t bus_read8(uint32_t addr)
{
    if ((addr - BUS_SED1375_BASE) < BUS_SED1375_SIZE)
        return bus_read8_slow(addr);
    addr &= BUS_ADDR_MASK;
    if (addr < BUS_RAM_SIZE)
        return g_bus_ram[addr];
    if (addr >= BUS_ROM_LO && addr < g_bus_rom_end)
        return g_bus_rom[addr - BUS_ROM_LO];
    return bus_read8_slow(addr);
}

static inline uint16_t bus_read16(uint32_t addr)
{
    if ((addr - BUS_SED1375_BASE) < BUS_SED1375_SIZE)
        return bus_read16_slow(addr);
    addr &= BUS_ADDR_MASK;
    const uint8_t *p;
    if (addr < BUS_RAM_SIZE)            p = g_bus_ram + addr;
    else if (addr >= BUS_ROM_LO && addr < g_bus_rom_end)
                                        p = g_bus_rom + (addr - BUS_ROM_LO);
    else                                return bus_read16_slow(addr);
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t bus_read32(uint32_t addr)
{
    if ((addr - BUS_SED1375_BASE) < BUS_SED1375_SIZE)
        return bus_read32_slow(addr);
    addr &= BUS_ADDR_MASK;
    const uint8_t *p;
    if (addr < BUS_RAM_SIZE)            p = g_bus_ram + addr;
    else if (addr >= BUS_ROM_LO && addr < g_bus_rom_end)
                                        p = g_bus_rom + (addr - BUS_ROM_LO);
    else                                return bus_read32_slow(addr);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void bus_write8(uint32_t addr, uint8_t val)
{
    if ((addr - BUS_SED1375_BASE) < BUS_SED1375_SIZE) {
        bus_write8_slow(addr, val);
        return;
    }
    addr &= BUS_ADDR_MASK;
    if (addr < BUS_RAM_SIZE) { g_bus_ram[addr] = val; return; }
    bus_write8_slow(addr, val);
}

static inline void bus_write16(uint32_t addr, uint16_t val)
{
    if ((addr - BUS_SED1375_BASE) < BUS_SED1375_SIZE) {
        bus_write16_slow(addr, val);
        return;
    }
    addr &= BUS_ADDR_MASK;
    if (addr < BUS_RAM_SIZE) {
        uint8_t *p = g_bus_ram + addr;
        p[0] = (uint8_t)(val >> 8);
        p[1] = (uint8_t)val;
        return;
    }
    bus_write16_slow(addr, val);
}

static inline void bus_write32(uint32_t addr, uint32_t val)
{
    if ((addr - BUS_SED1375_BASE) < BUS_SED1375_SIZE) {
        bus_write32_slow(addr, val);
        return;
    }
    addr &= BUS_ADDR_MASK;
    if (addr < BUS_RAM_SIZE) {
        uint8_t *p = g_bus_ram + addr;
        p[0] = (uint8_t)(val >> 24);
        p[1] = (uint8_t)(val >> 16);
        p[2] = (uint8_t)(val >> 8);
        p[3] = (uint8_t)val;
        return;
    }
    bus_write32_slow(addr, val);
}

/* --- Big-endian byte (de)serialization over raw host-side buffers ----------
 * The 68k's memory is big-endian. Code that parses or builds a raw byte image
 * off-bus — ROM files and the reset vector (rom_loader), PRC/PDB records
 * (installer/backup), exception vectors staged into the RAM image before
 * bus_init (main/host) — needs the same byte order as the bus_read/write
 * accessors above. These keep that the single source of truth instead of
 * re-open-coding the shifts at every such site. be* read, wb* write. */
static inline uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline void wb16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wb32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
