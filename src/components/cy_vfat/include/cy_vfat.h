#pragma once

/* A folder shown as an SD card: the blocks of a FAT16 disk (MBR, boot
 * sector, FATs, directories, file data) are made up on the fly from the
 * folder's files and subfolders, so files dropped into the folder on a PC
 * appear on the emulated card without any disk image.
 *
 * Every file is laid out in consecutive clusters in the order the folder is
 * scanned (once, when the card is opened). Long file names are included.
 * Writes from the emulated machine are kept in memory and override the
 * made-up blocks. cy_vfat_sync() carries what they did (files written,
 * deleted, renamed, folders made) over to the folder.
 *
 * Not thread safe. */

#include <stdbool.h>
#include <stdint.h>

typedef struct cy_vfat cy_vfat_t;

/* Scan `dir` (subfolders too). `max_written` is how many 512-byte blocks
 * of writes to keep in memory. Returns NULL if `dir` cannot be read. */
cy_vfat_t *cy_vfat_open(const char *dir, uint32_t max_written);
void cy_vfat_close(cy_vfat_t *v);

/* The disk's size in 512-byte blocks. */
uint32_t cy_vfat_blocks(const cy_vfat_t *v);

/* What the scan found: files laid out, and files left out (too many, too
 * big, or too deep). */
uint32_t cy_vfat_files(const cy_vfat_t *v);
uint32_t cy_vfat_skipped(const cy_vfat_t *v);

bool cy_vfat_read(cy_vfat_t *v, uint32_t block, uint8_t *buf);
bool cy_vfat_write(cy_vfat_t *v, uint32_t block, const uint8_t *buf);

/* What one cy_vfat_sync() did to the folder. */
typedef struct {
    uint32_t written;           /* files written (new or changed) */
    uint32_t removed;           /* files and folders deleted */
    uint32_t renamed;
    uint32_t made;              /* folders made */
    uint32_t failed;            /* changes that could not be made */
} cy_vfat_sync_t;

/* Read the card's file system as the machine left it and apply its changes
 * since the last sync to the folder. Only folders and files whose blocks
 * were written are looked at, so files the machine did not touch stay as
 * they are. Returns false if nothing was written since the last sync. */
bool cy_vfat_sync(cy_vfat_t *v, cy_vfat_sync_t *out);
