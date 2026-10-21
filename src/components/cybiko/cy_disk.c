/* Bigger RAM disk for CyOS, and reading/writing files in a RAM disk image.
 *
 * The RAM disk is a row of 258-byte pages from offset 0: a 16-bit checksum,
 * then a 256-byte block. The first CY_BOOT_PAGES pages are not used for
 * files. A file block is
 *
 *   0     flags, bit 7 set when the block is in use
 *   1     number of data bytes in this block
 *   2-3   file id (big endian, the same in every block of a file): the
 *         number of its first block, counted from the first file page
 *   4-5   part number, 0 for the first block
 *   part 0 only:
 *   6     attributes
 *   7     file name, NUL terminated
 *   45    7b in every file CyOS 1.5.08 wrote (meaning unknown)
 *   46-47 parent folder id, ffff at the top level
 *   4a-4d date, seconds since 1900-01-01 (big endian)
 *   4e    data (up to 178 bytes); later parts carry data from byte 6
 *
 * This matches MAME's imgtool "cybikoxt" module (Tim Schuerewegen), checked
 * against a RAM disk CyOS 1.5.08 wrote. CyOS rebuilds its tables by scanning
 * the pages at start-up, so files written here while the machine is stopped
 * show up on the next boot. */
#include <string.h>
#include "cy_internal.h"

#define PAGE 258u
#define CY_BOOT_PAGES 5u
#define HDR 0x4eu                   /* data offset in a part-0 block */
#define FIRST_DATA (256u - HDR)     /* 178 */
#define NEXT_DATA (256u - 6u)       /* 250 */
#define ATTR_FILE 0x20

static uint16_t page_sum(const uint8_t *b)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < 256; i++) {
        v = (v ^ b[i] ^ i) << 1;
        v |= (v >> 16) & 1;
    }
    return (uint16_t)v;
}

static uint32_t page_count(uint32_t size) { return size / PAGE; }
static uint8_t *block(uint8_t *disk, uint32_t p) { return disk + p * PAGE + 2; }
static const uint8_t *cblock(const uint8_t *disk, uint32_t p) { return disk + p * PAGE + 2; }
static uint16_t be16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }

static bool used(const uint8_t *disk, uint32_t p)
{
    const uint8_t *b = cblock(disk, p);
    return (b[0] & 0x80) && be16(b - 2) == page_sum(b);
}

/* First block of the file called `name`, or 0 (never a file page). */
static uint32_t find_first(const uint8_t *disk, uint32_t size, const char *name)
{
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size); p++) {
        const uint8_t *b = cblock(disk, p);
        if (used(disk, p) && be16(b + 4) == 0 &&
            strncmp((const char *)b + 7, name, HDR - 4 - 7) == 0)
            return p;
    }
    return 0;
}

static uint32_t file_size(const uint8_t *disk, uint32_t size, uint16_t id)
{
    uint32_t n = 0;
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size); p++)
        if (used(disk, p) && be16(cblock(disk, p) + 2) == id) n += cblock(disk, p)[1];
    return n;
}

static void fill_info(const uint8_t *disk, uint32_t size, uint32_t p, cy_disk_file_t *f)
{
    const uint8_t *b = cblock(disk, p);
    memset(f, 0, sizeof *f);
    strncpy(f->name, (const char *)b + 7, CY_DISK_NAME_MAX);
    f->id = be16(b + 2);
    f->size = file_size(disk, size, f->id);
}

void cy_disk_list(const uint8_t *disk, uint32_t size,
                  bool (*fn)(void *user, const cy_disk_file_t *f), void *user)
{
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size); p++) {
        if (!used(disk, p) || be16(cblock(disk, p) + 4) != 0) continue;
        cy_disk_file_t f;
        fill_info(disk, size, p, &f);
        if (!fn(user, &f)) return;
    }
}

bool cy_disk_find(const uint8_t *disk, uint32_t size, const char *name, cy_disk_file_t *out)
{
    uint32_t p = find_first(disk, size, name);
    if (!p) return false;
    if (out) fill_info(disk, size, p, out);
    return true;
}

static uint32_t bytes_in(uint32_t pages)
{
    return pages ? FIRST_DATA + (pages - 1) * NEXT_DATA : 0;
}

static uint32_t pages_for(uint32_t len)
{
    return len <= FIRST_DATA ? 1 : 1 + (len - FIRST_DATA + NEXT_DATA - 1) / NEXT_DATA;
}

uint32_t cy_disk_free(const uint8_t *disk, uint32_t size)
{
    uint32_t n = 0;
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size); p++)
        if (!used(disk, p)) n++;
    return bytes_in(n);
}

static void erase_id(uint8_t *disk, uint32_t size, uint16_t id)
{
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size); p++)
        if (used(disk, p) && be16(block(disk, p) + 2) == id) memset(disk + p * PAGE, 0, PAGE);
}

bool cy_disk_delete(uint8_t *disk, uint32_t size, const char *name)
{
    uint32_t p = find_first(disk, size, name);
    if (!p) return false;
    erase_id(disk, size, be16(block(disk, p) + 2));
    return true;
}

int cy_disk_write(uint8_t *disk, uint32_t size, const char *name,
                  const uint8_t *data, uint32_t len, uint32_t date)
{
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > CY_DISK_NAME_MAX) return -2;

    /* Room: free pages plus those of a file this one replaces. */
    uint32_t old = find_first(disk, size, name);
    uint16_t old_id = old ? be16(block(disk, old) + 2) : 0;
    uint32_t avail = 0;
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size); p++)
        if (!used(disk, p) || (old && be16(cblock(disk, p) + 2) == old_id)) avail++;
    uint32_t need = pages_for(len);
    if (need > avail) return -1;
    if (old) erase_id(disk, size, old_id);

    /* The id is the number of the first block (CyOS finds files by it). */
    uint32_t first = CY_BOOT_PAGES;
    while (used(disk, first)) first++;
    uint16_t id = (uint16_t)(first - CY_BOOT_PAGES);

    uint32_t part = 0, done = 0;
    for (uint32_t p = CY_BOOT_PAGES; p < page_count(size) && part < need; p++) {
        if (used(disk, p)) continue;
        uint8_t *b = block(disk, p);
        memset(b, 0, 256);
        uint32_t off = part ? 6 : HDR;
        uint32_t n = len - done < 256 - off ? len - done : 256 - off;
        b[0] = 0x80;
        b[1] = (uint8_t)n;
        b[2] = (uint8_t)(id >> 8); b[3] = (uint8_t)id;
        b[4] = (uint8_t)(part >> 8); b[5] = (uint8_t)part;
        if (!part) {
            b[6] = ATTR_FILE;
            memcpy(b + 7, name, nlen);
            b[0x45] = 0x7b;                 /* as in every file CyOS writes */
            b[0x46] = b[0x47] = 0xff;       /* parent folder: none (top level) */
            b[0x4a] = (uint8_t)(date >> 24); b[0x4b] = (uint8_t)(date >> 16);
            b[0x4c] = (uint8_t)(date >> 8);  b[0x4d] = (uint8_t)date;
        }
        if (n) memcpy(b + off, data + done, n);
        uint16_t sum = page_sum(b);
        b[-2] = (uint8_t)(sum >> 8); b[-1] = (uint8_t)sum;
        done += n;
        part++;
    }
    return 0;
}

int32_t cy_disk_read(const uint8_t *disk, uint32_t size, const char *name,
                     uint8_t *buf, uint32_t cap)
{
    uint32_t first = find_first(disk, size, name);
    if (!first) return -1;
    uint16_t id = be16(cblock(disk, first) + 2);
    uint32_t pos = 0;
    /* Parts can sit in any order on the disk; collect them by number. */
    for (uint32_t part = 0;; part++) {
        uint32_t p;
        for (p = CY_BOOT_PAGES; p < page_count(size); p++) {
            const uint8_t *b = cblock(disk, p);
            if (used(disk, p) && be16(b + 2) == id && be16(b + 4) == part) break;
        }
        if (p == page_count(size)) break;
        const uint8_t *b = cblock(disk, p);
        uint32_t off = part ? 6 : HDR, n = b[1];
        if (n > 256 - off) n = 256 - off;
        if (pos + n > cap) n = cap > pos ? cap - pos : 0;
        memcpy(buf + pos, b + off, n);
        pos += n;
    }
    return (int32_t)pos;
}

/* ------------------------------------------------------------------ */
/* Moving the RAM disk                                                 */
/* ------------------------------------------------------------------ */

/* CyOS 1.5.08 creates its RAM disk driver with
 *     push.l #2000 (blocks); push.l #256 (block size)
 *     mov.l #0x80000,er2 (size); mov.l #0x400000,er1 (base); jsr ...
 * The driver uses min(blocks + 5, size / 258) pages, so a large block count
 * lets the size decide. */
static const uint8_t ramdisk_ctor[] = {
    0x7a, 0x02, 0x00, 0x00, 0x07, 0xd0,     /* mov.l #2000,er2 */
    0x01, 0x00, 0x6d, 0xf2,                 /* push.l er2 */
    0x79, 0x02, 0x01, 0x00,                 /* mov.w #256,r2 */
    0x01, 0x00, 0x6d, 0xf2,                 /* push.l er2 */
    0x7a, 0x02, 0x00, 0x08, 0x00, 0x00,     /* mov.l #0x80000,er2 */
    0x7a, 0x01, 0x00, 0x40, 0x00, 0x00,     /* mov.l #0x400000,er1 */
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void patch_kernel(cybiko_t *m)
{
    uint8_t *ram = m->ram;
    for (uint32_t i = 0; i + sizeof ramdisk_ctor <= CY_RAM_SIZE; i += 2) {
        if (ram[i] != 0x7a || memcmp(ram + i, ramdisk_ctor, sizeof ramdisk_ctor) != 0) continue;
        put32(ram + i + 2, 0x7fff);                 /* blocks: let the size decide */
        put32(ram + i + 20, m->disk_size);
        put32(ram + i + 26, CY_DISK_BASE);
        m->disk_patched++;
    }
}

void cy_disk_console(cybiko_t *m, uint8_t b)
{
    static const char want[] = "Starting 0x";
    if (!m->disk) return;
    if (b == (uint8_t)want[m->con_match]) {
        if (++m->con_match == sizeof want - 1) {
            m->con_match = 0;
            /* The loader prints this once the kernel is unpacked, a line
             * before it jumps there. */
            patch_kernel(m);
        }
    } else {
        m->con_match = b == (uint8_t)want[0];
    }
}

void cy_disk_write8(cybiko_t *m, uint32_t off, uint8_t v)
{
    m->disk[off] = v;
    uint32_t c = off / CY_DISK_CHUNK;
    m->disk_dirty[c / 32] |= 1u << (c % 32);
}

void cybiko_set_disk(cybiko_t *m, uint8_t *disk, uint32_t size)
{
    if (size > CY_DISK_MAX) size = CY_DISK_MAX;
    size &= ~0xffffu;
    m->disk = size ? disk : NULL;
    m->disk_size = size;
    memset(m->disk_dirty, 0, sizeof m->disk_dirty);
    /* Reads go straight to memory; writes take the slow path to be tracked. */
    if (size) h8s_map(&m->cpu, CY_DISK_BASE, size, disk, size, false);
    cy_bus_timing(m);
}
