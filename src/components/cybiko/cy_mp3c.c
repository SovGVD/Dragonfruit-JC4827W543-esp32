/* The Cybiko Classic's MP3 player: the transport, then the player (see
 * cy_mp3c.h and docs/cybiko-classic-modules.md). */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "cy_mp3c.h"

void cy_mp3c_logf(cy_mp3c_t *p, const char *fmt, ...)
{
    if (!p->log) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    p->log(p->user, line);
}

void cy_mp3c_init(cy_mp3c_t *p, cy_mp3c_handler_t handler, void (*log)(void *, const char *), void *user)
{
    memset(p, 0, sizeof *p);
    p->present = true;
    p->ready = 1;
    p->handler = handler;
    p->log = log;
    p->user = user;
    p->song = CY_MP3C_NONE;
    p->volume = 30;
}

/* The player is ready for the next chunk of `left` bytes: it says so on P2.1 */
static void next_chunk(cy_mp3c_t *p, uint16_t left)
{
    p->chunk_left = left;
    p->ready ^= 1;
}

static uint16_t chunk(uint16_t left) { return left < CY_MP3C_CHUNK ? left : CY_MP3C_CHUNK; }

void cy_mp3c_select_pin(cy_mp3c_t *p, bool high)
{
    if (high == !p->selected) return;
    p->selected = !high;
    if (high) {                                       /* let go: idle */
        if (p->state != MP3C_CMD_HDR || p->pos)
            cy_mp3c_logf(p, "mp3 player: let go in the middle of a command (state %d, %u bytes)", p->state, p->pos);
        p->state = MP3C_CMD_HDR;
        p->ready = 1;
        return;
    }
    p->state = MP3C_CMD_HDR;                          /* selected: ready for a command */
    p->pos = 0;
    p->follow = 0;
    p->second = false;
    next_chunk(p, 4);
}

static void hex(char *out, size_t cap, const uint8_t *b, uint16_t n)
{
    size_t o = 0;
    for (uint16_t i = 0; i < n && o + 3 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%02x", b[i]);
    if (o < cap) out[o] = 0;
}

static void answer(cy_mp3c_t *p)
{
    uint16_t out_len = 0;
    uint8_t err = p->handler ? p->handler(p, p->cmd, p->data, p->len, p->ans, &out_len) : 0;
    if (out_len > CY_MP3C_MAX) out_len = CY_MP3C_MAX;
    p->ans_len = out_len;
    uint16_t status = (uint16_t)(err << 8 | (p->cmd & 0xff));
    p->ans_hdr[0] = (uint8_t)(out_len >> 8);
    p->ans_hdr[1] = (uint8_t)out_len;
    p->ans_hdr[2] = (uint8_t)(status >> 8);
    p->ans_hdr[3] = (uint8_t)status;
    p->commands++;
    if ((p->cmd & 0xff) != 0x31) p->software = true;
    p->state = MP3C_ANS_HDR;
    p->pos = 0;
    next_chunk(p, 4);
}

void cy_mp3c_arm(cy_mp3c_t *p) { p->armed = true; }

/* An answer is out: the next command, or the data the last one said follows
 * (cy_mp3c_t.follow) */
static void answered(cy_mp3c_t *p)
{
    p->pos = 0;
    if (p->follow) {
        p->len = p->follow > CY_MP3C_MAX ? CY_MP3C_MAX : p->follow;
        p->follow = 0;
        p->second = true;
        p->state = MP3C_CMD_DATA;
        next_chunk(p, chunk(p->len));
        return;
    }
    p->second = false;
    p->state = MP3C_CMD_HDR;
    next_chunk(p, 4);
}

uint8_t cy_mp3c_spi(cy_mp3c_t *p, uint8_t in, int mode)
{
    if (!p->present || !p->selected) return 0xff;
    bool reading = p->state == MP3C_ANS_HDR || p->state == MP3C_ANS_DATA;
    /* MP3.dl moves each chunk with one DTC transfer, the SCI switched on
     * for it and off after; the clock runs on for a byte or two past the
     * chunk (the DTC count is one more than the chunk when sending; the
     * receiver clocks on until it overruns). The player takes a chunk only
     * after the SCI was switched on again. CyOS's own check at start-up
     * moves byte by byte with both on (mode 3). */
    if (mode != 3 && (!p->armed || (mode == 2) == reading)) {
        p->dropped++;
        return 0xff;
    }
    uint8_t out = 0xff;
    switch (p->state) {
    case MP3C_CMD_HDR:
        p->hdr[p->pos++] = in;
        if (p->pos < 4) break;
        p->len = (uint16_t)(p->hdr[0] << 8 | p->hdr[1]);
        p->cmd = (uint16_t)(p->hdr[2] << 8 | p->hdr[3]);
        if (p->len > CY_MP3C_MAX) {
            cy_mp3c_logf(p, "mp3 player: command %04x with %u bytes, more than %u; cut", p->cmd, p->len, CY_MP3C_MAX);
            p->len = CY_MP3C_MAX;
        }
        p->pos = 0;
        p->armed = false;
        if (!p->len) { answer(p); break; }
        p->state = MP3C_CMD_DATA;
        next_chunk(p, chunk(p->len));
        break;
    case MP3C_CMD_DATA:
        p->data[p->pos++] = in;
        if (--p->chunk_left) break;
        p->armed = false;
        if (p->pos >= p->len) answer(p);
        else next_chunk(p, chunk((uint16_t)(p->len - p->pos)));
        break;
    case MP3C_ANS_HDR:
        out = p->ans_hdr[p->pos++];
        if (p->pos < 4) break;
        p->pos = 0;
        p->armed = false;
        if (p->ans_len) {
            p->state = MP3C_ANS_DATA;
            next_chunk(p, chunk(p->ans_len));
        } else
            answered(p);
        break;
    case MP3C_ANS_DATA:
        out = p->ans[p->pos++];
        if (--p->chunk_left) break;
        p->armed = false;
        if (p->pos < p->ans_len) {
            next_chunk(p, chunk((uint16_t)(p->ans_len - p->pos)));
        } else
            answered(p);
        break;
    }
    return out;
}

/* for the log: a command's data in hex */
void cy_mp3c_hex(char *out, size_t cap, const uint8_t *b, uint16_t n) { hex(out, cap, b, n); }

/* ---- the player ----
 *
 * What each command does is read from MP3.app 1.3.13 and MP3.dl 1.1.7 (the
 * player's own firmware was never dumped); the ones marked "guess" are
 * inferred from how the app uses them. Numbers are big-endian, as the H8S
 * keeps them, unless said otherwise. */

#define MAX_FILES 512

struct cy_mp3c_file {
    char name[64];
    uint32_t size;
    uint16_t time, date;       /* DOS */
};

static void put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void put32(uint8_t *b, uint32_t v) { put16(b, v >> 16); put16(b + 2, v); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

void cy_mp3c_set_root(cy_mp3c_t *p, const char *root)
{
    snprintf(p->root, sizeof p->root, "%s", root ? root : "");
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const struct cy_mp3c_file *)a)->name, ((const struct cy_mp3c_file *)b)->name);
}

/* The card's files, read again from the folder */
static void scan(cy_mp3c_t *p)
{
    if (!p->files) p->files = calloc(MAX_FILES, sizeof *p->files);
    p->nfiles = 0;
    p->no_card = true;
    if (!p->files || !p->root[0]) return;
    DIR *d = opendir(p->root);
    if (!d) { cy_mp3c_logf(p, "mp3 player: no card folder %s: no SmartMedia card in the player", p->root); return; }
    p->no_card = false;
    struct dirent *e;
    char path[sizeof p->root + 70];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= sizeof p->files[0].name) {
            cy_mp3c_logf(p, "mp3 player: %s: name too long, left out", e->d_name);
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", p->root, e->d_name);
        struct stat st;
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        if (p->nfiles == MAX_FILES) { cy_mp3c_logf(p, "mp3 player: more than %d files, the rest left out", MAX_FILES); break; }
        struct cy_mp3c_file *f = &p->files[p->nfiles++];
        snprintf(f->name, sizeof f->name, "%s", e->d_name);
        f->size = (uint32_t)st.st_size;
        struct tm tm;
        time_t t = st.st_mtime;
        if (localtime_r(&t, &tm) && tm.tm_year >= 80) {
            f->date = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
            f->time = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
        } else
            f->date = f->time = 0;
    }
    closedir(d);
    qsort(p->files, p->nfiles, sizeof *p->files, by_name);
}

/* File ids are 1 + the place in the listing; 0 and CY_MP3C_NONE are none */
static struct cy_mp3c_file *file(cy_mp3c_t *p, uint32_t id)
{
    if (!p->files) scan(p);
    return id >= 1 && id <= p->nfiles ? &p->files[id - 1] : NULL;
}

static FILE *open_file(cy_mp3c_t *p, const struct cy_mp3c_file *f)
{
    char path[sizeof p->root + 70];
    snprintf(path, sizeof path, "%s/%s", p->root, f->name);
    FILE *fp = fopen(path, "rb");
    if (!fp) cy_mp3c_logf(p, "mp3 player: cannot open %s", path);
    return fp;
}

static const uint16_t kbps_table[2][16] = {
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },   /* MPEG 1 layer 3 */
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },       /* MPEG 2 layer 3 */
};

/* Command 11: what the app shows for a song: the file's size (u32), its bit
 * rate in kbps (u16), stereo (u16, 0 mono), then ID3v1's title, artist,
 * album (30 bytes each), year (4), comment (30) and genre (1). MP3.app copies
 * 0x168 bytes from where it reads the answer, but anything past 0xb0 bytes
 * lands on its own variables (its play mode among them), so the player must
 * answer with less. */
#define SONG_INFO 0x85
static uint16_t song_info(cy_mp3c_t *p, uint32_t id, uint8_t *out)
{
    memset(out, 0, SONG_INFO);
    struct cy_mp3c_file *f = file(p, id);
    if (!f) return 0;
    put32(out, f->size);
    put16(out + 4, 128);
    put16(out + 6, 1);
    FILE *fp = open_file(p, f);
    if (!fp) return SONG_INFO;
    uint8_t b[10];
    long at = 0;
    if (fread(b, 1, 10, fp) == 10 && !memcmp(b, "ID3", 3))   /* skip an ID3v2 tag */
        at = 10 + ((long)(b[6] & 0x7f) << 21 | (long)(b[7] & 0x7f) << 14 | (b[8] & 0x7f) << 7 | (b[9] & 0x7f));
    fseek(fp, at, SEEK_SET);
    uint8_t buf[2048];
    size_t n = fread(buf, 1, sizeof buf, fp);
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xff || (buf[i + 1] & 0xe6) != 0xe2) continue;     /* sync, layer 3 */
        unsigned k = kbps_table[(buf[i + 1] & 0x08) ? 0 : 1][buf[i + 2] >> 4];
        if (!k) continue;
        put16(out + 4, k);
        put16(out + 6, (buf[i + 3] >> 6) != 3);
        break;
    }
    uint8_t tag[128];
    if (f->size >= 128 && !fseek(fp, -128, SEEK_END) && fread(tag, 1, 128, fp) == 128 && !memcmp(tag, "TAG", 3)) {
        memcpy(out + 8, tag + 3, 30);        /* title */
        memcpy(out + 0x26, tag + 33, 30);    /* artist */
        memcpy(out + 0x44, tag + 63, 30);    /* album */
        memcpy(out + 0x62, tag + 93, 4);     /* year */
        memcpy(out + 0x66, tag + 97, 30);    /* comment */
        out[0x84] = tag[127];                /* genre */
    } else {
        /* no tag: the file's name, without .mp3, as the title */
        size_t l = strlen(f->name);
        if (l > 4 && !strcasecmp(f->name + l - 4, ".mp3")) l -= 4;
        memcpy(out + 8, f->name, l < 30 ? l : 30);
    }
    fclose(fp);
    return SONG_INFO;
}

static void stop(cy_mp3c_t *p)
{
    if (p->fp) fclose(p->fp);
    p->fp = NULL;
    p->play_state = CY_MP3C_STOPPED;
    p->play_pos = 0;
}

static bool is_mp3(const struct cy_mp3c_file *f)
{
    size_t l = strlen(f->name);
    return l > 4 && !strcasecmp(f->name + l - 4, ".mp3");
}

static void play(cy_mp3c_t *p, uint32_t id, uint32_t pos)
{
    stop(p);
    struct cy_mp3c_file *f = file(p, id);
    if (!f) { cy_mp3c_logf(p, "mp3 player: play %u: no such file", (unsigned)id); return; }
    p->fp = open_file(p, f);
    if (!p->fp) return;
    if (pos > f->size) pos = f->size;
    fseek(p->fp, (long)pos, SEEK_SET);
    p->song = id;
    p->song_size = f->size;
    p->play_pos = pos;
    p->play_state = CY_MP3C_PLAYING;
    cy_mp3c_logf(p, "mp3 player: playing %s from %u", f->name, (unsigned)pos);
}

/* the next (or previous) MP3 file on the card, wrapping round */
static uint32_t step(cy_mp3c_t *p, int dir)
{
    if (!p->files) scan(p);
    if (p->shuffle && dir > 0 && (p->nlist || p->nfiles)) {   /* a song at random */
        uint32_t n = p->nlist ? p->nlist : p->nfiles, r = (uint32_t)rand() % n;
        uint32_t id = p->nlist ? p->list[r] : r + 1;
        if (p->nlist || is_mp3(&p->files[id - 1])) return id;
    }
    if (p->nlist) {                                   /* the play list */
        uint16_t i = 0;
        while (i < p->nlist && p->list[i] != p->song) i++;
        if (i == p->nlist) return p->list[dir > 0 ? 0 : p->nlist - 1];
        return p->list[dir > 0 ? (i + 1) % p->nlist : (i + p->nlist - 1) % p->nlist];
    }
    if (!p->nfiles) return CY_MP3C_NONE;
    uint32_t id = p->song == CY_MP3C_NONE || !p->song || p->song > p->nfiles ? (dir > 0 ? 0 : 1) : p->song;
    for (uint16_t i = 0; i < p->nfiles; i++) {
        id = dir > 0 ? id % p->nfiles + 1 : (id + p->nfiles - 2) % p->nfiles + 1;
        if (is_mp3(&p->files[id - 1])) return id;
    }
    return CY_MP3C_NONE;
}

size_t cy_mp3c_stream(cy_mp3c_t *p, uint8_t *buf, size_t max)
{
    if (!p || p->play_state != CY_MP3C_PLAYING || !p->fp) return 0;
    size_t n = fread(buf, 1, max, p->fp);
    p->play_pos += (uint32_t)n;
    if (n < max) {                                    /* the end: the next song */
        uint32_t now = p->song, next = step(p, 1);
        p->song = CY_MP3C_NONE;
        uint32_t first = step(p, 1);
        p->song = now;
        if (next == CY_MP3C_NONE || (!p->repeat && !p->shuffle && next == first)) {     /* round to the first: done */
            stop(p);
            cy_mp3c_logf(p, "mp3 player: end of the songs");
        } else
            play(p, next, 0);
    }
    return n;
}

uint8_t cy_mp3c_gain(const cy_mp3c_t *p)
{
    if (!p || !p->volume) return 0;
    return (uint8_t)(0x6a - (CY_MP3C_VOLUME_MAX - p->volume));   /* 1 dB a step */
}

void cy_mp3c_button(cy_mp3c_t *p, int button)
{
    switch (button) {
    case CY_MP3C_BTN_PLAY:
        if (p->play_state == CY_MP3C_PLAYING) p->play_state = CY_MP3C_PAUSED;
        else if (p->play_state == CY_MP3C_PAUSED && p->fp) p->play_state = CY_MP3C_PLAYING;
        else play(p, file(p, p->song) && is_mp3(file(p, p->song)) ? p->song : step(p, 1), 0);
        break;
    case CY_MP3C_BTN_STOP: stop(p); break;
    case CY_MP3C_BTN_NEXT: case CY_MP3C_BTN_PREV: {
        uint32_t id = step(p, button == CY_MP3C_BTN_NEXT ? 1 : -1);
        if (id == CY_MP3C_NONE) break;
        if (p->play_state == CY_MP3C_PLAYING) play(p, id, 0);
        else { stop(p); p->song = id; }
        break;
    }
    case CY_MP3C_BTN_VOL_UP: if (p->volume < CY_MP3C_VOLUME_MAX) p->volume += 2; break;
    case CY_MP3C_BTN_VOL_DOWN: p->volume = p->volume > 2 ? p->volume - 2 : 0; break;
    }
    cy_mp3c_logf(p, "mp3 player: button %d: state %u, song %d, volume %u", button, p->play_state, (int)p->song, p->volume);
}

void cy_mp3c_free(cy_mp3c_t *p)
{
    stop(p);
    free(p->files);
    p->files = NULL;
}

uint8_t cy_mp3c_player(cy_mp3c_t *p, uint16_t cmd, const uint8_t *data, uint16_t len, uint8_t *out, uint16_t *out_len)
{
    char h[100];
    *out_len = 0;
    if (cmd != 0x17) {                                /* the app asks for 17 several times a second */
        cy_mp3c_hex(h, sizeof h, data, len > 40 ? 40 : len);
        cy_mp3c_logf(p, "mp3 player: command %04x, %u bytes %s%s", cmd, len, h, len > 40 ? "..." : "");
    }
    uint32_t a = len >= 4 ? get32(data) : 0;
    switch (cmd & 0xff) {
    case 0x31:                                        /* versions: hardware, software */
        put16(out, 0x0001);
        put16(out + 2, 0x0103);
        *out_len = 4;
        return 0;
    case 0x3b: {                                      /* the card's size, 20 bytes: u32 total and u32 */
        if (!p->files) scan(p);                       /* free, in 512-byte blocks, @4 and @8 (the File */
        if (p->no_card) return 0x08;                  /* Manager's "free/total" on the SMC line); the */
        uint32_t used = 0, total = 64u << 11;         /* rest is not known (0). A 64 MB card, the */
        for (uint16_t i = 0; i < p->nfiles; i++)      /* biggest SmartMedia the player takes, unless */
            used += (p->files[i].size + 511) / 512;   /* the songs need more */
        if (used > total) total = used;
        memset(out, 0, 20);
        put32(out + 4, total);
        put32(out + 8, total - used);
        *out_len = 20;
        return 0;
    }
    case 0x17:                                        /* status, 20 bytes */
        if (!p->files) scan(p);
        if (p->no_card) return 0x08;                  /* MP3.app: "There is no SMC in your cartridge" */
        memset(out, 0, 20);
        put16(out, p->play_state);
        out[2] = (uint8_t)p->balance;
        out[3] = p->volume;
        out[10] = p->repeat;
        out[11] = p->shuffle;
        put32(out + 12, p->fp || p->song ? p->song : CY_MP3C_NONE);
        put32(out + 16, p->play_pos / 512);                /* where it plays, in 512-byte blocks */
        *out_len = 20;
        return 0;
    case 0x11:                                        /* a song's details */
        *out_len = song_info(p, a, out);
        return *out_len ? 0 : 1;
    case 0x12:                                        /* {u32 song}: pause or go on if it is the one */
        if (!file(p, a)) return 1;                    /* playing, else play it from the start (the File */
        if (p->play_state == CY_MP3C_PLAYING && a == p->song) p->play_state = CY_MP3C_PAUSED;   /* Manager's Play: 1f, 14, 12) */
        else if (p->play_state == CY_MP3C_PAUSED && a == p->song && p->fp) p->play_state = CY_MP3C_PLAYING;
        else play(p, a, 0);
        return 0;
    case 0x13: case 0x43:                             /* play a song from a block: {u32 id, u16 block} */
        play(p, a, len >= 6 ? (uint32_t)(data[4] << 8 | data[5]) * 512u : 0);
        return 0;
    case 0x14:                                        /* stop */
        stop(p);
        return 0;
    case 0x15:                                        /* play: the play list, or every song */
        cy_mp3c_button(p, CY_MP3C_BTN_PLAY);
        return 0;
    case 0x16:                                        /* {u32 block}: go to that 512-byte block of the song */
        if (p->fp && a * 512u <= p->song_size) {
            fseek(p->fp, (long)(a * 512u), SEEK_SET);
            p->play_pos = a * 512u;
        }
        return 0;
    case 0x18:                                        /* {u8} volume, 0 .. 46 */
        if (len >= 1) p->volume = data[0] > CY_MP3C_VOLUME_MAX ? CY_MP3C_VOLUME_MAX : data[0];
        return 0;
    case 0x19:                                        /* {s8} balance (guess) */
        if (len >= 1) p->balance = (int8_t)data[0];
        return 0;
    case 0x1a:                                        /* {u8, u8}: the equalizer (guess; not done) */
        if (len >= 2) { p->bass = data[0]; p->treble = data[1]; }
        return 0;
    case 0x1c:                                        /* {u8 repeat, u8 shuffle}; ff keeps one */
        if (len >= 2) {
            if (data[0] != 0xff) p->repeat = data[0];
            if (data[1] != 0xff) p->shuffle = data[1];
        }
        return 0;
    case 0x1e:                                        /* {u8}: 0 the previous song, 1 the next */
        if (len >= 1 && data[0] <= 1) { cy_mp3c_button(p, data[0] ? CY_MP3C_BTN_NEXT : CY_MP3C_BTN_PREV); return 0; }
        break;
    case 0x10: {                                      /* the play list (high byte 1: MP3's), {u16 max}: u32 ids */
        uint16_t max = len >= 2 ? (uint16_t)(data[0] << 8 | data[1]) : 0;
        uint16_t n = p->nlist < max ? p->nlist : max;
        for (uint16_t i = 0; i < n; i++) put32(out + 4 * i, p->list[i]);
        *out_len = (uint16_t)(4 * n);
        return 0;
    }
    case 0x1d:                                        /* a new play list: {u16 count}, then count u32 ids */
        if (!p->second) {
            uint16_t n = len >= 2 ? (uint16_t)(data[0] << 8 | data[1]) : 0;
            if (n > CY_MP3C_LIST) cy_mp3c_logf(p, "mp3 player: play list of %u songs, only %u kept", n, CY_MP3C_LIST);
            p->follow = (uint16_t)(4 * n);
            p->nlist = 0;
            return 0;
        }
        for (uint16_t i = 0; i + 4 <= len; i += 4)
            if (p->nlist < CY_MP3C_LIST) p->list[p->nlist++] = get32(data + i);
        cy_mp3c_logf(p, "mp3 player: play list of %u songs", p->nlist);
        return 0;
    case 0x1f:                                        /* mode: 0 MP3 */
        return 0;
    case 0x38: {                                      /* list a folder: {u16 max, u16, u32 from, u32 folder} */
        if (len < 12) return 1;
        uint32_t dir = get32(data + 8);
        uint16_t kind = (uint16_t)(data[2] << 8 | data[3]);   /* 1 from MP3.app: songs only (guess) */
        if (dir != 0xfffffffeu && dir != 0) {
            cy_mp3c_logf(p, "mp3 player: folder %08x: only the card's top folder is known", (unsigned)get32(data + 8));
            return 0;
        }
        if (!get32(data + 4)) scan(p);
        if (kind > 1) cy_mp3c_logf(p, "mp3 player: list of kind %u: not known, all files listed", kind);
        uint32_t from = get32(data + 4), max = (uint32_t)(data[0] << 8 | data[1]);
        uint16_t n = 0;
        uint32_t seen = 0;
        for (uint32_t i = 0; i < p->nfiles && n < max && (n + 1u) * 4u <= CY_MP3C_MAX; i++) {
            if (kind == 1 && !is_mp3(&p->files[i])) continue;
            if (seen++ < from) continue;
            put32(out + 4 * n++, i + 1);
        }
        *out_len = (uint16_t)(4 * n);
        return 0;
    }
    case 0x39: {                                      /* a file's details */
        struct cy_mp3c_file *f = file(p, a);
        if (!f) return 1;
        put32(out, f->size);
        out[4] = (uint8_t)f->time; out[5] = (uint8_t)(f->time >> 8);   /* little-endian */
        out[6] = (uint8_t)f->date; out[7] = (uint8_t)(f->date >> 8);
        out[8] = 0x20;                                /* archive */
        size_t l = strlen(f->name);
        memcpy(out + 9, f->name, l);
        *out_len = (uint16_t)(9 + l);
        return 0;
    }
    }
    p->unknown++;
    cy_mp3c_logf(p, "mp3 player: command %04x is not known; answered with nothing", cmd);
    return 0;
}
