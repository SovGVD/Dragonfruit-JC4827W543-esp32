#include "ram_state.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define HEADER_BYTES 32
#define BLOCK_BYTES 1024
static const uint8_t magic[8] = { 'D','F','R','A','M','0','0','1' };
static uint32_t crc_table[256];
static bool crc_ready;

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_ready) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int bit = 0; bit < 8; bit++) v = (v >> 1) ^ ((v & 1) ? 0xEDB88320u : 0);
            crc_table[i] = v;
        }
        crc_ready = true;
    }
    while (n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return crc;
}

uint32_t ram_state_crc(const void *p, size_t n)
{
    return crc_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static bool path_for(char *p, size_t cap, const char *prefix, int slot)
{
    int n = snprintf(p, cap, "%s.%d", prefix, slot);
    return n > 0 && (size_t)n < cap;
}

void ram_state_init(ram_state_t *s, uint32_t rom_crc, uint32_t ram_bytes)
{
    memset(s, 0, sizeof(*s));
    s->rom_crc = rom_crc; s->ram_bytes = ram_bytes; s->active_slot = -1;
}

static void progress(ram_state_t *s, ram_state_phase_t phase, uint32_t done, uint32_t total)
{
    if (s->progress && (!done || done == total || !(done % 65536)))
        s->progress(phase, done, total);
}

static ram_state_result_t validate(ram_state_t *s, const char *path, uint32_t *gen,
                                   uint32_t *sum, ram_state_phase_t phase)
{
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? RAM_STATE_MISSING : RAM_STATE_IO;
    uint8_t h[HEADER_BYTES], block[BLOCK_BYTES];
    ram_state_result_t r = RAM_STATE_INVALID;
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) goto done;
    if (memcmp(h, magic, sizeof(magic)) || get32(h + 8) != 1 ||
        get32(h + 12) != s->rom_crc || get32(h + 16) != s->ram_bytes ||
        get32(h + 28) != ram_state_crc(h, 28)) goto done;
    uint32_t crc = 0xFFFFFFFFu;
    progress(s, phase, 0, s->ram_bytes);
    for (size_t off = 0; off < s->ram_bytes;) {
        size_t n = s->ram_bytes - off;
        if (n > sizeof(block)) n = sizeof(block);
        if (fread(block, 1, n, f) != n) goto done;
        crc = crc_update(crc, block, n);
        off += n;
        progress(s, phase, (uint32_t)off, s->ram_bytes);
        if (s->yield && !(off % 65536)) s->yield();
    }
    if ((crc ^ 0xFFFFFFFFu) != get32(h + 24) || fgetc(f) != EOF || ferror(f)) goto done;
    *gen = get32(h + 20); *sum = get32(h + 24);
    r = RAM_STATE_OK;
done:
    if (ferror(f)) r = RAM_STATE_IO;
    if (fclose(f)) r = RAM_STATE_IO;
    return r;
}

ram_state_result_t ram_state_load(ram_state_t *s, const char *prefix, void *ram)
{
    char path[256];
    uint32_t gen[2] = {0}, sum[2] = {0};
    ram_state_result_t r[2];
    s->active_slot = -1;
    for (int i = 0; i < 2; i++) {
        if (!path_for(path, sizeof(path), prefix, i)) return RAM_STATE_IO;
        r[i] = validate(s, path, &gen[i], &sum[i],
                        i ? RAM_STATE_CHECK_SLOT_1 : RAM_STATE_CHECK_SLOT_0);
    }
    int slot = r[0] == RAM_STATE_OK ? 0 : -1;
    if (r[1] == RAM_STATE_OK && (slot < 0 || (int32_t)(gen[1] - gen[0]) > 0)) slot = 1;
    if (slot < 0) {
        if (r[0] == RAM_STATE_IO || r[1] == RAM_STATE_IO) return RAM_STATE_IO;
        return r[0] == RAM_STATE_MISSING && r[1] == RAM_STATE_MISSING
                 ? RAM_STATE_MISSING : RAM_STATE_INVALID;
    }
    if (!path_for(path, sizeof(path), prefix, slot)) return RAM_STATE_IO;
    FILE *f = fopen(path, "rb");
    if (!f) return RAM_STATE_IO;
    bool ok = fseek(f, HEADER_BYTES, SEEK_SET) == 0;
    uint32_t crc = 0xFFFFFFFFu;
    progress(s, RAM_STATE_READING, 0, s->ram_bytes);
    for (size_t off = 0; ok && off < s->ram_bytes;) {
        size_t n = s->ram_bytes - off;
        if (n > BLOCK_BYTES) n = BLOCK_BYTES;
        uint8_t *p = (uint8_t *)ram + off;
        ok = fread(p, 1, n, f) == n;
        if (ok) crc = crc_update(crc, p, n);
        off += n;
        if (ok) progress(s, RAM_STATE_READING, (uint32_t)off, s->ram_bytes);
        if (s->yield && !(off % 65536)) s->yield();
    }
    if (fclose(f)) ok = false;
    if (!ok || (crc ^ 0xFFFFFFFFu) != sum[slot]) return RAM_STATE_IO;
    s->active_slot = slot; s->generation = gen[slot]; s->data_crc = sum[slot];
    return RAM_STATE_OK;
}

ram_state_result_t ram_state_save(ram_state_t *s, const char *prefix, const void *ram)
{
    uint32_t crc = 0xFFFFFFFFu;
    progress(s, RAM_STATE_HASHING, 0, s->ram_bytes);
    for (size_t off = 0; off < s->ram_bytes;) {
        size_t n = s->ram_bytes - off;
        if (n > 65536) n = 65536;
        crc = crc_update(crc, (const uint8_t *)ram + off, n);
        off += n;
        progress(s, RAM_STATE_HASHING, (uint32_t)off, s->ram_bytes);
        if (s->yield) s->yield();
    }
    crc ^= 0xFFFFFFFFu;
    if (s->active_slot >= 0 && crc == s->data_crc) return RAM_STATE_UNCHANGED;
    int slot = s->active_slot == 0 ? 1 : 0;
    char path[256];
    if (!path_for(path, sizeof(path), prefix, slot)) return RAM_STATE_IO;
    FILE *f = fopen(path, "wb");
    if (!f) return RAM_STATE_IO;
    progress(s, RAM_STATE_WRITING, 0, s->ram_bytes);
    uint8_t h[HEADER_BYTES] = {0};  /* invalid until the payload is complete */
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    for (size_t off = 0; ok && off < s->ram_bytes;) {
        size_t n = s->ram_bytes - off;
        if (n > BLOCK_BYTES) n = BLOCK_BYTES;
        ok = fwrite((const uint8_t *)ram + off, 1, n, f) == n;
        off += n;
        if (ok) progress(s, RAM_STATE_WRITING, (uint32_t)off, s->ram_bytes);
        if (s->yield && !(off % 65536)) s->yield();
    }
    memcpy(h, magic, sizeof(magic));
    put32(h + 8, 1); put32(h + 12, s->rom_crc); put32(h + 16, s->ram_bytes);
    put32(h + 20, s->generation + 1); put32(h + 24, crc); put32(h + 28, ram_state_crc(h, 28));
    if (ok) ok = !fseek(f, 0, SEEK_SET) && fwrite(h, 1, sizeof(h), f) == sizeof(h);
    if (ok) progress(s, RAM_STATE_SYNCING, 0, 1);
    if (ok) ok = !fflush(f);
    if (ok) {
#ifdef _WIN32
        ok = !_commit(_fileno(f));
#else
        ok = !fsync(fileno(f));
#endif
    }
    if (fclose(f)) ok = false;
    if (!ok) return RAM_STATE_IO;
    s->active_slot = slot; s->generation++; s->data_crc = crc;
    progress(s, RAM_STATE_SYNCING, 1, 1);
    return RAM_STATE_OK;
}

const char *ram_state_result_str(ram_state_result_t r)
{
    switch (r) {
    case RAM_STATE_OK: return "saved/restored";
    case RAM_STATE_UNCHANGED: return "unchanged";
    case RAM_STATE_MISSING: return "no saved RAM";
    case RAM_STATE_INVALID: return "incompatible/corrupt RAM";
    default: return "SD I/O error";
    }
}
