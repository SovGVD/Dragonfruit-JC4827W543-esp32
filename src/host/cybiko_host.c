/* Headless Cybiko Xtreme runner for checking the machine model on a PC with
 * the real ROM and flash dumps:
 *
 *   cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin [options]
 *
 *   --seconds S        emulated seconds to run (default 20)
 *   --ramdisk FILE     load the RAM disk from FILE and save it back
 *   --disk-size KB     move the RAM disk to 800000 and make it KB kilobytes
 *                      (a multiple of 64), as the firmware does
 *   --install FILE     copy FILE onto the RAM disk before starting (repeatable)
 *   --ls               list the files on the RAM disk at the end
 *   --cart             plug in the MP3 player cartridge (--cart-log: and log it)
 *   --card IMG         ... with an SD card (a disk image, 512-byte blocks)
 *   --wav FILE         write what the cartridge plays as a 32 kHz mono WAV file
 *   --screenshot FILE  write the final screen as a PGM image
 *   --frames DIR       write DIR/frame_NNNN.pgm whenever the screen changes
 *                      (checked every 100 ms of emulated time)
 *   --press KEY@MS[:D] hold KEY for D ms (default 200) from MS milliseconds; KEY is
 *                      a letter, f1-f7, enter, esc, space, up/down/left/right,
 *                      select, del, tab, shift, fn, help, onoff
 *
 * Text on SCI1 (where the boot loader and CyOS print) and SCI2 is copied to
 * stdout; bytes on SCI0 go to stderr as hex. A summary with the speed and anything the model
 * saw but does not cover is printed at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>
#include "cybiko.h"
#include "cy_mp3.h"

static const struct { const char *name; int key; } key_names[] = {
    { "a", CYK_A }, { "b", CYK_B }, { "c", CYK_C }, { "d", CYK_D }, { "e", CYK_E },
    { "f", CYK_F }, { "g", CYK_G }, { "h", CYK_H }, { "i", CYK_I }, { "j", CYK_J },
    { "k", CYK_K }, { "l", CYK_L }, { "m", CYK_M }, { "n", CYK_N }, { "o", CYK_O },
    { "p", CYK_P }, { "q", CYK_Q }, { "r", CYK_R }, { "s", CYK_S }, { "t", CYK_T },
    { "u", CYK_U }, { "v", CYK_V }, { "w", CYK_W }, { "x", CYK_X }, { "y", CYK_Y },
    { "z", CYK_Z }, { "f1", CYK_F1 }, { "f2", CYK_F2 }, { "f3", CYK_F3 },
    { "f4", CYK_F4 }, { "f5", CYK_F5 }, { "f6", CYK_F6 }, { "f7", CYK_F7 },
    { "enter", CYK_ENTER }, { "esc", CYK_ESC }, { "space", CYK_SPACE },
    { "up", CYK_UP }, { "down", CYK_DOWN }, { "left", CYK_LEFT }, { "right", CYK_RIGHT },
    { "select", CYK_SELECT }, { "del", CYK_DEL }, { "tab", CYK_TAB }, { "as", CYK_AS },
    { "shift", CYK_SHIFT }, { "fn", CYK_FN }, { "help", CYK_HELP }, { "dot", CYK_DOT },
    { "onoff", CYK_ONOFF },
};

#define MAX_INSTALL 32
static const char *installs[MAX_INSTALL];
static int ninstalls;

static bool print_file(void *user, const cy_disk_file_t *f)
{
    (void)user;
    fprintf(stderr, "  %7u  %s\n", (unsigned)f->size, f->name);
    return true;
}

#define MAX_PRESSES 64
/* who: 0 first machine, 1 second (--pair, KEY written b:KEY), 2 both */
static struct { int key, who; uint64_t at, until; bool down; } presses[MAX_PRESSES];
static int npresses;

static int hex_port = -1;           /* --hex-port N: show that port's bytes as hex */
static cybiko_t *the_machine;

static void on_tx(void *user, int port, uint8_t b)
{
    (void)user;
    if (port == hex_port) {
        fprintf(stderr, "[%.3f sci%d %02x]\n", (double)the_machine->cpu.cycles / CY_CLOCK_HZ, port, b);
        return;
    }
    if (port == 1) { putchar(b); fflush(stdout); }
    else if (port == 0) fprintf(stderr, "[sci%d %02x]", port, b);
}

/* --trace-io LO-HI: count register accesses in [LO, HI] by PC, address and
 * direction, and list them at the end (for working out undocumented
 * peripherals such as the radio co-processor's port). */
#define TRACE_MAX 4096
static uint32_t trace_lo, trace_hi;
static struct { uint32_t pc, a; bool w; uint32_t n; uint8_t first, last; } traces[TRACE_MAX];
static int ntraces;
static void io_trace(cybiko_t *m, uint32_t a, uint8_t v, bool w)
{
    if (a < trace_lo || a > trace_hi) return;
    uint32_t pc = m->cpu.ppc;
    for (int i = 0; i < ntraces; i++)
        if (traces[i].pc == pc && traces[i].a == a && traces[i].w == w) {
            traces[i].n++;
            traces[i].last = v;
            return;
        }
    if (ntraces < TRACE_MAX)
        traces[ntraces++] = (typeof(traces[0])){ pc, a, w, 1, v, v };
}

/* --card IMG: the MP3 cartridge's SD card */
static FILE *card_file;
static bool card_read(void *user, uint32_t block, uint8_t *buf)
{
    (void)user;
    return fseek(card_file, (long)block * 512, SEEK_SET) == 0 && fread(buf, 1, 512, card_file) == 512;
}
static bool card_write(void *user, uint32_t block, const uint8_t *buf)
{
    (void)user;
    return fseek(card_file, (long)block * 512, SEEK_SET) == 0 && fwrite(buf, 1, 512, card_file) == 512;
}

/* --cart-log: what the cartridge model sees */
static void on_cart_log(cybiko_t *m, const char *msg)
{
    fprintf(stderr, "[%9.4f] cart: %s\n", (double)m->cpu.cycles / CY_CLOCK_HZ, msg);
}

/* The cartridge's decoder, run in step with emulated time. */
#define WAV_RATE 32000
static uint64_t wav_samples;

static void put_le(FILE *f, uint32_t v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 0xff, f); }

static void wav_header(FILE *f, uint32_t samples)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put_le(f, 36 + samples * 2, 4);
    fwrite("WAVEfmt ", 1, 8, f); put_le(f, 16, 4); put_le(f, 1, 2); put_le(f, 1, 2);
    put_le(f, WAV_RATE, 4); put_le(f, WAV_RATE * 2, 4); put_le(f, 2, 2); put_le(f, 16, 2);
    fwrite("data", 1, 4, f); put_le(f, samples * 2, 4);
}

static void mp3_pump(cybiko_t *m, cy_mp3_t *d, FILE *wav)
{
    uint64_t due = m->cpu.cycles * WAV_RATE / CY_CLOCK_HZ;
    static uint8_t in[4096];
    static int16_t pcm[1024];
    while (wav_samples < due) {
        size_t n = cybiko_mp3_take(m, in, cy_mp3_space(d));
        cy_mp3_feed(d, in, n);
        size_t k = due - wav_samples < 1024 ? (size_t)(due - wav_samples) : 1024;
        cy_mp3_render(d, pcm, k, WAV_RATE);
        if (wav) fwrite(pcm, 2, k, wav);
        wav_samples += k;
    }
}

/* --pair: a second machine on the same radio channel */
static cybiko_t *machines[2];

static void on_rf(void *user, const uint8_t *pkt, size_t len, uint8_t channel)
{
    int from = (int)(intptr_t)user;
    cybiko_t *to = machines[!from];
    if (to) cybiko_rf_receive(to, pkt, len, channel);
}

static int64_t wall(void *user) { (void)user; return (int64_t)time(NULL); }

static uint8_t *load(const char *path, size_t want, size_t *got, bool exact)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint8_t *buf = calloc(1, want);
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    if (exact && n != want) {
        fprintf(stderr, "%s: expected %zu bytes, got %zu\n", path, want, n);
        exit(1);
    }
    if (got) *got = n;
    return buf;
}

static void write_pgm(const char *path, const uint8_t *lum)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n%d %d\n255\n", CY_LCD_W, CY_LCD_H);
    fwrite(lum, 1, CY_LCD_W * CY_LCD_H, f);
    fclose(f);
}

static int key_by_name(const char *s)
{
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!strcasecmp(s, key_names[i].name)) return key_names[i].key;
    return -1;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *flash_path = NULL, *ramdisk_path = NULL;
    const char *shot = NULL, *frames = NULL, *dump = NULL;
    const char *pair_path = NULL;
    bool press_both = false;          /* --press keys go to the second machine too */
    uint32_t unit_id = 0;
    double seconds = 20;
    unsigned disk_kb = 0;
    bool ls = false, cart = false, cart_log = false;
    const char *card_path = NULL, *wav_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--flash") && v) { flash_path = v; i++; }
        else if (!strcmp(a, "--ramdisk") && v) { ramdisk_path = v; i++; }
        else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--screenshot") && v) { shot = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = v; i++; }
        else if (!strcmp(a, "--disk-size") && v) { disk_kb = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--install") && v && ninstalls < MAX_INSTALL) { installs[ninstalls++] = v; i++; }
        else if (!strcmp(a, "--ls")) ls = true;
        else if (!strcmp(a, "--cart")) cart = true;
        else if (!strcmp(a, "--card") && v) { card_path = v; cart = true; i++; }
        else if (!strcmp(a, "--cart-log")) cart = cart_log = true;
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--dump-ram") && v) { dump = v; i++; }
        else if (!strcmp(a, "--hex-port") && v) { hex_port = atoi(v); i++; }
        else if (!strcmp(a, "--pair") && v) { pair_path = v; i++; }
        else if (!strcmp(a, "--press-both")) press_both = true;
        else if (!strcmp(a, "--unit-id") && v) { unit_id = (uint32_t)strtoul(v, NULL, 16); i++; }
        else if (!strcmp(a, "--trace-io") && v) {
            trace_lo = (uint32_t)strtoul(v, NULL, 16);
            const char *dash = strchr(v, '-');
            trace_hi = dash ? (uint32_t)strtoul(dash + 1, NULL, 16) : trace_lo;
            i++;
        }
        else if (!strcmp(a, "--press") && v && npresses < MAX_PRESSES) {
            char name[32];
            unsigned ms, dur = 200;
            int who = 0;
            if (!strncmp(v, "b:", 2)) { who = 1; v += 2; }
            if (sscanf(v, "%31[^@]@%u:%u", name, &ms, &dur) < 2 || key_by_name(name) < 0) {
                fprintf(stderr, "bad --press %s\n", v);
                return 2;
            }
            presses[npresses].key = key_by_name(name);
            presses[npresses].who = who;
            presses[npresses].at = (uint64_t)ms * (CY_CLOCK_HZ / 1000);
            presses[npresses].until = presses[npresses].at + (uint64_t)dur * (CY_CLOCK_HZ / 1000);
            npresses++;
            i++;
        } else {
            fprintf(stderr, "usage: %s --rom FILE --flash FILE [--ramdisk FILE] [--seconds S]\n"
                            "       [--screenshot FILE.pgm] [--frames DIR] [--press KEY@MS[:D]]...\n"
                            "       [--disk-size KB] [--install FILE]... [--ls]\n"
                            "       [--unit-id HEX] [--pair RAMDISK2] [--press-both] [--hex-port N]\n"
                            "       [--trace-io LO-HI] [--dump-ram FILE] [--cart] [--cart-log] [--card IMG] [--wav FILE]\n", argv[0]);
            return 2;
        }
    }
    if (!rom_path || !flash_path) {
        fprintf(stderr, "--rom and --flash are required\n");
        return 2;
    }

    uint8_t *rom = calloc(1, CY_ROM_BUF);
    uint8_t *r = load(rom_path, CY_ROM_SIZE, NULL, true);
    memcpy(rom, r, CY_ROM_SIZE);
    free(r);
    uint8_t *flash = load(flash_path, CY_FLASH_SIZE, NULL, true);
    uint8_t *ram = calloc(1, CY_RAM_SIZE);
    uint32_t disk_size = disk_kb ? disk_kb * 1024u : CY_RAMDISK_SIZE;
    uint8_t *disk = disk_kb ? calloc(1, disk_size) : ram;
    if (ramdisk_path) {
        FILE *f = fopen(ramdisk_path, "rb");
        if (f) {
            size_t n = fread(disk, 1, disk_size, f);
            fclose(f);
            fprintf(stderr, "ramdisk: loaded %zu bytes\n", n);
        }
    }
    for (int i = 0; i < ninstalls; i++) {
        size_t len;
        uint8_t *data = load(installs[i], 8u << 20, &len, false);
        char path[512];
        snprintf(path, sizeof path, "%s", installs[i]);
        const char *name = basename(path);
        uint32_t date = (uint32_t)(time(NULL) + 2208988800LL);
        int r = cy_disk_write(disk, disk_size, name, data, (uint32_t)len, date);
        fprintf(stderr, "install %s (%zu bytes): %s\n", name, len,
                r == 0 ? "ok" : r == -1 ? "no room" : "bad name");
        free(data);
    }

    static cybiko_t m, m2;
    cy_host_t host = { .user = (void *)0, .serial_tx = on_tx, .wall_time = wall, .rf_send = on_rf,
                       .card_read = card_read, .card_write = card_write };
    cybiko_init(&m, &host, rom, flash, ram);
    if (disk_kb) cybiko_set_disk(&m, disk, disk_size);
    if (unit_id) cybiko_set_unit_id(&m, unit_id);
    if (trace_hi) m.io_trace = io_trace;
    m.cart.present = cart;
    if (card_path) {
        card_file = fopen(card_path, "r+b");
        if (!card_file) { perror(card_path); return 1; }
        fseek(card_file, 0, SEEK_END);
        m.cart.card_blocks = (uint32_t)(ftell(card_file) / 512);
    }
    if (cart_log) m.cart_log = on_cart_log;
    cy_mp3_t *mp3 = NULL;
    FILE *wav = NULL;
    if (cart) {
        mp3 = malloc(cy_mp3_size());
        cy_mp3_init(mp3);
        wav_samples = m.cpu.cycles * WAV_RATE / CY_CLOCK_HZ;
    }
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (!wav) { perror(wav_path); return 1; }
        wav_header(wav, 0);
    }
    the_machine = &m;
    machines[0] = &m;

    /* The second machine: same ROM and flash, its own RAM disk and ID. */
    uint8_t *disk2 = NULL;
    if (pair_path) {
        if (!disk_kb) { fprintf(stderr, "--pair needs --disk-size\n"); return 2; }
        uint8_t *rom2 = calloc(1, CY_ROM_BUF), *flash2 = malloc(CY_FLASH_SIZE);
        memcpy(rom2, rom, CY_ROM_SIZE);
        memcpy(flash2, flash, CY_FLASH_SIZE);
        disk2 = calloc(1, disk_size);
        FILE *f = fopen(pair_path, "rb");
        if (f) { if (fread(disk2, 1, disk_size, f) == 0) fprintf(stderr, "second RAM disk is empty\n"); fclose(f); }
        cy_host_t host2 = { .user = (void *)1, .wall_time = wall, .rf_send = on_rf };
        cybiko_init(&m2, &host2, rom2, flash2, calloc(1, CY_RAM_SIZE));
        cybiko_set_disk(&m2, disk2, disk_size);
        cybiko_set_unit_id(&m2, (unit_id ? unit_id : 0x4c000001u) + 1);
        machines[1] = &m2;
    }

    static uint8_t lum[CY_LCD_W * CY_LCD_H];
    uint32_t last_version = 0xffffffff;
    int frame_no = 0;
    const int32_t chunk = CY_CLOCK_HZ / 10;      /* 100 ms */
    uint64_t total = (uint64_t)(seconds * CY_CLOCK_HZ);
    clock_t t0 = clock();
    while (m.cpu.cycles < total) {
        /* press/release keys in 1 ms steps around scheduled presses */
        int32_t step = chunk;
        for (int i = 0; i < npresses; i++) {
            uint64_t now = m.cpu.cycles;
            if (!presses[i].down && now >= presses[i].at && now < presses[i].until) {
                int who = press_both ? 2 : presses[i].who;
                if (who != 1) cybiko_key(&m, presses[i].key, true);
                if (who != 0 && pair_path) cybiko_key(&m2, presses[i].key, true);
                presses[i].down = true;
            } else if (presses[i].down && now >= presses[i].until) {
                int who = press_both ? 2 : presses[i].who;
                if (who != 1) cybiko_key(&m, presses[i].key, false);
                if (who != 0 && pair_path) cybiko_key(&m2, presses[i].key, false);
                presses[i].down = false;
            }
            uint64_t next = !presses[i].down ? presses[i].at : presses[i].until;
            if (next > now && next - now < (uint64_t)step) step = (int32_t)(next - now);
        }
        if (mp3 && step > (int32_t)(CY_CLOCK_HZ / 200)) step = CY_CLOCK_HZ / 200;   /* keep the decoder fed */
        if (pair_path) {
            /* 1 ms turns keep radio replies well inside CyOS's timeouts */
            for (int32_t d = 0; d < step; d += CY_CLOCK_HZ / 1000) {
                int32_t n = step - d < (int32_t)(CY_CLOCK_HZ / 1000) ? step - d : (int32_t)(CY_CLOCK_HZ / 1000);
                cybiko_run(&m, n);
                cybiko_run(&m2, n);
            }
        } else {
            cybiko_run(&m, step);
        }
        if (mp3) mp3_pump(&m, mp3, wav);
        if (frames && m.cpu.cycles / chunk != (m.cpu.cycles - step) / chunk) {
            uint32_t v = cybiko_lcd_render(&m, lum);
            if (v != last_version) {
                char path[512];
                snprintf(path, sizeof path, "%s/frame_%04d.pgm", frames, frame_no++);
                write_pgm(path, lum);
                last_version = v;
            }
        }
    }
    double host_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (wav) {
        long bytes = ftell(wav) - 44;
        wav_header(wav, (uint32_t)(bytes / 2));
        fclose(wav);
    }

    if (shot) {
        cybiko_lcd_render(&m, lum);
        write_pgm(shot, lum);
        if (pair_path) {
            char path[512];
            snprintf(path, sizeof path, "%s.b.pgm", shot);
            cybiko_lcd_render(&m2, lum);
            write_pgm(path, lum);
        }
    }
    if (pair_path) {
        FILE *f = fopen(pair_path, "wb");
        if (f) { fwrite(disk2, 1, disk_size, f); fclose(f); }
    }
    fprintf(stderr, "radio: id %08x sent %u heard %u", (unsigned)cybiko_unit_id(&m),
            (unsigned)m.rf.sent, (unsigned)m.rf.heard);
    if (pair_path)
        fprintf(stderr, "; second machine id %08x sent %u heard %u", (unsigned)cybiko_unit_id(&m2),
                (unsigned)m2.rf.sent, (unsigned)m2.rf.heard);
    fprintf(stderr, "\n");
    if (ramdisk_path) {
        FILE *f = fopen(ramdisk_path, "wb");
        if (f) { fwrite(disk, 1, disk_size, f); fclose(f); }
    }
    for (int i = 0; i < ntraces; i++)
        fprintf(stderr, "io %06x %s %06x x%u first %02x last %02x\n", traces[i].pc,
                traces[i].w ? "W" : "R", traces[i].a, traces[i].n, traces[i].first, traces[i].last);
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(m.ram, 1, CY_RAM_SIZE, f); fclose(f); }
    }
    if (ls) {
        fprintf(stderr, "RAM disk (%u KB, %u bytes free):\n", (unsigned)(disk_size / 1024),
                (unsigned)cy_disk_free(disk, disk_size));
        cy_disk_list(disk, disk_size, print_file, NULL);
    }
    if (disk_kb) fprintf(stderr, "kernel patched %u time(s) for the bigger RAM disk\n", m.disk_patched);
    if (m.flash.dirty) {
        fprintf(stderr, "note: the OS wrote to flash (not saved by this runner)\n");
    }

    double emu_s = (double)m.cpu.cycles / CY_CLOCK_HZ;
    fprintf(stderr,
            "\n--- %.2f s emulated in %.2f s host (%.0f%% of real time, %.1f MHz)\n"
            "pc=%06x sleeping=%d off=%d lcd_on=%d frames=%d\n"
            "unmapped reads=%u writes=%u illegal=%u dma-dreq=%u wdt resets=%u\n",
            emu_s, host_s, host_s > 0 ? 100.0 * emu_s / host_s : 0.0,
            host_s > 0 ? m.cpu.cycles / host_s / 1e6 : 0.0,
            (unsigned)m.cpu.pc, m.cpu.sleeping, m.standby, (m.lcd.reg[0] & 0x40) != 0, frame_no,
            (unsigned)m.unmapped_reads, (unsigned)m.unmapped_writes, (unsigned)m.illegal_ops,
            (unsigned)m.dtc_dma_use, (unsigned)m.wdt_resets);
    return 0;
}
