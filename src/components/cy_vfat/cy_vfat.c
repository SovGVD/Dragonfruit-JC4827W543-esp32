/* A folder shown as a FAT16 SD card (see cy_vfat.h).
 *
 * Disk layout, in 512-byte blocks (the same as tools/cybiko_card.py):
 *   0            MBR with one partition (type 4, or 6 from 32 MB) at 63
 *   63           boot sector: 1 reserved sector, 2 FATs, 512 root entries
 *   64           FAT 1, then FAT 2 (the same)
 *   ...          root directory (32 blocks), then the data clusters
 * The scan lists the folder breadth first, so a folder's entries are
 * consecutive nodes, and gives each node its clusters in node order; a FAT
 * block or a data block is then found with a binary search over the nodes. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "cy_vfat.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define SECTOR 512
#define PART_START 63
#define ROOT_ENTRIES 512
#define ROOT_SECTORS (ROOT_ENTRIES * 32 / SECTOR)
#define MAX_NODES 8192
#define MAX_DEPTH 8
#define PATH_MAX_LEN 512
#define MIN_MB 32
#define FREE_MB 32              /* room left for the emulated machine's writes */
/* CyOS reads a FAT16 disk with 16 KB clusters (1 GB) but not with 32 KB
 * ones (tried on 2 GB: wrong data), and 1 GB is also all a CSD with 512-byte
 * blocks can describe. */
#define MAX_MB 1024
#define BIG_CLUSTER 32768       /* the largest cluster used, for the budget */

typedef struct {
    uint32_t name;              /* string pool offsets: long name (UTF-8), */
    uint32_t path;              /* and path below the folder */
    uint32_t first, count;      /* a folder's entries */
    uint32_t parent;
    uint32_t size, mtime;
    uint32_t cluster, clusters;
    uint8_t short_name[11];
    uint8_t lfn;                /* long name entries, 0 for a plain 8.3 name */
    uint8_t is_dir, depth;
} node_t;

typedef struct {
    uint32_t block;
    uint8_t *data;
} written_t;

struct cy_vfat {
    char root[PATH_MAX_LEN];
    node_t *nodes;
    uint32_t n_nodes, cap_nodes;
    char *pool;
    uint32_t pool_len, pool_cap;
    uint32_t *ext;              /* nodes with clusters, in cluster order */
    uint32_t n_ext;
    uint32_t files, skipped;
    uint32_t total, part, spc, fat_secs, clusters, data_start;
    FILE *file;
    uint32_t file_node;
    written_t *written;
    uint32_t n_written, max_written;
};

static void *vf_realloc(void *p, size_t n)
{
#ifdef ESP_PLATFORM
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    if (q) return q;
#endif
    return realloc(p, n);
}

static uint32_t pool_add(cy_vfat_t *v, const char *s)
{
    size_t n = strlen(s) + 1;
    if (v->pool_len + n > v->pool_cap) {
        uint32_t cap = v->pool_cap ? v->pool_cap * 2 : 16384;
        while (cap < v->pool_len + n) cap *= 2;
        char *p = vf_realloc(v->pool, cap);
        if (!p) return UINT32_MAX;
        v->pool = p;
        v->pool_cap = cap;
    }
    uint32_t at = v->pool_len;
    memcpy(v->pool + at, s, n);
    v->pool_len += (uint32_t)n;
    return at;
}

static node_t *node_add(cy_vfat_t *v)
{
    if (v->n_nodes >= MAX_NODES) return NULL;
    if (v->n_nodes == v->cap_nodes) {
        uint32_t cap = v->cap_nodes ? v->cap_nodes * 2 : 256;
        node_t *p = vf_realloc(v->nodes, cap * sizeof *p);
        if (!p) return NULL;
        v->nodes = p;
        v->cap_nodes = cap;
    }
    node_t *n = &v->nodes[v->n_nodes++];
    memset(n, 0, sizeof *n);
    return n;
}

/* ---- names ---- */

/* CyOS lists no file whose long name holds anything beyond ASCII (tried
 * with Latin-1 letters), so names are folded to ASCII: accents are dropped,
 * Cyrillic is transliterated and anything else becomes '_'. */
static const char latin1_fold[64][3] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "_", "o", "u", "u", "u", "u", "y", "th", "y",
};
static const char cyrillic_fold[32][4] = {    /* U+0430..U+044F; capitals map here too */
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "sch", "", "y", "", "e", "yu", "ya",
};

static int fold(uint32_t c, uint16_t *out, int room)
{
    const char *s = NULL;
    char one[2] = { '_', 0 };
    bool upper = false;
    if (c < 0x80) { one[0] = (char)c; s = one; }
    else if (c >= 0xc0 && c <= 0xff) s = latin1_fold[c - 0xc0];
    else if (c >= 0x410 && c <= 0x42f) { s = cyrillic_fold[c - 0x410]; upper = true; }
    else if (c >= 0x430 && c <= 0x44f) s = cyrillic_fold[c - 0x430];
    else if (c == 0x401 || c == 0x451) { s = "e"; upper = c == 0x401; }
    else s = one;
    int n = 0;
    for (; *s && n < room; s++, n++)
        out[n] = (uint16_t)(upper && n == 0 ? toupper((uint8_t)*s) : *s);
    return n;
}

/* A UTF-8 name as the ASCII long name the card shows; returns its length. */
static int utf16(const char *s, uint16_t *out, int cap)
{
    int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < cap) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (c >= 0x80 && !more) c = '_';
        else if (more) {
            c &= 0x3f >> more;
            for (int i = 0; i < more && (*p & 0xc0) == 0x80; i++) c = c << 6 | (*p++ & 0x3f);
        }
        n += fold(c, out + n, cap - n);
    }
    return n;
}

static bool short_ok(int ch)
{
    return ch > 0x20 && ch < 0x7f && !strchr("\"*+,./:;<=>?[\\]|", ch);
}

static bool plain_83(const char *name)
{
    const char *dot = strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base < 1 || base > 8 || ext > 3 || (dot && strchr(dot + 1, '.'))) return false;
    for (const char *p = name; *p; p++)
        if (p != dot && (!short_ok((uint8_t)*p) || islower((uint8_t)*p))) return false;
    return true;
}

static int short_part(const char *s, size_t len, char *out, int max)
{
    int n = 0;
    for (size_t i = 0; i < len && n < max; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch == ' ' || ch == '.') continue;
        if (ch >= 0x80) {                       /* one '_' per UTF-8 character */
            if ((ch & 0xc0) == 0x80) continue;
            ch = '_';
        }
        out[n++] = short_ok(ch) ? (char)toupper(ch) : '_';
    }
    return n;
}

static bool clashes(const cy_vfat_t *v, const node_t *n, uint32_t first, uint32_t self)
{
    for (uint32_t i = first; i < self; i++)
        if (!memcmp(v->nodes[i].short_name, n->short_name, 11)) return true;
    return false;
}

/* The 8.3 name: the name itself when it is one, otherwise a made-up
 * NAME~K.EXT and long name entries. `first`..`self` are the entries of the
 * same folder named so far. */
static void make_short(cy_vfat_t *v, node_t *n, uint32_t first, uint32_t self)
{
    const char *name = v->pool + n->name;
    memset(n->short_name, ' ', 11);
    n->lfn = 0;
    if (plain_83(name)) {
        const char *dot = strchr(name, '.');
        memcpy(n->short_name, name, dot ? (size_t)(dot - name) : strlen(name));
        if (dot) memcpy(n->short_name + 8, dot + 1, strlen(dot + 1));
        if (!clashes(v, n, first, self)) return;
        memset(n->short_name, ' ', 11);
    }
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    char b[8], e[3];
    int bl = short_part(name, dot ? (size_t)(dot - name) : strlen(name), b, 8);
    int el = dot ? short_part(dot + 1, strlen(dot + 1), e, 3) : 0;
    if (!bl) b[bl++] = '_';
    memcpy(n->short_name + 8, e, (size_t)el);
    for (uint32_t k = 1; ; k++) {
        char tail[12];
        int tl = snprintf(tail, sizeof tail, "~%u", (unsigned)k);
        int keep = bl < 8 - tl ? bl : 8 - tl;
        memset(n->short_name, ' ', 8);
        memcpy(n->short_name, b, (size_t)keep);
        memcpy(n->short_name + keep, tail, (size_t)tl);
        if (!clashes(v, n, first, self)) break;
    }
    uint16_t tmp[256];
    n->lfn = (uint8_t)((utf16(name, tmp, 255) + 12) / 13);
}

static uint32_t entries_of(const cy_vfat_t *v, const node_t *d, bool root)
{
    uint32_t n = root ? 1 : 2;                  /* volume label, or . and .. */
    for (uint32_t i = 0; i < d->count; i++) n += 1u + v->nodes[d->first + i].lfn;
    return n;
}

/* ---- scanning ---- */

static char *sort_pool;
static int cmp_nodes(const void *a, const void *b)
{
    return strcasecmp(sort_pool + ((const node_t *)a)->name, sort_pool + ((const node_t *)b)->name);
}

static void full_path(const cy_vfat_t *v, const node_t *n, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", v->root, v->pool + n->path);
}

static bool scan(cy_vfat_t *v)
{
    node_t *root = node_add(v);
    if (!root) return false;
    root->is_dir = 1;
    root->name = pool_add(v, "");
    root->path = pool_add(v, "");
    uint64_t budget = (uint64_t)(MAX_MB - FREE_MB) << 20, used = 0;
    uint32_t root_slots = 1;
    for (uint32_t i = 0; i < v->n_nodes; i++) {
        if (!v->nodes[i].is_dir) continue;
        char path[PATH_MAX_LEN];
        full_path(v, &v->nodes[i], path, sizeof path);
        DIR *dir = opendir(path);
        if (!dir) continue;
        uint32_t first = v->n_nodes;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            const char *nm = de->d_name;
            if (nm[0] == '.' || !strcasecmp(nm, "System Volume Information")) continue;
            char child[PATH_MAX_LEN];
            int len = snprintf(child, sizeof child, "%s/%s", path, nm);
            struct stat st;
            if (len >= (int)sizeof child || strlen(nm) > 255 || stat(child, &st) != 0) { v->skipped++; continue; }
            bool is_dir = S_ISDIR(st.st_mode);
            if (!is_dir && !S_ISREG(st.st_mode)) continue;
            uint64_t need = is_dir ? BIG_CLUSTER : ((uint64_t)st.st_size + BIG_CLUSTER - 1) / BIG_CLUSTER * BIG_CLUSTER;
            uint16_t tmp[256];
            uint32_t slots = 1 + (uint32_t)(utf16(nm, tmp, 255) + 12) / 13;
            if ((is_dir && v->nodes[i].depth + 1 >= MAX_DEPTH) || used + need > budget ||
                (i == 0 && root_slots + slots > ROOT_ENTRIES) || (uint64_t)st.st_size > 0xffffffffu) {
                v->skipped++;
                continue;
            }
            uint32_t parent_path = v->nodes[i].path;
            uint8_t depth = v->nodes[i].depth;
            node_t *n = node_add(v);
            if (!n) { v->skipped++; continue; }
            char rel[PATH_MAX_LEN];
            snprintf(rel, sizeof rel, "%s/%s", v->pool + parent_path, nm);
            n->name = pool_add(v, nm);
            n->path = pool_add(v, rel);
            if (n->name == UINT32_MAX || n->path == UINT32_MAX) { v->n_nodes--; v->skipped++; continue; }
            n->is_dir = is_dir;
            n->parent = i;
            n->depth = (uint8_t)(depth + 1);
            n->size = is_dir ? 0 : (uint32_t)st.st_size;
            n->mtime = (uint32_t)st.st_mtime;
            used += need;
            if (i == 0) root_slots += slots;
            if (!is_dir) v->files++;
        }
        closedir(dir);
        node_t *d = &v->nodes[i];
        d->first = first;
        d->count = v->n_nodes - first;
        sort_pool = v->pool;
        qsort(&v->nodes[first], d->count, sizeof(node_t), cmp_nodes);
        for (uint32_t k = first; k < v->n_nodes; k++) make_short(v, &v->nodes[k], first, k);
    }
    return true;
}

static bool layout(cy_vfat_t *v)
{
    uint64_t content = 0;
    for (uint32_t i = 1; i < v->n_nodes; i++) {
        const node_t *n = &v->nodes[i];
        content += n->is_dir ? (uint64_t)entries_of(v, n, false) * 32 : n->size;
    }
    uint32_t mb = (uint32_t)(content >> 20) + FREE_MB;
    if (mb < MIN_MB) mb = MIN_MB;
    if (mb > MAX_MB) mb = MAX_MB;
    for (;;) {
        v->total = mb * 2048u;
        v->part = v->total - PART_START;
        v->spc = 1;
        while (v->part / v->spc > 65524) v->spc *= 2;
        uint32_t c = (v->part - 1 - ROOT_SECTORS) / v->spc;
        v->fat_secs = (2 * (c + 2) + SECTOR - 1) / SECTOR;
        v->clusters = (v->part - 1 - ROOT_SECTORS - 2 * v->fat_secs) / v->spc;
        v->data_start = PART_START + 1 + 2 * v->fat_secs + ROOT_SECTORS;
        uint32_t csize = v->spc * SECTOR, next = 2;
        bool fits = true;
        for (uint32_t i = 1; i < v->n_nodes && fits; i++) {
            node_t *n = &v->nodes[i];
            uint32_t bytes = n->is_dir ? entries_of(v, n, false) * 32 : n->size;
            n->clusters = bytes || n->is_dir ? (bytes + csize - 1) / csize : 0;
            n->cluster = n->clusters ? next : 0;
            next += n->clusters;
            fits = next <= v->clusters + 2;
        }
        if (fits) break;
        if (mb >= MAX_MB) return false;        /* cannot happen with the scan's budget */
        mb = mb * 2 > MAX_MB ? MAX_MB : mb * 2;
    }
    v->ext = vf_realloc(NULL, (v->n_nodes ? v->n_nodes : 1) * sizeof *v->ext);
    if (!v->ext) return false;
    for (uint32_t i = 1; i < v->n_nodes; i++)
        if (v->nodes[i].clusters) v->ext[v->n_ext++] = i;
    return true;
}

cy_vfat_t *cy_vfat_open(const char *dir, uint32_t max_written)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;
    closedir(d);
    cy_vfat_t *v = vf_realloc(NULL, sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    snprintf(v->root, sizeof v->root, "%s", dir);
    size_t rl = strlen(v->root);
    while (rl > 1 && v->root[rl - 1] == '/') v->root[--rl] = 0;
    v->max_written = max_written;
    v->file_node = UINT32_MAX;
    if (!scan(v) || !layout(v)) {
        cy_vfat_close(v);
        return NULL;
    }
    return v;
}

void cy_vfat_close(cy_vfat_t *v)
{
    if (!v) return;
    if (v->file) fclose(v->file);
    for (uint32_t i = 0; i < v->n_written; i++) free(v->written[i].data);
    free(v->written);
    free(v->ext);
    free(v->nodes);
    free(v->pool);
    free(v);
}

uint32_t cy_vfat_blocks(const cy_vfat_t *v) { return v->total; }
uint32_t cy_vfat_files(const cy_vfat_t *v) { return v->files; }
uint32_t cy_vfat_skipped(const cy_vfat_t *v) { return v->skipped; }

/* ---- blocks ---- */

static void put16(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x) { put16(p, x & 0xffff); put16(p + 2, x >> 16); }

static void dos_time(uint32_t t, uint16_t *tt, uint16_t *dd)
{
    time_t tm_t = (time_t)t;
    struct tm tm;
    if (!localtime_r(&tm_t, &tm) || tm.tm_year < 80) {
        *tt = 0;
        *dd = 1 << 5 | 1;                       /* 1 Jan 1980 */
        return;
    }
    *tt = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *dd = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

static void dir_entry(uint8_t *e, const uint8_t *short_name, uint8_t attr, uint32_t cluster,
                      uint32_t size, uint32_t mtime)
{
    uint16_t tt, dd;
    dos_time(mtime, &tt, &dd);
    memset(e, 0, 32);
    memcpy(e, short_name, 11);
    e[11] = attr;
    put16(e + 14, tt); put16(e + 16, dd);       /* created */
    put16(e + 18, dd);                          /* accessed */
    put16(e + 22, tt); put16(e + 24, dd);       /* written */
    put16(e + 26, cluster);
    put32(e + 28, size);
}

static uint8_t lfn_sum(const uint8_t *s)
{
    uint8_t c = 0;
    for (int i = 0; i < 11; i++) c = (uint8_t)(((c & 1) << 7) + (c >> 1) + s[i]);
    return c;
}

/* Entry `k` (the last one first, as they are stored) of a long name. */
static void lfn_entry(uint8_t *e, const uint16_t *name, int units, int slots, int k, uint8_t sum)
{
    static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int part = slots - 1 - k;                   /* 0 = the first 13 characters */
    memset(e, 0, 32);
    e[0] = (uint8_t)((part + 1) | (part == slots - 1 ? 0x40 : 0));
    e[11] = 0x0f;
    e[13] = sum;
    for (int j = 0; j < 13; j++) {
        int c = part * 13 + j;
        put16(e + at[j], c < units ? name[c] : c == units ? 0 : 0xffff);
    }
}

/* Block `blk` of a folder's entries (the root's when `root`). */
static void dir_block(const cy_vfat_t *v, const node_t *d, bool root, uint32_t blk, uint8_t *buf)
{
    static const uint8_t dot[12] = ".          ", dotdot[12] = "..         ", label[12] = "CYBIKO     ";
    uint32_t first = blk * 16, idx = 0;
    memset(buf, 0, SECTOR);
#define WANT() (idx >= first && idx < first + 16)
#define SLOT() (buf + (idx - first) * 32)
    if (root) {
        if (WANT()) dir_entry(SLOT(), label, 0x08, 0, 0, 0);
        idx++;
    } else {
        uint32_t parent_cluster = v->nodes[d->parent].cluster;     /* 0 for the root */
        if (WANT()) dir_entry(SLOT(), dot, 0x10, d->cluster, 0, d->mtime);
        idx++;
        if (WANT()) dir_entry(SLOT(), dotdot, 0x10, parent_cluster, 0, d->mtime);
        idx++;
    }
    for (uint32_t i = 0; i < d->count && idx < first + 16; i++) {
        const node_t *c = &v->nodes[d->first + i];
        if (idx + 1u + c->lfn <= first) {
            idx += 1u + c->lfn;
            continue;
        }
        if (c->lfn) {
            uint16_t name[256];
            int units = utf16(v->pool + c->name, name, 255);
            uint8_t sum = lfn_sum(c->short_name);
            for (int k = 0; k < c->lfn; k++, idx++)
                if (WANT()) lfn_entry(SLOT(), name, units, c->lfn, k, sum);
        }
        if (WANT()) dir_entry(SLOT(), c->short_name, c->is_dir ? 0x10 : 0x20, c->cluster, c->size, c->mtime);
        idx++;
    }
#undef WANT
#undef SLOT
}

/* The node whose clusters hold `cl`, or NULL. */
static const node_t *node_at(const cy_vfat_t *v, uint32_t cl, uint32_t *ext_pos)
{
    uint32_t lo = 0, hi = v->n_ext;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (v->nodes[v->ext[mid]].cluster <= cl) lo = mid + 1; else hi = mid;
    }
    if (ext_pos) *ext_pos = lo;                 /* the first node after cl */
    if (!lo) return NULL;
    const node_t *n = &v->nodes[v->ext[lo - 1]];
    return cl < n->cluster + n->clusters ? n : NULL;
}

static void fat_block(const cy_vfat_t *v, uint32_t blk, uint8_t *buf)
{
    uint32_t e0 = blk * 256;
    for (uint32_t k = 0; k < 256; k++) {
        uint32_t e = e0 + k, val = 0;
        if (e == 0) val = 0xfff8;
        else if (e == 1) val = 0xffff;
        else if (e < v->clusters + 2) {
            const node_t *n = node_at(v, e, NULL);
            if (n) val = e + 1 == n->cluster + n->clusters ? 0xffff : e + 1;
        }
        put16(buf + 2 * k, val);
    }
}

static void boot_blocks(const cy_vfat_t *v, uint32_t block, uint8_t *buf)
{
    memset(buf, 0, SECTOR);
    if (block == 0) {
        uint8_t *p = buf + 446;
        p[1] = 1; p[2] = 1;                     /* CHS of the start: a guess, as on PCs */
        p[4] = v->part < 65536 ? 0x04 : 0x06;
        p[5] = 0xfe; p[6] = 0xff; p[7] = 0xff;
        put32(p + 8, PART_START);
        put32(p + 12, v->part);
    } else {
        memcpy(buf, "\xeb\x3c\x90MSDOS5.0", 11);
        put16(buf + 11, SECTOR);
        buf[13] = (uint8_t)v->spc;
        put16(buf + 14, 1);                     /* reserved sectors */
        buf[16] = 2;                            /* FATs */
        put16(buf + 17, ROOT_ENTRIES);
        put16(buf + 19, v->part < 65536 ? v->part : 0);
        buf[21] = 0xf8;
        put16(buf + 22, v->fat_secs);
        put16(buf + 24, 63);                    /* sectors per track */
        put16(buf + 26, 255);                   /* heads */
        put32(buf + 28, PART_START);
        put32(buf + 32, v->part < 65536 ? 0 : v->part);
        buf[36] = 0x80;
        buf[38] = 0x29;
        put32(buf + 39, 0x43594b4f);            /* serial "CYKO" */
        memcpy(buf + 43, "CYBIKO     FAT16   ", 19);
    }
    buf[510] = 0x55;
    buf[511] = 0xaa;
}

static bool file_block(cy_vfat_t *v, const node_t *n, uint32_t offset, uint8_t *buf)
{
    memset(buf, 0, SECTOR);
    if (offset >= n->size) return true;
    uint32_t idx = (uint32_t)(n - v->nodes);
    if (v->file_node != idx) {
        if (v->file) fclose(v->file);
        char path[PATH_MAX_LEN];
        full_path(v, n, path, sizeof path);
        v->file = fopen(path, "rb");
        v->file_node = v->file ? idx : UINT32_MAX;
        if (!v->file) return false;
    }
    uint32_t want = n->size - offset < SECTOR ? n->size - offset : SECTOR;
    return fseek(v->file, (long)offset, SEEK_SET) == 0 && fread(buf, 1, want, v->file) == want;
}

bool cy_vfat_read(cy_vfat_t *v, uint32_t block, uint8_t *buf)
{
    if (block >= v->total) return false;
    for (uint32_t i = 0; i < v->n_written; i++)
        if (v->written[i].block == block) {
            memcpy(buf, v->written[i].data, SECTOR);
            return true;
        }
    uint32_t fat0 = PART_START + 1, root0 = fat0 + 2 * v->fat_secs;
    if (block == 0 || block == PART_START) { boot_blocks(v, block, buf); return true; }
    if (block < fat0) { memset(buf, 0, SECTOR); return true; }
    if (block < root0) { fat_block(v, (block - fat0) % v->fat_secs, buf); return true; }
    if (block < v->data_start) { dir_block(v, &v->nodes[0], true, block - root0, buf); return true; }
    uint32_t rel = block - v->data_start, cl = 2 + rel / v->spc;
    const node_t *n = node_at(v, cl, NULL);
    if (!n) { memset(buf, 0, SECTOR); return true; }
    uint32_t blk = (cl - n->cluster) * v->spc + rel % v->spc;
    if (n->is_dir) { dir_block(v, n, false, blk, buf); return true; }
    return file_block(v, n, blk * SECTOR, buf);
}

bool cy_vfat_write(cy_vfat_t *v, uint32_t block, const uint8_t *buf)
{
    if (block >= v->total) return false;
    for (uint32_t i = 0; i < v->n_written; i++)
        if (v->written[i].block == block) {
            memcpy(v->written[i].data, buf, SECTOR);
            return true;
        }
    if (v->n_written >= v->max_written) return false;
    uint32_t n = v->n_written;
    if (n == 0 || (n >= 16 && (n & (n - 1)) == 0)) {        /* capacity 16, 32, 64... */
        uint32_t cap = n ? n * 2 : 16;
        written_t *w = vf_realloc(v->written, cap * sizeof *w);
        if (!w) return false;
        v->written = w;
    }
    uint8_t *data = vf_realloc(NULL, SECTOR);
    if (!data) return false;
    memcpy(data, buf, SECTOR);
    v->written[v->n_written].block = block;
    v->written[v->n_written].data = data;
    v->n_written++;
    return true;
}
