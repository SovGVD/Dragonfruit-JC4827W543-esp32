/* bench.c — a Palm-OS-flavoured 68000 workload for measuring emulator speed.
 *
 * Built for a plain 68000 (-mcpu=68000) with no libgcc, so every operation maps
 * to real 68000 instructions: 16-bit MULU/DIVU, word/byte memory moves, JSR/RTS
 * with LINK/UNLK/MOVEM frames, jump tables, bit shifting and DBcc-style loops.
 * The kernels mirror what Palm OS spends its time on: MemMove/MemSet, string
 * handling, sorting records, walking handle/chunk lists, CRC, blitting a 1bpp
 * framebuffer and a switch-dispatched event loop. The final checksum makes the
 * run self-checking: any emulation bug changes it.
 */
#include <stdint.h>
#include <stddef.h>

#define ROUNDS 4

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--) *dp++ = (uint8_t)c;
    return d;
}

/* 32x32 multiply for the few places GCC wants one (no libgcc for a 68000). */
uint32_t __mulsi3(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    while (b) { if (b & 1) r += a; a <<= 1; b >>= 1; }
    return r;
}

static uint16_t buf_a[2048], buf_b[2048];
static uint8_t  fb[20 * 160];               /* 160x160 1bpp, 20 bytes per row */

/* --- MemMove: word copy with a byte tail, like the Palm OS MemMove fast path */
static void mem_move(void *dst, const void *src, uint16_t n)
{
    uint16_t *d = dst; const uint16_t *s = src;
    uint16_t w = n >> 1;
    while (w--) *d++ = *s++;
    if (n & 1) *(uint8_t *)d = *(const uint8_t *)s;
}

static uint32_t k_memory(void)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < 2048; i++) buf_a[i] = (uint16_t)(i * 7 + 3);
    for (int r = 0; r < 8; r++) {
        mem_move(buf_b, buf_a, sizeof buf_a);
        memset(buf_a + 100, r, 512);
        memcpy(buf_a + 900, buf_b + 10, 700);
        sum += buf_a[r * 200] + buf_b[2047 - r];
    }
    return sum;
}

/* --- strings ------------------------------------------------------------ */
static const char text[] =
    "Palm OS was designed for small handhelds with a stylus and Graffiti. "
    "The DragonBall EZ runs at 16.58 MHz and executes 68000 code from ROM. "
    "Applications are stored as databases of records in battery-backed RAM.";

static uint16_t str_len(const char *s) { const char *p = s; while (*p) p++; return (uint16_t)(p - s); }

static int16_t str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int16_t)((uint8_t)*a - (uint8_t)*b);
}

static int16_t str_find(const char *h, const char *n)
{
    for (int16_t i = 0; h[i]; i++) {
        int16_t j = 0;
        while (n[j] && h[i + j] == n[j]) j++;
        if (!n[j]) return i;
    }
    return -1;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static uint32_t k_strings(void)
{
    static char tmp[256];
    uint32_t sum = 0;
    for (int r = 0; r < 6; r++) {
        uint16_t n = str_len(text);
        for (uint16_t i = 0; i <= n; i++) tmp[i] = lower(text[i]);
        sum += n + (uint16_t)str_cmp(tmp, text) + (uint16_t)str_find(text, "records");
        sum += (uint16_t)str_find(tmp, "graffiti") + (uint16_t)str_find(text, "zzz");
    }
    return sum;
}

/* --- records: insertion sort of 16-bit keys carried in small structs ----- */
typedef struct { uint16_t key; uint16_t id; uint8_t attr; uint8_t cat; } rec_t;
static rec_t recs[160];

static uint32_t k_sort(void)
{
    uint16_t seed = 12345;
    for (int i = 0; i < 160; i++) {
        seed = (uint16_t)(seed * 25173u + 13849u);
        recs[i].key = seed; recs[i].id = (uint16_t)i;
        recs[i].attr = (uint8_t)(seed >> 3); recs[i].cat = (uint8_t)(i & 15);
    }
    for (int i = 1; i < 160; i++) {
        rec_t v = recs[i];
        int j = i - 1;
        while (j >= 0 && recs[j].key > v.key) { recs[j + 1] = recs[j]; j--; }
        recs[j + 1] = v;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 160; i += 7) sum += (uint32_t)recs[i].id * 3u + recs[i].cat;
    return sum;
}

/* --- chunk list walk: pointer chasing with (d16,An) field access -------- */
typedef struct chunk { struct chunk *next; uint16_t size; uint8_t flags; uint8_t owner; uint32_t data; } chunk_t;
static chunk_t chunks[256];

static uint32_t k_list(void)
{
    for (int i = 0; i < 256; i++) {
        chunks[i].next = &chunks[(i * 37 + 11) & 255];
        chunks[i].size = (uint16_t)(i * 13);
        chunks[i].flags = (uint8_t)(i & 3);
        chunks[i].owner = (uint8_t)(i >> 4);
        chunks[i].data = (uint32_t)i << 8;
    }
    uint32_t sum = 0;
    chunk_t *c = &chunks[0];
    for (int n = 0; n < 6000; n++) {
        if (c->flags & 1) sum += c->size; else sum ^= c->data;
        if (c->owner == 7) c->flags ^= 2;
        c = c->next;
    }
    return sum;
}

/* --- CRC-16/CCITT, bit-serial ------------------------------------------- */
static uint32_t k_crc(void)
{
    uint16_t crc = 0xFFFF;
    const uint8_t *p = (const uint8_t *)buf_a;
    for (uint16_t i = 0; i < 1536; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* --- 1bpp blitter: filled/inverted rectangles with edge masks ----------- */
static void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t mode)
{
    for (int16_t row = y; row < y + h; row++) {
        uint8_t *line = fb + row * 20;
        for (int16_t px = x; px < x + w; ) {
            uint8_t mask, *b = line + (px >> 3);
            int16_t bit = px & 7, n = 8 - bit;
            if (n > x + w - px) n = (int16_t)(x + w - px);
            mask = (uint8_t)((0xFF >> bit) & (0xFF << (8 - bit - n)));
            if (mode == 0) *b |= mask; else if (mode == 1) *b &= (uint8_t)~mask; else *b ^= mask;
            px += n;
        }
    }
}

static uint32_t k_blit(void)
{
    uint32_t sum = 0;
    for (int r = 0; r < 4; r++) {
        fill_rect(0, 0, 160, 160, 1);
        fill_rect((int16_t)(3 + r), 5, 101, 60, 0);
        fill_rect(17, (int16_t)(20 + r), 77, 90, 2);
        fill_rect(60, 100, 99, 59, 2);
        for (int i = 0; i < 20 * 160; i += 37) sum += fb[i];
    }
    return sum;
}

/* --- arithmetic: 16x16 MULU/MULS and 32/16 DIVU, as in fixed-point UI code */
static uint32_t k_math(void)
{
    uint32_t sum = 0;
    for (uint16_t i = 1; i < 2000; i++) {
        uint32_t p = (uint32_t)(uint16_t)(i + 77) * (uint16_t)(i ^ 0x5A5A);
        int32_t  q = (int32_t)(int16_t)(i - 1000) * (int16_t)(i * 3);
        uint16_t d = (uint16_t)((p >> 4) & 0x7FFF);
        uint32_t qr;   /* DIVU.W: low word quotient, high word remainder */
        __asm__("divu.w %2,%0" : "=d"(qr) : "0"(p >> 2), "d"(i) : "cc");
        uint16_t quo = (uint16_t)qr;
        sum += p + (uint32_t)q + d + quo;
    }
    return sum;
}

/* --- calls: recursion with full frames (LINK/UNLK, MOVEM saves) -------- */
static int16_t fib(int16_t n) { return n < 2 ? n : (int16_t)(fib((int16_t)(n - 1)) + fib((int16_t)(n - 2))); }

static uint32_t k_calls(void) { return (uint32_t)(uint16_t)fib(17); }

/* --- event loop: switch-dispatched bytecode (jump tables) -------------- */
static uint32_t k_events(void)
{
    static const uint8_t prog[] = { 0, 3, 1, 4, 2, 5, 1, 6, 7, 2, 0, 3, 6, 5, 4, 7 };
    uint32_t acc = 1, sum = 0;
    for (int n = 0; n < 6000; n++) {
        switch (prog[n & 15]) {
        case 0: acc += 3; break;
        case 1: acc ^= (uint32_t)n; break;
        case 2: acc = (acc << 1) | (acc >> 31); break;
        case 3: sum += acc & 0xFF; break;
        case 4: acc -= (uint32_t)(n & 7); break;
        case 5: if (acc & 4) sum++; break;
        case 6: acc |= 0x100; break;
        case 7: sum += (acc >> 8) & 3; break;
        }
    }
    return sum + acc;
}

uint32_t bench_main(void)
{
    uint32_t sum = 0;
    for (int r = 0; r < ROUNDS; r++) {
        sum = (sum << 3) ^ (sum >> 29) ^ k_memory();
        sum = (sum << 3) ^ (sum >> 29) ^ k_strings();
        sum = (sum << 3) ^ (sum >> 29) ^ k_sort();
        sum = (sum << 3) ^ (sum >> 29) ^ k_list();
        sum = (sum << 3) ^ (sum >> 29) ^ k_crc();
        sum = (sum << 3) ^ (sum >> 29) ^ k_blit();
        sum = (sum << 3) ^ (sum >> 29) ^ k_math();
        sum = (sum << 3) ^ (sum >> 29) ^ k_calls();
        sum = (sum << 3) ^ (sum >> 29) ^ k_events();
    }
    return sum;
}
