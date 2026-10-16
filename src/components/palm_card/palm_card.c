/* Directory-backed Palm OS 4 expansion storage. ABI constants and layouts are
 * described by the Palm OS 4 SDK VFSMgr.h/ExpansionMgr.h, not host C structs.
 * This implements the managers at their trap boundary, including ROMs whose
 * A347/A348 handlers are unimplemented. No ROM bytes are patched or bundled. */
#include "palm_card.h"
#include "bus.h"
#include "installer.h"
#include "backup.h"
#include "dragonball.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "card_directory.h"
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define CARD_MKDIR(p) _mkdir(p)
#define fileno _fileno
#define fsync _commit
#define ftruncate _chsize
#else
#include <unistd.h>
#include <utime.h>
#define CARD_MKDIR(p) mkdir(p, 0777)
#ifndef ESP_PLATFORM
#include <sys/statvfs.h>
#endif
#endif

#define MAX_HANDLES 8
#define PATH_CAP 384
#define LABEL_CAP 32
#define IO_CHUNK 4096
#define DB_LIMIT (1024u * 1024u)
#define PALM_EPOCH 2082844800u
#define VOLUME 1
#define CREATOR 0x44466364u /* DFcd */
#define OK 0
#define BAD_PARAM 0x0502
#define NO_MEMORY 0x0102
#define ENUM_EMPTY 0x290d
#define BAD_SLOT 0x2904
#define VFS_ERR(n) (0x2a00u | (n))
#define OVERFLOW VFS_ERR(1)
#define IO_ERROR VFS_ERR(2)
#define BAD_REF VFS_ERR(3)
#define STILL_OPEN VFS_ERR(4)
#define DENIED VFS_ERR(5)
#define EXISTS VFS_ERR(6)
#define END_FILE VFS_ERR(7)
#define NOT_FOUND VFS_ERR(8)
#define BAD_VOLUME VFS_ERR(9)
#define BAD_DATA VFS_ERR(12)
#define BAD_NAME VFS_ERR(14)
#define UNIMPLEMENTED VFS_ERR(16)
#define NOT_DIR VFS_ERR(17)
#define IS_DIR VFS_ERR(18)

typedef struct {
    FILE *file;
    DIR *dir;
    uint32_t ref;
    uint16_t mode;
    char path[PATH_CAP];
} card_handle_t;

static card_handle_t handles[MAX_HANDLES];
static char card_root[PATH_CAP], card_label[LABEL_CAP] = "SD Card";
static bool configured, read_only, initialized, rejected;
static uint32_t next_ref;
bool (*palm_card_space)(uint32_t *used, uint32_t *total);

static uint16_t rb16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rb32(const uint8_t *p) { return ((uint32_t)rb16(p) << 16) | rb16(p + 2); }

/* Guest buffers must fit actual RAM/ROM, without bus wrapping, LCD aliasing or
 * peripheral side effects. Never pass guest pointers to host libc. */
static bool writable(uint32_t p, uint32_t n)
{
    return p && p < BUS_RAM_SIZE && n <= BUS_RAM_SIZE - p;
}

static bool readable(uint32_t p, uint32_t n)
{
    return writable(p, n) || (p >= BUS_ROM_BASE &&
        p - BUS_ROM_BASE < g_bus_rom_end - BUS_ROM_LO &&
        n <= g_bus_rom_end - BUS_ROM_LO - (p - BUS_ROM_BASE));
}

static bool guest_string(uint32_t p, char *dst, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        if (!readable(p, 1)) return false;
        dst[i] = (char)bus_read8(p++);
        if (!dst[i]) return true;
    }
    return false;
}

static void copy_out(uint32_t p, const void *src, size_t n)
{
    memcpy(bus_ram_ptr() + p, src, n); /* callers validate the complete range */
}

static uint16_t os_error(void)
{
    switch (errno) {
    case ENOENT: return NOT_FOUND;
    case EEXIST: return EXISTS;
    case EACCES: case EPERM: case EROFS: return DENIED;
    case ENOTDIR: return NOT_DIR;
    case ENOTEMPTY: return VFS_ERR(13);
    case ENOSPC: return VFS_ERR(15);
    default: return IO_ERROR;
    }
}

/* Reject traversal and host-specific paths. On hosts also reject symlinks at
 * every component (including the final file), rather than following them out
 * of the shared directory. FAT on the device has no symlinks. */
static uint16_t path_for(const char *guest, char out[PATH_CAP])
{
    size_t len = strlen(card_root);
    memcpy(out, card_root, len + 1);
    while (*guest == '/') guest++;
    while (*guest) {
        const char *end = strchr(guest, '/');
        size_t n = end ? (size_t)(end - guest) : strlen(guest);
        if (!n) { guest++; continue; }
        if ((n == 1 && guest[0] == '.') || (n == 2 && !memcmp(guest, "..", 2)))
            return BAD_NAME;
        /* FAT/Win32 trim these suffixes; accepting them can turn a component
         * such as '.. ' into traversal when the same card is used on a PC. */
        if (guest[n - 1] == '.' || guest[n - 1] == ' ') return BAD_NAME;
        if (len + n + 2 > PATH_CAP) return BAD_NAME;
        out[len++] = '/';
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)guest[i];
            if (ch < 32 || strchr("\\:*?\"<>|", ch)) return BAD_NAME;
            out[len++] = (char)ch;
        }
        out[len] = 0;
#if !defined(ESP_PLATFORM) && !defined(_WIN32)
        struct stat st;
        if (!lstat(out, &st) && S_ISLNK(st.st_mode)) return DENIED;
#elif defined(_WIN32)
        DWORD attr = GetFileAttributesA(out);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return DENIED;
#endif
        guest += n;
        while (*guest == '/') guest++;
    }
    return OK;
}

static uint16_t guest_path(uint32_t p, char out[PATH_CAP])
{
    char guest[PATH_CAP];
    if (!guest_string(p, guest, sizeof guest) || guest[0] != '/') return BAD_NAME;
    return path_for(guest, out);
}

static card_handle_t *lookup(uint32_t ref)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (handles[i].ref == ref && ref) return &handles[i];
    return NULL;
}

static uint16_t close_handle(card_handle_t *h)
{
    bool failed = false;
    if (h->file) {
        if (h->mode & 4) {
            if (fflush(h->file)) failed = true;
            if (fsync(fileno(h->file))) failed = true;
        }
        if (fclose(h->file)) failed = true;
    }
    if (h->dir && closedir(h->dir)) failed = true;
    memset(h, 0, sizeof *h);
    return failed ? IO_ERROR : OK;
}

void palm_card_reset(void)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (close_handle(&handles[i])) ESP_LOGW("palm_card", "Card close/sync failed");
    initialized = rejected = false;
}

void palm_card_close(void)
{
    palm_card_reset();
    configured = false;
}

bool palm_card_init(const char *root, bool ro)
{
    palm_card_close();
    if (!root || !root[0] || strlen(root) > PATH_CAP - 32) return false;
    snprintf(card_root, sizeof card_root, "%s", root);
    size_t n = strlen(card_root);
    while (n > 1 && card_root[n - 1] == '/') card_root[--n] = 0;
    read_only = ro;
    snprintf(card_label, sizeof card_label, "SD Card");
    struct stat st;
    if (stat(root, &st)) {
        if (ro || CARD_MKDIR(root)) return false;
    } else if (!S_ISDIR(st.st_mode)) return false;
    if (!ro) {
        const char *dirs[] = {"/PALM", "/PALM/Launcher", "/PALM/Programs", "/PALM/Backup"};
        for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
            char p[PATH_CAP];
            if (path_for(dirs[i], p) || (CARD_MKDIR(p) && errno != EEXIST)) return false;
        }
    }
    configured = true;
    return true;
}

bool palm_card_ready(void) { return initialized; }

bool palm_card_sync(void)
{
    bool ok = true;
    for (int i = 0; i < MAX_HANDLES; i++) {
        card_handle_t *h = &handles[i];
        if (h->file && (h->mode & 4)) {
            if (fflush(h->file)) ok = false;
            if (fsync(fileno(h->file))) ok = false;
        }
    }
    return ok;
}

static uint32_t handler(uint16_t trap)
{
    uint32_t table = bus_read32(0x122);
    if (!writable(table, ((trap & 0x7ff) + 1) * 4)) return 0;
    uint32_t pc = bus_read32(table + (trap & 0x7ff) * 4);
    return bus_addr_in_rom(pc) ? pc : 0;
}

static uint32_t call(m68k_cpu_t *cpu, uint16_t trap, const m68k_arg_t *args,
                     int count, uint32_t *a0)
{
    if (a0) *a0 = 0;
    uint32_t pc = handler(trap);
    bool timeout = false;
    if (!pc) return UNIMPLEMENTED;
    uint32_t result = m68k_call_trap(cpu, pc, args, count, a0, &timeout);
    if (timeout && a0) *a0 = 0;
    return timeout ? IO_ERROR : result;
}

#define W(v) { (v), M68K_ARG_W }
#define L(v) { (v), M68K_ARG_L }

static uint32_t alloc_ptr(m68k_cpu_t *cpu, uint32_t n)
{
    m68k_arg_t args[] = {L(n)};
    uint32_t p = 0;
    call(cpu, 0xa013, args, 1, &p);
    if (!writable(p, n)) return 0;
    return p;
}

static void free_ptr(m68k_cpu_t *cpu, uint32_t p)
{
    m68k_arg_t args[] = {L(p)};
    if (p) call(cpu, 0xa012, args, 1, NULL);
}

void palm_card_pump(m68k_cpu_t *cpu)
{
    if (!configured || initialized || rejected || !cpu->regs.stopped ||
        !m68k_ui_initialized() || db_is_asleep()) return;
    uint32_t p = alloc_ptr(cpu, 40);
    if (!p) return;
    m68k_arg_t get[] = {L(0x70737973), W(1), L(p)};
    uint32_t err = call(cpu, 0xa27b, get, 3, NULL);
    uint32_t version = bus_read32(p);
    if (err || version < 0x04000000 || version >= 0x05000000) {
        ESP_LOGW("palm_card", "Virtual card needs Palm OS 4; ROM version %08lX", (unsigned long)version);
        rejected = true;
        free_ptr(cpu, p);
        return;
    }
    /* These upgrade ROMs omit both managers. Register the documented features
     * using Palm's own Feature Manager, so normal feature enumeration works. */
    const uint32_t creators[] = {0x7666736d, 0x6578706e}; /* vfsm, expn */
    for (unsigned i = 0; i < 2; i++) {
        m68k_arg_t set[] = {L(creators[i]), W(0), L(200)};
        err |= call(cpu, 0xa27c, set, 3, NULL);
    }
    if (!err) {
        initialized = true;
        ESP_LOGI("palm_card", "SD Card mounted as Palm VFS volume 1 (%s): %s",
                 read_only ? "read only" : "read/write", card_root);
        /* 'volm' details are a VFSAnyMountParamType, NOT a host pointer. */
        memset(bus_ram_ptr() + p, 0, 40);
        bus_write32(p, 0x766f6c6d); /* volm */
        bus_write32(p + 4, CREATOR);
        bus_write32(p + 8, p + 24);
        bus_write8(p + 16, 3); /* don't auto-run start.prc or change active app */
        bus_write16(p + 24, VOLUME);
        bus_write32(p + 28, 0x706f7365); /* pose */
        /* Run notification handlers on Palm's UI event loop. Synchronously
         * invoking them from an idle STOP may reenter a ROM event wait. */
        m68k_arg_t notify[] = {L(p), W(8)};
        call(cpu, 0xa356, notify, 2, NULL);
    }
    free_ptr(cpu, p);
}

/* Stack arguments are the actual Palm ABI: 2-byte words and 4-byte longs.
 * There is no return PC on this stack until the ROM handles TRAP #15. */
typedef struct { uint32_t sp; } args_t;
static uint16_t aw(args_t *a) { uint16_t v = bus_read16(a->sp); a->sp += 2; return v; }
static uint32_t al(args_t *a) { uint32_t v = bus_read32(a->sp); a->sp += 4; return v; }

static uint16_t file_open(uint16_t vol, uint32_t name, uint16_t mode, uint32_t out)
{
    if (vol != VOLUME) return BAD_VOLUME;
    if (!writable(out, 4)) return BAD_PARAM;
    bus_write32(out, 0);
    if (!(mode & 6) || (mode & ~0x3f) || ((mode & 0x18) && !(mode & 4))) return BAD_PARAM;
    if (read_only && (mode & (4 | 0x18))) return DENIED;
    char path[PATH_CAP];
    uint16_t err = guest_path(name, path);
    if (err) return err;
    card_handle_t *h = NULL;
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].ref && !strcmp(handles[i].path, path) &&
            ((mode | handles[i].mode) & 5)) return STILL_OPEN;
        if (!handles[i].ref) h = &handles[i];
    }
    if (!h) return VFS_ERR(15);
    struct stat st;
    bool exists = !stat(path, &st);
    if (exists && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return DENIED;
    if (exists && S_ISDIR(st.st_mode)) {
        if (mode & (4 | 0x18)) return IS_DIR;
        h->dir = opendir(path);
        if (!h->dir) return os_error();
    } else {
        if (!exists && !(mode & 8)) return NOT_FOUND;
        h->file = fopen(path, mode & 4 ? ((mode & 16) || !exists ? "w+b" : "r+b") : "rb");
        if (!h->file) return os_error();
        /* Avoid a large libc buffer per guest handle on the ESP32. */
        setvbuf(h->file, NULL, _IONBF, 0);
    }
    h->mode = mode;
    if (++next_ref == 0) ++next_ref;
    h->ref = next_ref;
    snprintf(h->path, sizeof h->path, "%s", path);
    bus_write32(out, h->ref);
    return OK;
}

static uint16_t transfer(card_handle_t *h, uint32_t n, uint32_t p,
                         uint32_t done, bool writing)
{
    if (done && !writable(done, 4)) return BAD_PARAM;
    if (done) bus_write32(done, 0);
    if (!h) return BAD_REF;
    if (h->dir) return IS_DIR;
    if (!(h->mode & (writing ? 4 : 2))) return DENIED;
    if (n && !(writing ? readable(p, n) : writable(p, n))) return BAD_PARAM;
    uint32_t count = 0;
    uint8_t buf[IO_CHUNK];
    while (count < n) {
        size_t chunk = n - count;
        if (chunk > sizeof buf) chunk = sizeof buf;
        size_t got;
        if (writing) {
            for (size_t i = 0; i < chunk; i++) buf[i] = bus_read8(p + count + i);
            got = fwrite(buf, 1, chunk, h->file);
        } else {
            got = fread(buf, 1, chunk, h->file);
            copy_out(p + count, buf, got);
        }
        count += (uint32_t)got;
        if (got != chunk) break;
    }
    if (done) bus_write32(done, count);
    if (ferror(h->file)) { clearerr(h->file); return IO_ERROR; }
    return count < n ? (writing ? IO_ERROR : END_FILE) : OK;
}

static uint32_t attributes(const char *name, const struct stat *st)
{
    const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
    return (S_ISDIR(st->st_mode) ? 0x10 : 0x20) |
        (read_only || !(st->st_mode & 0222) ? 1 : 0) | (base[0] == '.' ? 2 : 0);
}

static uint16_t enumerate(card_handle_t *h, uint32_t iterator, uint32_t info)
{
    if (!h) return BAD_REF;
    if (!h->dir) return NOT_DIR;
    if (!writable(iterator, 4) || !writable(info, 10)) return BAD_PARAM;
    uint32_t index = bus_read32(iterator), name = bus_read32(info + 4);
    uint16_t cap = bus_read16(info + 8);
    if (name && (!cap || !writable(name, cap))) return BAD_PARAM;
    if (index == UINT32_MAX) return ENUM_EMPTY;
    rewinddir(h->dir);
    struct dirent *de;
    uint32_t seen = 0;
    while ((de = readdir(h->dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (seen++ < index) continue;
        char full[PATH_CAP];
        if (snprintf(full, sizeof full, "%s/%s", h->path, de->d_name) >= sizeof full) continue;
        struct stat st;
#if !defined(ESP_PLATFORM) && !defined(_WIN32)
        if (lstat(full, &st) || S_ISLNK(st.st_mode)) continue;
#else
        if (stat(full, &st)) continue;
#ifdef _WIN32
        if (GetFileAttributesA(full) & FILE_ATTRIBUTE_REPARSE_POINT) continue;
#endif
#endif
        if (name && strlen(de->d_name) + 1 > cap) return OVERFLOW;
        bus_write32(info, attributes(de->d_name, &st));
        if (name) copy_out(name, de->d_name, strlen(de->d_name) + 1);
        bus_write32(iterator, seen);
        return OK;
    }
    bus_write32(iterator, UINT32_MAX);
    return ENUM_EMPTY;
}

/* Read PRC/PDB metadata without installing the database. This is how the Palm
 * launcher obtains names and icons for applications on the card. */
typedef struct { uint8_t head[78]; uint32_t size; uint16_t count; bool resource; } db_file_t;

static uint16_t db_header(card_handle_t *h, db_file_t *db)
{
    if (!h) return BAD_REF;
    if (!h->file || !(h->mode & 2)) return BAD_DATA;
    long pos = ftell(h->file);
    if (pos < 0 || fseek(h->file, 0, SEEK_END)) return IO_ERROR;
    long size = ftell(h->file);
    if (size < 78 || (uint64_t)size > UINT32_MAX || fseek(h->file, 0, SEEK_SET)) return BAD_DATA;
    size_t n = fread(db->head, 1, 78, h->file);
    fseek(h->file, pos, SEEK_SET);
    if (n != 78 || !memchr(db->head, 0, 32)) return BAD_DATA;
    db->size = (uint32_t)size;
    db->resource = (db->head[33] & 1) != 0;
    db->count = rb16(db->head + 76);
    if (78u + db->count * (db->resource ? 10u : 8u) > db->size) return BAD_DATA;
    return OK;
}

static bool read_at(FILE *f, uint32_t offset, void *buf, size_t n)
{
    long pos = ftell(f);
    bool ok = pos >= 0 && !fseek(f, offset, SEEK_SET) && fread(buf, 1, n, f) == n;
    if (pos >= 0 && fseek(f, pos, SEEK_SET)) ok = false;
    return ok;
}

/* The next higher chunk offset, including app/sort info, bounds a PDB chunk. */
static uint16_t chunk_size(card_handle_t *h, const db_file_t *db, uint32_t start,
                           unsigned entry_index, uint32_t *n)
{
    uint32_t end = db->size;
    unsigned step = db->resource ? 10 : 8;
    uint32_t table_end = 78u + db->count * step;
    if (start < table_end || start > db->size) return BAD_DATA;
    for (unsigned j = 0; j < 2; j++) {
        uint32_t offset = rb32(db->head + 52 + j * 4);
        if (offset && (offset < table_end || offset > db->size)) return BAD_DATA;
        if (offset > start && offset < end) end = offset;
    }
    for (unsigned i = 0; i < db->count; i++) {
        uint8_t entry[10];
        if (!read_at(h->file, 78u + i * step, entry, step)) return IO_ERROR;
        uint32_t offset = rb32(entry + (db->resource ? 6 : 0));
        if (offset < table_end || offset > db->size) return BAD_DATA;
        if (entry_index != UINT32_MAX && i > entry_index) {
            /* Equal adjacent offsets represent an empty record/resource. */
            if (offset < start) return BAD_DATA;
            if (offset < end) end = offset;
        } else if (offset > start && offset < end) end = offset;
    }
    *n = end - start;
    return OK;
}

static void free_handle(m68k_cpu_t *cpu, uint32_t handle)
{
    m68k_arg_t args[] = {L(handle)};
    if (handle) call(cpu, 0xa02b, args, 1, NULL);
}

static uint16_t chunk_handle(m68k_cpu_t *cpu, card_handle_t *h, const db_file_t *db,
                             uint32_t start, unsigned entry_index, uint32_t out)
{
    if (!writable(out, 4)) return BAD_PARAM;
    bus_write32(out, 0);
    if (!start) return OK;
    uint32_t n;
    uint16_t err = chunk_size(h, db, start, entry_index, &n);
    if (err) return err;
    if (!n) return OK; /* no record data: documented NULL handle, not an allocation failure */
    if (n > DB_LIMIT) return BAD_DATA;
    m68k_arg_t alloc[] = {L(0), L(n)};
    uint32_t handle = 0, ptr = 0;
    /* VFS DB chunks belong to Palm's storage heap, which is much larger than
     * the dynamic heap on these ROMs. DmNewHandle chunks are caller-owned. */
    call(cpu, 0xa059, alloc, 2, &handle);
    if (!handle) return NO_MEMORY;
    m68k_arg_t lock[] = {L(handle)};
    call(cpu, 0xa021, lock, 1, &ptr);
    err = !writable(ptr, n) ? NO_MEMORY :
        read_at(h->file, start, bus_ram_ptr() + ptr, n) ? OK : IO_ERROR;
    if (ptr) call(cpu, 0xa022, lock, 1, NULL);
    if (err) free_handle(cpu, handle);
    else bus_write32(out, handle);
    return err;
}

static uint16_t db_resource(m68k_cpu_t *cpu, card_handle_t *h, uint32_t type,
                            uint16_t id, uint32_t out)
{
    if (!writable(out, 4)) return BAD_PARAM;
    bus_write32(out, 0);
    db_file_t db;
    uint16_t err = db_header(h, &db);
    if (err) return err;
    if (!db.resource) return 0x020d; /* dmErrNotResourceDB */
    for (unsigned i = 0; i < db.count; i++) {
        uint8_t entry[10];
        if (!read_at(h->file, 78u + i * 10, entry, 10)) return IO_ERROR;
        if (rb32(entry) == type && rb16(entry + 4) == id)
            return chunk_handle(cpu, h, &db, rb32(entry + 6), i, out);
    }
    return 0x0210; /* dmErrResourceNotFound */
}

static uint16_t db_info(m68k_cpu_t *cpu, card_handle_t *h, args_t *a)
{
    uint32_t out[12];
    for (int i = 0; i < 12; i++) out[i] = al(a);
    const unsigned sizes[] = {32,2,2,4,4,4,4,4,4,4,4,2};
    for (int i = 0; i < 12; i++) if (out[i] && !writable(out[i], sizes[i])) return BAD_PARAM;
    db_file_t db;
    uint16_t err = db_header(h, &db);
    if (err) return err;
    if (out[0]) copy_out(out[0], db.head, 32);
    for (int i = 1; i <= 2; i++) if (out[i]) bus_write16(out[i], rb16(db.head + 30 + i*2));
    for (int i = 3; i <= 6; i++) if (out[i]) bus_write32(out[i], rb32(db.head + 24 + i*4));
    if (out[9]) bus_write32(out[9], rb32(db.head + 60));
    if (out[10]) bus_write32(out[10], rb32(db.head + 64));
    if (out[11]) bus_write16(out[11], db.count);
    if (out[7]) bus_write32(out[7], 0);
    if (out[8]) bus_write32(out[8], 0);
    for (int i = 7; i <= 8; i++) {
        if (out[i]) {
            err = chunk_handle(cpu, h, &db, rb32(db.head + 52 + (i-7)*4), UINT32_MAX, out[i]);
            if (err) {
                if (i == 8 && out[7]) { free_handle(cpu, bus_read32(out[7])); bus_write32(out[7], 0); }
                return err;
            }
        }
    }
    return OK;
}

static uint16_t import_db(m68k_cpu_t *cpu, uint32_t name, uint32_t card, uint32_t dbid)
{
    if ((card && !writable(card, 2)) || (dbid && !writable(dbid, 4))) return BAD_PARAM;
    char path[PATH_CAP];
    uint16_t err = guest_path(name, path);
    if (err) return err;
    FILE *f = fopen(path, "rb");
    if (!f) return os_error();
    if (fseek(f, 0, SEEK_END)) { fclose(f); return IO_ERROR; }
    long n = ftell(f);
    if (n < 78 || n > DB_LIMIT || fseek(f, 0, SEEK_SET)) { fclose(f); return BAD_DATA; }
    uint8_t *image = malloc((size_t)n);
    if (!image) { fclose(f); return NO_MEMORY; }
    size_t got = fread(image, 1, (size_t)n, f);
    fclose(f);
    if (got != n || !memchr(image, 0, 32)) { free(image); return BAD_DATA; }
    uint32_t p = alloc_ptr(cpu, 32);
    if (!p) { free(image); return NO_MEMORY; }
    copy_out(p, image, 32);
    m68k_arg_t find[] = {W(0), L(p)};
    uint32_t existing = call(cpu, 0xa045, find, 2, NULL);
    /* VFS import must never overwrite an existing database (unlike the manual
     * installer). The launcher decides whether to delete/replace it. */
    installer_result_t result = existing ? INSTALLER_ALREADY_PRESENT :
        installer_install_image(cpu, image, n, NULL);
    free(image);
    uint32_t id = existing ? existing : call(cpu, 0xa045, find, 2, NULL);
    free_ptr(cpu, p);
    if (result != INSTALLER_OK && result != INSTALLER_NEEDS_RESET && result != INSTALLER_ALREADY_PRESENT)
        return result == INSTALLER_ERR_OUT_OF_MEMORY ? NO_MEMORY : BAD_DATA;
    if (card) bus_write16(card, 0);
    if (dbid) bus_write32(dbid, id);
    return result == INSTALLER_ALREADY_PRESENT ? 0x0219 : OK; /* dmErrAlreadyExists */
}

static uint16_t export_db(m68k_cpu_t *cpu, uint32_t name, uint16_t card, uint32_t id)
{
    if (read_only) return DENIED;
    if (card) return BAD_PARAM;
    char path[PATH_CAP];
    uint16_t err = guest_path(name, path);
    if (err) return err;
    struct stat st;
    if (!stat(path, &st)) return EXISTS;
    /* Metadata array is on the host heap, never the ESP32 task stack. */
    backup_db_info_t *dbs = malloc(sizeof(*dbs) * BACKUP_MAX_DBS);
    if (!dbs) return NO_MEMORY;
    int n = backup_enumerate(cpu, dbs, BACKUP_MAX_DBS), index = -1;
    for (int i = 0; i < n; i++) if (dbs[i].dbID == id) { index = i; break; }
    if (index < 0) { free(dbs); return BAD_DATA; }
    FILE *f = fopen(path, "wb");
    if (!f) { free(dbs); return os_error(); }
    backup_sink_t sink = {backup_file_sink, f};
    backup_result_t r = backup_serialize_db(cpu, &dbs[index], &sink);
    free(dbs);
    bool failed = r != BACKUP_OK;
    if (fflush(f)) failed = true;
    if (fsync(fileno(f))) failed = true;
    if (fclose(f)) failed = true;
    if (failed) { remove(path); return IO_ERROR; }
    return OK;
}

static uint16_t expansion(uint16_t sel, args_t *a)
{
    if (sel == 0) return OK;
    if (sel == 10) {
        uint32_t out = al(a), it = al(a);
        if (!writable(out, 2) || !writable(it, 4)) return BAD_PARAM;
        /* A host volume has no emulated physical slot driver. */
        bus_write32(it, UINT32_MAX);
        return ENUM_EMPTY;
    }
    return sel == 8 || sel == 9 || sel == 3 ? BAD_SLOT : 0x290c;
}

static uint16_t vfs(m68k_cpu_t *cpu, uint16_t sel, args_t *a)
{
    if (sel == 0) return OK;
    if (sel == 27) {
        uint32_t out = al(a), it = al(a);
        if (!writable(out, 2) || !writable(it, 4)) return BAD_PARAM;
        if (bus_read32(it) != 0) { bus_write32(it, UINT32_MAX); return ENUM_EMPTY; }
        bus_write16(out, VOLUME);
        bus_write32(it, UINT32_MAX);
        return OK;
    }
    if (sel == 2 || sel == 3 || sel == 8 || sel == 9 || sel == 19 ||
        (sel >= 28 && sel <= 31) || sel == 21 || sel == 34 || sel == 35 || sel == 39 || sel == 40) {
        uint16_t vol = aw(a);
        if (vol != VOLUME) return BAD_VOLUME;
        uint32_t p = al(a);
        if (sel == 3) { uint16_t mode = aw(a); uint32_t out = al(a); return file_open(vol, p, mode, out); }
        if (sel == 34 || sel == 39) {
            uint32_t card = al(a), id = al(a);
            if (sel == 39 && al(a)) return UNIMPLEMENTED; /* callback ABI not implemented */
            return import_db(cpu, p, card, id);
        }
        if (sel == 35 || sel == 40) {
            uint16_t card = aw(a); uint32_t id = al(a);
            if (sel == 40 && al(a)) return UNIMPLEMENTED;
            return export_db(cpu, p, card, id);
        }
        if (sel == 28) {
            if (!writable(p, 28)) return BAD_PARAM;
            memset(bus_ram_ptr() + p, 0, 28);
            bus_write32(p, read_only ? 2 : 0);
            bus_write32(p + 4, 0x76666174); /* vfat */
            bus_write32(p + 8, CREATOR);
            bus_write32(p + 12, 0x706f7365); /* pose */
            bus_write32(p + 20, 0x706f7365);
            return OK;
        }
        if (sel == 29) {
            uint16_t n = aw(a);
            if (!writable(p, n) || !n) return BAD_PARAM;
            size_t size = strlen(card_label) + 1;
            if (size > n) return OVERFLOW;
            copy_out(p, card_label, size); return OK;
        }
        if (sel == 30) {
            if (read_only) return DENIED;
            /* A directory-backed volume has a fixed label across boots. */
            return UNIMPLEMENTED;
        }
        if (sel == 31) {
            uint32_t total_out = al(a), used = 0, total = 0;
            if ((p && !writable(p, 4)) || (total_out && !writable(total_out, 4))) return BAD_PARAM;
            bool ok = palm_card_space && palm_card_space(&used, &total);
#if !defined(ESP_PLATFORM) && !defined(_WIN32)
            if (!ok) {
                struct statvfs st;
                if (!statvfs(card_root, &st)) {
                    uint64_t t = (uint64_t)st.f_blocks * st.f_frsize;
                    uint64_t free = (uint64_t)st.f_bavail * st.f_frsize;
                    total = t > UINT32_MAX ? UINT32_MAX : (uint32_t)t;
                    used = free >= total ? 0 : total - (uint32_t)free;
                    ok = true;
                }
            }
#endif
#ifdef _WIN32
            if (!ok) {
                ULARGE_INTEGER available, size, free;
                if (GetDiskFreeSpaceExA(card_root, &available, &size, &free)) {
                    total = size.QuadPart > UINT32_MAX ? UINT32_MAX : (uint32_t)size.QuadPart;
                    used = available.QuadPart >= total ? 0 : total - (uint32_t)available.QuadPart;
                    ok = true;
                }
            }
#endif
            if (!ok) return IO_ERROR;
            if (p) bus_write32(p, used);
            if (total_out) bus_write32(total_out, total);
            return OK;
        }
        if (sel == 21) {
            char ext[40];
            if (!guest_string(p, ext, sizeof ext)) return BAD_PARAM;
            uint32_t out = al(a), len = al(a);
            if (!writable(len, 2)) return BAD_PARAM;
            uint16_t cap = bus_read16(len);
            const char *dir = !strcmp(ext, ".prc") || !strcmp(ext, ".pdb") || !strcmp(ext, ".pqa") ?
                "/PALM/Launcher/" : "/PALM/Programs/";
            bus_write16(len, (uint16_t)(strlen(dir) + 1));
            if (cap < strlen(dir) + 1) return OVERFLOW;
            if (!writable(out, cap)) return BAD_PARAM;
            copy_out(out, dir, strlen(dir) + 1); return OK;
        }
        if (read_only) return DENIED;
        char path[PATH_CAP];
        uint16_t err = guest_path(p, path);
        if (err) return err;
        if (!strcmp(path, card_root)) return DENIED;
        size_t path_len = strlen(path);
        for (int i = 0; i < MAX_HANDLES; i++)
            if (handles[i].ref && (!strcmp(handles[i].path, path) ||
                (!strncmp(handles[i].path, path, path_len) && handles[i].path[path_len] == '/')))
                return STILL_OPEN;
        if (sel == 19) return CARD_MKDIR(path) ? os_error() : OK;
        if (sel == 8) return remove(path) ? os_error() : OK;
        if (sel == 9) {
            char name[PATH_CAP], to[PATH_CAP];
            if (!guest_string(al(a), name, sizeof name) || !name[0] || strchr(name, '/')) return BAD_NAME;
            const char *base = strrchr(path, '/');
            size_t prefix = (size_t)(base - path - strlen(card_root));
            char relative[PATH_CAP];
            size_t suffix = strlen(name);
            if (prefix + 1 + suffix >= sizeof relative) return BAD_NAME;
            memcpy(relative, path + strlen(card_root), prefix);
            relative[prefix] = '/';
            memcpy(relative + prefix + 1, name, suffix + 1);
            err = path_for(relative, to);
            if (err) return err;
            struct stat st;
            if (!stat(to, &st)) return EXISTS;
            return rename(path, to) ? os_error() : OK;
        }
        struct stat st;
        if (!stat(path, &st)) return EXISTS;
        FILE *f = fopen(path, "wb");
        if (!f) return os_error();
        return fclose(f) ? IO_ERROR : OK;
    }
    if (sel == 1 || (sel >= 22 && sel <= 26) || sel == 32 || sel == 33 || sel >= 41)
        return UNIMPLEMENTED;
    uint32_t ref = al(a);
    card_handle_t *h = lookup(ref);
    if (!h) return BAD_REF;
    if (sel == 4) return close_handle(h);
    if (sel == 20) { uint32_t it = al(a), info = al(a); return enumerate(h, it, info); }
    if (sel == 36) { uint32_t type = al(a); uint16_t id = aw(a); uint32_t out = al(a); return db_resource(cpu,h,type,id,out); }
    if (sel == 37) return db_info(cpu, h, a);
    if (sel == 38) {
        uint16_t index = aw(a); uint32_t out = al(a), attr = al(a), uid = al(a);
        if ((!out && !attr && !uid) || (out && !writable(out, 4)) ||
            (attr && !writable(attr, 1)) || (uid && !writable(uid, 4))) return BAD_PARAM;
        db_file_t db;
        uint16_t err = db_header(h, &db);
        if (err) return err;
        if (db.resource) return 0x020c; /* dmErrNotRecordDB */
        if (index >= db.count) return 0x0202; /* dmErrIndexOutOfRange */
        uint8_t entry[8];
        if (!read_at(h->file, 78u + index * 8, entry, 8)) return IO_ERROR;
        err = out ? chunk_handle(cpu, h, &db, rb32(entry), index, out) : OK;
        if (!err) {
            if (attr) bus_write8(attr, entry[4]);
            if (uid) bus_write32(uid, ((uint32_t)entry[5] << 16) | ((uint32_t)entry[6] << 8) | entry[7]);
        }
        return err;
    }
    if (sel == 5 || sel == 6 || sel == 7) {
        uint32_t n = al(a), p = al(a);
        if (sel == 5) { uint32_t offset = al(a); if (offset > UINT32_MAX - p) return BAD_PARAM; p += offset; }
        return transfer(h, n, p, al(a), sel == 7);
    }
    if (sel == 14 || sel == 16) {
        uint16_t which = sel == 16 ? aw(a) : 0;
        uint32_t out = al(a);
        if (!writable(out, 4)) return BAD_PARAM;
        struct stat st;
        if (stat(h->path, &st)) return os_error();
        if (sel == 14) bus_write32(out, attributes(h->path, &st));
        else {
            if (which < 1 || which > 3) return BAD_PARAM;
            time_t date = which == 3 ? st.st_atime : st.st_mtime;
            bus_write32(out, (uint32_t)date + PALM_EPOCH);
        }
        return OK;
    }
    if (sel == 15 || sel == 17) return read_only ? DENIED : UNIMPLEMENTED;
    if (!h->file) return IS_DIR;
    if (sel == 10) {
        uint16_t origin = aw(a); int32_t offset = (int32_t)al(a);
        if (origin > 2) return BAD_PARAM;
        if (fseek(h->file, offset, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END)) return IO_ERROR;
        clearerr(h->file); return OK;
    }
    if (sel == 13) {
        uint32_t n = al(a);
        if (!(h->mode & 4) || read_only) return DENIED;
        if (n > INT32_MAX) return BAD_PARAM;
        return fflush(h->file) || ftruncate(fileno(h->file), n) ? IO_ERROR : OK;
    }
    long pos = ftell(h->file);
    if (pos < 0) return IO_ERROR;
    if (sel == 12) {
        uint32_t out = al(a);
        if (!writable(out, 4)) return BAD_PARAM;
        bus_write32(out, (uint32_t)pos); return OK;
    }
    if (sel == 11 || sel == 18) {
        if (fseek(h->file, 0, SEEK_END)) return IO_ERROR;
        long size = ftell(h->file);
        if (fseek(h->file, pos, SEEK_SET) || size < 0) return IO_ERROR;
        if (sel == 11) return pos >= size ? END_FILE : OK;
        uint32_t out = al(a);
        if (!writable(out, 4)) return BAD_PARAM;
        bus_write32(out, (uint32_t)size); return OK;
    }
    return UNIMPLEMENTED;
}

bool palm_card_trap(m68k_cpu_t *cpu, uint16_t trap)
{
    if (configured && trap == 0xa08c) { palm_card_reset(); return false; } /* SysReset */
    if (!configured || rejected || (trap != 0xa348 && trap != 0xa347)) return false;
    if (!writable(cpu->regs.a[7], 64)) { cpu->regs.d[0] = BAD_PARAM; return true; }
    args_t args = {cpu->regs.a[7]};
    uint16_t sel = (uint16_t)cpu->regs.d[2];
    cpu->regs.d[0] = trap == 0xa348 ? vfs(cpu, sel, &args) : expansion(sel, &args);
    return true;
}
