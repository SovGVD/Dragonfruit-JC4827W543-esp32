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
#include <unistd.h>
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
#define READ_AHEAD 8192         /* bytes read from a file at a time */
#define BACKUP_PREFIX ".cyold"  /* a changed file's old contents, until close */

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
    uint8_t gone;               /* deleted from the folder by a sync */
    uint8_t backup;             /* path is a hidden copy of the old contents */
} node_t;

/* What the folder holds, as of the last cy_vfat_sync: the scan's files and
 * folders, then the ones the machine made. */
typedef struct {
    uint32_t parent;            /* record index, UINT32_MAX for the root */
    uint32_t node;              /* its node, UINT32_MAX if the machine made it */
    uint32_t name;              /* pool offset: the name in the folder */
    uint32_t cluster, size;     /* as on the card */
    uint8_t short_name[11];
    uint8_t is_dir, gone, seen;
    uint8_t as_scanned;         /* contents still those the scan found */
} rec_t;

typedef struct {
    uint32_t block;
    uint8_t *data;
    bool synced;                /* carried over to the folder by cy_vfat_sync */
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
    uint8_t *cache;             /* read-ahead: file_node's bytes from cache_off */
    uint32_t cache_off, cache_len;
    written_t *written;
    uint32_t n_written, max_written;
    rec_t *recs;
    uint32_t n_recs, cap_recs;
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
            char child[PATH_MAX_LEN];
            int len = snprintf(child, sizeof child, "%s/%s", path, nm);
            if (!strncmp(nm, BACKUP_PREFIX, strlen(BACKUP_PREFIX)) && len < (int)sizeof child) {
                remove(child);                  /* left by a sync that did not finish */
                continue;
            }
            if (nm[0] == '.' || !strcasecmp(nm, "System Volume Information")) continue;
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
    for (uint32_t i = 0; i < v->n_nodes; i++)
        if (v->nodes[i].backup) {
            char path[PATH_MAX_LEN];
            full_path(v, &v->nodes[i], path, sizeof path);
            remove(path);
        }
    free(v->cache);
    for (uint32_t i = 0; i < v->n_written; i++) free(v->written[i].data);
    free(v->written);
    free(v->recs);
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

/* Files are read READ_AHEAD bytes at a time: the card is read in runs of
 * blocks, and one read of the board's SD card per block is slow. */
static bool file_block(cy_vfat_t *v, const node_t *n, uint32_t offset, uint8_t *buf)
{
    memset(buf, 0, SECTOR);
    if (offset >= n->size || n->gone) return true;     /* a deleted file's clusters are free */
    uint32_t idx = (uint32_t)(n - v->nodes);
    if (v->file_node != idx) {
        if (v->file) fclose(v->file);
        char path[PATH_MAX_LEN];
        full_path(v, n, path, sizeof path);
        v->file = fopen(path, "rb");
        v->file_node = v->file ? idx : UINT32_MAX;
        v->cache_len = 0;
        if (!v->file) return false;
    }
    if (!v->cache && !(v->cache = vf_realloc(NULL, READ_AHEAD))) return false;
    if (offset < v->cache_off || offset >= v->cache_off + v->cache_len) {
        uint32_t want = n->size - offset < READ_AHEAD ? n->size - offset : READ_AHEAD;
        v->cache_len = 0;
        if (fseek(v->file, (long)offset, SEEK_SET) != 0 || fread(v->cache, 1, want, v->file) != want)
            return false;
        v->cache_off = offset;
        v->cache_len = want;
    }
    uint32_t have = v->cache_off + v->cache_len - offset;
    memcpy(buf, v->cache + (offset - v->cache_off), have < SECTOR ? have : SECTOR);
    return true;
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
            v->written[i].synced = false;
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
    v->written[v->n_written].synced = false;
    v->n_written++;
    return true;
}
/* ---- carrying the machine's writes over to the folder ---- */

#define MAX_DIR_BLOCKS 128      /* a folder of up to 2048 entries */

typedef struct {
    char name[256];             /* long name, or the 8.3 one, as UTF-8 */
    uint8_t short_name[11];
    uint8_t attr;
    uint32_t cluster, size;
} entry_t;

typedef struct {
    cy_vfat_t *v;
    cy_vfat_sync_t *out;
    uint8_t buf[SECTOR];
    uint32_t fat_blk;           /* the FAT block in fat_buf, UINT32_MAX for none */
    uint8_t fat_buf[SECTOR];
    uint32_t *dirty;            /* blocks written since the last sync, sorted */
    uint32_t n_dirty;
    bool fat_dirty, data_dirty; /* any of them in the FATs, in the data area */
} sync_t;

static bool unsynced(const sync_t *s, uint32_t block)
{
    uint32_t lo = 0, hi = s->n_dirty;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s->dirty[mid] < block) lo = mid + 1; else hi = mid;
    }
    return lo < s->n_dirty && s->dirty[lo] == block;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void drop_file(cy_vfat_t *v)
{
    if (v->file) fclose(v->file);
    v->file = NULL;
    v->file_node = UINT32_MAX;
    v->cache_len = 0;
}

static uint32_t fat_next(sync_t *s, uint32_t cl)
{
    uint32_t blk = PART_START + 1 + cl / 256;
    if (blk != s->fat_blk) {
        if (!cy_vfat_read(s->v, blk, s->fat_buf)) return 0xffff;
        s->fat_blk = blk;
    }
    return s->fat_buf[(cl % 256) * 2] | s->fat_buf[(cl % 256) * 2 + 1] << 8;
}

static bool cluster_ok(const cy_vfat_t *v, uint32_t cl) { return cl >= 2 && cl < v->clusters + 2; }

/* The blocks of a folder (the root's when cluster is 0); returns how many. */
static uint32_t dir_blocks(sync_t *s, uint32_t cluster, uint32_t *blocks)
{
    cy_vfat_t *v = s->v;
    uint32_t n = 0;
    if (!cluster) {
        uint32_t root0 = PART_START + 1 + 2 * v->fat_secs;
        for (; n < ROOT_SECTORS; n++) blocks[n] = root0 + n;
        return n;
    }
    for (uint32_t cl = cluster, steps = 0; cluster_ok(v, cl) && steps < MAX_DIR_BLOCKS; cl = fat_next(s, cl), steps++)
        for (uint32_t k = 0; k < v->spc && n < MAX_DIR_BLOCKS; k++)
            blocks[n++] = v->data_start + (cl - 2) * v->spc + k;
    return n;
}

static void put_utf8(char *out, size_t *len, uint32_t c)
{
    if (*len + 4 >= 256) return;
    if (c < 0x80) out[(*len)++] = (char)c;
    else if (c < 0x800) { out[(*len)++] = (char)(0xc0 | c >> 6); out[(*len)++] = (char)(0x80 | (c & 0x3f)); }
    else {
        out[(*len)++] = (char)(0xe0 | c >> 12);
        out[(*len)++] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[(*len)++] = (char)(0x80 | (c & 0x3f));
    }
    out[*len] = 0;
}

/* Read a folder's entries; returns how many, or -1 if it could not be read. */
static int read_entries(sync_t *s, const uint32_t *blocks, uint32_t nb, entry_t **out)
{
    static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    entry_t *list = NULL;
    int n = 0, cap = 0;
    uint16_t lfn[260];
    int lfn_parts = 0, lfn_seen = 0;
    uint8_t lfn_csum = 0;
    for (uint32_t b = 0; b < nb; b++) {
        if (!cy_vfat_read(s->v, blocks[b], s->buf)) { free(list); return -1; }
        for (int k = 0; k < 16; k++) {
            const uint8_t *e = s->buf + k * 32;
            if (e[0] == 0) goto done;
            if (e[0] == 0xe5) { lfn_parts = 0; continue; }
            if (e[11] == 0x0f) {
                int ord = e[0] & 0x1f;
                if (e[0] & 0x40) {
                    lfn_parts = ord;
                    lfn_seen = 0;
                    lfn_csum = e[13];
                    for (int i = 0; i < 260; i++) lfn[i] = 0xffff;
                }
                if (!lfn_parts || ord < 1 || ord > lfn_parts || ord > 20 || e[13] != lfn_csum) { lfn_parts = 0; continue; }
                for (int j = 0; j < 13; j++) lfn[(ord - 1) * 13 + j] = (uint16_t)(e[at[j]] | e[at[j] + 1] << 8);
                lfn_seen++;
                continue;
            }
            if ((e[11] & 0x08) || e[0] == '.') { lfn_parts = 0; continue; }   /* label, . and .. */
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                entry_t *p = realloc(list, (size_t)cap * sizeof *p);
                if (!p) { free(list); return -1; }
                list = p;
            }
            entry_t *t = &list[n++];
            memcpy(t->short_name, e, 11);
            t->attr = e[11];
            t->cluster = e[26] | e[27] << 8;
            t->size = (uint32_t)e[28] | (uint32_t)e[29] << 8 | (uint32_t)e[30] << 16 | (uint32_t)e[31] << 24;
            size_t len = 0;
            t->name[0] = 0;
            if (lfn_parts && lfn_seen == lfn_parts && lfn_csum == lfn_sum(e)) {
                for (int i = 0; i < lfn_parts * 13 && lfn[i] && lfn[i] != 0xffff; i++) put_utf8(t->name, &len, lfn[i]);
            } else {
                for (int i = 0; i < 8 && e[i] != ' '; i++)
                    put_utf8(t->name, &len, (e[12] & 0x08) ? (uint32_t)tolower(e[i]) : e[i]);
                if (e[8] != ' ') put_utf8(t->name, &len, '.');
                for (int i = 8; i < 11 && e[i] != ' '; i++)
                    put_utf8(t->name, &len, (e[12] & 0x10) ? (uint32_t)tolower(e[i]) : e[i]);
                if (t->name[0] == 0x05) t->name[0] = (char)0xe5;
            }
            lfn_parts = 0;
        }
    }
done:
    *out = list;
    return n;
}

static bool name_ok(const char *nm)
{
    return nm[0] && strcmp(nm, ".") && strcmp(nm, "..") && !strchr(nm, '/') && !strchr(nm, '\\') &&
           strncmp(nm, BACKUP_PREFIX, strlen(BACKUP_PREFIX));
}

/* A node's backing file, below the folder: its own path, or for a changed
 * file the hidden copy of its old contents. */
static void set_path(cy_vfat_t *v, uint32_t idx, const char *rel)
{
    uint32_t at = pool_add(v, rel);
    if (at == UINT32_MAX) return;
    v->nodes[idx].path = at;
    const node_t *n = &v->nodes[idx];
    if (!n->is_dir) return;
    for (uint32_t i = 0; i < n->count; i++) {
        uint32_t c = n->first + i;
        char sub[PATH_MAX_LEN];
        if (v->nodes[c].backup) snprintf(sub, sizeof sub, "%s/" BACKUP_PREFIX "%u", rel, (unsigned)c);
        else snprintf(sub, sizeof sub, "%s/%s", rel, v->pool + v->nodes[c].name);
        set_path(v, c, sub);
    }
}

static void node_gone(cy_vfat_t *v, uint32_t idx)
{
    node_t *n = &v->nodes[idx];
    n->gone = 1;
    if (n->is_dir)
        for (uint32_t i = 0; i < n->count; i++) node_gone(v, n->first + i);
}

static void rec_gone(cy_vfat_t *v, uint32_t idx)
{
    rec_t *r = &v->recs[idx];
    r->gone = 1;
    if (r->node != UINT32_MAX) node_gone(v, r->node);
    if (r->is_dir)
        for (uint32_t i = 0; i < v->n_recs; i++)
            if (v->recs[i].parent == idx && !v->recs[i].gone) rec_gone(v, i);
}

static uint32_t rec_add(cy_vfat_t *v)
{
    if (v->n_recs == v->cap_recs) {
        uint32_t cap = v->cap_recs ? v->cap_recs * 2 : 256;
        rec_t *p = vf_realloc(v->recs, cap * sizeof *p);
        if (!p) return UINT32_MAX;
        v->recs = p;
        v->cap_recs = cap;
    }
    rec_t *r = &v->recs[v->n_recs];
    memset(r, 0, sizeof *r);
    r->node = UINT32_MAX;
    return v->n_recs++;
}

/* Delete a file or a folder with everything in it. */
static bool remove_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return true;
    if (S_ISDIR(st.st_mode) && depth < MAX_DEPTH + 2) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char sub[PATH_MAX_LEN];
                if (snprintf(sub, sizeof sub, "%s/%s", path, de->d_name) < (int)sizeof sub) remove_tree(sub, depth + 1);
            }
            closedir(d);
        }
        return rmdir(path) == 0;
    }
    return remove(path) == 0;
}

/* Is the file on the card different from what the folder holds: another
 * size or first cluster, blocks written since the last sync, or (for a file
 * still as scanned) a chain that left the scanned clusters? */
static bool file_changed(sync_t *s, const entry_t *e, const rec_t *r)
{
    cy_vfat_t *v = s->v;
    if (!r || e->size != r->size || e->cluster != r->cluster) return true;
    if (!s->data_dirty && (!s->fat_dirty || !r->as_scanned)) return false;
    const node_t *n = r->as_scanned ? &v->nodes[r->node] : NULL;
    uint32_t want = (e->size + v->spc * SECTOR - 1) / (v->spc * SECTOR), k = 0;
    for (uint32_t cl = e->cluster; cluster_ok(v, cl) && k < want; cl = fat_next(s, cl), k++) {
        if (n && cl != n->cluster + k) return true;
        for (uint32_t b = 0; b < v->spc; b++)
            if (unsynced(s, v->data_start + (cl - 2) * v->spc + b)) return true;
    }
    return false;
}

static bool write_file(sync_t *s, const entry_t *e, const char *path)
{
    cy_vfat_t *v = s->v;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t left = e->size, steps = 0;
    bool ok = true;
    for (uint32_t cl = e->cluster; left && ok; cl = fat_next(s, cl)) {
        if (!cluster_ok(v, cl) || steps++ > v->clusters) { ok = false; break; }
        for (uint32_t b = 0; b < v->spc && left && ok; b++) {
            ok = cy_vfat_read(v, v->data_start + (cl - 2) * v->spc + b, s->buf);
            uint32_t n = left < SECTOR ? left : SECTOR;
            ok = ok && fwrite(s->buf, 1, n, f) == n;
            left -= n;
        }
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

/* One folder: `cluster` its first cluster (0 for the root), `path` where it
 * is in the folder, `dir` its record. Entries are matched to the records
 * of the last sync (at first, the scan) by 8.3 name and first cluster, or
 * by the cluster alone for a rename. Records not on the card any more are
 * deleted first, so a file the machine replaced (deleted, then written
 * again under the same name) ends up written. */
static void sync_dir(sync_t *s, uint32_t cluster, const char *path, uint32_t dir, int depth)
{
    cy_vfat_t *v = s->v;
    uint32_t blocks[MAX_DIR_BLOCKS];
    uint32_t nb = dir_blocks(s, cluster, blocks);
    bool dirty = false;
    for (uint32_t i = 0; i < nb && !dirty; i++) dirty = unsynced(s, blocks[i]);
    entry_t *list = NULL;
    int n = read_entries(s, blocks, nb, &list);
    uint32_t *match = n > 0 ? malloc((size_t)n * sizeof *match) : NULL;
    if (n < 0 || (n > 0 && !match)) { free(list); s->out->failed++; return; }
    for (uint32_t k = 0; k < v->n_recs; k++)
        if (v->recs[k].parent == dir) v->recs[k].seen = 0;

    /* 1: match the entries to the records */
    for (int i = 0; i < n; i++) {
        const entry_t *e = &list[i];
        bool is_dir = e->attr & 0x10;
        match[i] = UINT32_MAX;
        for (int pass = 0; pass < 2 && match[i] == UINT32_MAX; pass++)
            for (uint32_t k = 1; k < v->n_recs && match[i] == UINT32_MAX; k++) {
                const rec_t *r = &v->recs[k];
                if (r->parent != dir || r->gone || r->seen || r->is_dir != is_dir) continue;
                if (pass == 0 ? !memcmp(r->short_name, e->short_name, 11) &&
                                    (r->cluster == e->cluster || (!r->cluster && !is_dir))
                              : e->cluster && r->cluster == e->cluster)
                    match[i] = k;
            }
        if (match[i] != UINT32_MAX) v->recs[match[i]].seen = 1;
    }

    /* 2: delete what the machine deleted */
    for (uint32_t k = 1; dirty && k < v->n_recs; k++) {
        rec_t *r = &v->recs[k];
        if (r->parent != dir || r->gone || r->seen) continue;
        char real[PATH_MAX_LEN];
        snprintf(real, sizeof real, "%s/%s", path, v->pool + r->name);
        drop_file(v);
        if (remove_tree(real, 0)) s->out->removed++; else s->out->failed++;
        rec_gone(v, k);
    }

    /* 3: renames, new and changed files, folders */
    for (int i = 0; i < n; i++) {
        entry_t *e = &list[i];
        bool is_dir = e->attr & 0x10;
        uint32_t ri = match[i];
        if (ri != UINT32_MAX && !memcmp(v->recs[ri].short_name, e->short_name, 11)) {
            /* the same name: keep the folder's (the card may show it folded) */
            snprintf(e->name, sizeof e->name, "%s", v->pool + v->recs[ri].name);
        } else if (!name_ok(e->name)) {
            s->out->failed++;
            continue;
        }
        char real[PATH_MAX_LEN];
        if (snprintf(real, sizeof real, "%s/%s", path, e->name) >= (int)sizeof real) { s->out->failed++; continue; }
        if (ri != UINT32_MAX && memcmp(v->recs[ri].short_name, e->short_name, 11)) {
            /* renamed: the 8.3 name changed, the cluster did not */
            rec_t *r = &v->recs[ri];
            char old[PATH_MAX_LEN];
            snprintf(old, sizeof old, "%s/%s", path, v->pool + r->name);
            drop_file(v);
            if (strcmp(old, real) && rename(old, real) != 0) { s->out->failed++; continue; }
            s->out->renamed++;
            uint32_t at = pool_add(v, e->name);
            if (at != UINT32_MAX) r->name = at;
            memcpy(r->short_name, e->short_name, 11);
            if (r->node != UINT32_MAX && !v->nodes[r->node].backup) {
                node_t *nd = &v->nodes[r->node];
                if (at != UINT32_MAX) nd->name = at;
                char rel[PATH_MAX_LEN];
                snprintf(rel, sizeof rel, "%s/%s", v->pool + v->nodes[nd->parent].path, e->name);
                set_path(v, r->node, rel);
            }
        }
        if (ri == UINT32_MAX) {
            ri = rec_add(v);
            if (ri == UINT32_MAX) { s->out->failed++; continue; }
            rec_t *r = &v->recs[ri];
            r->parent = dir;
            r->is_dir = is_dir;
            r->seen = 1;
            memcpy(r->short_name, e->short_name, 11);
            r->cluster = is_dir ? e->cluster : UINT32_MAX;      /* a new file is always written */
            r->name = pool_add(v, e->name);
            if (r->name == UINT32_MAX) { r->gone = 1; s->out->failed++; continue; }
            if (is_dir) {
                if (mkdir(real, 0777) == 0) s->out->made++;
                else {
                    struct stat st;
                    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) { s->out->failed++; continue; }
                }
            }
        }
        rec_t *r = &v->recs[ri];
        if (is_dir) {
            r->cluster = e->cluster;
            if (depth + 1 < MAX_DEPTH && cluster_ok(v, e->cluster)) sync_dir(s, e->cluster, real, ri, depth + 1);
            continue;
        }
        if (!file_changed(s, e, r)) continue;
        node_t *nd = r->node != UINT32_MAX ? &v->nodes[r->node] : NULL;
        if (nd && !nd->backup && !nd->gone && nd->size) {
            /* keep the old contents readable (blocks the machine did not
             * rewrite still come from them) under a hidden name */
            char bak[PATH_MAX_LEN], rel[PATH_MAX_LEN];
            snprintf(rel, sizeof rel, "%s/" BACKUP_PREFIX "%u", v->pool + v->nodes[nd->parent].path, (unsigned)r->node);
            if (snprintf(bak, sizeof bak, "%s%s", v->root, rel) >= (int)sizeof bak) { s->out->failed++; continue; }
            drop_file(v);
            if (rename(real, bak) != 0) { s->out->failed++; continue; }
            nd->backup = 1;
            set_path(v, r->node, rel);
        }
        if (write_file(s, e, real)) {
            s->out->written++;
            r->cluster = e->cluster;
            r->size = e->size;
            r->as_scanned = 0;
        } else {
            s->out->failed++;
        }
    }
    free(match);
    free(list);
}

bool cy_vfat_sync(cy_vfat_t *v, cy_vfat_sync_t *out)
{
    cy_vfat_sync_t dummy;
    if (!out) out = &dummy;
    memset(out, 0, sizeof *out);
    bool any = false;
    for (uint32_t i = 0; i < v->n_written && !any; i++) any = !v->written[i].synced;
    if (!any) return false;
    if (!v->n_recs) {
        /* the first sync: the folder is as scanned */
        for (uint32_t i = 0; i < v->n_nodes; i++) {
            uint32_t k = rec_add(v);
            if (k == UINT32_MAX) { v->n_recs = 0; out->failed++; return true; }
            const node_t *nd = &v->nodes[i];
            rec_t *r = &v->recs[k];
            r->parent = i ? nd->parent : UINT32_MAX;
            r->node = i;
            r->name = nd->name;
            memcpy(r->short_name, nd->short_name, 11);
            r->cluster = nd->cluster;
            r->size = nd->size;
            r->is_dir = nd->is_dir;
            r->as_scanned = 1;
        }
    }
    sync_t *s = malloc(sizeof *s);
    if (!s) { out->failed++; return true; }
    s->v = v;
    s->out = out;
    s->fat_blk = UINT32_MAX;
    s->dirty = malloc(v->n_written * sizeof *s->dirty);
    s->n_dirty = 0;
    s->fat_dirty = s->data_dirty = false;
    if (!s->dirty) { free(s); out->failed++; return true; }
    for (uint32_t i = 0; i < v->n_written; i++) {
        uint32_t b = v->written[i].block;
        if (v->written[i].synced) continue;
        s->dirty[s->n_dirty++] = b;
        if (b > PART_START && b <= PART_START + 2 * v->fat_secs) s->fat_dirty = true;
        if (b >= v->data_start) s->data_dirty = true;
    }
    qsort(s->dirty, s->n_dirty, sizeof *s->dirty, cmp_u32);
    drop_file(v);
    sync_dir(s, 0, v->root, 0, 0);
    free(s->dirty);
    free(s);
    for (uint32_t i = 0; i < v->n_written; i++) v->written[i].synced = true;
    return true;
}
