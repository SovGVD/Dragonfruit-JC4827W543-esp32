#pragma once
#include "esp_err.h"
typedef struct { int mounted; } sdmmc_card_t;
#define M5_SD_MOUNT_POINT TEST_SD_DIR
esp_err_t m5_sd_mount(sdmmc_card_t **card);
void m5_sd_unmount(sdmmc_card_t *card);
