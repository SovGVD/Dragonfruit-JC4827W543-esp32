/* The MP3 player cartridge (CY45P01) in Cybiko mode: the cartridge's SD
 * card is a folder on the board's SD card (shown to CyOS as a FAT16 card by
 * cy_vfat) or a disk image, and the MPEG stream CyOS
 * sends to the cartridge's decoder is decoded here (cy_mp3) and played on
 * the speaker. See cybiko_cart.h for the settings. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "cybiko_cart.h"
#include "cybiko_app.h"
#include "m5_sd.h"
#include "cy_mp3.h"
#include "cy_vfat.h"
#include "audio.h"
#include "palm_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "cybiko_cart";

#define CARD_DIR CYBIKO_SD_DIR "/card"
#define CARD_PATH CYBIKO_SD_DIR "/mp3card.img"
#define CARD_WRITES 1024            /* blocks of CyOS's writes kept in PSRAM */
#define MPEG_QUEUE 8192             /* bytes between the CPU task and the decoder */
#define DECODER_STACK (22 * 1024)   /* minimp3 keeps 16 KB of scratch on the stack */
#define BLOCK 512

static FILE *card;
static cy_vfat_t *card_dir;
static StreamBufferHandle_t mpeg;
static volatile uint8_t volume = 0x6a;

/* ---- the SD card (CPU task) ---- */

static bool card_read(void *user, uint32_t block, uint8_t *buf)
{
    (void)user;
    if (card_dir) return cy_vfat_read(card_dir, block, buf);
    return fseek(card, (long)block * BLOCK, SEEK_SET) == 0 && fread(buf, 1, BLOCK, card) == BLOCK;
}

static bool card_write(void *user, uint32_t block, const uint8_t *buf)
{
    (void)user;
    if (card_dir) return cy_vfat_write(card_dir, block, buf);
    bool ok = fseek(card, (long)block * BLOCK, SEEK_SET) == 0 && fwrite(buf, 1, BLOCK, card) == BLOCK;
    fflush(card);
    return ok;
}

/* ---- the decoder task ---- */

static void decoder_task(void *arg)
{
    cy_mp3_t *d = arg;
    static int16_t pcm[256];
    uint8_t in[512];
    for (;;) {
        /* Nothing left: let the speaker stop and wait for the next stream. */
        if (!cy_mp3_busy(d) && xStreamBufferIsEmpty(mpeg)) {
            audio_pcm_stop();
            size_t n = xStreamBufferReceive(mpeg, in, sizeof in, portMAX_DELAY);
            cy_mp3_feed(d, in, n);
        }
        size_t room = cy_mp3_space(d);
        while (room) {
            size_t n = xStreamBufferReceive(mpeg, in, room < sizeof in ? room : sizeof in, 0);
            if (!n) break;
            cy_mp3_feed(d, in, n);
            room -= n;
        }
        size_t real = cy_mp3_render(d, pcm, sizeof pcm / sizeof *pcm, AUDIO_RATE);
        if (!real) {
            /* Ran dry mid-stream: wait a little for more; if none comes,
             * play out what is left. */
            size_t n = xStreamBufferReceive(mpeg, in, sizeof in, pdMS_TO_TICKS(50));
            if (n) cy_mp3_feed(d, in, n);
            else cy_mp3_end(d);
            continue;
        }
        /* The player's volume, relative to its default (a guess at the
         * codec's scale: 1 dB a step). */
        uint8_t v = volume;
        int32_t gain = v ? (int32_t)(32768.0f * powf(10.0f, ((int)v - 0x6a) / 20.0f)) : 0;
        if (gain > 65536) gain = 65536;
        for (size_t i = 0; i < real; i++) {
            int32_t s = pcm[i] * gain >> 15;
            pcm[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        audio_pcm_write(pcm, sizeof pcm / sizeof *pcm);
    }
}

/* ---- set-up ---- */

void cybiko_cart_init(cybiko_t *m, const char *cfg)
{
    char v[16] = "auto";
    if (cfg) config_get_value(cfg, "cart", v, sizeof v);
    /* The folder is made on first start, to show where the files go. */
    mkdir(CARD_DIR, 0777);
    bool have_dir = false;
    DIR *dir = opendir(CARD_DIR);
    if (dir) {
        struct dirent *de;
        while (!have_dir && (de = readdir(dir)) != NULL) have_dir = de->d_name[0] != '.';
        closedir(dir);
    }
    struct stat st;
    bool have_image = !have_dir && stat(CARD_PATH, &st) == 0 && st.st_size >= 64 * BLOCK;
    bool on = !strcmp(v, "mp3") || (!strcmp(v, "auto") && (have_dir || have_image));
    if (!on) {
        if (have_dir || have_image) ESP_LOGI(TAG, "cartridge off (cart = %s)", v);
        return;
    }
    m->cart.present = true;
    if (have_dir && (card_dir = cy_vfat_open(CARD_DIR, CARD_WRITES)) != NULL) {
        m->host.card_read = card_read;
        m->host.card_write = card_write;
        m->cart.card_blocks = cy_vfat_blocks(card_dir);
        ESP_LOGI(TAG, "MP3 cartridge, card = %s: %u files on a %u MB disk", CARD_DIR,
                 (unsigned)cy_vfat_files(card_dir), (unsigned)(cy_vfat_blocks(card_dir) >> 11));
        if (cy_vfat_skipped(card_dir))
            ESP_LOGW(TAG, "%u files left out (over 1 GB, too many, or names too long)",
                     (unsigned)cy_vfat_skipped(card_dir));
    } else if (have_image && (card = fopen(CARD_PATH, "r+b")) != NULL) {
        m->host.card_read = card_read;
        m->host.card_write = card_write;
        m->cart.card_blocks = (uint32_t)(st.st_size / BLOCK);
        ESP_LOGI(TAG, "MP3 cartridge with a %u MB card (%s)", (unsigned)(st.st_size >> 20), CARD_PATH);
    } else {
        ESP_LOGI(TAG, "MP3 cartridge, no card (put files in %s)", CARD_DIR);
    }

    /* The decoder runs at playback speed on core 0, its stack in PSRAM. */
    static StaticStreamBuffer_t mpeg_struct;
    uint8_t *store = heap_caps_malloc(MPEG_QUEUE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cy_mp3_t *d = heap_caps_malloc(cy_mp3_size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!store || !d) { ESP_LOGW(TAG, "no memory for the decoder: no MP3 sound"); return; }
    mpeg = xStreamBufferCreateStatic(MPEG_QUEUE, 1, store, &mpeg_struct);
    cy_mp3_init(d);
    if (xTaskCreatePinnedToCoreWithCaps(decoder_task, "cy_mp3", DECODER_STACK, d, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "could not start the decoder: no MP3 sound");
        vStreamBufferDelete(mpeg);
        mpeg = NULL;
    }
}

void cybiko_cart_poll(cybiko_t *m)
{
    if (!mpeg) return;
    volume = cybiko_mp3_volume(m);
    uint8_t buf[256];
    size_t room = xStreamBufferSpacesAvailable(mpeg);
    while (room) {
        size_t n = cybiko_mp3_take(m, buf, room < sizeof buf ? room : sizeof buf);
        if (!n) break;
        xStreamBufferSend(mpeg, buf, n, 0);
        room -= n;
    }
}
