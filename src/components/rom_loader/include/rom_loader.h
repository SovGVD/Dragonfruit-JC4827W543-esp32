#pragma once

#include <stddef.h>
#include <stdint.h>

/* Outcome of trying to load the Palm ROM off the SD card. The caller maps each
 * failure to a distinct on-screen message before halting. */
typedef enum {
    ROM_LOAD_OK = 0,
    ROM_LOAD_NO_SD,     /* SD card absent or the FAT volume would not mount   */
    ROM_LOAD_NO_FILE,   /* SD mounted but /palm.rom is missing / unopenable   */
    ROM_LOAD_BAD_ROM,   /* file present but wrong size, not a IIIx image, or  */
                        /* a read / PSRAM-allocation error                    */
} rom_load_result_t;

/* Mount the M5Paper microSD, read /palm.rom into a freshly heap_caps_malloc'd
 * PSRAM buffer, sanity-check it, then unmount. On ROM_LOAD_OK the caller owns
 * *out_data (PSRAM) and passes it straight to bus_init(); *out_size is the file
 * size. On any failure *out_data is NULL and nothing is left mounted.
 *
 * The SD shares SPI3_HOST with the IT8951 display, so display_init() (which
 * calls spi_bus_initialize) MUST have run first — this function only adds an
 * sdspi device to the existing bus and removes it again before returning, so
 * the display keeps sole use of the bus during emulation. */
rom_load_result_t rom_load_from_sd(uint8_t **out_data, size_t *out_size);
