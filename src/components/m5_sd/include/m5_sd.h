#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

/* ------------------------------------------------------------------ *
 *  M5Paper microSD mount helper — the single owner of the SD wiring.
 *
 *  The microSD shares the e-ink IT8951 SPI bus (SPI3_HOST, CS GPIO4).
 *  display_init()'s spi_bus_initialize() must run first; these helpers only
 *  ADD an sdspi device to that already-initialized bus and remove it again, so
 *  the display keeps sole use of the bus during emulation. Both rom_loader and
 *  config use this so the mount sequence (and its retry) lives in one place.
 * ------------------------------------------------------------------ */

/* FAT mount point shared by every SD user; build paths as M5_SD_MOUNT_POINT "/file". */
#define M5_SD_MOUNT_POINT "/sdcard"

/* Mount the microSD at M5_SD_MOUNT_POINT. The FIRST sdspi transaction after the
 * IT8951 has been driving the shared bus intermittently CRC-fails until the
 * bus/card settle, so this retries a few times before giving up. On ESP_OK
 * *out_card holds the mounted card (hand it back to m5_sd_unmount); on failure
 * nothing is left mounted and the last error is returned. */
esp_err_t m5_sd_mount(sdmmc_card_t **out_card);

/* Unmount the card from m5_sd_mount and remove the sdspi device, returning the
 * bus to the display. */
void m5_sd_unmount(sdmmc_card_t *card);
