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
 * Hot loops inside ROM functions (the blitters) are found by their exact
 * code bytes. The first word of each becomes an F-line opcode ($FFxx, never
 * used by Palm OS) and m68k_palm_hle_fline() runs the loop in C, leaving
 * registers, flags and memory exactly as the ROM loop would. A ROM whose code
 * differs simply keeps running its own loops.
 *
 * Native work is skipped (the ROM code runs as before) whenever the CPU is
 * not in supervisor mode, is tracing, the TRAP #15 vector does not point at
 * an unmodified TrapDispatcher, the trap is a library trap, or a leaf call's
 * memory is not plain RAM or video RAM.
 *
 * Host builds can check every Nth native call against the ROM code
 * (dragonfruit --hle-check N): the call runs natively, then again through the
 * interpreter from the same state, and registers, cycles and all of RAM are
 * compared. Compiled into the m68k unity build (m68k_all.c). */

#ifdef HOST_BUILD
#include <stdlib.h>
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

static bool hle_dispatcher_at(uint32_t vec)
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
    cpu->regs.cycles = hle_cyc(cyc);
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

typedef struct {
    const char    *name;
    const uint8_t *code;
    uint16_t       len;
    uint16_t       entry;                   /* offset of the patched word */
    void         (*run)(m68k_cpu_t *cpu, uint32_t at);
} hle_loop_t;

static const hle_loop_t k_loops[] = {
    { "PrvBltConstCopy rows", k_const_copy_rows, sizeof k_const_copy_rows, 0, loop_const_copy_rows },
    { "PrvBltSwap words", k_swap_words, sizeof k_swap_words, 0x52, loop_swap_words },
    { "PrvBlit1To8Copy rows", k_blit1to8_rows, sizeof k_blit1to8_rows, 0x66, loop_blit1to8_rows },
    { "PrvGetBltInfo colour tables", k_ctab_compare, sizeof k_ctab_compare, 0x2E, loop_ctab_compare },
    { "PrvBltMisAlignCopy words", k_misalign_words, sizeof k_misalign_words, 0, loop_misalign_words },
    { "PrvConvert1To8", k_convert_1to8, sizeof k_convert_1to8, 0, loop_convert_1to8 },
};
#define HLE_NLOOPS (sizeof k_loops / sizeof k_loops[0])
#define HLE_LOOP_OP 0xFF00                /* | loop index */

static uint32_t hle_loop_at[HLE_NLOOPS];  /* patched ROM address, 0 if absent */

static void hle_patch_loops(uint8_t *rom, uint32_t size)
{
    for (uint32_t k = 0; k < HLE_NLOOPS; k++) {
        const hle_loop_t *l = &k_loops[k];
        hle_loop_at[k] = 0;
        for (uint32_t i = 0; i + l->len <= size; i += 2) {
            if (rom[i] != l->code[0] || memcmp(rom + i, l->code, l->len) != 0) continue;
            uint32_t at = i + l->entry;
            rom[at] = (uint8_t)(HLE_LOOP_OP >> 8);
            rom[at + 1] = (uint8_t)k;
            hle_loop_at[k] = BUS_ROM_BASE + at;
            break;
        }
        ESP_LOGI(TAG_CPU, "native loop %s: %s", l->name, hle_loop_at[k] ? "on" : "not in this ROM");
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

static void hle_dispatch(m68k_cpu_t *cpu, uint16_t opcode)
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
int g_palm_hle_check;   /* check every Nth native call against the ROM code */

/* Run one native call (a system call when loop < 0, else native loop `loop`),
 * then the same thing through the ROM code from the same starting state with
 * interrupts held off, compare, and carry on from the ROM code's result. */
static void hle_check(m68k_cpu_t *cpu, int loop)
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
        start = hle_loop_at[loop];
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
    uint32_t skip_lo = leaf ? nat.a[7] - 512 : 0, skip_hi = leaf ? nat.a[7] : 0;
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
bool m68k_palm_hle_trap15(m68k_cpu_t *cpu)
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
            hle_check(cpu, -1);
            return true;
        }
    }
#endif
    hle_dispatch(cpu, opcode);
    return true;
}

/* Called for an F-line opcode; true if it was a patched native loop. */
bool m68k_palm_hle_fline(m68k_cpu_t *cpu, uint16_t opcode)
{
    uint32_t k = opcode & 0xFF;
    if ((opcode & 0xFF00) != HLE_LOOP_OP || k >= HLE_NLOOPS ||
        hle_loop_at[k] != cpu->regs.inst_pc)
        return false;
    g_palm_hle_stats.loops++;
#ifdef HOST_BUILD
    static uint32_t n[HLE_NLOOPS];
    if (g_palm_hle_check && !hle.busy && ++n[k] % (uint32_t)g_palm_hle_check == 0) {
        hle_check(cpu, (int)k);
        return true;
    }
#endif
    k_loops[k].run(cpu, hle_loop_at[k]);
    return true;
}
