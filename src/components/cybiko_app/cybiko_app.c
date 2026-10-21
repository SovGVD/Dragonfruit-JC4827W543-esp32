/* Cybiko Xtreme mode: loads the ROM and CyOS flash from /Cybiko, runs the
 * machine in a CPU task on core 1 and the screen, on-screen keyboard and
 * saving in a UI task on core 0.
 *
 * Files in /Cybiko (see cybiko_app.h for the accepted ROM/flash names):
 *   ramdisk.bin  the 512 KB battery-backed RAM disk (user files); written
 *                when it changes
 *   flash.bin    written only if CyOS reprograms its flash; it is the first
 *                flash name searched, so it then replaces the original image
 *   state.bin    the Cybiko clock, so the time survives restarts
 *   cybiko.cfg   optional settings: sound.volume = 0-100 (default 60) and
 *                the radio keys described in cybiko_link.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cybiko_app.h"
#include "cybiko_ui.h"
#include "cybiko_link.h"
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

static const char *TAG = "cybiko";

#define RAMDISK_PATH CYBIKO_SD_DIR "/ramdisk.bin"
#define FLASH_SAVE_PATH CYBIKO_SD_DIR "/flash.bin"
#define STATE_PATH CYBIKO_SD_DIR "/state.bin"
#define CFG_PATH CYBIKO_SD_DIR "/cybiko.cfg"

#define CHUNK_STATES (CY_CLOCK_HZ / 1000)   /* run 1 ms of Cybiko time at a go */

static cybiko_t machine;
static uint8_t *rom, *flash, *ram;
static uint8_t *ramdisk_saved;              /* RAM disk as last written to SD */
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
    /* The boot loader and CyOS print text on SCI1, and SCI2 is a debug port
     * too: log both a line at a time. SCI0 and SCI1 also go to the radio
     * bridge when it carries that port. */
    if (port == 1 || port == 2) {
        static char line[2][128];
        static size_t len[2];
        char *l = line[port - 1];
        size_t *n = &len[port - 1];
        if (b == '\n' || b == '\r' || *n == sizeof line[0] - 1) {
            if (*n) {
                l[*n] = 0;
                ESP_LOGI("cyos", "%s", l);
                *n = 0;
            }
        }
        if (b >= 0x20 && b < 0x7f) l[(*n)++] = (char)b;
    }
    if (port != 2) cybiko_link_serial_tx(port, b);
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
    uint64_t c0 = machine.cpu.cycles;
    int64_t last_yield = t0;
    for (;;) {
        if (pause_req) {
            paused = true;
            while (pause_req) vTaskDelay(1);
            paused = false;
            t0 = esp_timer_get_time();
            c0 = machine.cpu.cycles;
        }
        key_event_t ev;
        while (xQueueReceive(key_queue, &ev, 0) == pdTRUE) cybiko_key(&machine, ev.key, ev.down);
        cybiko_link_poll(&machine);

        cybiko_run(&machine, CHUNK_STATES);

        int64_t now = esp_timer_get_time();
        int64_t emu_us = (int64_t)((machine.cpu.cycles - c0) * 1000000ull / CY_CLOCK_HZ);
        int64_t ahead = emu_us - (now - t0);
        if (ahead > 2000) {
            vTaskDelay(pdMS_TO_TICKS(ahead / 1000));
            last_yield = esp_timer_get_time();
        } else {
            if (ahead < -200000) {               /* running slow: don't try to catch up */
                t0 = now;
                c0 = machine.cpu.cycles;
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

void cybiko_app_save(bool force)
{
    static uint8_t *flash_copy;
    bool disk = false, fl = false;
    cpu_pause(true);
    if (force || memcmp(ramdisk_saved, ram, CY_RAMDISK_SIZE) != 0) {
        memcpy(ramdisk_saved, ram, CY_RAMDISK_SIZE);
        disk = true;
    }
    if (machine.flash.dirty) {
        if (!flash_copy) flash_copy = heap_caps_malloc(CY_FLASH_SIZE, MALLOC_CAP_SPIRAM);
        if (flash_copy) {
            memcpy(flash_copy, flash, CY_FLASH_SIZE);
            machine.flash.dirty = false;
            fl = true;
        }
    }
    cpu_pause(false);
    if (disk) {
        ESP_LOGI(TAG, "saving RAM disk");
        write_file(RAMDISK_PATH, ramdisk_saved, CY_RAMDISK_SIZE);
    }
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

cybiko_t *cybiko_app_machine(void) { return &machine; }

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

void cybiko_app_start(void)
{
    sdmmc_card_t *card;
    if (m5_sd_mount(&card) != ESP_OK) { fail("CYBIKO\n\nNO SD CARD"); return; }

    char rom_path[96], flash_path[96];
    if (!cybiko_app_find_files(rom_path, flash_path, sizeof rom_path)) {
        fail("CYBIKO\n\nPUT ROM AND FLASH\nIN /CYBIKO/");
        return;
    }

    rom = heap_caps_calloc(1, CY_ROM_BUF, MALLOC_CAP_SPIRAM);
    flash = heap_caps_malloc(CY_FLASH_SIZE, MALLOC_CAP_SPIRAM);
    ram = heap_caps_calloc(1, CY_RAM_SIZE, MALLOC_CAP_SPIRAM);
    ramdisk_saved = heap_caps_calloc(1, CY_RAMDISK_SIZE, MALLOC_CAP_SPIRAM);
    if (!rom || !flash || !ram || !ramdisk_saved) { fail("CYBIKO\n\nOUT OF MEMORY"); return; }

    ESP_LOGI(TAG, "ROM %s, flash %s", rom_path, flash_path);
    if (!load_file(rom_path, rom, CY_ROM_SIZE, true)) { fail("CYBIKO\n\nBAD ROM FILE"); return; }
    if (!load_file(flash_path, flash, CY_FLASH_SIZE, true)) { fail("CYBIKO\n\nBAD FLASH FILE"); return; }
    if (load_file(RAMDISK_PATH, ram, CY_RAMDISK_SIZE, false))
        ESP_LOGI(TAG, "RAM disk restored");
    memcpy(ramdisk_saved, ram, CY_RAMDISK_SIZE);

    struct { char magic[4]; uint32_t version; int64_t clock; } st;
    FILE *f = fopen(STATE_PATH, "rb");
    if (f) {
        if (fread(&st, 1, sizeof st, f) == sizeof st && !memcmp(st.magic, "CYST", 4))
            clock_base = st.clock - (int64_t)time(NULL);
        fclose(f);
    }

    /* Optional settings: sound.volume (0-100, default 60) and the radio
     * keys described in cybiko_link.h. */
    static char cfg[1024];
    size_t cfg_len;
    bool have_cfg = config_read_file(CFG_PATH, cfg, sizeof cfg, &cfg_len) == CONFIG_OK;
    int volume = 60;
    char v[16];
    if (have_cfg && config_get_value(cfg, "sound.volume", v, sizeof v)) {
        volume = atoi(v);
        if (volume < 0 || volume > 100) volume = 60;
    }
    if (audio_init((uint8_t)volume) != ESP_OK) ESP_LOGW(TAG, "no sound");

    cy_host_t host = { .serial_tx = on_serial_tx, .wall_time = wall_time, .speaker = on_speaker };
    cybiko_init(&machine, &host, rom, flash, ram);
    cybiko_link_init(have_cfg ? cfg : NULL);

    key_queue = xQueueCreate(32, sizeof(key_event_t));
    cybiko_ui_start();
    xTaskCreatePinnedToCore(cpu_task, "cybiko", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "running; free PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
