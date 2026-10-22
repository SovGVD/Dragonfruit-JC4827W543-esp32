/* Native ("high level") versions of hot Palm OS ROM code.
 *
 * Every Palm OS system call is TRAP #15 followed by a selector word ($Axxx).
 * The CPU takes the exception and the ROM's TrapDispatcher (15 instructions)
 * looks the selector up in the trap table and RTEs into the handler, which leaves the
 * stack exactly as if the caller had done a JSR. That round trip is about 8%
 * of all emulated instructions while Palm OS is busy. m68k_palm_hle_trap15()
 * does the same work in C: same registers, same stack words, same cycle count.
 *
 * Two leaf calls, MemMove and MemSet, are also done natively right from the
 * dispatcher: the copy or fill happens in C and the CPU returns straight to
 * the caller. Their ROM versions are found by the MacsBug names Palm OS keeps
 * after each function, so they track whatever ROM is loaded, and they are only
 * taken over while the trap table still points at them (an app that patches
 * the trap with SysSetTrapAddress keeps its patch). A leaf call leaves the
 * scratch registers the dispatcher already clobbers (D1, A0, A1) and the
 * condition codes as they fall; D0 holds the result like the ROM's.
 *
 * Hot loops inside ROM functions (the blitters) and a few whole functions
 * (the Memory Manager semaphore, the compiler's multiply helper) are found by
 * their exact code bytes. The first word of each becomes an F-line opcode
 * ($FFxx, never used by Palm OS) and m68k_palm_hle_fline() runs it in C,
 * leaving registers, flags and memory exactly as the ROM code would (a native
 * function may leave different bytes in its own dead stack frame). A ROM whose
 * code differs simply keeps running its own. The table covers the Palm OS 4.1
 * colour ROM, the IIIx 3.1 ROM and the IIIx 4.1 ROM.
 *
 * Native work is skipped (the ROM code runs as before) whenever the CPU is
 * not in supervisor mode, is tracing, the TRAP #15 vector does not point at
 * an unmodified TrapDispatcher, the trap is a library trap, or a leaf call's
 * memory is not plain RAM or video RAM.
 *
 * Host builds can check every Nth native call against the ROM code
 * (dragonfruit --hle-check N): the call runs natively, then again through the
 * interpreter from the same state, and registers, cycles and all of RAM are
 * compared. The host build compiles this into the m68k unity build
 * (m68k_all.c); the device builds it on its own and keeps it in flash. */

#ifdef HOST_BUILD
#include <stdlib.h>
#define HLE_HOT
#else
/* On the device this is its own translation unit, in flash (linker.lf); the
 * entry points every system call goes through are in IRAM. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "m68k.h"
#include "bus.h"
#define HLE_HOT IRAM_ATTR
static const char *TAG_CPU = "m68k";
#endif

/* TrapDispatcher as in the Palm OS 4.1 ROM, byte for byte. */
static const uint8_t k_trap_dispatcher[] = {
    0x30, 0x1F, 0x20, 0x57, 0x32, 0x18, 0x2E, 0x88, 0x02, 0x41, 0x0F, 0xFF,
    0x08, 0x81, 0x00, 0x0B, 0x67, 0x16, 0x22, 0x78, 0x01, 0x40, 0x34, 0x2F,
    0x00, 0x04, 0xE9, 0x4A, 0x22, 0x71, 0x20, 0x00, 0xE3, 0x49, 0xD2, 0xF1,
    0x10, 0x00, 0x60, 0x0A, 0x22, 0x78, 0x01, 0x22, 0xE5, 0x49, 0x22, 0x71,
    0x10, 0x00, 0x2F, 0x09, 0x3F, 0x00, 0x08, 0x00, 0x00, 0x0F, 0x67, 0x04,
    0x00, 0x7C, 0x80, 0x00, 0x4E, 0x73,
};

/* TRAP exception (34) plus the dispatcher's system-trap path as the
 * interpreter times it. */
#define HLE_DISPATCH_CYCLES (34 + 142)

static struct {
    bool     enabled;
    bool     busy;                 /* checker is running the ROM code */
    uint32_t disp_ok, disp_bad;    /* TRAP #15 vector targets already checked */
    uint32_t memmove, memset;      /* ROM entry points, 0 if not found */
} hle;

m68k_palm_hle_stats_t g_palm_hle_stats;
bool g_palm_hle_jumped;

/* --- ROM symbol lookup --------------------------------------------------- */

/* Palm OS ROMs keep a MacsBug name after each function's final RTS/RTE/
 * JMP (A0): a length byte with bit 7 set (or $80 and a length byte), the
 * name, a pad to even, then a word giving the size of the literal pool that
 * follows. The next function starts after the pool. Returns the ROM offset of
 * the function called `name`, or -1. */
static long rom_find_function(const uint8_t *rom, uint32_t size, const char *name)
{
    size_t want = strlen(name);
    uint32_t next_start = 0;
    for (uint32_t i = 2; i + 2 < size; i += 2) {
        uint16_t w = (uint16_t)((rom[i - 2] << 8) | rom[i - 1]);
        if (w != 0x4E75 && w != 0x4E73 && w != 0x4ED0) continue;
        uint8_t b = rom[i];
        uint32_t n, s;
        if (b == 0x80)                  { n = rom[i + 1]; s = i + 2; }
        else if (b > 0x80 && b < 0xA0)  { n = b & 0x1F;   s = i + 1; }
        else continue;
        if (n < 2 || s + n > size) continue;
        bool ok = true;
        for (uint32_t k = 0; k < n && ok; k++) {
            uint8_t c = rom[s + k];
            ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                 (c >= 'a' && c <= 'z') || c == '_';
        }
        if (!ok) continue;
        uint32_t start = next_start;
        uint32_t nx = (s + n + 1) & ~1u;
        if (nx + 2 <= size) nx += 2 + (uint32_t)((rom[nx] << 8) | rom[nx + 1]);
        next_start = nx;
        if (n == want && memcmp(rom + s, name, n) == 0)
            return (long)start;
    }
    return -1;
}

static void hle_patch_loops(uint8_t *rom, uint32_t size);

void m68k_palm_hle_init(uint8_t *rom, uint32_t size)
{
    memset(&hle, 0, sizeof hle);
    memset(&g_palm_hle_stats, 0, sizeof g_palm_hle_stats);
    long mm = rom_find_function(rom, size, "MemMove");
    long ms = rom_find_function(rom, size, "MemSet");
    /* Both start with LINK A6 in every ROM seen; anything else is a misread. */
    if (mm >= 0 && rom[mm] == 0x4E && rom[mm + 1] == 0x56) hle.memmove = BUS_ROM_BASE + (uint32_t)mm;
    if (ms >= 0 && rom[ms] == 0x4E && rom[ms + 1] == 0x56) hle.memset  = BUS_ROM_BASE + (uint32_t)ms;
    hle_patch_loops(rom, size);
    hle.enabled = true;
    ESP_LOGI(TAG_CPU, "native ROM calls: MemMove %s, MemSet %s",
             hle.memmove ? "found" : "not found", hle.memset ? "found" : "not found");
}

/* --- Native calls -------------------------------------------------------- */

static HLE_HOT bool hle_dispatcher_at(uint32_t vec)
{
    if (vec == hle.disp_ok) return true;
    if (vec == hle.disp_bad) return false;
    for (uint32_t i = 0; i < sizeof k_trap_dispatcher; i++) {
        if (bus_read8(vec + i) != k_trap_dispatcher[i]) {
            if (hle.disp_bad != vec)
                ESP_LOGW(TAG_CPU, "TRAP #15 vector %08lX is not the known TrapDispatcher; "
                         "system calls run the ROM code", (unsigned long)vec);
            hle.disp_bad = vec;
            return false;
        }
    }
    hle.disp_ok = vec;
    return true;
}

/* Host pointer for [addr, addr+n) if it lies wholly in RAM or video RAM. */
static uint8_t *hle_mem(uint32_t addr, uint32_t n)
{
    if (addr < BUS_RAM_SIZE && n <= BUS_RAM_SIZE - addr)
        return g_bus_ram + addr;
    uint32_t v = addr - BUS_SED1375_BASE;
    if (g_bus_vram && v < BUS_SED1375_VRAM_SIZE && n <= BUS_SED1375_VRAM_SIZE - v)
        return g_bus_vram + v;
    return NULL;
}

/* Big-endian access for the native loops: plain RAM and video RAM directly,
 * anything else through the bus. */
static inline uint16_t hr16(uint32_t a)
{
    const uint8_t *p = hle_mem(a, 2);
    return p ? be16(p) : bus_read16(a);
}
static inline uint8_t hr8(uint32_t a)
{
    const uint8_t *p = hle_mem(a, 1);
    return p ? *p : bus_read8(a);
}
static inline uint32_t hr32(uint32_t a)
{
    const uint8_t *p = hle_mem(a, 4);
    return p ? be32(p) : bus_read32(a);
}
static inline void hw8(uint32_t a, uint8_t v)
{
    uint8_t *p = hle_mem(a, 1);
    if (p) *p = v; else bus_write8(a, v);
}
static inline void hw16(uint32_t a, uint16_t v)
{
    uint8_t *p = hle_mem(a, 2);
    if (p) wb16(p, v); else bus_write16(a, v);
}
static inline void hw32(uint32_t a, uint32_t v)
{
    uint8_t *p = hle_mem(a, 4);
    if (p) wb32(p, v); else bus_write32(a, v);
}
#define LO16(r)        ((uint16_t)(r))
#define SETW(r, v)     ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))
#define SETB(r, v)     ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define CCR(cpu, f)    ((cpu)->regs.sr = (uint16_t)(((cpu)->regs.sr & 0xFFE0) | (f)))

/* PrvBltConstCopy, per row: masked left edge word, a run of longs and words
 * of the fill pattern, masked right edge word; then the next row. Entered at
 * the top of the row loop, left at the function's exit when the row count at
 * 12(A6) reaches zero. */
static const uint8_t k_const_copy_rows[] = {
    0x20, 0x41, 0x30, 0x10, 0xC0, 0x46, 0x80, 0x6E, 0xFF, 0xFE, 0x54, 0x81,
    0x30, 0x80, 0x22, 0x41, 0x32, 0x2C, 0x00, 0x50, 0x51, 0x41, 0x60, 0x0A,
    0x22, 0xC2, 0x22, 0xC2, 0x22, 0xC2, 0x22, 0xC2, 0x51, 0x41, 0x4A, 0x41,
    0x6C, 0xF2, 0x26, 0x49, 0x50, 0x41, 0x6F, 0x06, 0x36, 0xC5, 0x53, 0x41,
    0x66, 0xFA, 0x4A, 0x41, 0x66, 0x0A, 0x30, 0x13, 0xC0, 0x47, 0x80, 0x6E,
    0xFF, 0xFC, 0x36, 0xC0, 0x53, 0x6E, 0x00, 0x0C, 0x67, 0x0A, 0x20, 0x4B,
    0xD1, 0xEC, 0x00, 0x44, 0x22, 0x08, 0x60, 0xB0,
};

static void loop_const_copy_rows(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6], a4 = a[4];
    uint16_t left = hr16(fp - 2), right = hr16(fp - 4);
    uint16_t width = hr16(a4 + 80);
    uint32_t stride = hr32(a4 + 68);
    long cyc = 0;
    for (;;) {
        a[0] = d[1];
        SETW(d[0], (hr16(a[0]) & LO16(d[6])) | left);
        d[1] += 2;
        hw16(a[0], LO16(d[0]));
        a[1] = d[1];
        SETW(d[1], width - 8);
        cyc += 74 + 12;
        while ((int16_t)LO16(d[1]) >= 0) {
            hw32(a[1], d[2]); hw32(a[1] + 4, d[2]);
            hw32(a[1] + 8, d[2]); hw32(a[1] + 12, d[2]);
            a[1] += 16;
            SETW(d[1], LO16(d[1]) - 8);
            cyc += 66;
        }
        a[3] = a[1];
        SETW(d[1], LO16(d[1]) + 8);
        cyc += 18;
        if ((int16_t)LO16(d[1]) > 0) {
            do {
                hw16(a[3], LO16(d[5]));
                a[3] += 2;
                SETW(d[1], LO16(d[1]) - 1);
                cyc += 22;
            } while (LO16(d[1]) != 0);
            cyc -= 2;
        }
        cyc += 12;
        if (LO16(d[1]) == 0) {
            SETW(d[0], (hr16(a[3]) & LO16(d[7])) | right);
            hw16(a[3], LO16(d[0]));
            a[3] += 2;
            cyc += 32;
        }
        uint16_t rows = (uint16_t)(hr16(fp + 12) - 1);
        hw16(fp + 12, rows);
        cyc += 26;
        if (rows == 0) break;
        a[0] = a[3] + stride;
        d[1] = a[0];
        cyc += 36;
    }
    CCR(cpu, SR_Z);                       /* SUBQ.W #1 that reached zero */
    cpu->regs.pc = at + sizeof k_const_copy_rows;
    cpu->regs.cycles = (int)(cyc > 0x3FFFFFFF ? 0x3FFFFFFF : cyc);
}

/* Condition codes the way the 68000 sets them. */
static inline uint16_t ccr_x(m68k_cpu_t *cpu) { return cpu->regs.sr & SR_X; }
static inline uint16_t ccr_nz16(uint16_t v)
{
    return (uint16_t)((v & 0x8000 ? SR_N : 0) | (v ? 0 : SR_Z));
}
/* SUB/CMP flags (N, Z, V, C) of dst - src at width `bits`. */
static inline uint16_t ccr_sub(uint32_t dst, uint32_t src, int bits)
{
    uint32_t msb = 1u << (bits - 1), mask = msb | (msb - 1);
    dst &= mask; src &= mask;
    uint32_t r = (dst - src) & mask;
    return (uint16_t)((r & msb ? SR_N : 0) | (r ? 0 : SR_Z) |
                      ((dst ^ src) & (dst ^ r) & msb ? SR_V : 0) | (src > dst ? SR_C : 0));
}
/* LSL.W / LSR.W by a register: the count is taken modulo 64. */
static inline uint16_t lsl16(uint16_t v, uint32_t n) { n &= 63; return n >= 16 ? 0 : (uint16_t)(v << n); }
static inline uint16_t lsr16(uint16_t v, uint32_t n) { n &= 63; return n >= 16 ? 0 : (uint16_t)(v >> n); }
static inline uint8_t  lsl8(uint8_t v, uint32_t n)   { n &= 63; return n >= 8 ? 0 : (uint8_t)(v << n); }
static inline int hle_cyc(long c) { return (int)(c > 0x3FFFFFFF ? 0x3FFFFFFF : c); }

/* PrvBltSwap, the run of whole words in a row: for each pixel field (D3 is
 * the field mask, shifted right by 2(A6) bits a step) a pixel equal to colour
 * D4 becomes D5 and one equal to D5 becomes D4. Entered at the TST.W -4(A6)
 * that guards the word loop, left just after its BGT. If the shift is zero the
 * ROM loops forever; then only that TST runs here and the ROM code goes on. */
static const uint8_t k_swap_words[] = {
    0x36, 0x2E, 0xFF, 0xFA, 0x3D, 0x6E, 0xFF, 0xF6, 0xFF, 0xFE, 0x60, 0x3C,
    0x30, 0x04, 0xC0, 0x43, 0x3C, 0x12, 0x32, 0x06, 0xC2, 0x43, 0x3E, 0x01,
    0xB2, 0x40, 0x66, 0x10, 0x30, 0x03, 0x46, 0x40, 0xC0, 0x46, 0x32, 0x05,
    0xC2, 0x43, 0x82, 0x40, 0x34, 0x81, 0x60, 0x16, 0x30, 0x05, 0xC0, 0x43,
    0xBE, 0x40, 0x66, 0x0E, 0x30, 0x03, 0x46, 0x40, 0xC0, 0x46, 0x32, 0x04,
    0xC2, 0x43, 0x82, 0x40, 0x34, 0x81, 0x30, 0x2E, 0xFF, 0xFE, 0xE0, 0x6B,
    0x4A, 0x43, 0x66, 0xC0, 0x54, 0x8A, 0x53, 0x6E, 0xFF, 0xFC, 0x4A, 0x6E,
    0xFF, 0xFC, 0x6E, 0xA8,
};

static void loop_swap_words(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6];
    uint32_t exit = at + 6;                   /* past TST.W -4(A6) / BGT */
    uint16_t n = hr16(fp - 4);
    if ((int16_t)n > 0 && (hr16(fp - 10) & 63) == 0 && hr16(fp - 6)) {
        CCR(cpu, ccr_x(cpu) | ccr_nz16(n));   /* just the TST; the ROM code loops */
        cpu->regs.pc = at + 4;
        cpu->regs.cycles = 12;
        return;
    }
    uint16_t x = ccr_x(cpu);
    long cyc = 0;
    while ((int16_t)n > 0) {
        SETW(d[3], hr16(fp - 6));
        uint16_t sh = hr16(fp - 10);
        hw16(fp - 2, sh);
        cyc += 12 + 22 + 10;
        while (LO16(d[3])) {
            uint16_t m = LO16(d[3]);
            SETW(d[0], LO16(d[4]) & m);
            SETW(d[6], hr16(a[2]));
            SETW(d[1], LO16(d[6]) & m);
            SETW(d[7], LO16(d[1]));
            cyc += 4 + 4 + 8 + 4 + 4 + 4 + 4 + 10;
            if (LO16(d[1]) == LO16(d[0])) {
                SETW(d[0], (uint16_t)~m & LO16(d[6]));
                SETW(d[1], (LO16(d[5]) & m) | LO16(d[0]));
                hw16(a[2], LO16(d[1]));
                cyc += 4 + 4 + 4 + 4 + 4 + 4 + 8 + 10 - 2;
            } else {
                SETW(d[0], LO16(d[5]) & m);
                cyc += 4 + 4 + 4 + 10 - 2;
                if (LO16(d[7]) == LO16(d[0])) {
                    SETW(d[0], (uint16_t)~m & LO16(d[6]));
                    SETW(d[1], (LO16(d[4]) & m) | LO16(d[0]));
                    hw16(a[2], LO16(d[1]));
                    cyc += 4 + 4 + 4 + 4 + 4 + 4 + 8 - 2;
                }
            }
            SETW(d[0], hr16(fp - 2));
            SETW(d[3], lsr16(m, d[0]));
            cyc += 12 + 6 + 2 * (d[0] & 63) + 4 + 10;
        }
        cyc += 4 + 8 - 10;
        a[2] += 2;
        n = (uint16_t)(n - 1);
        hw16(fp - 4, n);
        x = 0;                                /* SUBQ from a positive count */
        cyc += 8 + 16 + 12 + 10;
    }
    CCR(cpu, x | ccr_nz16(n));                /* the TST.W that ended it */
    cpu->regs.pc = exit;
    cpu->regs.cycles = hle_cyc(cyc + 12 + 8);
}

/* PrvBlit1To8Copy rows: expand a row of 1-bit pixels to bytes, D5 for set
 * bits and D6 for clear ones; the row count is at 30(A6). Entered at the
 * SUBQ.W #1,30(A6) / BGE that closes the row loop, left after it. */
static const uint8_t k_blit1to8_rows[] = {
    0x3E, 0x2E, 0x00, 0x1C, 0x16, 0x1B, 0x12, 0x2E, 0xFF, 0xFD, 0xE3, 0x2B,
    0x38, 0x2E, 0xFF, 0xFE, 0x60, 0x0C, 0x4A, 0x03, 0x6C, 0x04, 0x14, 0xC5,
    0x60, 0x02, 0x14, 0xC6, 0xD6, 0x03, 0x53, 0x44, 0x6C, 0xF0, 0x9E, 0x6E,
    0xFF, 0xFE, 0x60, 0x16, 0x16, 0x1B, 0x78, 0x08, 0x4A, 0x03, 0x6C, 0x04,
    0x14, 0xC5, 0x60, 0x02, 0x14, 0xC6, 0xD6, 0x03, 0x53, 0x44, 0x6E, 0xF0,
    0x51, 0x47, 0x0C, 0x47, 0x00, 0x08, 0x6C, 0xE4, 0x0C, 0x47, 0x00, 0x01,
    0x6D, 0x16, 0x16, 0x1B, 0x38, 0x07, 0x60, 0x0C, 0x4A, 0x03, 0x6C, 0x04,
    0x14, 0xC5, 0x60, 0x02, 0x14, 0xC6, 0xD6, 0x03, 0x53, 0x44, 0x6C, 0xF0,
    0xD7, 0xEE, 0xFF, 0xF8, 0xD5, 0xCC, 0x53, 0x6E, 0x00, 0x1E, 0x6C, 0x94,
};

/* One bit to one byte, MSB first; returns cycles. */
static inline long blit1to8_bit(uint32_t *d, uint32_t *a)
{
    bool set = (int8_t)(uint8_t)d[3] < 0;
    hw8(a[2]++, (uint8_t)(set ? d[5] : d[6]));
    SETB(d[3], (uint8_t)d[3] << 1);
    return 4 + (set ? 8 + 8 + 10 : 10 + 8) + 4;
}

static void loop_blit1to8_rows(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6];
    uint16_t width = hr16(fp + 28), first = hr16(fp - 2);
    uint8_t shift = hr8(fp - 3);
    uint32_t src_skip = hr32(fp - 8);
    long cyc = 0;
    uint16_t f;
    for (;;) {
        uint16_t rows = hr16(fp + 30);
        uint16_t left = (uint16_t)(rows - 1);
        hw16(fp + 30, left);
        f = ccr_sub(rows, 1, 16);
        cyc += 16 + 10;
        if (((f & SR_N) != 0) != ((f & SR_V) != 0)) break;   /* BGE not taken */
        SETW(d[7], width);
        SETB(d[3], hr8(a[3]++));
        SETB(d[1], shift);
        SETB(d[3], lsl8((uint8_t)d[3], d[1]));
        SETW(d[4], first);
        cyc += 12 + 8 + 12 + 6 + 2 * (d[1] & 63) + 12 + 10;
        for (;;) {                                       /* SUBQ.W #1,D4 / BGE */
            uint16_t o = LO16(d[4]); SETW(d[4], o - 1);
            uint16_t g = ccr_sub(o, 1, 16);
            cyc += 4 + 10;
            if (((g & SR_N) != 0) != ((g & SR_V) != 0)) { cyc -= 2; break; }
            cyc += blit1to8_bit(d, a);
        }
        SETW(d[7], LO16(d[7]) - first);
        cyc += 12 + 10;
        while ((int16_t)LO16(d[7]) >= 8) {               /* CMPI.W #8,D7 / BGE */
            SETB(d[3], hr8(a[3]++));
            d[4] = 8;
            cyc += 8 + 8 + 4;
            do {
                cyc += blit1to8_bit(d, a) + 4 + 10;
                SETW(d[4], LO16(d[4]) - 1);
            } while ((int16_t)LO16(d[4]) > 0);
            SETW(d[7], LO16(d[7]) - 8);
            cyc += 4 - 2 + 8 + 10;
        }
        cyc += 8 + 8 + 8;
        if ((int16_t)LO16(d[7]) >= 1) {                  /* CMPI.W #1,D7 / BLT */
            SETB(d[3], hr8(a[3]++));
            SETW(d[4], LO16(d[7]));
            cyc += 8 + 4 + 10;
            for (;;) {
                uint16_t o = LO16(d[4]); SETW(d[4], o - 1);
                uint16_t g = ccr_sub(o, 1, 16);
                cyc += 4 + 10;
                if (((g & SR_N) != 0) != ((g & SR_V) != 0)) { cyc -= 2; break; }
                cyc += blit1to8_bit(d, a);
            }
        } else {
            cyc += 2;
        }
        a[3] += src_skip;
        a[2] += a[4];
        cyc += 18 + 8;
    }
    CCR(cpu, ((f & SR_C) ? SR_X : 0) | f);
    cpu->regs.pc = at + 6;                    /* past SUBQ.W #1,30(A6) / BGE */
    cpu->regs.cycles = hle_cyc(cyc - 2);
}

/* PrvGetBltInfo: compare two colour tables entry by entry (the low 24 bits,
 * via LSL.L #8). Entered at the CMP.W D4,D7 / BCS that guards the loop; left
 * after the BCS when all D4 entries match, or at the "differ" store if not. */
static const uint8_t k_ctab_compare[] = {
    0x70, 0x00, 0x30, 0x07, 0xE5, 0x88, 0x2D, 0x40, 0xFF, 0xF8, 0x20, 0x6E,
    0xFF, 0xE4, 0x20, 0x30, 0x08, 0x00, 0xE1, 0x88, 0x20, 0x43, 0x22, 0x2E,
    0xFF, 0xF8, 0x22, 0x30, 0x18, 0x00, 0xE1, 0x89, 0xB2, 0x80, 0x67, 0x08,
    0x1D, 0x7C, 0x00, 0x01, 0xFF, 0xE9, 0x60, 0x0E, 0x52, 0x47, 0xBE, 0x44,
    0x65, 0xCE,
};

static void loop_ctab_compare(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6];
    uint32_t tab = hr32(fp - 28);
    uint16_t x = ccr_x(cpu);
    long cyc = 0;
    for (;;) {
        uint16_t f = ccr_sub(LO16(d[7]), LO16(d[4]), 16);   /* CMP.W D4,D7 */
        cyc += 4 + 10;
        if (!(f & SR_C)) {                                  /* all equal */
            CCR(cpu, x | f);
            cpu->regs.pc = at + 4;
            cyc -= 2;
            break;
        }
        uint32_t off = (uint32_t)LO16(d[7]) << 2;
        hw32(fp - 8, off);
        a[0] = tab;
        uint32_t v0 = hr32(tab + off);
        d[0] = v0 << 8;
        a[0] = d[3];
        d[1] = off;
        uint32_t v1 = hr32(d[3] + off);
        d[1] = v1 << 8;
        cyc += 4 + 4 + 10 + 16 + 16 + 20 + 24 + 4 + 16 + 20 + 24 + 6 + 10;
        if (d[1] != d[0]) {
            f = ccr_sub(d[1], d[0], 32);                    /* CMP.L D0,D1 */
            CCR(cpu, ((v1 & 0x01000000u) ? SR_X : 0) | f);  /* X from LSL.L #8 */
            cpu->regs.pc = at - 0x2E + 0x24;                /* MOVE.B #1,-23(A6) */
            cyc -= 2;
            break;
        }
        x = (LO16(d[7]) == 0xFFFF) ? SR_X : 0;              /* ADDQ.W #1,D7 */
        SETW(d[7], LO16(d[7]) + 1);
        cyc += 4;
    }
    cpu->regs.cycles = hle_cyc(cyc * 5 / 6);
}

/* PrvBltMisAlignCopy_Generic, the middle of a row: each destination word
 * takes the source shifted across a word boundary (LSL D7 / LSR D6). */
static const uint8_t k_misalign_words[] = {
    0x3A, 0x03, 0xEF, 0x6D, 0x36, 0x1C, 0x30, 0x03, 0xEC, 0x68, 0x80, 0x45,
    0x36, 0xC0, 0x53, 0x44, 0x66, 0xEE,
};

static void loop_misalign_words(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t sl = d[7] & 63, sr = d[6] & 63;
    long per = 4 + 6 + 2 * sl + 8 + 4 + 6 + 2 * sr + 4 + 8 + 4 + 10;
    long cyc = 0;
    do {
        SETW(d[5], lsl16(LO16(d[3]), sl));
        SETW(d[3], hr16(a[4])); a[4] += 2;
        SETW(d[0], lsr16(LO16(d[3]), sr) | LO16(d[5]));
        hw16(a[3], LO16(d[0])); a[3] += 2;
        SETW(d[4], LO16(d[4]) - 1);
        cyc += per;
    } while (LO16(d[4]));
    CCR(cpu, SR_Z);                           /* SUBQ.W #1,D4 that reached zero */
    cpu->regs.pc = at + sizeof k_misalign_words;
    cpu->regs.cycles = hle_cyc(cyc - 2);
}

/* PrvConvert1To8: each source byte indexes an 8-byte table entry (eight
 * 1-bit pixels as 0/FF bytes) that selects between the foreground and
 * background colour longs, two longs out per byte. */
static const uint8_t k_convert_1to8[] = {
    0x70, 0x00, 0x10, 0x1C, 0xE7, 0x88, 0x20, 0x6E, 0xFF, 0xF4, 0xD1, 0xC0,
    0x24, 0x10, 0x28, 0x28, 0x00, 0x04, 0x20, 0x02, 0x46, 0x80, 0xC0, 0xAE,
    0xFF, 0xF8, 0x22, 0x02, 0xC2, 0xAE, 0xFF, 0xFC, 0x82, 0x80, 0x22, 0xC1,
    0x20, 0x04, 0x46, 0x80, 0xC0, 0x86, 0x22, 0x04, 0xC2, 0x85, 0x82, 0x80,
    0x22, 0xC1, 0x53, 0x47, 0x66, 0xCA,
};

static void loop_convert_1to8(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6];
    uint32_t tab = hr32(fp - 12), fg = hr32(fp - 4), bg = hr32(fp - 8);
    long cyc = 0;
    do {
        d[0] = (uint32_t)hr8(a[4]++) << 3;
        a[0] = tab + d[0];
        d[2] = hr32(a[0]);
        d[4] = hr32(a[0] + 4);
        d[0] = ~d[2] & bg;
        d[1] = (d[2] & fg) | d[0];
        hw32(a[1], d[1]); a[1] += 4;
        d[0] = ~d[4] & d[6];
        d[1] = (d[4] & d[5]) | d[0];
        hw32(a[1], d[1]); a[1] += 4;
        SETW(d[7], LO16(d[7]) - 1);
        cyc += 4 + 8 + 14 + 16 + 8 + 12 + 16 + 4 + 6 + 18 + 4 + 18 + 8 + 12 +
               4 + 6 + 8 + 4 + 8 + 8 + 12 + 4 + 10;
    } while (LO16(d[7]));
    CCR(cpu, SR_Z);
    cpu->regs.pc = at + sizeof k_convert_1to8;
    cpu->regs.cycles = hle_cyc(cyc - 2);
}

/* --- Palm OS 3.1 (IIIx, mono) loops -------------------------------------- */

/* LSL.W / LSR.W that also give the X bit (the last bit shifted out; a zero
 * count leaves X alone). */
static inline uint16_t lsl16x(uint16_t v, uint32_t n, uint16_t *x)
{
    n &= 63;
    if (!n) return v;
    *x = (n <= 16 && ((v >> (16 - n)) & 1)) ? SR_X : 0;
    return n >= 16 ? 0 : (uint16_t)(v << n);
}
static inline uint16_t lsr16x(uint16_t v, uint32_t n, uint16_t *x)
{
    n &= 63;
    if (!n) return v;
    *x = (n <= 16 && ((v >> (n - 1)) & 1)) ? SR_X : 0;
    return n >= 16 ? 0 : (uint16_t)(v >> n);
}
static inline uint16_t ccr_nz32(uint32_t v)
{
    return (uint16_t)((v & 0x80000000u ? SR_N : 0) | (v ? 0 : SR_Z));
}

/* A loop that can't run natively from here runs its first instruction,
 * MOVE.W (A2)+,Dn, and leaves the rest to the ROM code. */
static void hle_move_a2_post(m68k_cpu_t *cpu, uint32_t at, int reg)
{
    uint16_t v = hr16(cpu->regs.a[2]);
    cpu->regs.a[2] += 2;
    SETW(cpu->regs.d[reg], v);
    CCR(cpu, ccr_x(cpu) | ccr_nz16(v));
    cpu->regs.pc = at + 2;
    cpu->regs.cycles = 8;
}

/* SysLaunch at boot: clear RAM a byte at a time from A2 up to A4, skipping
 * [D4, $2510) and [-4(A6), A3), and add each old byte (times $010101) to the
 * long at $158. Entered at the CMPA.L A4,A2 / BCS that closes the loop. */
static const uint8_t k_sys_ram_clear[] = {
    0xB5, 0xC4, 0x65, 0x0C, 0xB4, 0xFC, 0x25, 0x10, 0x64, 0x06, 0x24, 0x7C,
    0x00, 0x00, 0x25, 0x10, 0xB5, 0xEE, 0xFF, 0xFC, 0x65, 0x06, 0xB5, 0xCB,
    0x64, 0x02, 0x24, 0x4B, 0x76, 0x00, 0x16, 0x12, 0x20, 0x03, 0xE1, 0x88,
    0x80, 0x83, 0x22, 0x03, 0x74, 0x10, 0xE5, 0xA9, 0x82, 0x80, 0xD3, 0xB8,
    0x01, 0x58, 0x14, 0xC5, 0xB5, 0xCC, 0x65, 0xC8,
};

static void loop_sys_ram_clear(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t skip_lo = hr32(a[6] - 4);
    uint16_t x = ccr_x(cpu);
    long cyc = 0;
    for (;;) {
        uint16_t f = ccr_sub(a[2], a[4], 32);
        cyc += 6 + 10;
        if (!(f & SR_C)) { CCR(cpu, x | f); cyc -= 2; break; }
        cyc += 6 + 8;
        if (a[2] >= d[4]) {
            cyc += 10 + 10;
            if (a[2] < 0x2510) { a[2] = 0x2510; cyc += 12 - 2; }
        } else {
            cyc += 2;
        }
        cyc += 18 + 8;
        if (a[2] >= skip_lo) {
            cyc += 6 + 10;
            if (a[2] < a[3]) { a[2] = a[3]; cyc += 4 - 2; }
        } else {
            cyc += 2;
        }
        d[3] = hr8(a[2]);
        d[0] = (d[3] << 8) | d[3];
        d[1] = (d[3] << 16) | d[0];
        d[2] = 16;
        uint32_t s = hr32(0x158), r = s + d[1];
        x = r < s ? SR_X : 0;
        hw32(0x158, r);
        hw8(a[2]++, (uint8_t)d[5]);
        cyc += 4 + 8 + 4 + 24 + 8 + 4 + 4 + 40 + 8 + 24 + 8;
        skip_lo = hr32(a[6] - 4);
    }
    cpu->regs.pc = at + 4;
    cpu->regs.cycles = hle_cyc(cyc * 9 / 10);   /* fitted to --hle-check */
}

/* ScrRectangleRoutine, solid fill rows: masked left word, D4 words, masked
 * right word. The row count is at 2(A3). Entered at the MOVE.W 2(A3),D0 /
 * SUBQ / TST / BNE that closes the row loop. */
static const uint8_t k_scr_rect_rows[] = {
    0x30, 0x04, 0xC0, 0x46, 0x32, 0x12, 0xC2, 0x6E, 0xFF, 0xFE, 0x82, 0x40,
    0x34, 0xC1, 0x36, 0x2B, 0x00, 0x40, 0x4A, 0x43, 0x6F, 0x06, 0x34, 0xC4,
    0x53, 0x43, 0x66, 0xFA, 0x4A, 0x43, 0x66, 0x0E, 0x30, 0x04, 0xC0, 0x47,
    0x32, 0x12, 0xC2, 0x6E, 0xFF, 0xFC, 0x82, 0x40, 0x34, 0xC1, 0xD5, 0xEB,
    0x00, 0x38, 0x30, 0x2B, 0x00, 0x02, 0x53, 0x6B, 0x00, 0x02, 0x4A, 0x40,
    0x66, 0xC2,
};

static void loop_scr_rect_rows(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6], a3 = a[3];
    long cyc = 0;
    for (;;) {
        uint16_t rows = hr16(a3 + 2);
        SETW(d[0], rows);
        hw16(a3 + 2, (uint16_t)(rows - 1));
        cyc += 12 + 16 + 4 + 10;
        if (!rows) break;
        SETW(d[0], LO16(d[4]) & LO16(d[6]));
        SETW(d[1], (hr16(a[2]) & hr16(fp - 2)) | LO16(d[0]));
        hw16(a[2], LO16(d[1])); a[2] += 2;
        SETW(d[3], hr16(a3 + 64));
        cyc += 4 + 4 + 8 + 12 + 4 + 8 + 12 + 4 + 8;
        if ((int16_t)LO16(d[3]) > 0) {
            uint16_t n = LO16(d[3]);
            uint8_t *p = hle_mem(a[2], (uint32_t)n * 2);
            uint16_t w = LO16(d[4]);
            if (p) {
                for (uint32_t i = 0; i < n; i++) wb16(p + 2 * i, w);
            } else {
                for (uint32_t i = 0; i < n; i++) hw16(a[2] + 2 * i, w);
            }
            a[2] += (uint32_t)n * 2;
            SETW(d[3], 0);
            cyc += 10 + (long)n * (8 + 4 + 10) - 2;
        } else {
            cyc += 10;
        }
        cyc += 4 + 10;
        if (LO16(d[3]) == 0) {
            SETW(d[0], LO16(d[4]) & LO16(d[7]));
            SETW(d[1], (hr16(a[2]) & hr16(fp - 4)) | LO16(d[0]));
            hw16(a[2], LO16(d[1])); a[2] += 2;
            cyc += 4 + 4 + 8 + 12 + 4 + 8 - 2;
        }
        a[2] += hr32(a3 + 56);
        cyc += 18;
    }
    CCR(cpu, SR_X | SR_Z);                    /* SUBQ from 0 borrows, TST.W 0 */
    cpu->regs.pc = at + 0x0C;
    cpu->regs.cycles = hle_cyc((cyc - 2) * 5 / 6);
}

/* PrvMisAlignedForwardInnerBitBlt row loops for narrow blits: each row's
 * source is two words shifted across a word boundary (LSL D6, LSR -2(A6)),
 * then a2 steps by -14(A6) and the destination by 56(A4); -4(A6) counts the
 * rows. D7 non-zero means a pattern source the ROM code refills with a call
 * each row; that case stays with the ROM code. */
static inline long mis_row_tail(m68k_cpu_t *cpu)
{
    uint32_t fp = cpu->regs.a[6];
    cpu->regs.a[2] += (uint32_t)(int32_t)(int16_t)hr16(fp - 14);
    uint16_t n = (uint16_t)(hr16(fp - 4) - 1);
    hw16(fp - 4, n);
    return 4 + 8 + 16 + 10 + 16 + 10;
}

/* Bytes: one destination byte per row at D5 (source high byte only). */
static const uint8_t k_mis_b1[] = {
    0x38, 0x1A, 0xED, 0x6C, 0x30, 0x1A, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68,
    0x80, 0x44, 0xC0, 0x6E, 0xFF, 0xFA, 0x20, 0x45, 0x72, 0x00, 0x12, 0x10,
    0xC2, 0x6E, 0xFF, 0xF0, 0x82, 0x40, 0x10, 0x81, 0x54, 0x85, 0xDA, 0xAC,
    0x00, 0x38, 0x4A, 0x07, 0x66, 0x06, 0xD4, 0xEE, 0xFF, 0xF2, 0x60, 0x28,
    0x20, 0x6C, 0x00, 0x42, 0xD1, 0xEC, 0x00, 0x1E, 0x29, 0x48, 0x00, 0x42,
    0x2F, 0x0C, 0x2F, 0x2C, 0x00, 0x42, 0x20, 0x6E, 0xFF, 0xF4, 0x4E, 0x90,
    0x34, 0x6E, 0xFF, 0xF8, 0x20, 0x0A, 0xD0, 0x80, 0x24, 0x6C, 0x00, 0x28,
    0xD5, 0xC0, 0x50, 0x4F, 0x53, 0x6E, 0xFF, 0xFC, 0x66, 0xA2,
};

/* Bytes, the low half of the shifted word into the byte at D5. */
static const uint8_t k_mis_b2[] = {
    0x38, 0x1A, 0xED, 0x6C, 0x30, 0x1A, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68,
    0x88, 0x40, 0x30, 0x04, 0xE0, 0x48, 0xC0, 0x6E, 0xFF, 0xFA, 0x20, 0x45,
    0x72, 0x00, 0x12, 0x10, 0xC2, 0x6E, 0xFF, 0xF0, 0x82, 0x40, 0x10, 0x81,
    0x54, 0x85, 0xDA, 0xAC, 0x00, 0x38, 0x4A, 0x07, 0x66, 0x06, 0xD4, 0xEE,
    0xFF, 0xF2, 0x60, 0x28, 0x20, 0x6C, 0x00, 0x42, 0xD1, 0xEC, 0x00, 0x1E,
    0x29, 0x48, 0x00, 0x42, 0x2F, 0x0C, 0x2F, 0x2C, 0x00, 0x42, 0x20, 0x6E,
    0xFF, 0xF4, 0x4E, 0x90, 0x34, 0x6E, 0xFF, 0xF8, 0x20, 0x0A, 0xD0, 0x80,
    0x24, 0x6C, 0x00, 0x28, 0xD5, 0xC0, 0x50, 0x4F, 0x53, 0x6E, 0xFF, 0xFC,
    0x66, 0x9E,
};

/* Two destination bytes per row, D5+1 and D5+2 (masks -6/-16 and -18/-20). */
static const uint8_t k_mis_b3[] = {
    0x38, 0x1A, 0xED, 0x6C, 0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE,
    0xE2, 0x68, 0x80, 0x44, 0xC0, 0x6E, 0xFF, 0xFA, 0x20, 0x45, 0x72, 0x00,
    0x12, 0x10, 0xC2, 0x6E, 0xFF, 0xF0, 0x82, 0x40, 0x52, 0x85, 0x10, 0x81,
    0x38, 0x03, 0xED, 0x6C, 0x30, 0x1A, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68,
    0x88, 0x40, 0x30, 0x04, 0xE0, 0x48, 0xC0, 0x6E, 0xFF, 0xEE, 0x20, 0x45,
    0x72, 0x00, 0x12, 0x10, 0xC2, 0x6E, 0xFF, 0xEC, 0x82, 0x40, 0x10, 0x81,
    0x56, 0x85, 0x20, 0x45, 0xD1, 0xEC, 0x00, 0x38, 0x2A, 0x08, 0x4A, 0x07,
    0x66, 0x06, 0xD4, 0xEE, 0xFF, 0xF2, 0x60, 0x28, 0x20, 0x6C, 0x00, 0x42,
    0xD1, 0xEC, 0x00, 0x1E, 0x29, 0x48, 0x00, 0x42, 0x2F, 0x0C, 0x2F, 0x2C,
    0x00, 0x42, 0x20, 0x6E, 0xFF, 0xF4, 0x4E, 0x90, 0x34, 0x6E, 0xFF, 0xF8,
    0x20, 0x0A, 0xD0, 0x80, 0x24, 0x6C, 0x00, 0x28, 0xD5, 0xC0, 0x50, 0x4F,
    0x53, 0x6E, 0xFF, 0xFC, 0x66, 0x00, 0xFF, 0x76,
};

/* The masked merge into one destination byte: (byte & mask) | bits. */
static inline void mis_byte(uint32_t *d, uint32_t *a, uint16_t bits, uint16_t keep)
{
    a[0] = d[5];
    d[1] = hr8(a[0]);
    SETW(d[1], (LO16(d[1]) & keep) | bits);
    hw8(a[0], (uint8_t)d[1]);
}

static void loop_mis_bytes(m68k_cpu_t *cpu, uint32_t at, int kind)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    if ((uint8_t)d[7]) { hle_move_a2_post(cpu, at, 4); return; }
    uint32_t fp = a[6], a4 = a[4];
    uint16_t x = 0;
    long cyc = 0;
    do {
        SETW(d[4], hr16(a[2])); a[2] += 2;
        SETW(d[4], lsl16x(LO16(d[4]), d[6], &x));
        uint16_t sh = hr16(fp - 2);
        cyc += 8 + 6 + 2 * (d[6] & 63) + 8 + 12 + 6 + 2 * (sh & 63);
        if (kind == 1) {
            SETW(d[0], hr16(a[2])); a[2] += 2;
            SETW(d[1], sh);
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x) | LO16(d[4]));
            SETW(d[0], LO16(d[0]) & hr16(fp - 6));
            mis_byte(d, a, LO16(d[0]), hr16(fp - 16));
            d[5] += 2;
            d[5] += hr32(a4 + 56);
            cyc += 4 + 12 + 4 + 4 + 8 + 12 + 4 + 8 + 8 + 18;
        } else if (kind == 2) {
            SETW(d[0], hr16(a[2])); a[2] += 2;
            SETW(d[1], sh);
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x));
            SETW(d[4], LO16(d[4]) | LO16(d[0]));
            SETW(d[0], lsr16x(LO16(d[4]), 8, &x));
            SETW(d[0], LO16(d[0]) & hr16(fp - 6));
            mis_byte(d, a, LO16(d[0]), hr16(fp - 16));
            d[5] += 2;
            d[5] += hr32(a4 + 56);
            cyc += 4 + 4 + 22 + 12 + 4 + 4 + 8 + 12 + 4 + 8 + 8 + 18;
        } else {
            SETW(d[3], hr16(a[2])); a[2] += 2;
            SETW(d[0], LO16(d[3]));
            SETW(d[1], sh);
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x) | LO16(d[4]));
            SETW(d[0], LO16(d[0]) & hr16(fp - 6));
            mis_byte(d, a, LO16(d[0]), hr16(fp - 16));
            d[5] += 1;
            SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
            SETW(d[0], hr16(a[2])); a[2] += 2;
            uint16_t sh2 = hr16(fp - 2);
            SETW(d[1], sh2);
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x));
            SETW(d[4], LO16(d[4]) | LO16(d[0]));
            SETW(d[0], lsr16x(LO16(d[4]), 8, &x));
            SETW(d[0], LO16(d[0]) & hr16(fp - 18));
            mis_byte(d, a, LO16(d[0]), hr16(fp - 20));
            d[5] += 3;
            a[0] = d[5] + hr32(a4 + 56);
            d[5] = a[0];
            cyc += 8 + 4 + 4 + 12 + 4 + 4 + 8 + 12 + 4 + 8 + 8 + 4 + 6 + 2 * (d[6] & 63) +
                   8 + 12 + 6 + 2 * (sh2 & 63) + 4 + 4 + 22 + 12 + 4 + 4 + 8 + 12 + 4 + 8 +
                   8 + 4 + 18 + 4;
        }
        (void)x;
        cyc += mis_row_tail(cpu);
    } while (hr16(fp - 4));
    CCR(cpu, SR_Z);                           /* SUBQ.W that reached zero */
    cpu->regs.pc = at + (kind == 1 ? sizeof k_mis_b1 : kind == 2 ? sizeof k_mis_b2 : sizeof k_mis_b3);
    cpu->regs.cycles = hle_cyc((cyc - 2) * 4 / 5);   /* fitted to --hle-check */
}
static void loop_mis_b1(m68k_cpu_t *cpu, uint32_t at) { loop_mis_bytes(cpu, at, 1); }
static void loop_mis_b2(m68k_cpu_t *cpu, uint32_t at) { loop_mis_bytes(cpu, at, 2); }
static void loop_mis_b3(m68k_cpu_t *cpu, uint32_t at) { loop_mis_bytes(cpu, at, 3); }

/* Word destinations at A3: masked left word, 64(A4) whole words, masked
 * right word (when that count ends at zero). Copy, NOT-copy and XOR. */
enum { MIS_COPY, MIS_NOT, MIS_XOR };

static const uint8_t k_mis_wcopy[] = {
    0x38, 0x1A, 0xED, 0x6C, 0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE,
    0xE2, 0x68, 0x88, 0x40, 0x30, 0x03, 0xE2, 0x68, 0x80, 0x44, 0xC0, 0x6E,
    0xFF, 0xFA, 0x32, 0x13, 0xC2, 0x6E, 0xFF, 0xF0, 0x82, 0x40, 0x36, 0xC1,
    0x3A, 0x2C, 0x00, 0x40, 0x4A, 0x45, 0x6F, 0x16, 0x38, 0x03, 0xED, 0x6C,
    0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68, 0x80, 0x44,
    0x36, 0xC0, 0x53, 0x45, 0x66, 0xEA, 0x4A, 0x45, 0x66, 0x1E, 0x38, 0x03,
    0xED, 0x6C, 0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68,
    0x80, 0x44, 0xC0, 0x6E, 0xFF, 0xEE, 0x32, 0x13, 0xC2, 0x6E, 0xFF, 0xEC,
    0x82, 0x40, 0x36, 0xC1, 0xD7, 0xEC, 0x00, 0x38, 0x4A, 0x07, 0x66, 0x06,
    0xD4, 0xEE, 0xFF, 0xF2, 0x60, 0x28, 0x20, 0x6C, 0x00, 0x42, 0xD1, 0xEC,
    0x00, 0x1E, 0x29, 0x48, 0x00, 0x42, 0x2F, 0x0C, 0x2F, 0x2C, 0x00, 0x42,
    0x20, 0x6E, 0xFF, 0xF4, 0x4E, 0x90, 0x34, 0x6E, 0xFF, 0xF8, 0x20, 0x0A,
    0xD0, 0x80, 0x24, 0x6C, 0x00, 0x28, 0xD5, 0xC0, 0x50, 0x4F, 0x53, 0x6E,
    0xFF, 0xFC, 0x66, 0x00, 0xFF, 0x60,
};
static const uint8_t k_mis_wnot[] = {
    0x36, 0x1A, 0x46, 0x43, 0x38, 0x03, 0xED, 0x6C, 0x36, 0x1A, 0x46, 0x43,
    0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68, 0x80, 0x44, 0xC0, 0x6E,
    0xFF, 0xFA, 0x32, 0x13, 0xC2, 0x6E, 0xFF, 0xF0, 0x82, 0x40, 0x36, 0xC1,
    0x3A, 0x2C, 0x00, 0x40, 0x4A, 0x45, 0x6F, 0x18, 0x38, 0x03, 0xED, 0x6C,
    0x36, 0x1A, 0x46, 0x43, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68,
    0x80, 0x44, 0x36, 0xC0, 0x53, 0x45, 0x66, 0xE8, 0x4A, 0x45, 0x66, 0x20,
    0x38, 0x03, 0xED, 0x6C, 0x36, 0x1A, 0x46, 0x43, 0x30, 0x03, 0x32, 0x2E,
    0xFF, 0xFE, 0xE2, 0x68, 0x80, 0x44, 0xC0, 0x6E, 0xFF, 0xEE, 0x32, 0x13,
    0xC2, 0x6E, 0xFF, 0xEC, 0x82, 0x40, 0x36, 0xC1, 0xD7, 0xEC, 0x00, 0x38,
    0x4A, 0x07, 0x66, 0x06, 0xD4, 0xEE, 0xFF, 0xF2, 0x60, 0x28, 0x20, 0x6C,
    0x00, 0x42, 0xD1, 0xEC, 0x00, 0x1E, 0x29, 0x48, 0x00, 0x42, 0x2F, 0x0C,
    0x2F, 0x2C, 0x00, 0x42, 0x20, 0x6E, 0xFF, 0xF4, 0x4E, 0x90, 0x34, 0x6E,
    0xFF, 0xF8, 0x20, 0x0A, 0xD0, 0x80, 0x24, 0x6C, 0x00, 0x28, 0xD5, 0xC0,
    0x50, 0x4F, 0x53, 0x6E, 0xFF, 0xFC, 0x66, 0x00, 0xFF, 0x5C,
};
static const uint8_t k_mis_wxor[] = {
    0x38, 0x1A, 0xED, 0x6C, 0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE,
    0xE2, 0x68, 0x80, 0x44, 0xC0, 0x6E, 0xFF, 0xFA, 0xB1, 0x5B, 0x3A, 0x2C,
    0x00, 0x40, 0x4A, 0x45, 0x6F, 0x16, 0x38, 0x03, 0xED, 0x6C, 0x36, 0x1A,
    0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68, 0x80, 0x44, 0xB1, 0x5B,
    0x53, 0x45, 0x66, 0xEA, 0x4A, 0x45, 0x66, 0x16, 0x38, 0x03, 0xED, 0x6C,
    0x30, 0x1A, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68, 0x80, 0x44, 0xC0, 0x6E,
    0xFF, 0xEE, 0xB1, 0x53, 0x54, 0x8B, 0xD7, 0xEC, 0x00, 0x38, 0x4A, 0x07,
    0x66, 0x06, 0xD4, 0xEE, 0xFF, 0xF2, 0x60, 0x28, 0x20, 0x6C, 0x00, 0x42,
    0xD1, 0xEC, 0x00, 0x1E, 0x29, 0x48, 0x00, 0x42, 0x2F, 0x0C, 0x2F, 0x2C,
    0x00, 0x42, 0x20, 0x6E, 0xFF, 0xF4, 0x4E, 0x90, 0x34, 0x6E, 0xFF, 0xF8,
    0x20, 0x0A, 0xD0, 0x80, 0x24, 0x6C, 0x00, 0x28, 0xD5, 0xC0, 0x50, 0x4F,
    0x53, 0x6E, 0xFF, 0xFC, 0x66, 0x00, 0xFF, 0x76,
};

static void loop_mis_words(m68k_cpu_t *cpu, uint32_t at, int mode)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    if ((uint8_t)d[7]) { hle_move_a2_post(cpu, at, mode == MIS_NOT ? 3 : 4); return; }
    uint32_t fp = a[6], a4 = a[4];
    uint16_t x = 0, inv = mode == MIS_NOT ? 0xFFFF : 0;
    uint32_t sl = d[6] & 63;
    long cyc = 0;
    do {
        uint16_t sh = hr16(fp - 2);
        long shc = 6 + 2 * (sh & 63), slc = 6 + 2 * sl;
        /* left edge */
        if (mode == MIS_NOT) {
            SETW(d[3], hr16(a[2]) ^ inv); a[2] += 2;
            SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
            SETW(d[3], hr16(a[2]) ^ inv); a[2] += 2;
            SETW(d[0], LO16(d[3]));
            SETW(d[1], sh);
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x) | LO16(d[4]));
            cyc += 8 + 4 + 4 + slc + 8 + 4 + 4 + 12 + shc + 4;
        } else {
            SETW(d[4], hr16(a[2])); a[2] += 2;
            SETW(d[4], lsl16x(LO16(d[4]), d[6], &x));
            SETW(d[3], hr16(a[2])); a[2] += 2;
            SETW(d[0], LO16(d[3]));
            SETW(d[1], sh);
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x));
            cyc += 8 + slc + 8 + 4 + 12 + shc;
            if (mode == MIS_COPY) {
                SETW(d[4], LO16(d[4]) | LO16(d[0]));
                SETW(d[0], lsr16x(LO16(d[3]), d[1], &x) | LO16(d[4]));
                cyc += 4 + 4 + shc + 4;
            } else {
                SETW(d[0], LO16(d[0]) | LO16(d[4]));
                cyc += 4;
            }
        }
        SETW(d[0], LO16(d[0]) & hr16(fp - 6));
        cyc += 12;
        if (mode == MIS_XOR) {
            hw16(a[3], hr16(a[3]) ^ LO16(d[0]));
            cyc += 12;
        } else {
            SETW(d[1], (hr16(a[3]) & hr16(fp - 16)) | LO16(d[0]));
            hw16(a[3], LO16(d[1]));
            cyc += 8 + 12 + 4 + 8;
        }
        a[3] += 2;
        SETW(d[5], hr16(a4 + 64));
        cyc += 12 + 4 + 10;
        /* whole words */
        if ((int16_t)LO16(d[5]) > 0) {
            do {
                SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
                SETW(d[3], hr16(a[2]) ^ inv); a[2] += 2;
                SETW(d[0], LO16(d[3]));
                SETW(d[1], hr16(fp - 2));
                SETW(d[0], lsr16x(LO16(d[0]), d[1], &x) | LO16(d[4]));
                if (mode == MIS_XOR) hw16(a[3], hr16(a[3]) ^ LO16(d[0]));
                else                 hw16(a[3], LO16(d[0]));
                a[3] += 2;
                SETW(d[5], LO16(d[5]) - 1);
                cyc += 4 + slc + 8 + (inv ? 4 : 0) + 4 + 12 + 6 + 2 * (d[1] & 63) + 4 +
                       (mode == MIS_XOR ? 12 : 8) + 4 + 10;
            } while (LO16(d[5]));
            cyc -= 2;
        } else {
            cyc += 2;
        }
        cyc += 4 + 10;
        /* right edge */
        if (LO16(d[5]) == 0) {
            SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
            uint16_t src = hr16(a[2]) ^ inv; a[2] += 2;
            if (mode == MIS_XOR) SETW(d[0], src);
            else { SETW(d[3], src); SETW(d[0], src); }
            SETW(d[1], hr16(fp - 2));
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x) | LO16(d[4]));
            SETW(d[0], LO16(d[0]) & hr16(fp - 18));
            if (mode == MIS_XOR) {
                hw16(a[3], hr16(a[3]) ^ LO16(d[0]));
            } else {
                SETW(d[1], (hr16(a[3]) & hr16(fp - 20)) | LO16(d[0]));
                hw16(a[3], LO16(d[1]));
            }
            a[3] += 2;
            cyc += 4 + slc + 8 + 4 + 12 + 6 + 2 * (d[1] & 63) + 4 + 12 + 32 - 2;
        }
        a[3] += hr32(a4 + 56);
        cyc += 18;
        cyc += mis_row_tail(cpu);
    } while (hr16(fp - 4));
    (void)x;
    CCR(cpu, SR_Z);
    cpu->regs.pc = at + (mode == MIS_COPY ? 0xA2 : mode == MIS_NOT ? 0xA6 : 0x8C);
    cpu->regs.cycles = hle_cyc((cyc - 2) * 4 / 5);
}
static void loop_mis_wcopy(m68k_cpu_t *cpu, uint32_t at) { loop_mis_words(cpu, at, MIS_COPY); }
static void loop_mis_wnot(m68k_cpu_t *cpu, uint32_t at)  { loop_mis_words(cpu, at, MIS_NOT); }
static void loop_mis_wxor(m68k_cpu_t *cpu, uint32_t at)  { loop_mis_words(cpu, at, MIS_XOR); }

/* ScrDeCompressScanLine(src, dst, n): the whole function. Each flag byte
 * says, MSB first, which of the next eight destination bytes get a new byte
 * from the source; the others are left alone. Returns the source bytes used. */
static const uint8_t k_decompress_line[] = {
    0x4E, 0x56, 0x00, 0x00, 0x48, 0xE7, 0x04, 0x18, 0x28, 0x6E, 0x00, 0x08,
    0x26, 0x6E, 0x00, 0x0C, 0x3A, 0x2E, 0x00, 0x10, 0x72, 0xFF, 0x22, 0x4C,
    0x4A, 0x41, 0x6C, 0x04, 0x14, 0x19, 0x72, 0x07, 0x10, 0x02, 0x02, 0x40,
    0x00, 0x80, 0x67, 0x04, 0x16, 0xD9, 0x60, 0x02, 0x52, 0x8B, 0xD4, 0x02,
    0x53, 0x41, 0x53, 0x45, 0x66, 0xE2, 0x20, 0x49, 0x91, 0xCC, 0x20, 0x08,
    0x4C, 0xDF, 0x18, 0x20, 0x4E, 0x5E, 0x4E, 0x75,
};

static void loop_decompress_line(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t sp = a[7], ret = hr32(sp);
    hw32(sp - 4, a[6]);                       /* LINK A6,#0 */
    uint32_t fp = sp - 4, s = fp - 12;
    hw32(s, d[5]); hw32(s + 4, a[3]); hw32(s + 8, a[4]);   /* MOVEM.L D5/A3-A4 */
    uint32_t src = hr32(fp + 8), dst = hr32(fp + 12);
    uint16_t n = hr16(fp + 16);
    long cyc = 16 + 8 + 32 + 16 + 16 + 12 + 4 + 4;
    d[1] = 0xFFFFFFFFu;
    a[1] = src;
    do {
        if ((int16_t)LO16(d[1]) < 0) {
            SETB(d[2], hr8(a[1]++));
            d[1] = 7;
            cyc += 4 + 8 + 8 + 4;
        } else {
            cyc += 4 + 10;
        }
        SETW(d[0], (uint8_t)d[2] & 0x80);
        if (LO16(d[0])) {
            hw8(dst++, hr8(a[1]++));
            cyc += 4 + 8 + 8 + 12 + 10;
        } else {
            dst++;
            cyc += 4 + 8 + 10 + 8;
        }
        SETB(d[2], (uint8_t)d[2] << 1);
        SETW(d[1], LO16(d[1]) - 1);
        n = (uint16_t)(n - 1);
        cyc += 4 + 4 + 4 + 10;
    } while (n);
    a[0] = a[1] - src;
    d[0] = a[0];
    CCR(cpu, ccr_nz32(d[0]));                 /* MOVE.L A0,D0; X from SUBQ */
    d[5] = hr32(s); a[3] = hr32(s + 4); a[4] = hr32(s + 8);
    a[6] = hr32(fp);
    a[7] = sp + 4;
    cpu->regs.pc = ret;
    cpu->regs.cycles = hle_cyc(cyc - 2 + 4 + 8 + 4 + 28 + 12 + 16);
    (void)at;
}

/* FntLineWidth: add up the character widths of D5 bytes at A2 from the font's
 * width table at A3 (first char D6, last D7, missing glyphs take the last
 * entry), tabs to the next multiple of 20. Entered at the TST.W D5 / BNE
 * that closes the loop. */
static const uint8_t k_fnt_width[] = {
    0x18, 0x12, 0x0C, 0x04, 0x00, 0x09, 0x66, 0x12, 0x30, 0x43, 0x20, 0x08,
    0x81, 0xFC, 0x00, 0x14, 0x48, 0x40, 0x72, 0x14, 0x92, 0x40, 0xD6, 0x41,
    0x60, 0x4E, 0x70, 0x00, 0x10, 0x04, 0xB0, 0x46, 0x65, 0x32, 0x70, 0x00,
    0x10, 0x04, 0xB0, 0x47, 0x62, 0x2A, 0x70, 0x00, 0x10, 0x04, 0x90, 0x46,
    0x72, 0x00, 0x32, 0x00, 0xD2, 0x81, 0x0C, 0x33, 0xFF, 0xFF, 0x18, 0x01,
    0x67, 0x16, 0x70, 0x00, 0x10, 0x04, 0x90, 0x46, 0x72, 0x00, 0x32, 0x00,
    0xD2, 0x81, 0x12, 0x33, 0x18, 0x01, 0x48, 0x81, 0xD6, 0x41, 0x60, 0x14,
    0x30, 0x07, 0x90, 0x46, 0x52, 0x40, 0x72, 0x00, 0x32, 0x00, 0xD2, 0x81,
    0x12, 0x33, 0x18, 0x01, 0x48, 0x81, 0xD6, 0x41, 0x52, 0x8A, 0x53, 0x45,
    0x4A, 0x45, 0x66, 0x90,
};

static void loop_fnt_width(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint16_t x = ccr_x(cpu);
    uint16_t first = LO16(d[6]), last = LO16(d[7]);
    long cyc = 0;
    while (LO16(d[5])) {
        SETB(d[4], hr8(a[2]));
        uint8_t c = (uint8_t)d[4];
        cyc += 4 + 10 + 8 + 8 + 10;
        if (c == 9) {
            a[0] = (uint32_t)(int32_t)(int16_t)LO16(d[3]);
            d[0] = a[0];
            int32_t q = (int32_t)d[0] / 20, r = (int32_t)d[0] % 20;
            d[0] = ((uint32_t)(uint16_t)q << 16) | (uint16_t)r;   /* DIVS, SWAP */
            d[1] = (uint32_t)(uint16_t)(20 - (uint16_t)r);
            SETW(d[3], LO16(d[3]) + LO16(d[1]));
            cyc += 4 + 4 + 158 + 4 + 4 + 4 + 4 + 10 - 2;
        } else {
            bool missing;
            d[0] = c;
            cyc += 4 - 2 + 4 + 4 + 8;
            if (c < first) {
                missing = true; cyc += 10;
            } else {
                d[0] = c;
                cyc += 8 + 4 + 4 + 4 + 8;
                if (c > last) {
                    missing = true; cyc += 10 - 8;
                } else {
                    d[0] = c;
                    SETW(d[0], LO16(d[0]) - first);
                    d[1] = (uint32_t)LO16(d[0]) << 1;
                    missing = hr8(a[3] + 1 + d[1]) == 0xFF;
                    cyc += 4 + 4 + 4 + 4 + 4 + 8 + 22 + 10;
                    if (!missing) {
                        d[0] = c;
                        SETW(d[0], LO16(d[0]) - first);
                        d[1] = (uint32_t)LO16(d[0]) << 1;
                        SETB(d[1], hr8(a[3] + 1 + d[1]));
                        SETW(d[1], (uint16_t)(int16_t)(int8_t)d[1]);
                        SETW(d[3], LO16(d[3]) + LO16(d[1]));
                        cyc += 4 + 4 + 4 + 4 + 4 + 8 + 14 + 4 + 4 + 10 - 10;
                    }
                }
            }
            if (missing) {
                SETW(d[0], last);
                SETW(d[0], LO16(d[0]) - first + 1);
                d[1] = (uint32_t)LO16(d[0]) << 1;
                SETB(d[1], hr8(a[3] + 1 + d[1]));
                SETW(d[1], (uint16_t)(int16_t)(int8_t)d[1]);
                SETW(d[3], LO16(d[3]) + LO16(d[1]));
                cyc += 4 + 4 + 4 + 4 + 4 + 8 + 14 + 4 + 4;
            }
        }
        a[2] += 1;
        SETW(d[5], LO16(d[5]) - 1);
        x = 0;                                /* SUBQ from non-zero */
        cyc += 8 + 4;
    }
    CCR(cpu, x | SR_Z);                       /* TST.W D5 */
    cpu->regs.pc = at + 4;
    cpu->regs.cycles = hle_cyc((cyc + 4 + 8) * 14 / 15);
}

/* PrvCompressedInnerBitBlt, copying one decompressed row: the same left
 * edge / whole words / right edge as above, straight or shifted by -2(A6).
 * A straight run of code from the TST.W -2(A6) to the TST.B 16(A6). */
static const uint8_t k_compressed_row[] = {
    0x4A, 0x6E, 0xFF, 0xFE, 0x67, 0x14, 0x36, 0x1A, 0x38, 0x03, 0xED, 0x6C,
    0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68, 0x88, 0x40,
    0x60, 0x02, 0x38, 0x1A, 0x30, 0x04, 0xC0, 0x6E, 0xFF, 0xEA, 0x32, 0x13,
    0xC2, 0x6E, 0xFF, 0xE6, 0x82, 0x40, 0x36, 0xC1, 0x3A, 0x2C, 0x00, 0x40,
    0x4A, 0x6E, 0xFF, 0xFE, 0x67, 0x1C, 0x4A, 0x45, 0x6F, 0x22, 0x38, 0x03,
    0xED, 0x6C, 0x36, 0x1A, 0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68,
    0x80, 0x44, 0x36, 0xC0, 0x53, 0x45, 0x66, 0xEA, 0x60, 0x0A, 0x4A, 0x45,
    0x6F, 0x06, 0x36, 0xDA, 0x53, 0x45, 0x66, 0xFA, 0x4A, 0x45, 0x66, 0x2A,
    0x4A, 0x6E, 0xFF, 0xFE, 0x67, 0x12, 0x38, 0x03, 0xED, 0x6C, 0x36, 0x1A,
    0x30, 0x03, 0x32, 0x2E, 0xFF, 0xFE, 0xE2, 0x68, 0x88, 0x40, 0x60, 0x02,
    0x38, 0x1A, 0x30, 0x04, 0xC0, 0x6E, 0xFF, 0xE8, 0x32, 0x13, 0xC2, 0x6E,
    0xFF, 0xE4, 0x82, 0x40, 0x36, 0xC1,
};

static void loop_compressed_row(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6], a4 = a[4];
    uint16_t x = ccr_x(cpu), f;
    long cyc = 12;
    if (hr16(fp - 2)) {
        SETW(d[3], hr16(a[2])); a[2] += 2;
        SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
        SETW(d[3], hr16(a[2])); a[2] += 2;
        SETW(d[0], LO16(d[3]));
        SETW(d[1], hr16(fp - 2));
        SETW(d[0], lsr16x(LO16(d[0]), d[1], &x));
        SETW(d[4], LO16(d[4]) | LO16(d[0]));
        cyc += 8 + 8 + 4 + 6 + 2 * (d[6] & 63) + 8 + 4 + 12 + 6 + 2 * (d[1] & 63) + 4 + 10;
    } else {
        SETW(d[4], hr16(a[2])); a[2] += 2;
        cyc += 10 + 8;
    }
    SETW(d[0], LO16(d[4]) & hr16(fp - 22));
    SETW(d[1], (hr16(a[3]) & hr16(fp - 26)) | LO16(d[0]));
    hw16(a[3], LO16(d[1])); a[3] += 2;
    SETW(d[5], hr16(a4 + 64));
    cyc += 4 + 12 + 8 + 12 + 4 + 8 + 12 + 12;
    if (hr16(fp - 2)) {
        cyc += 8 + 4 + 10;
        if ((int16_t)LO16(d[5]) > 0) {
            do {
                SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
                SETW(d[3], hr16(a[2])); a[2] += 2;
                SETW(d[0], LO16(d[3]));
                SETW(d[1], hr16(fp - 2));
                SETW(d[0], lsr16x(LO16(d[0]), d[1], &x) | LO16(d[4]));
                hw16(a[3], LO16(d[0])); a[3] += 2;
                SETW(d[5], LO16(d[5]) - 1);
                x = 0;                        /* SUBQ from non-zero */
                cyc += 4 + 6 + 2 * (d[6] & 63) + 8 + 4 + 12 + 6 + 2 * (d[1] & 63) + 4 + 8 + 4 + 10;
            } while (LO16(d[5]));
            cyc += 10 - 2;
        } else {
            cyc += 2;
        }
    } else {
        cyc += 10 + 4 + 10;
        if ((int16_t)LO16(d[5]) > 0) {
            do {
                hw16(a[3], hr16(a[2])); a[2] += 2; a[3] += 2;
                SETW(d[5], LO16(d[5]) - 1);
                x = 0;
                cyc += 12 + 4 + 10;
            } while (LO16(d[5]));
            cyc -= 2;
        } else {
            cyc += 2;
        }
    }
    cyc += 4 + 10;
    if (LO16(d[5])) {
        f = ccr_nz16(LO16(d[5]));             /* TST.W D5 / BNE taken */
    } else {
        cyc += 12;
        if (hr16(fp - 2)) {
            SETW(d[4], lsl16x(LO16(d[3]), d[6], &x));
            SETW(d[3], hr16(a[2])); a[2] += 2;
            SETW(d[0], LO16(d[3]));
            SETW(d[1], hr16(fp - 2));
            SETW(d[0], lsr16x(LO16(d[0]), d[1], &x));
            SETW(d[4], LO16(d[4]) | LO16(d[0]));
            cyc += 8 + 4 + 6 + 2 * (d[6] & 63) + 8 + 4 + 12 + 6 + 2 * (d[1] & 63) + 4 + 10;
        } else {
            SETW(d[4], hr16(a[2])); a[2] += 2;
            cyc += 10 + 8;
        }
        SETW(d[0], LO16(d[4]) & hr16(fp - 24));
        SETW(d[1], (hr16(a[3]) & hr16(fp - 28)) | LO16(d[0]));
        hw16(a[3], LO16(d[1])); a[3] += 2;
        f = ccr_nz16(LO16(d[1]));             /* MOVE.W D1,(A3)+ */
        cyc += 4 + 12 + 8 + 12 + 4 + 8;
    }
    CCR(cpu, x | f);
    cpu->regs.pc = at + sizeof k_compressed_row;
    cpu->regs.cycles = hle_cyc(cyc * 6 / 7);
}

/* --- Palm OS 4.x: the Memory Manager semaphore --------------------------- */

/* MemSemaphoreReserve/Release take the AMX kernel semaphore at $106 (unless
 * already held, count at $24F) and, for write access, turn off the storage
 * RAM write protect with sysTrap $A24A/$A24B. Run natively when nothing would
 * wait or switch tasks: the semaphore is free or already ours, nothing is
 * pending in the kernel, and we're not in an interrupt ($110) or a debugger
 * ($111). Anything else runs the ROM code. The functions are matched by their
 * bytes and the kernel code they call by a hash of the whole span. */
static const uint8_t k_mem_sem_reserve[] = {
    0x4E, 0x56, 0x00, 0x00, 0x2F, 0x03, 0x4A, 0x38, 0x01, 0x10, 0x67, 0x18,
    0x52, 0x38, 0x02, 0x4F, 0x0C, 0x38, 0x00, 0x01, 0x02, 0x4F, 0x6F, 0x04,
    0x70, 0x00, 0x60, 0x5A, 0x4E, 0x4F, 0xA2, 0x4A, 0x70, 0x00, 0x60, 0x52,
    0x4A, 0x38, 0x02, 0x4F, 0x67, 0x0E, 0x4A, 0x2E, 0x00, 0x08, 0x67, 0x04,
    0x52, 0x38, 0x02, 0x4F, 0x70, 0x00, 0x60, 0x3E, 0x42, 0xA7, 0x42, 0xA7,
    0x2F, 0x38, 0x01, 0x06, 0x4E, 0xBA, 0x37, 0x10, 0x36, 0x00, 0x4A, 0x2E,
    0x00, 0x08, 0x4F, 0xEF, 0x00, 0x0C, 0x66, 0x04, 0x30, 0x03, 0x60, 0x22,
    0x4A, 0x38, 0x02, 0x4F, 0x6F, 0x08, 0x52, 0x38, 0x02, 0x4F, 0x70, 0x00,
    0x60, 0x14, 0x48, 0x78, 0x00, 0x01, 0x4E, 0xBA, 0x2E, 0xD4, 0x4E, 0x4F,
    0xA2, 0x4A, 0x52, 0x38, 0x02, 0x4F, 0x30, 0x03, 0x58, 0x4F, 0x26, 0x1F,
    0x4E, 0x5E, 0x4E, 0x75,
};
static const uint8_t k_mem_sem_release[] = {
    0x4E, 0x56, 0x00, 0x00, 0x2F, 0x03, 0x4A, 0x38, 0x01, 0x10, 0x67, 0x12,
    0x53, 0x38, 0x02, 0x4F, 0x6F, 0x04, 0x70, 0x00, 0x60, 0x56, 0x4E, 0x4F,
    0xA2, 0x4B, 0x70, 0x00, 0x60, 0x4E, 0x4A, 0x38, 0x02, 0x4F, 0x67, 0x1A,
    0x4A, 0x2E, 0x00, 0x08, 0x66, 0x04, 0x70, 0x00, 0x60, 0x3E, 0x0C, 0x38,
    0x00, 0x01, 0x02, 0x4F, 0x6F, 0x08, 0x53, 0x38, 0x02, 0x4F, 0x70, 0x00,
    0x60, 0x2E, 0x2F, 0x38, 0x01, 0x06, 0x4E, 0xBA, 0x36, 0x9E, 0x36, 0x00,
    0x4A, 0x2E, 0x00, 0x08, 0x58, 0x4F, 0x66, 0x04, 0x30, 0x03, 0x60, 0x18,
    0x53, 0x38, 0x02, 0x4F, 0x6F, 0x04, 0x70, 0x00, 0x60, 0x0E, 0x4E, 0x4F,
    0xA2, 0x4B, 0x42, 0xA7, 0x4E, 0xBA, 0x2E, 0x44, 0x30, 0x03, 0x58, 0x4F,
    0x26, 0x1F, 0x4E, 0x5E, 0x4E, 0x75,
};
/* The sysTrap $A24A / $A24B handlers: storage write protect off / on. Some
 * ROMs wrap the same code in LINK A6,#0 / UNLK A6, which changes nothing
 * but the dead stack. */
static const uint8_t k_wp_off[] = {
    0x30, 0x38, 0xF1, 0x16, 0x02, 0x40, 0x20, 0x00, 0x66, 0x04, 0x70, 0x00,
    0x60, 0x08, 0x02, 0x78, 0xDF, 0xFF, 0xF1, 0x16, 0x70, 0x01,
};
static const uint8_t k_wp_on[] = { 0x00, 0x78, 0x20, 0x00, 0xF1, 0x16 };
static const uint8_t k_mem_sem_reserve31[] = {
    0x4E, 0x56, 0x00, 0x00, 0x48, 0xE7, 0x18, 0x20, 0x18, 0x2E, 0x00, 0x08,
    0x4A, 0x38, 0x01, 0x10, 0x67, 0x20, 0x52, 0x38, 0x02, 0x4F, 0x0C, 0x38,
    0x00, 0x01, 0x02, 0x4F, 0x6F, 0x04, 0x70, 0x00, 0x60, 0x66, 0x24, 0x7C,
    0xFF, 0xFF, 0xF0, 0x00, 0x02, 0x6A, 0xDF, 0xFF, 0x01, 0x16, 0x70, 0x00,
    0x60, 0x56, 0x4A, 0x38, 0x02, 0x4F, 0x67, 0x0C, 0x4A, 0x04, 0x67, 0x04,
    0x52, 0x38, 0x02, 0x4F, 0x70, 0x00, 0x60, 0x44, 0x42, 0xA7, 0x42, 0xA7,
    0x2F, 0x38, 0x01, 0x06, 0x4E, 0xBA, 0xEC, 0xEE, 0x36, 0x00, 0x4A, 0x04,
    0x4F, 0xEF, 0x00, 0x0C, 0x66, 0x04, 0x30, 0x03, 0x60, 0x2A, 0x4A, 0x38,
    0x02, 0x4F, 0x6F, 0x08, 0x52, 0x38, 0x02, 0x4F, 0x70, 0x00, 0x60, 0x1C,
    0x48, 0x78, 0x00, 0x01, 0x4E, 0xBA, 0xE2, 0xD8, 0x24, 0x7C, 0xFF, 0xFF,
    0xF0, 0x00, 0x02, 0x6A, 0xDF, 0xFF, 0x01, 0x16, 0x52, 0x38, 0x02, 0x4F,
    0x30, 0x03, 0x58, 0x4F, 0x4C, 0xDF, 0x04, 0x18, 0x4E, 0x5E, 0x4E, 0x75,
};
static const uint8_t k_mem_sem_release31[] = {
    0x4E, 0x56, 0x00, 0x00, 0x48, 0xE7, 0x18, 0x20, 0x18, 0x2E, 0x00, 0x08,
    0x4A, 0x38, 0x01, 0x10, 0x67, 0x1E, 0x53, 0x38, 0x02, 0x4F, 0x4A, 0x38,
    0x02, 0x4F, 0x6F, 0x04, 0x70, 0x00, 0x60, 0x6A, 0x24, 0x7C, 0xFF, 0xFF,
    0xF0, 0x00, 0x00, 0x6A, 0x20, 0x00, 0x01, 0x16, 0x70, 0x00, 0x60, 0x5A,
    0x4A, 0x38, 0x02, 0x4F, 0x67, 0x18, 0x4A, 0x04, 0x66, 0x04, 0x70, 0x00,
    0x60, 0x4C, 0x0C, 0x38, 0x00, 0x01, 0x02, 0x4F, 0x6F, 0x08, 0x53, 0x38,
    0x02, 0x4F, 0x70, 0x00, 0x60, 0x3C, 0x48, 0x78, 0x00, 0x01, 0x2F, 0x38,
    0x01, 0x06, 0x4E, 0xBA, 0xEE, 0xEC, 0x36, 0x00, 0x4A, 0x04, 0x50, 0x4F,
    0x66, 0x04, 0x30, 0x03, 0x60, 0x24, 0x53, 0x38, 0x02, 0x4F, 0x4A, 0x38,
    0x02, 0x4F, 0x6F, 0x04, 0x70, 0x00, 0x60, 0x16, 0x24, 0x7C, 0xFF, 0xFF,
    0xF0, 0x00, 0x00, 0x6A, 0x20, 0x00, 0x01, 0x16, 0x42, 0xA7, 0x4E, 0xBA,
    0xE2, 0x20, 0x30, 0x03, 0x58, 0x4F, 0x4C, 0xDF, 0x04, 0x18, 0x4E, 0x5E,
    0x4E, 0x75,
};
#define HLE_WP_REG 0xFFFFF116u               /* chip select with the write protect bit */

/* LINK A6,#0, the first instruction, when the ROM code has to run. */
static void hle_link0(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *a = cpu->regs.a;
    a[7] -= 4;
    hw32(a[7], a[6]);
    a[6] = a[7];
    cpu->regs.pc = at + 4;
    cpu->regs.cycles = 16;
}

/* Is sysTrap `sel` (with the TrapDispatcher in place) still the ROM handler
 * `code` (followed by RTS, or wrapped in LINK/UNLK)? Gives its address. */
static bool hle_handler_is(uint32_t h, const uint8_t *code, uint32_t len)
{
    uint32_t o = 0;
    bool link = bus_read16(h) == 0x4E56 && bus_read16(h + 2) == 0;
    if (link) o = 4;
    for (uint32_t i = 0; i < len; i++)
        if (bus_read8(h + o + i) != code[i]) return false;
    if (link && bus_read16(h + o + len) != 0x4E5E) return false;
    return bus_read16(h + o + len + (link ? 2 : 0)) == 0x4E75;
}

static bool hle_trap_is(uint16_t sel, const uint8_t *code, uint32_t len, uint32_t *handler)
{
    static uint32_t ok[2];
    if (!hle_dispatcher_at(bus_read32((VEC_TRAP_BASE + 15) * 4))) return false;
    uint32_t h = bus_read32(bus_read32(0x122) + sel * 4u);
    int slot = code == k_wp_off ? 0 : 1;
    if (h != ok[slot]) {
        if (!hle_handler_is(h, code, len)) return false;
        if (h >= BUS_ROM_BASE) ok[slot] = h;     /* ROM code can't change */
    }
    *handler = h;
    return true;
}

/* The kernel's exit switches tasks when its nesting count at 5(K) is back to
 * $80 and something is ready to run (0(K) > 0). */
static bool kern_would_switch(uint32_t k)
{
    return hr8(k + 5) == 0x80 && (int8_t)hr8(k) > 0;
}

/* cjsmwait(sem, 0, 0) without waiting: the count at 36(S) is positive, or we
 * already own it (28(S) is the current task, 20(K)). */
static bool smwait_ok(uint32_t sem, uint32_t k, bool *nested, bool v31)
{
    if (hr8(sem + 19) != 'S') return false;
    uint16_t f = ccr_sub(hr32(sem + 36), 1, 32);
    if (v31) *nested = (f & SR_N) != 0;      /* SUBQ, TST.L, BLT */
    else     *nested = ((f & SR_N) != 0) != ((f & SR_V) != 0);
    if (*nested && hr32(sem + 28) != hr32(k + 20)) return false;
    return !kern_would_switch(k);
}

/* Do it; returns the X bit it leaves. D0/D1/A0 as the ROM code leaves them. */
static uint16_t smwait_do(m68k_cpu_t *cpu, uint32_t sem, uint32_t k, bool nested, uint16_t x)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    if (!nested) {
        hw32(sem + 36, hr32(sem + 36) - 1);
        if (hr32(sem + 32) == 0xFFFFFFFFu) {  /* owned semaphore: record the owner */
            hw32(sem + 28, hr32(k + 20));
            hw32(sem + 40, 1);
        }
    } else {
        uint32_t n = hr32(sem + 40);
        hw32(sem + 40, n + 1);
        x = n == 0xFFFFFFFFu ? SR_X : 0;
    }
    d[0] = 0;
    SETW(d[1], (cpu->regs.sr & 0xFF00) | x | ccr_nz32(k));   /* the exit's MOVE SR,D1 */
    a[0] = k;
    return x;
}

/* cjsmrls(sem, 1) when we own it and nobody is waiting. */
static bool smrls_ok(uint32_t sem, uint32_t k, bool *signal)
{
    if (hr8(sem + 19) != 'S' || hr32(sem + 28) != hr32(k + 20)) return false;
    *signal = hr32(sem + 40) == 1;
    if (*signal) {
        int32_t c = (int32_t)hr32(sem + 36);
        if (c < 0 || c == INT32_MAX) return false;
    }
    return !kern_would_switch(k);
}

static uint16_t smrls_do(m68k_cpu_t *cpu, uint32_t sem, uint32_t k, bool signal)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t n = hr32(sem + 40);
    uint16_t x = n == 0 ? SR_X : 0;           /* SUB.L D3,40(A2) */
    hw32(sem + 40, n - 1);
    if (signal) {
        hw32(sem + 28, 0);
        hw32(sem + 36, hr32(sem + 36) + 1);
    }
    d[0] = 0;
    SETW(d[1], (cpu->regs.sr & 0xFF00) | x | ccr_nz32(k));
    a[0] = k;
    return x;
}

/* Palm OS 3.1 has the same kernel and logic, but writes the write protect
 * bit itself, ends with MOVEM (no flags) and tests a few things differently. */
static void mem_sem_reserve(m68k_cpu_t *cpu, uint32_t at, bool v31)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t sp = a[7];
    bool write = hr8(sp + 4) != 0;
    uint16_t x = ccr_x(cpu);
    long cyc;
    if ((cpu->regs.sr & (SR_S | SR_T)) != SR_S || hr8(0x110) || hr8(0x111)) {
        hle_link0(cpu, at);
        return;
    }
    uint8_t held = hr8(0x24F);
    if (held) {
        if (write) { hw8(0x24F, (uint8_t)(held + 1)); x = held == 0xFF ? SR_X : 0; }
        d[0] = 0;
        cyc = 16 + 12 + 16 + 8 + 16 + 8 + 12 + (write ? 8 + 16 : 10) + 4 + 10 + 12 + 12 + 16;
    } else {
        uint32_t k = hr32(0x11E), sem = hr32(0x106), h = 0;
        bool nested;
        if (!smwait_ok(sem, k, &nested, v31) ||
            (write && !v31 && !hle_trap_is(0x24A, k_wp_off, sizeof k_wp_off, &h))) {
            hle_link0(cpu, at);
            return;
        }
        x = smwait_do(cpu, sem, k, nested, x);
        cyc = 800;
        if (write) {
            if (v31) SETW(d[1], (cpu->regs.sr & 0xFF00) | x | ccr_nz32(k));
            hw8(k + 1, hr8(k + 1) | 0x80);    /* task switching off */
            hw8(k + 4, (uint8_t)(hr8(k + 4) + 1));
            uint16_t r = bus_read16(HLE_WP_REG);
            if (v31) {
                bus_write16(HLE_WP_REG, r & ~0x2000);
            } else {
                if (r & 0x2000) bus_write16(HLE_WP_REG, r & ~0x2000);
                SETW(d[1], 0x24A * 4);        /* what the TrapDispatcher leaves */
                a[0] = at + 0x6E;
                a[1] = h;
            }
            uint8_t v = hr8(0x24F);
            hw8(0x24F, (uint8_t)(v + 1));
            x = v == 0xFF ? SR_X : 0;
            d[0] = 0;
            cyc += v31 ? 360 : 450 + HLE_DISPATCH_CYCLES;
        }
    }
    CCR(cpu, x | (v31 ? SR_Z : ccr_nz32(d[3])));   /* MOVE.W D3,D0 / MOVE.L (SP)+,D3 */
    a[7] = sp + 4;
    cpu->regs.pc = hr32(sp);
    cpu->regs.cycles = hle_cyc(cyc);
}

static void mem_sem_release(m68k_cpu_t *cpu, uint32_t at, bool v31)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t sp = a[7];
    bool write = hr8(sp + 4) != 0;
    uint16_t x = ccr_x(cpu);
    long cyc;
    if ((cpu->regs.sr & (SR_S | SR_T)) != SR_S || hr8(0x110) || hr8(0x111)) {
        hle_link0(cpu, at);
        return;
    }
    uint8_t held = hr8(0x24F);
    if (held && (!write || (int8_t)held > 1)) {
        if (write) { hw8(0x24F, (uint8_t)(held - 1)); x = 0; }
        d[0] = 0;
        cyc = 16 + 12 + 16 + 8 + 16 + 8 + 12 + 10 + (write ? 20 + 8 + 16 : 0) + 4 + 10 + 12 + 12 + 16;
    } else {
        uint32_t k = hr32(0x11E), sem = hr32(0x106), h = 0;
        bool signal;
        uint16_t f = ccr_sub(held, 1, 8);
        bool unprotect = write && (v31 ? (int8_t)(held - 1) <= 0
                                       : (f & SR_Z) || (((f & SR_N) != 0) != ((f & SR_V) != 0)));
        if (!smrls_ok(sem, k, &signal) ||
            (unprotect && !v31 && !hle_trap_is(0x24B, k_wp_on, sizeof k_wp_on, &h))) {
            hle_link0(cpu, at);
            return;
        }
        uint8_t k4 = hr8(k + 4);
        bool k_last = unprotect && k4 == 1 && (int8_t)hr8(k) > 0;
        if (k_last && (hr8(k + 1) & 0x7F)) {   /* would switch tasks */
            hle_link0(cpu, at);
            return;
        }
        x = smrls_do(cpu, sem, k, signal);
        cyc = v31 ? 1050 : 960;
        if (write) {
            hw8(0x24F, (uint8_t)(held - 1));
            x = held == 0 ? SR_X : 0;
            if (unprotect) {
                bus_write16(HLE_WP_REG, bus_read16(HLE_WP_REG) | 0x2000);
                if (!v31) {
                    SETW(d[1], 0x24B * 4);
                    a[1] = h;
                }
                if (k4) {                     /* task switching back on */
                    SETW(d[1], (cpu->regs.sr & 0xFF00) | x | ccr_nz32(k));
                    hw8(k + 4, (uint8_t)(k4 - 1));
                    if (k_last) hw8(k + 1, hr8(k + 1) & 0x7F);
                }
                a[0] = k;
                cyc += v31 ? 280 : 400 + HLE_DISPATCH_CYCLES;
            }
            d[0] = 0;
        }
    }
    CCR(cpu, x | (v31 ? SR_Z : ccr_nz32(d[3])));
    a[7] = sp + 4;
    cpu->regs.pc = hr32(sp);
    cpu->regs.cycles = hle_cyc(cyc);
}
static void fn_mem_sem_reserve(m68k_cpu_t *cpu, uint32_t at)   { mem_sem_reserve(cpu, at, false); }
static void fn_mem_sem_release(m68k_cpu_t *cpu, uint32_t at)   { mem_sem_release(cpu, at, false); }
static void fn_mem_sem_reserve31(m68k_cpu_t *cpu, uint32_t at) { mem_sem_reserve(cpu, at, true); }
static void fn_mem_sem_release31(m68k_cpu_t *cpu, uint32_t at) { mem_sem_release(cpu, at, true); }

/* --- Small helpers (Palm OS 4.x, some also 3.1) --------------------------- */

/* FntLineWidth (4.x): same idea as the 3.1 loop above, with signed range
 * checks against D6 / -4(A6) and the missing-glyph width at 1(A4). Entered at
 * the TST.W D4 / BNE that closes the loop. */
static const uint8_t k_fnt_width41[] = {
    0x1A, 0x12, 0x1E, 0x05, 0x0C, 0x05, 0x00, 0x09, 0x66, 0x12, 0x30, 0x43,
    0x20, 0x08, 0x81, 0xFC, 0x00, 0x14, 0x48, 0x40, 0x72, 0x14, 0x92, 0x40,
    0xD6, 0x41, 0x60, 0x32, 0x70, 0x00, 0x10, 0x07, 0x3A, 0x00, 0xB0, 0x46,
    0x6D, 0x20, 0xBA, 0x6E, 0xFF, 0xFC, 0x6E, 0x1A, 0x30, 0x05, 0x90, 0x46,
    0x48, 0xC0, 0xD0, 0x80, 0x1E, 0x33, 0x08, 0x01, 0x0C, 0x07, 0xFF, 0xFF,
    0x67, 0x08, 0x10, 0x07, 0x48, 0x80, 0xD6, 0x40, 0x60, 0x08, 0x10, 0x2C,
    0x00, 0x01, 0x48, 0x80, 0xD6, 0x40, 0x52, 0x8A, 0x53, 0x44, 0x4A, 0x44,
    0x66, 0xAA,
};

static void loop_fnt_width41(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6];
    uint16_t x = ccr_x(cpu);
    long cyc = 0;
    while (LO16(d[4])) {
        SETB(d[5], hr8(a[2]));
        SETB(d[7], (uint8_t)d[5]);
        uint8_t c = (uint8_t)d[5];
        cyc += 4 + 10 + 8 + 4 + 8 + 10;
        if (c == 9) {
            a[0] = (uint32_t)(int32_t)(int16_t)LO16(d[3]);
            d[0] = a[0];
            int32_t q = (int32_t)d[0] / 20, r = (int32_t)d[0] % 20;
            d[0] = ((uint32_t)(uint16_t)q << 16) | (uint16_t)r;
            d[1] = (uint32_t)(uint16_t)(20 - (uint16_t)r);
            SETW(d[3], LO16(d[3]) + LO16(d[1]));
            cyc += 4 + 4 + 158 + 4 + 4 + 4 + 4 + 10 - 2;
        } else {
            d[0] = c;
            SETW(d[5], c);
            cyc += 4 - 2 + 4 + 4 + 4 + 4;
            bool missing = (int16_t)c < (int16_t)LO16(d[6]);
            cyc += missing ? 10 : 8 + 12 + 8;
            if (!missing) missing = (int16_t)c > (int16_t)hr16(fp - 4);
            if (!missing) {
                d[0] = (uint32_t)(int32_t)(int16_t)(uint16_t)(c - LO16(d[6]));
                d[0] <<= 1;
                SETB(d[7], hr8(a[3] + 1 + d[0]));
                missing = (uint8_t)d[7] == 0xFF;
                cyc += 2 + 4 + 4 + 4 + 8 + 14 + 8 + 10;
                if (!missing) {
                    SETB(d[0], (uint8_t)d[7]);
                    SETW(d[0], (uint16_t)(int16_t)(int8_t)d[0]);
                    SETW(d[3], LO16(d[3]) + LO16(d[0]));
                    cyc += 4 + 4 + 4 + 10 - 2;
                }
            }
            if (missing) {
                SETB(d[0], hr8(a[4] + 1));
                SETW(d[0], (uint16_t)(int16_t)(int8_t)d[0]);
                SETW(d[3], LO16(d[3]) + LO16(d[0]));
                cyc += 12 + 4 + 4;
            }
        }
        a[2] += 1;
        SETW(d[4], LO16(d[4]) - 1);
        x = 0;                                /* SUBQ from non-zero */
        cyc += 8 + 4;
    }
    CCR(cpu, x | SR_Z);
    cpu->regs.pc = at + 4;
    cpu->regs.cycles = hle_cyc((cyc + 4 + 8) * 9 / 10);
}

/* Crc16CalcBlock: CRC-16/CCITT by table (A1), D4 bytes at A0, CRC in D2. */
static const uint8_t k_crc16[] = {
    0x70, 0x00, 0x10, 0x18, 0x32, 0x02, 0xE0, 0x49, 0xB1, 0x41, 0x70, 0x00,
    0x10, 0x01, 0xD0, 0x80, 0x32, 0x02, 0xE1, 0x49, 0x30, 0x31, 0x08, 0x00,
    0xB1, 0x41, 0x34, 0x01, 0x53, 0x44, 0x66, 0xE0,
};

static void loop_crc16(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint16_t crc = LO16(d[2]), n = LO16(d[4]), t = 0;
    uint32_t tab = a[1], src = a[0];
    long cyc = 0;
    do {
        uint8_t i = (uint8_t)((crc >> 8) ^ hr8(src++));
        t = hr16(tab + 2u * i);
        crc = (uint16_t)((crc << 8) ^ t);
        n = (uint16_t)(n - 1);
        cyc += 114;
    } while (n);
    a[0] = src;
    d[0] = t;                                 /* the last table word */
    SETW(d[1], crc);
    SETW(d[2], crc);
    SETW(d[4], 0);
    CCR(cpu, SR_Z);                           /* SUBQ.W #1,D4 that reached zero */
    cpu->regs.pc = at + sizeof k_crc16;
    cpu->regs.cycles = hle_cyc(cyc - 2);
}

/* DmFindResource, the scan of a resource list: 10-byte entries from -4(A6)
 * to -8(A6), type at 0, id at 4. Stops at a match (type 12(A6), id D7) or at
 * the end of the list, where the ROM code fetches the next one. Entered at
 * the MOVEA.L -4(A6),A0 / CMPA.L -8(A6),A0 / BNE at the top. */
static const uint8_t k_dm_find_res[] = {
    0x20, 0x6E, 0xFF, 0xFC, 0xB1, 0xEE, 0xFF, 0xF8, 0x66, 0x1E, 0x48, 0x6E,
    0xFF, 0xF8, 0x48, 0x6E, 0xFF, 0xFC, 0x30, 0x04, 0x52, 0x44, 0x3F, 0x00,
    0x2F, 0x0A, 0x4E, 0xBA, 0x09, 0xDC, 0x4A, 0x00, 0x4F, 0xEF, 0x00, 0x0E,
    0x67, 0x6A, 0x60, 0xD8, 0x20, 0x6E, 0xFF, 0xFC, 0x20, 0x10, 0xB0, 0xAE,
    0x00, 0x0C, 0x66, 0x06, 0xBE, 0x68, 0x00, 0x04, 0x67, 0x56, 0x70, 0x0A,
    0xD1, 0xAE, 0xFF, 0xFC, 0x52, 0x43, 0x60, 0xBC,
};

static void loop_dm_find_res(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t fp = a[6];
    uint32_t type = hr32(fp + 12);
    uint16_t x = ccr_x(cpu);
    long cyc = 0;
    for (;;) {
        uint32_t cur = hr32(fp - 4), end = hr32(fp - 8);
        a[0] = cur;
        cyc += 16 + 18 + 10;
        if (cur == end) {                     /* end of this list */
            CCR(cpu, x | SR_Z);
            cpu->regs.pc = at + 0x0A;
            cyc -= 2;
            break;
        }
        d[0] = hr32(cur);
        cyc += 16 + 12 + 18 + 10;
        if (d[0] == type) {
            uint16_t f = ccr_sub(LO16(d[7]), hr16(cur + 4), 16);
            cyc += 12 - 2 + 10;
            if (f & SR_Z) {                   /* found */
                CCR(cpu, x | f);
                cpu->regs.pc = at + 0x90;
                break;
            }
            cyc -= 2;
        } else {
            cyc -= 2;
        }
        d[0] = 10;
        hw32(fp - 4, cur + 10);
        SETW(d[3], LO16(d[3]) + 1);
        x = LO16(d[3]) == 0 ? SR_X : 0;       /* ADDQ.W #1,D3 */
        cyc += 4 + 24 + 4 + 10;
    }
    cpu->regs.cycles = hle_cyc(cyc * 5 / 9);   /* fitted to --hle-check */
}

/* The compiler's 32-bit multiply (D0 * D1 -> D0), linked into the ROM many
 * times over; every copy is patched. */
static const uint8_t k_mul32[] = {
    0x48, 0xE7, 0x30, 0x00, 0x74, 0x02, 0x4E, 0xFB, 0x22, 0x00, 0x60, 0x0A,
    0x4C, 0x01, 0x00, 0x00, 0x4C, 0xDF, 0x00, 0x0C, 0x4E, 0x75, 0x24, 0x00,
    0x48, 0x42, 0xC4, 0xC1, 0x26, 0x01, 0x48, 0x43, 0xC6, 0xC0, 0xD4, 0x43,
    0x48, 0x42, 0x42, 0x42, 0xC0, 0xC1, 0xD0, 0x82, 0x4C, 0xDF, 0x00, 0x0C,
    0x4E, 0x75,
};

static void fn_mul32(m68k_cpu_t *cpu, uint32_t at)
{
    uint32_t *d = cpu->regs.d, *a = cpu->regs.a;
    uint32_t lo0 = d[0] & 0xFFFF, hi0 = d[0] >> 16, lo1 = d[1] & 0xFFFF, hi1 = d[1] >> 16;
    uint32_t hi = ((hi0 * lo1) + (hi1 * lo0)) << 16;   /* ADD.W, SWAP, CLR.W */
    uint32_t p = lo0 * lo1, r = p + hi;
    d[0] = r;
    uint16_t c = r < p ? (SR_C | SR_X) : 0;
    uint16_t v = (~(p ^ hi) & (p ^ r) & 0x80000000u) ? SR_V : 0;
    CCR(cpu, c | v | ccr_nz32(r));
    uint32_t sp = a[7];
    hw32(sp - 8, d[2]); hw32(sp - 4, d[3]);   /* MOVEM.L D2-D3,-(SP) */
    cpu->regs.pc = hr32(sp);
    a[7] = sp + 4;
    cpu->regs.cycles = 24 + 4 + 14 + 10 + 4 + 4 + 70 + 4 + 4 + 70 + 4 + 4 + 4 + 70 + 8 + 28 + 16;
    (void)at;
}

typedef struct {
    const char    *name;
    const uint8_t *code;
    uint16_t       len;
    uint16_t       entry;                   /* offset of the patched word */
    void         (*run)(m68k_cpu_t *cpu, uint32_t at);
    int32_t        span_off;                /* if span: the bytes from match+span_off */
    uint16_t       span;                    /* (the code it calls too) must hash */
    uint32_t       span_fnv;                /* (FNV-1a) to this */
    bool           every;                   /* patch every copy, not just the first */
} hle_loop_t;

#define HLE_LOOP(name, code, entry, run) { name, code, sizeof code, entry, run, 0, 0, 0, false }

static const hle_loop_t k_loops[] = {
    HLE_LOOP("PrvBltConstCopy rows", k_const_copy_rows, 0, loop_const_copy_rows),
    HLE_LOOP("PrvBltSwap words", k_swap_words, 0x52, loop_swap_words),
    HLE_LOOP("PrvBlit1To8Copy rows", k_blit1to8_rows, 0x66, loop_blit1to8_rows),
    HLE_LOOP("PrvGetBltInfo colour tables", k_ctab_compare, 0x2E, loop_ctab_compare),
    HLE_LOOP("PrvBltMisAlignCopy words", k_misalign_words, 0, loop_misalign_words),
    HLE_LOOP("PrvConvert1To8", k_convert_1to8, 0, loop_convert_1to8),
    /* Palm OS 3.1 */
    HLE_LOOP("SysLaunch RAM clear", k_sys_ram_clear, 0x34, loop_sys_ram_clear),
    HLE_LOOP("ScrRectangleRoutine rows", k_scr_rect_rows, 0x32, loop_scr_rect_rows),
    HLE_LOOP("PrvMisAlignedForwardInnerBitBlt bytes", k_mis_b1, 0, loop_mis_b1),
    HLE_LOOP("PrvMisAlignedForwardInnerBitBlt high bytes", k_mis_b2, 0, loop_mis_b2),
    HLE_LOOP("PrvMisAlignedForwardInnerBitBlt byte pairs", k_mis_b3, 0, loop_mis_b3),
    HLE_LOOP("PrvMisAlignedForwardInnerBitBlt copy", k_mis_wcopy, 0, loop_mis_wcopy),
    HLE_LOOP("PrvMisAlignedForwardInnerBitBlt not-copy", k_mis_wnot, 0, loop_mis_wnot),
    HLE_LOOP("PrvMisAlignedForwardInnerBitBlt xor", k_mis_wxor, 0, loop_mis_wxor),
    HLE_LOOP("ScrDeCompressScanLine", k_decompress_line, 0, loop_decompress_line),
    HLE_LOOP("FntLineWidth", k_fnt_width, 0x6C, loop_fnt_width),
    HLE_LOOP("PrvCompressedInnerBitBlt row", k_compressed_row, 0, loop_compressed_row),
    /* Palm OS 4.x kernel */
    { "MemSemaphoreReserve", k_mem_sem_reserve, sizeof k_mem_sem_reserve, 0, fn_mem_sem_reserve,
      0, 0x3D2E, 0x07BF46E0, false },
    { "MemSemaphoreRelease", k_mem_sem_release, sizeof k_mem_sem_release, 0, fn_mem_sem_release,
      0, 0x3C9C, 0x4C53ABFB, false },
    /* Palm OS 3.1 kernel: the kernel code sits before the functions */
    { "MemSemaphoreReserve (3.1)", k_mem_sem_reserve31, sizeof k_mem_sem_reserve31, 0,
      fn_mem_sem_reserve31, -0x23E6, 0x251E, 0xCE417AD9, false },
    { "MemSemaphoreRelease (3.1)", k_mem_sem_release31, sizeof k_mem_sem_release31, 0,
      fn_mem_sem_release31, -0x248C, 0x251E, 0xCE417AD9, false },
    /* small helpers */
    HLE_LOOP("FntLineWidth (4.x)", k_fnt_width41, 0x52, loop_fnt_width41),
    HLE_LOOP("Crc16CalcBlock", k_crc16, 0, loop_crc16),
    HLE_LOOP("DmFindResource scan", k_dm_find_res, 0, loop_dm_find_res),
    { "32-bit multiply", k_mul32, sizeof k_mul32, 0, fn_mul32, 0, 0, 0, true },
};
#define HLE_NLOOPS (sizeof k_loops / sizeof k_loops[0])
#define HLE_LOOP_OP 0xFF00                /* | loop index */

static uint32_t hle_loop_at[HLE_NLOOPS];  /* patched ROM address, 0 if absent */

static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

static void hle_patch_loops(uint8_t *rom, uint32_t size)
{
    /* Find everything first: a span may cover another loop's patch. */
    for (uint32_t k = 0; k < HLE_NLOOPS; k++) {
        const hle_loop_t *l = &k_loops[k];
        hle_loop_at[k] = 0;
        for (uint32_t i = 0; i + l->len <= size; i += 2) {
            if (rom[i] != l->code[0] || memcmp(rom + i, l->code, l->len) != 0) continue;
            if (l->span) {
                int64_t lo = (int64_t)i + l->span_off;
                if (lo < 0 || lo + l->span > size || fnv1a(rom + lo, l->span) != l->span_fnv) continue;
            }
            hle_loop_at[k] = BUS_ROM_BASE + i + l->entry;
            break;
        }
    }
    for (uint32_t k = 0; k < HLE_NLOOPS; k++) {
        const hle_loop_t *l = &k_loops[k];
        unsigned n = 0;
        for (uint32_t i = hle_loop_at[k] ? hle_loop_at[k] - BUS_ROM_BASE - l->entry : size;
             i + l->len <= size; i += 2) {
            if (rom[i] != l->code[0] || memcmp(rom + i, l->code, l->len) != 0) continue;
            rom[i + l->entry] = (uint8_t)(HLE_LOOP_OP >> 8);
            rom[i + l->entry + 1] = (uint8_t)k;
            n++;
            if (!l->every) break;
        }
        if (n > 1)
            ESP_LOGI(TAG_CPU, "native loop %s: on, %u copies", l->name, n);
        else
            ESP_LOGI(TAG_CPU, "native loop %s: %s", l->name, n ? "on" : "not in this ROM");
    }
}

/* MemMove(void *dst, const void *src, Int32 n): Err. Returns the cycles the
 * ROM code would take (roughly), or -1 to let the ROM code run. */
static int hle_memmove(uint32_t sp)
{
    uint32_t dst = bus_read32(sp), src = bus_read32(sp + 4), n = bus_read32(sp + 8);
    if (n == 0) return 120;
    if (dst == 0) return -1;              /* the ROM code reports the bad call */
    uint8_t *d = hle_mem(dst, n), *s = hle_mem(src, n);
    if (!d || !s) return -1;
    memmove(d, s, n);
    /* Word loop when both ends share even alignment, else byte loop.
     * Fitted to the ROM code's cycles in the --hle-check runs. */
    bool words = !((dst ^ src) & 1);
    return 360 + (int)(words ? n * 15 : n * 30);
}

/* MemSet(void *dst, Int32 n, UInt8 value): Err. */
static int hle_memset(uint32_t sp)
{
    uint32_t dst = bus_read32(sp), n = bus_read32(sp + 4);
    uint8_t value = bus_read8(sp + 8);
    if (n == 0) return 120;
    if (dst == 0) return -1;
    uint8_t *d = hle_mem(dst, n);
    if (!d) return -1;
    memset(d, value, n);
    return n < 8 ? 360 + (int)n * 45 : 640 + (int)(n * 20 / 3);
}

static HLE_HOT void hle_dispatch(m68k_cpu_t *cpu, uint16_t opcode)
{
    uint32_t ret = cpu->regs.pc + 2;          /* past the selector word */
    uint32_t sp = cpu->regs.a[7];
    uint16_t sr = cpu->regs.sr;
    uint16_t sel4 = (uint16_t)((opcode & 0x07FF) << 2);
    uint32_t handler = bus_read32(bus_read32(0x122) + sel4);

    /* What TrapDispatcher leaves behind: D0.W = caller's SR, D1.W = the
     * selector times 4, A0 = return address, A1 = handler; on the stack the
     * return address, and below it the handler/SR pair its RTE consumed. */
    cpu->regs.d[0] = (cpu->regs.d[0] & 0xFFFF0000u) | sr;
    cpu->regs.d[1] = (cpu->regs.d[1] & 0xFFFF0000u) | sel4;
    cpu->regs.a[0] = ret;
    cpu->regs.a[1] = handler;
    bus_write32(sp - 4, ret);
    bus_write32(sp - 8, handler);
    bus_write16(sp - 10, sr);
    cpu->regs.a[7] = sp - 4;
    cpu->regs.pc = handler;
    cpu->regs.cycles = HLE_DISPATCH_CYCLES;
    g_palm_hle_stats.dispatches++;

    int leaf = -1;
    if (handler == hle.memmove)     leaf = hle_memmove(sp);
    else if (handler == hle.memset) leaf = hle_memset(sp);
    if (leaf >= 0) {
        cpu->regs.d[0] = 0;                   /* errNone */
        cpu->regs.a[7] = sp;                  /* the handler's RTS */
        cpu->regs.pc = ret;
        cpu->regs.cycles += leaf;
        g_palm_hle_stats.leaf_calls++;
    }
}

#ifdef HOST_BUILD
int g_palm_hle_check;
static struct { uint32_t n; uint64_t rom, native; } hle_loop_stats[HLE_NLOOPS];

/* Per native loop: checked calls and the cycles each way. */
void m68k_palm_hle_print_loops(void)
{
    for (uint32_t k = 0; k < HLE_NLOOPS; k++)
        if (hle_loop_stats[k].n)
            fprintf(stderr, "  %-44s %7u checked, cycles rom %10llu native %10llu (%+.1f%%)\n",
                    k_loops[k].name, hle_loop_stats[k].n,
                    (unsigned long long)hle_loop_stats[k].rom,
                    (unsigned long long)hle_loop_stats[k].native,
                    100.0 * ((double)hle_loop_stats[k].native - (double)hle_loop_stats[k].rom) /
                        (double)(hle_loop_stats[k].rom ? hle_loop_stats[k].rom : 1));
}   /* check every Nth native call against the ROM code */

/* Run one native call (a system call when loop < 0, else native loop `loop`),
 * then the same thing through the ROM code from the same starting state with
 * interrupts held off, compare, and carry on from the ROM code's result. */
static void hle_check(m68k_cpu_t *cpu, int loop, uint32_t at)
{
    static uint8_t *ram0, *ram1, *vram0, *vram1;
    if (!ram0) {
        ram0 = malloc(BUS_RAM_SIZE); ram1 = malloc(BUS_RAM_SIZE);
        vram0 = malloc(BUS_SED1375_VRAM_SIZE); vram1 = malloc(BUS_SED1375_VRAM_SIZE);
    }
    m68k_regs_t before = cpu->regs;
    memcpy(ram0, g_bus_ram, BUS_RAM_SIZE);
    if (g_bus_vram) memcpy(vram0, g_bus_vram, BUS_SED1375_VRAM_SIZE);

    uint16_t opcode = 0;
    uint32_t start;
    if (loop < 0) {
        opcode = bus_read16(cpu->regs.pc);
        start = cpu->regs.pc - 2;                 /* the TRAP #15 */
        hle_dispatch(cpu, opcode);
    } else {
        start = at;
        k_loops[loop].run(cpu, start);
    }
    m68k_regs_t nat = cpu->regs;
    bool leaf = loop < 0 && nat.pc == before.pc + 2;
    memcpy(ram1, g_bus_ram, BUS_RAM_SIZE);
    if (g_bus_vram) memcpy(vram1, g_bus_vram, BUS_SED1375_VRAM_SIZE);

    cpu->regs = before;
    cpu->regs.pc = start;
    memcpy(g_bus_ram, ram0, BUS_RAM_SIZE);
    if (g_bus_vram) memcpy(g_bus_vram, vram0, BUS_SED1375_VRAM_SIZE);
    uint8_t *patch = (uint8_t *)g_bus_rom + (start - BUS_ROM_BASE);
    const uint8_t *orig = loop >= 0 ? k_loops[loop].code + k_loops[loop].entry : NULL;
    if (orig) { patch[0] = orig[0]; patch[1] = orig[1]; }
    hle.busy = true;
    long cyc = 0;
    for (long steps = 0; steps < 20000000; steps++) {
        interp_one(cpu);
        cyc += cpu->regs.cycles;
        if (cpu->regs.pc == nat.pc && cpu->regs.a[7] == nat.a[7]) break;
    }
    hle.busy = false;
    if (loop >= 0) { patch[0] = (uint8_t)(HLE_LOOP_OP >> 8); patch[1] = (uint8_t)loop; }

    g_palm_hle_stats.checked++;
    char why[160] = "";
    for (int i = 0; i < 8 && !why[0]; i++) {
        if (leaf && i == 1) continue;                     /* D1: scratch */
        if (cpu->regs.d[i] != nat.d[i])
            snprintf(why, sizeof why, "D%d rom=%08lX native=%08lX", i,
                     (unsigned long)cpu->regs.d[i], (unsigned long)nat.d[i]);
    }
    for (int i = 0; i < 8 && !why[0]; i++) {
        if (leaf && (i == 0 || i == 1)) continue;         /* A0/A1: scratch */
        if (cpu->regs.a[i] != nat.a[i])
            snprintf(why, sizeof why, "A%d rom=%08lX native=%08lX", i,
                     (unsigned long)cpu->regs.a[i], (unsigned long)nat.a[i]);
    }
    uint16_t sr_mask = leaf ? 0xFFE0 : 0xFFFF;            /* leaf: CCR as it falls */
    if (!why[0] && cpu->regs.pc != nat.pc)
        snprintf(why, sizeof why, "PC rom=%08lX native=%08lX",
                 (unsigned long)cpu->regs.pc, (unsigned long)nat.pc);
    if (!why[0] && ((cpu->regs.sr ^ nat.sr) & sr_mask))
        snprintf(why, sizeof why, "SR rom=%04X native=%04X", cpu->regs.sr, nat.sr);
    if (!why[0] && loop < 0 && !leaf && cyc != nat.cycles)
        snprintf(why, sizeof why, "cycles rom=%ld native=%d", cyc, nat.cycles);
    /* A leaf call's own stack frame below the final SP is dead and differs. */
    bool dead = leaf || loop >= 0;    /* native functions skip their stack frames */
    uint32_t skip_lo = dead ? nat.a[7] - 512 : 0, skip_hi = dead ? nat.a[7] : 0;
    for (uint32_t i = 0; i < BUS_RAM_SIZE && !why[0]; i++) {
        if (g_bus_ram[i] != ram1[i] && !(i >= skip_lo && i < skip_hi))
            snprintf(why, sizeof why, "RAM %06lX rom=%02X native=%02X",
                     (unsigned long)i, g_bus_ram[i], ram1[i]);
    }
    for (uint32_t i = 0; g_bus_vram && i < BUS_SED1375_VRAM_SIZE && !why[0]; i++) {
        if (g_bus_vram[i] != vram1[i])
            snprintf(why, sizeof why, "video RAM +%05lX rom=%02X native=%02X",
                     (unsigned long)i, g_bus_vram[i], vram1[i]);
    }
    if (loop >= 0) {
        hle_loop_stats[loop].n++;
        hle_loop_stats[loop].rom += (uint64_t)cyc;
        hle_loop_stats[loop].native += (uint64_t)nat.cycles;
    }
    if (leaf || loop >= 0) {
        g_palm_hle_stats.leaf_rom_cycles += (uint64_t)cyc;
        g_palm_hle_stats.leaf_native_cycles +=
            (uint64_t)(nat.cycles - (leaf ? HLE_DISPATCH_CYCLES : 0));
    }
    if (why[0]) {
        g_palm_hle_stats.mismatches++;
        if (g_palm_hle_stats.mismatches <= 20) {
            if (loop < 0)
                fprintf(stderr, "[HLE] MISMATCH trap %04X -> %08lX%s: %s\n", opcode,
                        (unsigned long)nat.a[1], leaf ? " (leaf)" : "", why);
            else
                fprintf(stderr, "[HLE] MISMATCH loop %s: %s\n", k_loops[loop].name, why);
        }
    }
    cpu->regs.cycles = (int)cyc;
}
#endif

/* Called for TRAP #15 with PC at the selector word. */
HLE_HOT bool m68k_palm_hle_trap15(m68k_cpu_t *cpu)
{
    if (!hle.enabled || hle.busy || (cpu->regs.sr & (SR_S | SR_T)) != SR_S)
        return false;
    uint16_t opcode = bus_read16(cpu->regs.pc);
    if ((opcode & 0xF800) != 0xA000)            /* library traps: ROM code */
        return false;
    if (!hle_dispatcher_at(bus_read32((VEC_TRAP_BASE + 15) * 4)))
        return false;
#ifdef HOST_BUILD
    if (g_palm_hle_check) {
        /* Sample leaf calls on their own count; they are much rarer. */
        static uint32_t n_disp, n_leaf;
        uint32_t h = bus_read32(bus_read32(0x122) + ((opcode & 0x07FF) << 2));
        bool leaf = h && (h == hle.memmove || h == hle.memset);
        if (++*(leaf ? &n_leaf : &n_disp) % (uint32_t)g_palm_hle_check == 0) {
            hle_check(cpu, -1, 0);
            return true;
        }
    }
#endif
    hle_dispatch(cpu, opcode);
    g_palm_hle_jumped = true;
    return true;
}

/* Called for an F-line opcode; true if it was a patched native loop. */
HLE_HOT bool m68k_palm_hle_fline(m68k_cpu_t *cpu, uint16_t opcode)
{
    uint32_t k = opcode & 0xFF;
    uint32_t at = cpu->regs.inst_pc;
    if ((opcode & 0xFF00) != HLE_LOOP_OP || k >= HLE_NLOOPS || !hle_loop_at[k] ||
        (k_loops[k].every ? !BUS_FAST_ROM(at) : hle_loop_at[k] != at))
        return false;
    g_palm_hle_stats.loops++;
#ifdef HOST_BUILD
    static uint32_t n[HLE_NLOOPS];
    if (g_palm_hle_check && !hle.busy && ++n[k] % (uint32_t)g_palm_hle_check == 0) {
        hle_check(cpu, (int)k, at);
        return true;
    }
#endif
    k_loops[k].run(cpu, at);
    g_palm_hle_jumped = true;
    return true;
}
