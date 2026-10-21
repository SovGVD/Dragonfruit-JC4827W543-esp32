/* Headless Cybiko Xtreme runner for checking the machine model on a PC with
 * the real ROM and flash dumps:
 *
 *   cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin [options]
 *
 *   --seconds S        emulated seconds to run (default 20)
 *   --ramdisk FILE     load the 512 KB RAM disk from FILE and save it back
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
#include "cybiko.h"

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

#define MAX_PRESSES 64
static struct { int key; uint64_t at, until; bool down; } presses[MAX_PRESSES];
static int npresses;

static void on_tx(void *user, int port, uint8_t b)
{
    (void)user;
    if (port == 1 || port == 2) { putchar(b); fflush(stdout); }
    else fprintf(stderr, "[sci%d %02x]", port, b);
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
    const char *shot = NULL, *frames = NULL;
    double seconds = 20;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--flash") && v) { flash_path = v; i++; }
        else if (!strcmp(a, "--ramdisk") && v) { ramdisk_path = v; i++; }
        else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--screenshot") && v) { shot = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = v; i++; }
        else if (!strcmp(a, "--press") && v && npresses < MAX_PRESSES) {
            char name[32];
            unsigned ms, dur = 200;
            if (sscanf(v, "%31[^@]@%u:%u", name, &ms, &dur) < 2 || key_by_name(name) < 0) {
                fprintf(stderr, "bad --press %s\n", v);
                return 2;
            }
            presses[npresses].key = key_by_name(name);
            presses[npresses].at = (uint64_t)ms * (CY_CLOCK_HZ / 1000);
            presses[npresses].until = presses[npresses].at + (uint64_t)dur * (CY_CLOCK_HZ / 1000);
            npresses++;
            i++;
        } else {
            fprintf(stderr, "usage: %s --rom FILE --flash FILE [--ramdisk FILE] [--seconds S]\n"
                            "       [--screenshot FILE.pgm] [--frames DIR] [--press KEY@MS[:D]]...\n", argv[0]);
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
    if (ramdisk_path) {
        FILE *f = fopen(ramdisk_path, "rb");
        if (f) {
            size_t n = fread(ram, 1, CY_RAMDISK_SIZE, f);
            fclose(f);
            fprintf(stderr, "ramdisk: loaded %zu bytes\n", n);
        }
    }

    static cybiko_t m;
    cy_host_t host = { .serial_tx = on_tx, .wall_time = wall };
    cybiko_init(&m, &host, rom, flash, ram);

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
                cybiko_key(&m, presses[i].key, true);
                presses[i].down = true;
            } else if (presses[i].down && now >= presses[i].until) {
                cybiko_key(&m, presses[i].key, false);
                presses[i].down = false;
            }
            uint64_t next = !presses[i].down ? presses[i].at : presses[i].until;
            if (next > now && next - now < (uint64_t)step) step = (int32_t)(next - now);
        }
        cybiko_run(&m, step);
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

    if (shot) {
        cybiko_lcd_render(&m, lum);
        write_pgm(shot, lum);
    }
    if (ramdisk_path) {
        FILE *f = fopen(ramdisk_path, "wb");
        if (f) { fwrite(ram, 1, CY_RAMDISK_SIZE, f); fclose(f); }
    }
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
