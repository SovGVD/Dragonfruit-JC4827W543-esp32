#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
typedef struct { bool format_if_mount_failed; int max_files; size_t allocation_unit_size; }
    esp_vfs_fat_sdmmc_mount_config_t;
esp_err_t esp_vfs_fat_sdspi_mount(const char *path, const sdmmc_host_t *host,
    const sdspi_device_config_t *slot, const esp_vfs_fat_sdmmc_mount_config_t *cfg,
    sdmmc_card_t **card);
void esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card);
