#include "rom_loader.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "m5_sd.h"

#include "bus.h"   /* BUS_ROM_BASE, BUS_RAM_SIZE, BUS_ROM_MAX_SIZE */

static const char *TAG = "rom_loader";

#define ROM_PATH PALM_SD_DIR "/palm.rom"

/* A IIIx ROM is ~2 MB; anything wildly off this is not a ROM we can run. The
 * upper bound is the bus's ROM window size. */
#define ROM_MIN_SIZE 0x40000u            /* 256 KB floor — reject stray files */
#define ROM_MAX_SIZE BUS_ROM_MAX_SIZE    /* 2 MB — the mapped ROM window       */

/* be32() (big-endian reset-vector read) comes from bus.h. */

rom_load_result_t rom_load_from_sd(uint8_t **out_data, size_t *out_size)
{
    *out_data = NULL;
    *out_size = 0;

    /* The board SD component owns bus wiring, initialization and retries.
     * On JC4827W543 this uses SPI2 shared with resistive touch; the display
     * runs independently on SPI3. */
    sdmmc_card_t *card = NULL;
    esp_err_t err = m5_sd_mount(&card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return ROM_LOAD_NO_SD;
    }
    ESP_LOGI(TAG, "SD mounted, reading %s", ROM_PATH);

    rom_load_result_t result = ROM_LOAD_OK;
    uint8_t *buf = NULL;

    FILE *f = fopen(ROM_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", ROM_PATH);
        result = ROM_LOAD_NO_FILE;
        goto cleanup;
    }

    long sz;
    struct stat st;
    if (stat(ROM_PATH, &st) == 0) {
        sz = (long)st.st_size;
    } else {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
    }

    if (sz < (long)ROM_MIN_SIZE || sz > (long)ROM_MAX_SIZE) {
        ESP_LOGE(TAG, "ROM size %ld out of range [%u, %u]",
                 sz, (unsigned)ROM_MIN_SIZE, (unsigned)ROM_MAX_SIZE);
        result = ROM_LOAD_BAD_ROM;
        fclose(f);
        goto cleanup;
    }

    buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM alloc of %ld bytes failed", sz);
        result = ROM_LOAD_BAD_ROM;
        fclose(f);
        goto cleanup;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        ESP_LOGE(TAG, "short read: %u of %ld bytes", (unsigned)rd, sz);
        result = ROM_LOAD_BAD_ROM;
        goto cleanup;
    }

    /* Sanity-check the 68k reset vector at file offset 0/4 (big-endian): a IIIx
     * ROM puts the initial SSP in low RAM and the reset PC inside the ROM
     * window (file byte 0 maps to BUS_ROM_BASE). We don't pin the exact PC so
     * different 3.x point releases still load; we just confirm it's a ROM with
     * a IIIx-shaped reset vector rather than e.g. a text file named palm.rom. */
    uint32_t ssp = be32(buf);
    uint32_t pc  = be32(buf + 4);
    bool pc_in_rom = (pc >= BUS_ROM_BASE) && (pc < BUS_ROM_BASE + (uint32_t)sz);
    if (ssp == 0 || ssp > BUS_RAM_SIZE || !pc_in_rom) {
        ESP_LOGE(TAG, "bad reset vector SSP=%08lX PC=%08lX (not a Palm IIIx ROM?)",
                 (unsigned long)ssp, (unsigned long)pc);
        result = ROM_LOAD_BAD_ROM;
        goto cleanup;
    }

    ESP_LOGI(TAG, "ROM loaded: %ld bytes, reset SSP=%08lX PC=%08lX",
             sz, (unsigned long)ssp, (unsigned long)pc);
    *out_data = buf;
    *out_size = (size_t)sz;
    buf = NULL;   /* ownership transferred to caller */

cleanup:
    if (buf) heap_caps_free(buf);
    m5_sd_unmount(card);
    ESP_LOGI(TAG, "SD unmounted");
    return result;
}
