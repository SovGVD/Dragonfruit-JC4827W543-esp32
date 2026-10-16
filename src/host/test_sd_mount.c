/* Exercise dedicated SPI2 ownership and retry/remount behavior without
 * requiring display_init(). Only the ESP-IDF driver boundary is substituted. */
#include "m5_sd.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static int bus_calls, mount_calls, unmount_calls, fail_bus, fail_mounts;
static sdmmc_card_t card;
static int reject_crc;

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    assert(cfg->pin_bit_mask == ((1ULL << 10) | (1ULL << 38)));
    assert(cfg->mode == GPIO_MODE_OUTPUT);
    return ESP_OK;
}
int gpio_set_level(int pin, int level)
{
    assert((pin == 10 || pin == 38) && level == 1);
    return ESP_OK;
}

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma)
{
    bus_calls++;
    assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    assert(cfg->mosi_io_num == 11 && cfg->miso_io_num == 13 &&
           cfg->sclk_io_num == 12 && cfg->quadwp_io_num == -1 && cfg->quadhd_io_num == -1);
    return fail_bus ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_vfs_fat_sdspi_mount(const char *path, const sdmmc_host_t *host,
    const sdspi_device_config_t *slot, const esp_vfs_fat_sdmmc_mount_config_t *cfg,
    sdmmc_card_t **out)
{
    assert(!strcmp(path, "/sdcard"));
    assert(host->slot == SPI2_HOST && host->max_freq_khz == 10000);
    assert(slot->gpio_cs == 10 && slot->host_id == SPI2_HOST);
    assert(!cfg->format_if_mount_failed && cfg->max_files == 12);
    assert(!card.mounted);
    mount_calls++;
    if (reject_crc && !(host->flags & SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC))
        return ESP_ERR_NOT_SUPPORTED;
    if (!reject_crc) assert(!(host->flags & SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC));
    if (fail_mounts) { fail_mounts--; return ESP_FAIL; }
    card.mounted = 1;
    *out = &card;
    return ESP_OK;
}
void esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *c)
{
    assert(!strcmp(path, "/sdcard") && c == &card && card.mounted);
    card.mounted = 0;
    unmount_calls++;
}
void vTaskDelay(int ticks) { assert(ticks == 40); }
esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    assert(!strcmp(path, "/sdcard"));
    *total = 8000000000ull; *free_bytes = 2000000000ull;
    return ESP_OK;
}

int main(void)
{
    sdmmc_card_t *out = (sdmmc_card_t *)1;
    fail_bus = 1;
    assert(m5_sd_mount(&out) == ESP_FAIL && !out && !mount_calls);
    fail_bus = 0; fail_mounts = 4;
    assert(m5_sd_mount(&out) == ESP_FAIL && !out && bus_calls == 2 && mount_calls == 4);
    fail_mounts = 2;
    assert(m5_sd_mount(&out) == ESP_OK && out == &card && mount_calls == 7);
    sdmmc_card_t *shared = NULL;
    assert(m5_sd_mount(&shared) == ESP_OK && shared == out && mount_calls == 7);
    m5_sd_unmount(shared);
    assert(card.mounted && !unmount_calls);
    m5_sd_unmount(out);
    assert(m5_sd_mount(&out) == ESP_OK && out == &card);
    m5_sd_unmount(out);
    assert(bus_calls == 2 && mount_calls == 8 && unmount_calls == 2);
    reject_crc = 1;
    assert(m5_sd_mount(&out) == ESP_OK && out == &card);
    m5_sd_unmount(out);
    assert(bus_calls == 2 && mount_calls == 10 && unmount_calls == 3);
    uint32_t used, total;
    assert(m5_sd_space(&used, &total) == ESP_OK && total == UINT32_MAX && used == UINT32_MAX - 2000000000u);
    puts("ALL SD MOUNT TESTS PASSED");
    return 0;
}
