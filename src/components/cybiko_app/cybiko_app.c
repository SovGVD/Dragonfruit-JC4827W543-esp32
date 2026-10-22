/* Cybiko Xtreme mode: loads the ROM and CyOS flash from /Cybiko, runs the
 * machine in a CPU task on core 1 and the screen, on-screen keyboard and
 * saving in a UI task on core 0.
 *
 * Files in /Cybiko (see cybiko_app.h for the accepted ROM/flash names):
 *   ramdisk.bin  the battery-backed RAM disk (user files), disk.size MB
 *                (default 4; CyOS alone had 512 KB); changed parts are
 *                written back
 *   apps/        files copied onto the RAM disk at start when it does not
 *                have them yet, so games and apps can be dropped in here
 *   flash.bin    written only if CyOS reprograms its flash; it is the first
 *                flash name searched, so it then replaces the original image
 *                (always with the dump's own Cybiko ID; see cybiko_link.h)
 *   state.bin    the Cybiko clock, so the time survives restarts
 *   card/        the MP3 cartridge's SD card: .mp3 files and MXD clips
 *                dropped here are on the card (see cybiko_cart.h)
 *   mp3card.img  the card as a FAT disk image instead, used while card/
 *                is empty
 *   cybiko.cfg   optional settings: sound.volume = 0-100 (default 60),
 *                disk.size = 1-6 (MB, default 4), the radio keys described
 *                in cybiko_link.h and the cartridge key in cybiko_cart.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cybiko_app.h"
#include "cybiko_ui.h"
#include "cybiko_link.h"
#include "cybiko_cart.h"
#include "cybiko.h"
#include "display.h"
#include "m5_sd.h"
#include "palm_config.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR    /* host builds */
#endif

static const char *TAG = "cybiko";

#define RAMDISK_PATH CYBIKO_SD_DIR "/ramdisk.bin"
#define FLASH_SAVE_PATH CYBIKO_SD_DIR "/flash.bin"
#define STATE_PATH CYBIKO_SD_DIR "/state.bin"
#define CFG_PATH CYBIKO_SD_DIR "/cybiko.cfg"
#define APPS_DIR CYBIKO_SD_DIR "/apps"
#define DISK_DEFAULT_MB 4

#define CHUNK_STATES (CY_CLOCK_HZ / 1000)   /* run 1 ms of Cybiko time at a go */

static cybiko_t *machine;                   /* allocated only in Cybiko mode */
static uint8_t *rom, *flash, *ram;
static uint8_t *disk;                       /* the RAM disk, moved to CY_DISK_BASE */
static uint32_t disk_size;
static QueueHandle_t key_queue;
static volatile bool pause_req, paused;
static int64_t clock_base;                  /* Cybiko clock minus time(NULL) */

typedef struct { int16_t key; bool down; } key_event_t;

/* ------------------------------------------------------------------ */
/* Host hooks (called on the CPU task)                                 */
/* ------------------------------------------------------------------ */

static void on_serial_tx(void *user, int port, uint8_t b)
{
    (void)user;
    /* The boot loader and CyOS print text on SCI1: log it a line at a time.
     * SCI2 is the radio co-processor (cy_rf.c, cybiko_link.c). */
    if (port != 1) return;
    static char line[128];
    static size_t len;
    if (b == '\n' || b == '\r' || len == sizeof line - 1) {
        if (len) {
            line[len] = 0;
            ESP_LOGI("cyos", "%s", line);
            len = 0;
        }
    }
    if (b >= 0x20 && b < 0x7f) line[len++] = (char)b;
}

static void on_speaker(void *user, float freq_hz, float duty)
{
    (void)user;
    audio_tone(freq_hz, duty);
}

static int64_t wall_time(void *user)
{
    (void)user;
    return (int64_t)time(NULL) + clock_base;
}

/* ------------------------------------------------------------------ */
/* CPU task                                                            */
/* ------------------------------------------------------------------ */

static void cpu_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    uint64_t c0 = machine->cpu.cycles;
    int64_t last_yield = t0;
    for (;;) {
        if (pause_req) {
            paused = true;
            while (pause_req) vTaskDelay(1);
            paused = false;
            t0 = esp_timer_get_time();
            c0 = machine->cpu.cycles;
        }
        key_event_t ev;
        while (xQueueReceive(key_queue, &ev, 0) == pdTRUE) cybiko_key(machine, ev.key, ev.down);
        cybiko_link_poll(machine);
        cybiko_cart_poll(machine);

        int64_t run_start = esp_timer_get_time();
        cybiko_run(machine, CHUNK_STATES);

        int64_t now = esp_timer_get_time();
        /* Every 5 s: how fast the emulation runs against real time, and how
         * much of the core the CPU loop used to get there. */
        static int64_t stat_t0, stat_busy;
        static uint64_t stat_c0;
        stat_busy += now - run_start;
        if (!stat_t0) { stat_t0 = now; stat_c0 = machine->cpu.cycles; stat_busy = 0; }
        if (now - stat_t0 >= 5000000) {
            uint64_t emu_us = (machine->cpu.cycles - stat_c0) * 1000000ull / CY_CLOCK_HZ;
            ESP_LOGI(TAG, "speed %d%% of real time, CPU loop busy %d%%",
                     (int)(emu_us * 100 / (uint64_t)(now - stat_t0)),
                     (int)(stat_busy * 100 / (now - stat_t0)));
            stat_t0 = now;
            stat_c0 = machine->cpu.cycles;
            stat_busy = 0;
        }
        /* Say whether the kernel took the bigger RAM disk (cy_disk.c). */
        static uint8_t patched_seen;
        static bool unpatched_warned;
        if (machine->disk_patched != patched_seen) {
            patched_seen = machine->disk_patched;
            ESP_LOGI(TAG, "RAM disk moved to %06x, %u KB", (unsigned)CY_DISK_BASE,
                     (unsigned)(machine->disk_size >> 10));
        } else if (!patched_seen && !unpatched_warned && machine->cpu.cycles > 30ull * CY_CLOCK_HZ) {
            unpatched_warned = true;
            ESP_LOGW(TAG, "CyOS RAM disk set-up not recognised: the RAM disk stays at 512 KB");
        }
        int64_t emu_us = (int64_t)((machine->cpu.cycles - c0) * 1000000ull / CY_CLOCK_HZ);
        int64_t ahead = emu_us - (now - t0);
        if (ahead > 2000) {
            vTaskDelay(pdMS_TO_TICKS(ahead / 1000));
            last_yield = esp_timer_get_time();
        } else {
            /* Behind: idle stretches (CyOS sleeping) run far faster than real
             * time and repay a busy burst, so keep up to a second of debt
             * before giving up on it. */
            if (ahead < -1000000) {
                t0 = now;
                c0 = machine->cpu.cycles;
            }
            if (now - last_yield > 100000) {     /* let the idle task feed the watchdog */
                vTaskDelay(1);
                last_yield = esp_timer_get_time();
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Saving                                                              */
/* ------------------------------------------------------------------ */

static void cpu_pause(bool on)
{
    if (on) {
        pause_req = true;
        while (!paused) vTaskDelay(1);
    } else {
        pause_req = false;
    }
}

static bool write_file(const char *path, const void *data, size_t len)
{
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        ESP_LOGW(TAG, "could not write %s", path);
    }
    return ok;
}

static void save_state(void)
{
    struct { char magic[4]; uint32_t version; int64_t clock; } st = {
        { 'C', 'Y', 'S', 'T' }, 1, (int64_t)time(NULL) + clock_base,
    };
    write_file(STATE_PATH, &st, sizeof st);
}

/* Write the RAM disk chunks CyOS changed into ramdisk.bin in place: the
 * file is several MB, too slow to rewrite whole. */
static void save_disk(const uint32_t *dirty)
{
    FILE *f = fopen(RAMDISK_PATH, "r+b");
    if (!f) { ESP_LOGW(TAG, "could not open %s", RAMDISK_PATH); return; }
    int n = 0;
    bool ok = true;
    for (uint32_t c = 0; c < disk_size / CY_DISK_CHUNK; c++) {
        if (!(dirty[c / 32] & (1u << (c % 32)))) continue;
        ok = fseek(f, (long)(c * CY_DISK_CHUNK), SEEK_SET) == 0 &&
             fwrite(disk + c * CY_DISK_CHUNK, 1, CY_DISK_CHUNK, f) == CY_DISK_CHUNK && ok;
        n++;
    }
    ok = fclose(f) == 0 && ok;
    ESP_LOGI(TAG, "saved %d KB of the RAM disk%s", n * (int)(CY_DISK_CHUNK / 1024), ok ? "" : " (write error)");
}

void cybiko_app_save(bool force)
{
    (void)force;                                /* only changed parts are ever written */
    static uint8_t *flash_copy;
    uint32_t dirty[sizeof machine->disk_dirty / sizeof machine->disk_dirty[0]];
    bool disk_changed = false, fl = false;
    cpu_pause(true);
    /* A CyOS whose RAM disk could not be moved keeps it at the start of
     * main RAM: carry that into the disk image. */
    if (!machine->disk_patched) {
        for (uint32_t c = 0; c < CY_RAMDISK_SIZE / CY_DISK_CHUNK; c++) {
            uint32_t off = c * CY_DISK_CHUNK;
            if (memcmp(disk + off, ram + off, CY_DISK_CHUNK) == 0) continue;
            memcpy(disk + off, ram + off, CY_DISK_CHUNK);
            machine->disk_dirty[c / 32] |= 1u << (c % 32);
        }
    }
    memcpy(dirty, machine->disk_dirty, sizeof dirty);
    memset(machine->disk_dirty, 0, sizeof machine->disk_dirty);
    for (size_t i = 0; i < sizeof dirty / sizeof dirty[0]; i++) disk_changed |= dirty[i] != 0;
    /* The CPU stays paused while the disk is written so no chunk is caught
     * half changed; it is usually a few KB. */
    if (disk_changed) save_disk(dirty);
    if (machine->flash.dirty) {
        if (!flash_copy) flash_copy = heap_caps_malloc(CY_FLASH_SIZE, MALLOC_CAP_SPIRAM);
        if (flash_copy) {
            memcpy(flash_copy, flash, CY_FLASH_SIZE);
            cybiko_flash_original(machine, flash_copy);   /* the dump's own ID */
            machine->flash.dirty = false;
            fl = true;
        }
    }
    cpu_pause(false);
    if (fl) {
        ESP_LOGI(TAG, "saving reprogrammed flash");
        write_file(FLASH_SAVE_PATH, flash_copy, CY_FLASH_SIZE);
    }
    save_state();
}

/* ------------------------------------------------------------------ */
/* UI hooks                                                            */
/* ------------------------------------------------------------------ */

void cybiko_app_key(int key, bool down)
{
    key_event_t ev = { (int16_t)key, down };
    xQueueSend(key_queue, &ev, pdMS_TO_TICKS(50));
}

cybiko_t *cybiko_app_machine(void) { return machine; }

/* ------------------------------------------------------------------ */
/* Orientation                                                         */
/* ------------------------------------------------------------------ */

bool cybiko_app_portrait(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open("cybiko", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "portrait", &v);
        nvs_close(h);
    }
    return v != 0;
}

void cybiko_app_set_portrait(bool portrait)
{
    nvs_handle_t h;
    if (nvs_open("cybiko", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "portrait", portrait);
    nvs_commit(h);
    nvs_close(h);
}

bool cybiko_app_cart_in(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open("cybiko", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "cart", &v);
        nvs_close(h);
    }
    return v != 0;
}

void cybiko_app_set_cart_in(bool in)
{
    nvs_handle_t h;
    if (nvs_open("cybiko", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "cart", in);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Start                                                               */
/* ------------------------------------------------------------------ */

static bool load_file(const char *path, uint8_t *dst, size_t len, bool exact)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(dst, 1, len, f);
    fclose(f);
    if (exact && n != len) {
        ESP_LOGE(TAG, "%s: %u bytes, expected %u", path, (unsigned)n, (unsigned)len);
        return false;
    }
    return true;
}

static void fail(const char *msg)
{
    ESP_LOGE(TAG, "%s", msg);
    display_show_message(msg);
}

/* Copy the files in /Cybiko/apps that the RAM disk does not have yet. Files
 * already there are left alone, so CyOS's changes to them survive; delete a
 * file on the Cybiko (or ramdisk.bin) to have it copied again. */
static int install_apps(void)
{
    DIR *dir = opendir(APPS_DIR);
    if (!dir) return 0;
    int installed = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        const char *name = e->d_name;
        if (name[0] == '.' || cy_disk_find(disk, disk_size, name, NULL)) continue;
        char path[300];
        snprintf(path, sizeof path, "%s/%s", APPS_DIR, name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (strlen(name) > CY_DISK_NAME_MAX) {
            ESP_LOGW(TAG, "apps: %s: name longer than %d characters, skipped", name, CY_DISK_NAME_MAX);
            continue;
        }
        if ((uint32_t)st.st_size > cy_disk_free(disk, disk_size)) {
            ESP_LOGW(TAG, "apps: no room for %s (%ld bytes)", name, (long)st.st_size);
            continue;
        }
        uint8_t *buf = heap_caps_malloc(st.st_size ? st.st_size : 1, MALLOC_CAP_SPIRAM);
        if (!buf) { ESP_LOGW(TAG, "apps: no memory for %s", name); continue; }
        if (!installed) display_show_message("CYBIKO\n\nCOPYING APPS");
        if (load_file(path, buf, st.st_size, true)) {
            uint32_t date = (uint32_t)((int64_t)st.st_mtime + 2208988800LL);   /* 1970 -> 1900 */
            if (cy_disk_write(disk, disk_size, name, buf, (uint32_t)st.st_size, date) == 0) {
                ESP_LOGI(TAG, "apps: copied %s (%ld bytes)", name, (long)st.st_size);
                installed++;
            }
        }
        free(buf);
    }
    closedir(dir);
    return installed;
}

/* Allocate the RAM disk (disk.size MB, never smaller than an existing
 * ramdisk.bin) and load ramdisk.bin into it; a 512 KB one from before the
 * disk grew keeps its files. Returns false when out of memory. */
static bool load_disk(const char *cfg)
{
    char v[16];
    int mb = DISK_DEFAULT_MB;
    if (cfg && config_get_value(cfg, "disk.size", v, sizeof v)) mb = atoi(v);
    if (mb < 1) mb = 1;
    if (mb > (int)(CY_DISK_MAX >> 20)) mb = (int)(CY_DISK_MAX >> 20);
    uint32_t want = (uint32_t)mb << 20, have = 0;
    struct stat st;
    if (stat(RAMDISK_PATH, &st) == 0) have = (uint32_t)st.st_size;
    uint32_t min = (have + 0xffffu) & ~0xffffu;
    if (min > CY_DISK_MAX) min = CY_DISK_MAX;
    if (min < CY_RAMDISK_SIZE) min = CY_RAMDISK_SIZE;
    if (want < min) want = min;
    for (disk_size = want; disk_size >= min; disk_size -= 0x100000u) {
        disk = heap_caps_calloc(1, disk_size, MALLOC_CAP_SPIRAM);
        if (disk || disk_size - min < 0x100000u) break;
    }
    if (!disk && (disk = heap_caps_calloc(1, min, MALLOC_CAP_SPIRAM)) != NULL) disk_size = min;
    if (!disk) return false;
    if (disk_size < want) ESP_LOGW(TAG, "RAM disk: only %u KB of PSRAM free for it", (unsigned)(disk_size >> 10));
    if (have && load_file(RAMDISK_PATH, disk, have < disk_size ? have : disk_size, false))
        ESP_LOGI(TAG, "RAM disk restored (%u KB file)", (unsigned)(have >> 10));

    int copied = install_apps();
    /* Rewrite the whole file when it is new, has grown or got apps; later
     * saves only patch changed chunks into it. */
    if (copied || have != disk_size) {
        if (have != disk_size) display_show_message("CYBIKO\n\nPREPARING RAM DISK");
        write_file(RAMDISK_PATH, disk, disk_size);
    }
    ESP_LOGI(TAG, "RAM disk %u KB, %u KB free, %d app file(s) copied",
             (unsigned)(disk_size >> 10), (unsigned)(cy_disk_free(disk, disk_size) >> 10), copied);
    return true;
}

void cybiko_app_start(void)
{
    sdmmc_card_t *card;
    if (m5_sd_mount(&card) != ESP_OK) { fail("CYBIKO\n\nNO SD CARD"); return; }

    char rom_path[96], flash_path[96];
    if (!cybiko_app_find_files(rom_path, flash_path, sizeof rom_path)) {
        fail("CYBIKO\n\nPUT ROM AND FLASH\nIN /CYBIKO/");
        return;
    }

    /* Allocated here, not static, so Palm mode keeps that internal RAM. */
    machine = heap_caps_calloc(1, sizeof *machine, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!machine) machine = heap_caps_calloc(1, sizeof *machine, MALLOC_CAP_SPIRAM);
    rom = heap_caps_calloc(1, CY_ROM_BUF, MALLOC_CAP_SPIRAM);
    flash = heap_caps_malloc(CY_FLASH_SIZE, MALLOC_CAP_SPIRAM);
    ram = heap_caps_calloc(1, CY_RAM_SIZE, MALLOC_CAP_SPIRAM);
    if (!machine || !rom || !flash || !ram) { fail("CYBIKO\n\nOUT OF MEMORY"); return; }

    ESP_LOGI(TAG, "ROM %s, flash %s", rom_path, flash_path);
    if (!load_file(rom_path, rom, CY_ROM_SIZE, true)) { fail("CYBIKO\n\nBAD ROM FILE"); return; }
    if (!load_file(flash_path, flash, CY_FLASH_SIZE, true)) { fail("CYBIKO\n\nBAD FLASH FILE"); return; }

    struct { char magic[4]; uint32_t version; int64_t clock; } st;
    FILE *f = fopen(STATE_PATH, "rb");
    if (f) {
        if (fread(&st, 1, sizeof st, f) == sizeof st && !memcmp(st.magic, "CYST", 4))
            clock_base = st.clock - (int64_t)time(NULL);
        fclose(f);
    }

    /* Optional settings: sound.volume (0-100, default 60), disk.size and
     * the radio keys described in cybiko_link.h. */
    static EXT_RAM_BSS_ATTR char cfg[1024];
    size_t cfg_len;
    bool have_cfg = config_read_file(CFG_PATH, cfg, sizeof cfg, &cfg_len) == CONFIG_OK;
    if (!load_disk(have_cfg ? cfg : NULL)) { fail("CYBIKO\n\nOUT OF MEMORY"); return; }
    int volume = 60;
    char v[16];
    if (have_cfg && config_get_value(cfg, "sound.volume", v, sizeof v)) {
        volume = atoi(v);
        if (volume < 0 || volume > 100) volume = 60;
    }
    uint8_t saved_volume;                       /* the boot menu's volume wins */
    if (audio_saved_volume(&saved_volume)) volume = saved_volume;
    if (audio_init((uint8_t)volume) != ESP_OK) ESP_LOGW(TAG, "no sound");

    cy_host_t host = { .serial_tx = on_serial_tx, .wall_time = wall_time, .speaker = on_speaker,
                       .rf_send = cybiko_link_send };
    cybiko_init(machine, &host, rom, flash, ram);
    cybiko_set_disk(machine, disk, disk_size);
    memcpy(ram, disk, CY_RAMDISK_SIZE);         /* in case the disk cannot be moved */
    cybiko_link_init(machine, have_cfg ? cfg : NULL);
    cybiko_cart_init(machine, have_cfg ? cfg : NULL);

    key_queue = xQueueCreate(32, sizeof(key_event_t));
    cybiko_ui_start();
    xTaskCreatePinnedToCore(cpu_task, "cybiko", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "running; free PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
