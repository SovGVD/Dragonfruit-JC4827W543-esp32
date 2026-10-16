#include "m5_sd.h"
#include "board.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "m5_sd";

/* JC4827W543: SD and XPT2046 share SPI2, independent of the SPI3 display.
 * The original target shares its display SPI3 bus instead. */
#if CONFIG_BOARD_JC4827W543
#define SD_HOST SPI2_HOST
#else
#define SD_HOST SPI3_HOST
#endif
#define SD_CS_GPIO BOARD_SD_CS

#if CONFIG_BOARD_JC4827W543
static bool bus_ready;
static sdmmc_card_t *mounted_card;
static unsigned mount_refs;

/* Own the shared SD/touch bus for the lifetime of the application. Unmounting
 * removes only the card device, so later config/install/backup mounts work. */
esp_err_t m5_sd_bus_init(void)
{
    if (bus_ready) return ESP_OK;
    /* Both slaves must be deselected before the first SD command. The
     * resistive panel otherwise may drive MISO during card initialization. */
    const gpio_config_t cs = {
        .pin_bit_mask = (1ULL << BOARD_SD_CS) | (1ULL << BOARD_TOUCH_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(BOARD_SD_CS, 1);
    gpio_set_level(BOARD_TOUCH_CS, 1);
    esp_err_t gpio_err = gpio_config(&cs);
    if (gpio_err != ESP_OK) return gpio_err;
    const spi_bus_config_t cfg = {
        .mosi_io_num = BOARD_SD_MOSI,
        .miso_io_num = BOARD_SD_MISO,
        .sclk_io_num = BOARD_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_HOST, &cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) bus_ready = true;
    return err;
}
#endif

#define SD_MOUNT_ATTEMPTS 4
#define SD_RETRY_DELAY_MS 40

esp_err_t m5_sd_mount(sdmmc_card_t **out_card)
{
    if (out_card) *out_card = NULL;

#if CONFIG_BOARD_JC4827W543
    if (mounted_card) {
        mount_refs++;
        if (out_card) *out_card = mounted_card;
        return ESP_OK;
    }
    esp_err_t bus_err = m5_sd_bus_init();
    if (bus_err != ESP_OK) return bus_err;
#endif
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
#if CONFIG_BOARD_JC4827W543
    host.max_freq_khz = 10000;  /* same conservative 10 MHz as esp32-pipboy */
#endif

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_GPIO;
    slot.host_id = SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
#if CONFIG_BOARD_JC4827W543
        .max_files              = 12,
#else
        .max_files              = 2,
#endif
        .allocation_unit_size   = 0,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < SD_MOUNT_ATTEMPTS; attempt++) {
        err = esp_vfs_fat_sdspi_mount(M5_SD_MOUNT_POINT, &host, &slot, &mcfg, &card);
        if (err == ESP_OK) break;
#if CONFIG_BOARD_JC4827W543
        /* Some SPI cards reject CMD59 (enable optional data CRC), including
         * the card observed during JC4827W543 bring-up. Retry using IDF's
         * supported CRC-off mode, as the working Arduino SD stack does. */
        if (err == ESP_ERR_NOT_SUPPORTED &&
            !(host.flags & SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC)) {
            ESP_LOGW(TAG, "SD rejected CRC initialization; retrying without optional SPI data CRC");
            host.flags |= SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC;
        }
#endif
        ESP_LOGW(TAG, "SD mount attempt %d/%d failed: %s",
                 attempt + 1, SD_MOUNT_ATTEMPTS, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
    }
    if (err != ESP_OK) return err;

#if CONFIG_BOARD_JC4827W543
    mounted_card = card;
    mount_refs = 1;
#endif
    if (out_card) *out_card = card;
    return ESP_OK;
}

void m5_sd_unmount(sdmmc_card_t *card)
{
#if CONFIG_BOARD_JC4827W543
    if (!mounted_card || card != mounted_card || !mount_refs) return;
    if (--mount_refs) return;
    mounted_card = NULL;
#endif
    esp_vfs_fat_sdcard_unmount(M5_SD_MOUNT_POINT, card);
}

esp_err_t m5_sd_space(uint32_t *used, uint32_t *total)
{
    uint64_t bytes, free_bytes;
    esp_err_t err = esp_vfs_fat_info(M5_SD_MOUNT_POINT, &bytes, &free_bytes);
    if (err != ESP_OK) return err;
    *total = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
    *used = free_bytes >= *total ? 0 : *total - (uint32_t)free_bytes;
    return ESP_OK;
}
