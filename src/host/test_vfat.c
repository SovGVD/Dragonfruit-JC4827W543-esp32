/* Tests for cy_vfat: a folder shown as a FAT16 card.
 *
 *   test_vfat                  build a folder in /tmp and check the disk
 *   test_vfat DIR OUT.img      write the disk made from DIR to OUT.img
 *                              (to check it with fsck.fat or a PC) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cy_vfat.h"

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static void put_file(const char *path, size_t size, unsigned seed)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    for (size_t i = 0; i < size; i++) fputc((int)((i * 7 + seed) & 0xff), f);
    fclose(f);
}

typedef struct {
    cy_vfat_t *v;
    uint32_t part, spc, fat0, root0, data0;
} disk_t;

static void blk(disk_t *d, uint32_t b, uint8_t *buf) { assert(cy_vfat_read(d->v, b, buf)); }

static uint32_t next_cluster(disk_t *d, uint32_t c)
{
    uint8_t buf[512];
    blk(d, d->fat0 + c / 256, buf);
    return le16(buf + 2 * (c % 256));
}

/* Read a file (cluster chain) into out and check it against the pattern. */
static void check_chain(disk_t *d, uint32_t cluster, uint32_t size, unsigned seed)
{
    uint8_t buf[512];
    uint32_t pos = 0;
    while (pos < size) {
        assert(cluster >= 2 && cluster < 0xfff0);
        for (uint32_t s = 0; s < d->spc && pos < size; s++) {
            blk(d, d->data0 + (cluster - 2) * d->spc + s, buf);
            for (uint32_t i = 0; i < 512 && pos < size; i++, pos++)
                assert(buf[i] == (uint8_t)((pos * 7 + seed) & 0xff));
        }
        cluster = next_cluster(d, cluster);
    }
    assert(cluster == 0xffff);
}

/* Find `short_name` among 16-entry blocks starting at block b (n blocks). */
static const uint8_t *find(disk_t *d, uint32_t b, uint32_t n, const char *short_name, uint8_t *buf, bool *lfn)
{
    for (uint32_t k = 0; k < n; k++) {
        blk(d, b + k, buf);
        for (int e = 0; e < 16; e++) {
            const uint8_t *ent = buf + 32 * e;
            if (!memcmp(ent, short_name, 11)) {
                if (lfn) *lfn = e > 0 && ent[-32 + 11] == 0x0f;
                return ent;
            }
        }
    }
    return NULL;
}

static void self_test(void)
{
    char dir[] = "/tmp/vfatXXXXXX";
    assert(mkdtemp(dir));
    char p[256];
    snprintf(p, sizeof p, "%s/HELLO.TXT", dir); put_file(p, 1000, 1);
    snprintf(p, sizeof p, "%s/A long song name.mp3", dir); put_file(p, 70000, 2);
    snprintf(p, sizeof p, "%s/empty.txt", dir); put_file(p, 0, 3);
    snprintf(p, sizeof p, "%s/.hidden", dir); put_file(p, 10, 4);
    snprintf(p, sizeof p, "%s/Clips", dir); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/Clips/movie.app", dir); put_file(p, 5000, 5);

    disk_t d = { .v = cy_vfat_open(dir, 64) };
    assert(d.v);
    assert(cy_vfat_files(d.v) == 4);
    assert(cy_vfat_blocks(d.v) == 32 * 2048);
    uint8_t buf[512], buf2[512];
    blk(&d, 0, buf);
    assert(buf[510] == 0x55 && buf[511] == 0xaa && le32(buf + 454) == 63);
    d.part = le32(buf + 458);
    blk(&d, 63, buf);
    assert(le16(buf + 11) == 512 && buf[16] == 2 && le16(buf + 17) == 512 && !memcmp(buf + 54, "FAT16", 5));
    d.spc = buf[13];
    uint32_t fat_secs = le16(buf + 22);
    d.fat0 = 63 + le16(buf + 14);
    d.root0 = d.fat0 + 2 * fat_secs;
    d.data0 = d.root0 + 32;
    assert(next_cluster(&d, 0) == 0xfff8);

    const uint8_t *e = find(&d, d.root0, 32, "HELLO   TXT", buf, NULL);
    assert(e && e[11] == 0x20 && le32(e + 28) == 1000);
    check_chain(&d, le16(e + 26), 1000, 1);
    bool lfn = false;
    e = find(&d, d.root0, 32, "ALONGS~1MP3", buf, &lfn);
    assert(e && lfn && le32(e + 28) == 70000);
    check_chain(&d, le16(e + 26), 70000, 2);
    e = find(&d, d.root0, 32, "EMPTY~1 TXT", buf, &lfn);
    assert(e && lfn && le16(e + 26) == 0 && le32(e + 28) == 0);
    assert(!find(&d, d.root0, 32, "HIDDEN     ", buf, NULL) && !find(&d, d.root0, 32, "_HIDDEN    ", buf, NULL));
    e = find(&d, d.root0, 32, "CLIPS~1    ", buf, &lfn);
    assert(e && e[11] == 0x10);
    uint32_t sub = le16(e + 26);
    e = find(&d, d.data0 + (sub - 2) * d.spc, 1, "MOVIE~1 APP", buf, NULL);
    assert(e);
    check_chain(&d, le16(e + 26), 5000, 5);

    /* Writes stay in memory and win over the made-up blocks. */
    memset(buf2, 0xa5, sizeof buf2);
    assert(cy_vfat_write(d.v, d.root0, buf2));
    blk(&d, d.root0, buf);
    assert(!memcmp(buf, buf2, 512));
    for (uint32_t i = 1; i < 64; i++) assert(cy_vfat_write(d.v, d.data0 + 1000 + i, buf2));
    assert(!cy_vfat_write(d.v, d.data0 + 2000, buf2));      /* the 65th block does not fit */
    assert(!cy_vfat_read(d.v, cy_vfat_blocks(d.v), buf));
    cy_vfat_close(d.v);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
    puts("vfat: all tests passed");
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        cy_vfat_t *v = cy_vfat_open(argv[1], 0);
        if (!v) { perror(argv[1]); return 1; }
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        uint8_t buf[512];
        for (uint32_t b = 0; b < cy_vfat_blocks(v); b++) {
            if (!cy_vfat_read(v, b, buf)) { fprintf(stderr, "block %u failed\n", (unsigned)b); return 1; }
            fwrite(buf, 1, sizeof buf, f);
        }
        fclose(f);
        printf("%u MB, %u files, %u left out\n", (unsigned)(cy_vfat_blocks(v) >> 11),
               (unsigned)cy_vfat_files(v), (unsigned)cy_vfat_skipped(v));
        cy_vfat_close(v);
        return 0;
    }
    self_test();
    return 0;
}
