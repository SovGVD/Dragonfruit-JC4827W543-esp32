/* The Cybiko Xtreme emulator as a desktop app (SDL3): a window with the
 * screen, the PC keyboard as the Cybiko's keys, sound, and the MP3
 * cartridge.
 *
 *   cybiko [DIR] [--zoom N]
 *
 * DIR (default: the current folder) is laid out like /Cybiko on the board's
 * SD card, so the same folder works in both places:
 *
 *   bootrom.bin | cyrom150.bin | xtreme-rom.bin     boot ROM (32 KB, yours)
 *   flash.bin | cyos_v1508.bin | xtreme-flash.bin   CyOS flash (512 KB, yours)
 *   ramdisk.bin   the RAM disk, created and saved here (on exit and every 30 s)
 *   apps/         files copied onto the RAM disk at start if it lacks them
 *   card/         the MP3 cartridge's card: .mp3 files and MXD clips
 *   mp3card.img   the card as a disk image instead, used while card/ is empty
 *   cybiko.cfg    disk.size = 1-6 (MB), sound.volume = 0-100, cart = auto|mp3|off
 *
 * A flash that CyOS reprograms (an OS update) is saved as flash.bin. A CyOS
 * update image dropped in DIR (*.boot, such as the Russian
 * ccyphRU1.5.08r.boot) is installed on start the way EZ Loader did it; the
 * new flash is saved once the install is over and the file is renamed to
 * *.boot.done.
 *
 * Built with the web browser (src/components/cy_browser, the cybiko-browser
 * submodule), the Browser launcher is put on the RAM disk and opens it;
 * pages come through the curl program, and bookmarks and settings are kept
 * in DIR/browser. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cybiko.h"
#include "cy_mp3.h"
#include "cy_vfat.h"
#ifdef CY_WITH_BROWSER
#include "cybiko_browser.h"
#include "cyb_net_curl.h"
#endif

#define RATE 32000
#define CHUNK (CY_CLOCK_HZ / 1000)          /* run 1 ms at a go */
#define SAVE_EVERY_MS 30000
#define SOUND_AHEAD (RATE / 10)             /* samples kept queued for the sound card */

static char dir[1024];
static cybiko_t m;
static uint8_t *disk;
static uint32_t disk_size;

/* ---- files ---- */

static void path_in(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

static uint8_t *load_first(const char *const *names, int n, size_t size, char *found, size_t cap)
{
    for (int i = 0; i < n; i++) {
        char p[1200];
        path_in(p, sizeof p, names[i]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        uint8_t *buf = calloc(1, size < CY_ROM_BUF ? CY_ROM_BUF : size);
        size_t got = fread(buf, 1, size, f);
        fclose(f);
        if (got != size) {
            fprintf(stderr, "%s: expected %zu bytes, got %zu\n", p, size, got);
            free(buf);
            continue;
        }
        snprintf(found, cap, "%s", p);
        return buf;
    }
    return NULL;
}

static bool write_file(const char *name, const void *data, size_t len)
{
    char p[1200], tmp[1210];
    path_in(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) { perror(tmp); return false; }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, p) == 0;
    if (!ok) fprintf(stderr, "could not save %s\n", p);
    return ok;
}

/* cybiko.cfg: "key = value" lines, # comments. */
static char cfg[4096];
static bool cfg_get(const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *line = cfg; *line; ) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        while (line < end && (*line == ' ' || *line == '\t')) line++;
        if (!strncmp(line, key, kl)) {
            const char *p = line + kl;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p < end && *p == '=') {
                p++;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                size_t n = 0;
                while (p + n < end && p[n] != '#' && p[n] != '\r') n++;
                while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
                if (n >= cap) n = cap - 1;
                memcpy(out, p, n);
                out[n] = 0;
                return true;
            }
        }
        line = *end ? end + 1 : end;
    }
    return false;
}

static void install_apps(void)
{
    char p[1200];
    path_in(p, sizeof p, "apps");
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.' || strlen(name) > CY_DISK_NAME_MAX || cy_disk_find(disk, disk_size, name, NULL)) continue;
        char fp[2300];
        snprintf(fp, sizeof fp, "%s/%s", p, name);
        FILE *f = fopen(fp, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
        if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len) {
            uint32_t date = (uint32_t)(time(NULL) + 2208988800LL);
            int r = cy_disk_write(disk, disk_size, name, buf, (uint32_t)len, date);
            printf("apps: %s %s\n", name, r == 0 ? "copied" : r == -1 ? "does not fit" : "has a bad name");
        }
        free(buf);
        fclose(f);
    }
    closedir(d);
}

/* ---- the web browser ---- */

#ifdef CY_WITH_BROWSER
static cyb_t *browser;
static bool browse_requested, browsing;
static uint8_t lcd_saved[sizeof m.lcd.ram];

static uint32_t on_app_call(void *user, uint32_t fn, uint32_t arg)
{
    (void)user;
    (void)arg;
    if (fn == CY_APP_VERSION) return CY_APP_API;
    if (fn == CY_APP_BROWSER) { browse_requested = true; return 1; }
    return 0;
}

static void install_browser(void)
{
    cy_disk_file_t f;
    if (cy_disk_find(disk, disk_size, "Browser.app", &f) && f.size == cyb_launcher_app_size) return;
    uint32_t date = (uint32_t)(time(NULL) + 2208988800LL);
    if (cy_disk_write(disk, disk_size, "Browser.app", cyb_launcher_app, (uint32_t)cyb_launcher_app_size, date) == 0)
        printf("apps: Browser.app put on the RAM disk\n");
}

static void browser_open(const uint8_t *flash)
{
    if (!browser) {
        static cyb_net_t net;
        char p[1200];
        cyb_net_curl(&net, NULL);
        path_in(p, sizeof p, "browser");
        mkdir(p, 0777);
        browser = cyb_create(&net, flash, CY_FLASH_SIZE, p);
    }
    if (!browser) return;
    memcpy(lcd_saved, m.lcd.ram, sizeof lcd_saved);
    cyb_show(browser);
    browsing = true;
}

/* Runs the browser a bit; false once it has closed (CyOS gets the screen
 * back). */
static bool browser_step(void)
{
    static uint8_t fb[CYB_FB_SIZE];
    if (!cyb_step(browser, (uint32_t)SDL_GetTicks())) {
        memcpy(m.lcd.ram, lcd_saved, sizeof lcd_saved);
        m.lcd.version++;
        browsing = false;
        return false;
    }
    if (cyb_render(browser, fb)) cybiko_lcd_show(&m, fb);
    return true;
}
#endif

/* ---- the cartridge's card ---- */

static FILE *card_file;
static cy_vfat_t *card_dir;

static bool card_read(void *user, uint32_t block, uint8_t *buf)
{
    (void)user;
    if (card_dir) return cy_vfat_read(card_dir, block, buf);
    return fseek(card_file, (long)block * 512, SEEK_SET) == 0 && fread(buf, 1, 512, card_file) == 512;
}

static bool card_write(void *user, uint32_t block, const uint8_t *buf)
{
    (void)user;
    if (card_dir) return cy_vfat_write(card_dir, block, buf);
    return fseek(card_file, (long)block * 512, SEEK_SET) == 0 && fwrite(buf, 1, 512, card_file) == 512;
}

static void setup_cart(void)
{
    char v[16] = "auto", p[1200];
    cfg_get("cart", v, sizeof v);
    path_in(p, sizeof p, "card");
    cy_vfat_t *vd = NULL;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *de;
        bool any = false;
        while (!any && (de = readdir(d)) != NULL) any = de->d_name[0] != '.';
        closedir(d);
        if (any) vd = cy_vfat_open(p, 8192);
    }
    char img[1200];
    path_in(img, sizeof img, "mp3card.img");
    FILE *fi = vd ? NULL : fopen(img, "r+b");
    bool on = !strcmp(v, "mp3") || (!strcmp(v, "auto") && (vd || fi));
    if (!on) {
        if (vd) cy_vfat_close(vd);
        if (fi) fclose(fi);
        return;
    }
    m.cart.present = true;
    if (vd) {
        card_dir = vd;
        m.cart.card_blocks = cy_vfat_blocks(vd);
        printf("MP3 cartridge, card = %s: %u files on a %u MB disk (%u left out)\n", p,
               (unsigned)cy_vfat_files(vd), (unsigned)(cy_vfat_blocks(vd) >> 11), (unsigned)cy_vfat_skipped(vd));
    } else if (fi) {
        card_file = fi;
        fseek(fi, 0, SEEK_END);
        m.cart.card_blocks = (uint32_t)(ftell(fi) / 512);
        printf("MP3 cartridge, card = %s\n", img);
    } else {
        printf("MP3 cartridge, no card\n");
    }
}

/* ---- sound: the beeper and the cartridge, mixed at RATE ---- */

static float beep_freq, beep_duty, beep_phase;
static int volume = 60;                     /* sound.volume, 0-100 */
static cy_mp3_t *mp3;
static uint64_t samples_done;

static void on_speaker(void *user, float freq_hz, float duty)
{
    (void)user;
    beep_freq = freq_hz;
    beep_duty = duty;
}

static int64_t wall_time(void *user)
{
    (void)user;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return (int64_t)now + tm.tm_gmtoff;     /* the Cybiko shows local time */
}

static void on_tx(void *user, int port, uint8_t b)
{
    (void)user;
    if (port == 1 && (b == '\n' || (b >= 0x20 && b < 0x7f))) { putchar(b); if (b == '\n') fflush(stdout); }
}

/* Make the samples for the emulated time run so far. */
static void make_sound(SDL_AudioStream *out)
{
    uint64_t due = h8s_time(&m.cpu) * RATE / CY_CLOCK_HZ;
    static int16_t pcm[1024], music[1024];
    static uint8_t in[4096];
    while (samples_done < due) {
        size_t n = due - samples_done < 1024 ? (size_t)(due - samples_done) : 1024;
        size_t real = 0;
        if (mp3) {
            size_t got = cybiko_mp3_take(&m, in, cy_mp3_space(mp3));
            cy_mp3_feed(mp3, in, got);
            real = cy_mp3_render(mp3, music, n, RATE);
        }
        /* the cartridge's volume register, 1 dB a step from its default */
        uint8_t v = mp3 ? cybiko_mp3_volume(&m) : 0;
        float gain = v ? powf(10.0f, ((int)v - 0x6a) / 20.0f) : 0.0f;
        if (gain > 2.0f) gain = 2.0f;
        float level = volume / 100.0f;
        for (size_t i = 0; i < n; i++) {
            float s = 0;
            if (beep_freq > 0) {
                s = beep_phase < beep_duty ? 0.25f : -0.25f;
                beep_phase += beep_freq / RATE;
                if (beep_phase >= 1.0f) beep_phase -= floorf(beep_phase);
            }
            if (i < real) s += music[i] / 32768.0f * gain;
            s *= level;
            pcm[i] = (int16_t)(s > 1 ? 32767 : s < -1 ? -32768 : s * 32767);
        }
        if (out && SDL_GetAudioStreamQueued(out) < RATE / 4 * 2)      /* cap the delay at 250 ms */
            SDL_PutAudioStreamData(out, pcm, (int)(n * 2));
        samples_done += n;
    }
}

/* ---- keys ---- */

static const struct { SDL_Keycode sdl; int key; const char *label; } keymap[] = {
    { SDLK_RETURN, CYK_ENTER, "Enter" }, { SDLK_KP_ENTER, CYK_ENTER, NULL },
    { SDLK_ESCAPE, CYK_ESC, "Esc" }, { SDLK_SPACE, CYK_SPACE, "Space" },
    { SDLK_UP, CYK_UP, "arrows" }, { SDLK_DOWN, CYK_DOWN, NULL },
    { SDLK_LEFT, CYK_LEFT, NULL }, { SDLK_RIGHT, CYK_RIGHT, NULL },
    { SDLK_TAB, CYK_TAB, "Tab" }, { SDLK_BACKSPACE, CYK_DEL, "Backspace/Delete = Del" },
    { SDLK_DELETE, CYK_DEL, NULL }, { SDLK_LSHIFT, CYK_SHIFT, "Shift" }, { SDLK_RSHIFT, CYK_SHIFT, NULL },
    { SDLK_LCTRL, CYK_FN, "Ctrl = Fn" }, { SDLK_RCTRL, CYK_FN, NULL },
    { SDLK_PERIOD, CYK_DOT, "." }, { SDLK_F1, CYK_F1, "F1-F7" }, { SDLK_F2, CYK_F2, NULL },
    { SDLK_F3, CYK_F3, NULL }, { SDLK_F4, CYK_F4, NULL }, { SDLK_F5, CYK_F5, NULL },
    { SDLK_F6, CYK_F6, NULL }, { SDLK_F7, CYK_F7, NULL },
    { SDLK_COMMA, CYK_COMMA, "," }, { SDLK_SEMICOLON, CYK_SEMI, ";" },
    { SDLK_LEFTBRACKET, CYK_LPAREN, "[ = (" }, { SDLK_RIGHTBRACKET, CYK_RPAREN, "] = )" },
    { SDLK_SLASH, CYK_EXCL, "/ = !" },
    { SDLK_F8, CYK_SELECT, "F8 = Select" }, { SDLK_HOME, CYK_SELECT, NULL },
    { SDLK_F9, CYK_HELP, "F9 = Help" }, { SDLK_F10, CYK_INS, "F10 or Insert = Ins" },
    { SDLK_INSERT, CYK_INS, NULL }, { SDLK_F11, CYK_MENU, "F11 or the menu key = Menu" },
    { SDLK_APPLICATION, CYK_MENU, NULL }, { SDLK_MENU, CYK_MENU, NULL },
    { SDLK_F12, CYK_ONOFF, "F12 = On/Off" },
};
static const int letters[26] = {
    CYK_A, CYK_B, CYK_C, CYK_D, CYK_E, CYK_F, CYK_G, CYK_H, CYK_I, CYK_J, CYK_K, CYK_L, CYK_M,
    CYK_N, CYK_O, CYK_P, CYK_Q, CYK_R, CYK_S, CYK_T, CYK_U, CYK_V, CYK_W, CYK_X, CYK_Y, CYK_Z,
};

static int map_key(SDL_Keycode k)
{
    if (k >= SDLK_A && k <= SDLK_Z) return letters[k - SDLK_A];
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (keymap[i].sdl == k) return keymap[i].key;
    return -1;
}

/* ---- the RAM disk and flash ---- */

static void save_disk(bool force)
{
    cy_vfat_sync_t r;
    if (card_dir && cy_vfat_sync(card_dir, &r) && (r.written | r.removed | r.renamed | r.made | r.failed))
        printf("card: %u written, %u deleted, %u renamed, %u folders made, %u failed\n", (unsigned)r.written,
               (unsigned)r.removed, (unsigned)r.renamed, (unsigned)r.made, (unsigned)r.failed);
    bool dirty = force;
    for (size_t i = 0; i < sizeof m.disk_dirty / sizeof m.disk_dirty[0]; i++) dirty |= m.disk_dirty[i] != 0;
    if (!dirty) return;
    memset(m.disk_dirty, 0, sizeof m.disk_dirty);
    write_file("ramdisk.bin", disk, disk_size);
}

/* A CyOS update being installed, and the flash write count at the last
 * save: flash.bin is written only once CyOS's installer has stopped. */
static char update_path[1200];
static uint32_t update_writes;

static bool find_update(void)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 6 || SDL_strcasecmp(e->d_name + n - 5, ".boot")) continue;
        char p[1200];
        path_in(p, sizeof p, e->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        /* the small avr*.boot updates the AVR co-processor, not CyOS */
        if (size < 64 * 1024 || size > 512 * 1024) continue;
        snprintf(update_path, sizeof update_path, "%s", p);
        found = true;
    }
    closedir(d);
    return found;
}

static bool stage_update(uint8_t *flash, const char *const *dump_names)
{
    FILE *f = fopen(update_path, "rb");
    if (!f) return false;
    uint8_t *boot = malloc(512 * 1024);
    size_t len = fread(boot, 1, 512 * 1024, f);
    fclose(f);
    int r = cybiko_flash_stage_boot(flash, boot, len);
    if (r == CY_BOOT_NO_LOADER) {       /* flash.bin from an interrupted install */
        char p[1200];
        uint8_t *dump = load_first(dump_names, 2, CY_FLASH_SIZE, p, sizeof p);
        if (dump) {
            memcpy(flash, dump, CY_FLASH_SIZE);
            free(dump);
            r = cybiko_flash_stage_boot(flash, boot, len);
        }
    }
    free(boot);
    printf("CyOS update %s: %s\n", update_path, r == CY_BOOT_OK ? "installing" :
           r == CY_BOOT_TOO_BIG ? "too big" : r == CY_BOOT_NO_LOADER ? "no CyOS flash to install it on" :
           "not a CyOS update image");
    if (r != CY_BOOT_OK) update_path[0] = 0;
    return r == CY_BOOT_OK;
}

static void save_flash(uint8_t *flash)
{
    if (!m.flash.dirty) return;
    bool updated = false;
    if (update_path[0]) {
        updated = m.flash.writes == update_writes;
        update_writes = m.flash.writes;
        if (!updated) return;           /* still installing */
    }
    uint8_t *copy = malloc(CY_FLASH_SIZE);
    memcpy(copy, flash, CY_FLASH_SIZE);
    cybiko_flash_original(&m, copy);
    if (write_file("flash.bin", copy, CY_FLASH_SIZE)) {
        printf("saved the reprogrammed flash as flash.bin\n");
        if (updated) {
            char done[1210];
            snprintf(done, sizeof done, "%s.done", update_path);
            rename(update_path, done);
            update_path[0] = 0;
        }
    }
    m.flash.dirty = false;
    free(copy);
}

static void show_lcd(SDL_Texture *tex, const uint8_t *lum)
{
    void *pixels;
    int pitch;
    if (!SDL_LockTexture(tex, NULL, &pixels, &pitch)) return;
    for (int y = 0; y < CY_LCD_H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
        for (int x = 0; x < CY_LCD_W; x++) {
            /* a grey-green LCD: lum 0 black, 255 the panel colour */
            uint32_t l = lum[y * CY_LCD_W + x];
            uint32_t r = 0x18 + l * (0xc6 - 0x18) / 255, g = 0x1c + l * (0xd0 - 0x1c) / 255,
                     b = 0x18 + l * (0xbc - 0x18) / 255;
            row[x] = r << 16 | g << 8 | b;
        }
    }
    SDL_UnlockTexture(tex);
}

int main(int argc, char **argv)
{
    int zoom = 4;
    snprintf(dir, sizeof dir, ".");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom = atoi(argv[++i]);
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [DIR] [--zoom N]\n"
                            "DIR holds the ROM and flash dumps; see docs/cybiko.md\n", argv[0]);
            return 2;
        } else snprintf(dir, sizeof dir, "%s", argv[i]);
    }
    if (zoom < 1) zoom = 1;

    static const char *const rom_names[] = { "bootrom.bin", "cyrom150.bin", "xtreme-rom.bin" };
    static const char *const flash_names[] = { "flash.bin", "cyos_v1508.bin", "xtreme-flash.bin" };
    char rom_path[1200] = "", flash_path[1200] = "";
    uint8_t *rom = load_first(rom_names, 3, CY_ROM_SIZE, rom_path, sizeof rom_path);
    uint8_t *flash = load_first(flash_names, 3, CY_FLASH_SIZE, flash_path, sizeof flash_path);
    if (!rom || !flash) {
        fprintf(stderr, "%s: need the boot ROM (bootrom.bin, cyrom150.bin or xtreme-rom.bin, 32 KB)\n"
                        "and the flash (flash.bin, cyos_v1508.bin or xtreme-flash.bin, 512 KB)\n", dir);
        return 1;
    }
    printf("ROM %s, flash %s\n", rom_path, flash_path);
    if (find_update() && !stage_update(flash, flash_names + 1)) return 1;

    char p[1200], v[32];
    path_in(p, sizeof p, "cybiko.cfg");
    FILE *cf = fopen(p, "rb");
    if (cf) { size_t n = fread(cfg, 1, sizeof cfg - 1, cf); cfg[n] = 0; fclose(cf); }
    int mb = 4;
    if (cfg_get("disk.size", v, sizeof v)) mb = atoi(v);
    if (mb < 1) mb = 1;
    if (mb > (int)(CY_DISK_MAX >> 20)) mb = (int)(CY_DISK_MAX >> 20);
    if (cfg_get("sound.volume", v, sizeof v)) volume = atoi(v);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    /* The RAM disk: disk.size MB, never smaller than an existing file. */
    disk_size = (uint32_t)mb << 20;
    struct stat st;
    path_in(p, sizeof p, "ramdisk.bin");
    if (stat(p, &st) == 0 && (uint32_t)st.st_size > disk_size && st.st_size <= (off_t)CY_DISK_MAX)
        disk_size = ((uint32_t)st.st_size + 0xffff) & ~0xffffu;
    disk = calloc(1, disk_size);
    FILE *df = fopen(p, "rb");
    if (df) { size_t n = fread(disk, 1, disk_size, df); fclose(df); printf("RAM disk: %zu KB loaded\n", n >> 10); }
    install_apps();
#ifdef CY_WITH_BROWSER
    install_browser();
#endif

    cy_host_t host = { .serial_tx = on_tx, .wall_time = wall_time, .speaker = on_speaker,
                       .card_read = card_read, .card_write = card_write };
#ifdef CY_WITH_BROWSER
    host.app_call = on_app_call;
#endif
    uint8_t *ram = calloc(1, CY_RAM_SIZE);
    cybiko_init(&m, &host, rom, flash, ram);
    cybiko_set_disk(&m, disk, disk_size);
    setup_cart();
    if (m.cart.present) {
        mp3 = malloc(cy_mp3_size());
        cy_mp3_init(mp3);
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win;
    SDL_Renderer *ren;
    if (!SDL_CreateWindowAndRenderer("Cybiko Xtreme", CY_LCD_W * zoom, CY_LCD_H * zoom, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, CY_LCD_W, CY_LCD_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, CY_LCD_W, CY_LCD_H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, RATE };
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "no sound: %s\n", SDL_GetError());

    printf("Keys: letters, ");
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (keymap[i].label) printf("%s%s", keymap[i].label, i + 1 < sizeof keymap / sizeof keymap[0] ? ", " : "\n");
    printf("Close the window to quit (the RAM disk is saved).\n");

    uint64_t start = SDL_GetTicksNS(), last_save = SDL_GetTicks();
    uint64_t base_cycles = m.cpu.cycles;
    bool sound_clock = audio != NULL;
    uint64_t full_since = 0;
    uint32_t shown = 0;
    static uint8_t lum[CY_LCD_W * CY_LCD_H];
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_KEY_UP) {
                int k = map_key(e.key.key);
#ifdef CY_WITH_BROWSER
                if (browsing) {
                    int bk = k >= 0 ? cybiko_browser_key(k) : 0;
                    if (bk) cyb_key(browser, bk, e.type == SDL_EVENT_KEY_DOWN);
                    /* releases reach CyOS too, so no key stays held down */
                    if (k >= 0 && e.type == SDL_EVENT_KEY_UP) cybiko_key(&m, k, false);
                    continue;
                }
#endif
                if (k >= 0) cybiko_key(&m, k, e.type == SDL_EVENT_KEY_DOWN);
            }
        }
#ifdef CY_WITH_BROWSER
        /* CyOS waits in the launcher while the browser is open */
        if (browsing && browser_step()) {
            uint32_t ver = cybiko_lcd_render(&m, lum);
            if (ver != shown) {
                shown = ver;
                show_lcd(tex, lum);
            }
            SDL_SetRenderDrawColor(ren, 0x30, 0x30, 0x30, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
            SDL_Delay(cyb_busy(browser) ? 1 : 10);
            start = SDL_GetTicksNS();
            base_cycles = m.cpu.cycles;
            continue;
        }
#endif
        /* With sound, the sound card sets the pace: run until SOUND_AHEAD
         * samples are queued, so it never runs dry (a wall-clock pace left
         * it only a few ms ahead, and sound broke up on PulseAudio and
         * PipeWire). Without sound, or if the card stops taking samples, run
         * up to the wall clock. Either way at most 100 ms at a time; if the
         * PC falls further behind, the emulated time slips. */
        uint64_t elapsed = (SDL_GetTicksNS() - start) * (CY_CLOCK_HZ / 1000) / 1000000;
        uint64_t want = base_cycles + elapsed;
        if (sound_clock) {
            int queued = SDL_GetAudioStreamQueued(audio) / 2;
            want = m.cpu.cycles;
            if (queued < SOUND_AHEAD) {
                want += (uint64_t)(SOUND_AHEAD - queued) * CY_CLOCK_HZ / RATE;
                full_since = 0;
            } else if (!full_since) {
                full_since = SDL_GetTicks();
            } else if (SDL_GetTicks() - full_since > 1000) {
                fprintf(stderr, "the sound card stopped playing; keeping time by the clock\n");
                sound_clock = false;
            }
        }
        if (want > m.cpu.cycles + CY_CLOCK_HZ / 10) want = m.cpu.cycles + CY_CLOCK_HZ / 10;
        base_cycles = want - elapsed;
        while (m.cpu.cycles < want) {
            cybiko_run(&m, CHUNK);
            make_sound(audio);
#ifdef CY_WITH_BROWSER
            if (browse_requested) {
                browse_requested = false;
                browser_open(flash);
                break;
            }
#endif
        }
        uint32_t ver = cybiko_lcd_render(&m, lum);
        if (ver != shown) {
            shown = ver;
            show_lcd(tex, lum);
        }
        SDL_SetRenderDrawColor(ren, 0x30, 0x30, 0x30, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (SDL_GetTicks() - last_save > SAVE_EVERY_MS) {
            save_disk(false);
            save_flash(flash);
            last_save = SDL_GetTicks();
        }
        SDL_Delay(5);
    }
    save_disk(false);
    save_flash(flash);
    if (card_dir) cy_vfat_close(card_dir);
    if (card_file) fclose(card_file);
    SDL_Quit();
    return 0;
}
