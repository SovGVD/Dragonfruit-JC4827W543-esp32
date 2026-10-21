#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Cybiko Xtreme mode of the firmware: everything it reads or writes lives in
 * CYBIKO_SD_DIR (/Cybiko on the SD card). */

/* Find the boot ROM and CyOS flash image in /Cybiko (SD must be mounted).
 * Accepted names, first match wins:
 *   boot ROM (32 KB):  bootrom.bin, cyrom150.bin, xtreme-rom.bin
 *   flash   (512 KB):  flash.bin,   cyos_v1508.bin, xtreme-flash.bin
 * Either output may be NULL. Returns true when both files exist. */
bool cybiko_app_find_files(char *rom_path, char *flash_path, size_t cap);

/* Load the images, start the emulator tasks and return. display_init() and
 * input_init() must already have run. On a fatal error the reason is shown
 * on the panel and the function returns without starting anything. */
void cybiko_app_start(void);
