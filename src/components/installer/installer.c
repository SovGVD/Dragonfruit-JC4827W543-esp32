#include "installer.h"
#include "backup.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"   /* host: stubbed by esp_shim.h */
#include "bus.h"        /* bus_read/write, g_bus_ram, BUS_RAM_SIZE */
#include "dragonball.h" /* db_is_asleep, db_lcd_framebuffer_addr */

#ifndef HOST_BUILD
#include "esp_heap_caps.h"   /* PSRAM alloc for the per-DB backup entry table */
#endif

void (*g_installer_yield)(void) = NULL;
int g_installer_last_fail_step = 0;   /* stage of the most recent failed attempt */
uint16_t g_installer_last_dm_err = 0; /* raw Data Manager error from that attempt */

/* ---- Palm OS trap selectors (A-line dispatch numbers; see CoreTraps.h). The
 * handler address for a selector `sel` is ram32[globals + (sel & 0x7FF)*4]. --- */
#define TRAP_MemChunkFree         0xA012   /* MemPtrFree */
#define TRAP_MemPtrNew            0xA013
#define TRAP_MemHandleToLocalID   0xA020
#define TRAP_MemHandleLock        0xA021
#define TRAP_MemHandleUnlock      0xA022
#define TRAP_DmCreateDatabase     0xA041
#define TRAP_DmDeleteDatabase     0xA042
#define TRAP_DmFindDatabase       0xA045
#define TRAP_DmSetDatabaseInfo    0xA047
#define TRAP_DmOpenDatabase       0xA049
#define TRAP_DmCloseDatabase      0xA04A
#define TRAP_DmSetRecordInfo      0xA051
#define TRAP_DmNewRecord          0xA055
#define TRAP_DmNewHandle          0xA059
#define TRAP_DmReleaseRecord      0xA05E
#define TRAP_DmReleaseResource    0xA061
#define TRAP_DmNewResource        0xA06C
#define TRAP_DmWrite              0xA076

/* Additional selectors used by the BACKUP (read/enumerate) path. */
#define TRAP_MemPtrSize           0xA016
#define TRAP_MemLocalIDToGlobal   0xA023
#define TRAP_MemHandleSize        0xA02D
#define TRAP_MemPtrUnlock         0xA035
#define TRAP_MemLocalIDToLockedPtr 0xA036
#define TRAP_DmNumDatabases       0xA043
#define TRAP_DmGetDatabase        0xA044
#define TRAP_DmDatabaseInfo       0xA046   /* same ABI as DmSetDatabaseInfo, out-params */
#define TRAP_DmNumRecords         0xA04F
#define TRAP_DmRecordInfo         0xA050
#define TRAP_DmQueryRecord        0xA05B
#define TRAP_DmNumResources       0xA067
#define TRAP_DmResourceInfo       0xA068
#define TRAP_DmGetResourceIndex   0xA06E

/* Palm OS constants. */
#define DM_MODE_READWRITE         0x0003
#define DM_MODE_READONLY          0x0001
#define DM_HDR_ATTR_RESDB         0x0001
#define DM_HDR_ATTR_BACKUP        0x0008   /* dmHdrAttrBackup (HotSync backup conduit) */
#define DM_HDR_ATTR_RESET         0x0020   /* dmHdrAttrResetAfterInstall */
#define DM_HDR_ATTR_LAUNCHABLE    0x0200   /* dmHdrAttrLaunchableData (PQA-style) */
#define DM_HDR_ATTR_OPEN          0x8000   /* dmHdrAttrOpen ("not closed properly") */

#define DM_ERR_MEM_ERROR          0x0201
#define DM_ERR_DATABASE_OPEN      0x0205
#define DM_ERR_ROM_BASED          0x020E

/* PDB/PRC on-disk layout (all big-endian). */
#define HDR_SIZE        78
#define HDR_NAME_LEN    32
#define REC_ENTRY_SIZE  8    /* localChunkID(4) attr(1) uniqueID(3) */
#define RES_ENTRY_SIZE  10   /* type(4) id(2) localChunkID(4) */

/* Big-endian byte (de)serialization (be16/be32 read, wb16/wb32 write) over raw
 * image buffers lives in bus.h — the 68k memory image is big-endian, so this
 * shares the same byte order as the bus accessors. */

const char *installer_result_str(installer_result_t r)
{
    switch (r) {
    case INSTALLER_OK:                  return "ok";
    case INSTALLER_NEEDS_RESET:         return "ok (needs reset)";
    case INSTALLER_ERR_NOT_READY:       return "not ready";
    case INSTALLER_ERR_BAD_IMAGE:       return "bad image";
    case INSTALLER_ERR_DB_OPEN:         return "database in use";
    case INSTALLER_ERR_OUT_OF_MEMORY:   return "out of memory";
    case INSTALLER_ERR_CORRUPT:         return "data manager error";
    case INSTALLER_ERR_TRAP_UNRESOLVED: return "trap unresolved";
    case INSTALLER_ERR_TIMEOUT:         return "trap timed out";
    default:                            return "unknown error";
    }
}

static int kind_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return -1;
    /* case-insensitive 3-char extension match */
    char e[4];
    for (int i = 0; i < 3; i++) {
        char c = dot[1 + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        e[i] = c;
    }
    e[3] = '\0';
    if (dot[4] != '\0') return -1;           /* extension longer than 3 chars */
    if (!strcmp(e, "prc")) return IMG_PRC;
    if (!strcmp(e, "pdb")) return IMG_PDB;
    if (!strcmp(e, "pqa")) return IMG_PQA;
    return -1;
}

bool installer_add_buffer(install_set_t *set, const char *name,
                          uint8_t *data, size_t size)
{
    if (set->count >= INSTALL_MAX_FILES) return false;
    if (size == 0 || size > INSTALL_MAX_FILE_BYTES) return false;
    if (set->total_bytes + size > INSTALL_MAX_TOTAL_BYTES) return false;
    int k = kind_of(name);
    if (k < 0) return false;

    install_image_t *im = &set->img[set->count];
    snprintf(im->name, sizeof(im->name), "%.*s", (int)sizeof(im->name) - 1, name);
    im->data   = data;
    im->size   = size;
    im->kind   = (install_kind_t)k;
    im->status = IMG_PENDING;
    im->result = INSTALLER_OK;
    set->total_bytes += size;
    set->count++;
    return true;
}

/* ---- trap resolution ------------------------------------------------------ */
static uint32_t trap_handler(uint32_t sel)
{
    uint32_t globals = bus_read32(0x122);
    if (!globals || (uint32_t)(globals + (sel & 0x7FF) * 4 + 4) >= BUS_RAM_SIZE)
        return 0;
    uint32_t h = bus_read32(globals + (sel & 0x7FF) * 4);
    return bus_addr_in_rom(h) ? h : 0;
}

/* Every trap the install engine calls. Readiness requires ALL of these to
 * resolve into ROM — early in boot the dispatch table is only partially
 * populated, so checking a single trap (e.g. DmCreateDatabase, which appears
 * almost immediately) reports ready far too early. */
static const uint32_t REQUIRED_TRAPS[] = {
    TRAP_MemChunkFree, TRAP_MemPtrNew, TRAP_MemHandleToLocalID, TRAP_MemHandleLock,
    TRAP_MemHandleUnlock, TRAP_DmCreateDatabase, TRAP_DmDeleteDatabase,
    TRAP_DmFindDatabase, TRAP_DmSetDatabaseInfo, TRAP_DmOpenDatabase,
    TRAP_DmCloseDatabase, TRAP_DmSetRecordInfo, TRAP_DmNewRecord, TRAP_DmNewHandle,
    TRAP_DmReleaseRecord, TRAP_DmReleaseResource, TRAP_DmNewResource, TRAP_DmWrite,
};

static bool all_traps_resolve(void)
{
    for (size_t i = 0; i < sizeof(REQUIRED_TRAPS) / sizeof(REQUIRED_TRAPS[0]); i++)
        if (!trap_handler(REQUIRED_TRAPS[i])) return false;
    return true;
}

/* Cheap, READ-ONLY readiness check: awake, LCD up, and every trap the engine
 * needs resolves into ROM. This does NOT call any trap — calling one before the
 * memory manager is initialised (which happens after the dispatch table is
 * populated) crashes the OS. So this is necessary but not sufficient on its own;
 * before driving any install trap the caller must also wait for a settle signal
 * (the boot->UI framebuffer switch, or an idle STOP on builds that doze) and a
 * functional heap probe — which is exactly what installer_pump() does. */
bool installer_os_ready(void)
{
    if (db_is_asleep()) return false;
    if (db_lcd_framebuffer_addr() == 0) return false;
    return all_traps_resolve();
}

/* Functional readiness probe (see installer.h): round-trip MemPtrNew + MemPtrFree
 * to prove the dynamic heap — and thus the memory/data managers — is actually
 * live. This DRIVES TRAPS, so the caller must only invoke it once installer_os_
 * ready() passes AND a cheap settle signal says we are past MemInit; calling
 * MemPtrNew before MemInit crashes the OS. */
bool installer_probe_heap(m68k_cpu_t *cpu)
{
    uint32_t h_new  = trap_handler(TRAP_MemPtrNew);
    uint32_t h_free = trap_handler(TRAP_MemChunkFree);
    if (!h_new || !h_free) return false;

    m68k_arg_t na[] = { { 16, M68K_ARG_L } };
    uint32_t p = 0;
    bool to = false;
    m68k_call_trap(cpu, h_new, na, 1, &p, &to);       /* MemPtrNew(16) -> A0 */
    if (to || !p || p >= BUS_RAM_SIZE) return false;  /* timeout => not ready */

    m68k_arg_t fa[] = { { p, M68K_ARG_L } };
    m68k_call_trap(cpu, h_free, fa, 1, NULL, NULL);   /* MemPtrFree(p) */
    return true;
}

/* ---- install context: resolved handlers + live state for cleanup ---------- */
typedef struct {
    m68k_cpu_t *cpu;
    uint32_t h_create, h_delete, h_find, h_setinfo, h_open, h_close;
    uint32_t h_setrecinfo, h_newrec, h_newhandle, h_relrec, h_relres, h_newres, h_write;
    uint32_t h_memfree, h_memnew, h_memh2lid, h_memlock, h_memunlock;
    /* Backup (read/enumerate) handlers — resolved by resolve_export_traps. */
    uint32_t h_numdb, h_getdb, h_dbinfo, h_numrec, h_recinfo, h_query;
    uint32_t h_numres, h_resinfo, h_getresidx, h_handlesize, h_ptrsize, h_lid2g;
    uint32_t h_lidlocked, h_ptrunlock;
    uint32_t scratch;   /* emulated dynamic-heap scratch block (0 = none) */
    uint32_t openRef;   /* open database (0 = none) */
    uint32_t dbID;      /* created database LocalID (0 = none) */
    bool     aborted;   /* a driven trap hit the runaway guard (see ix_call) */
} ictx_t;

/* Every Data Manager / Memory Manager trap call funnels through here so that a
 * runaway ROM trap (the single-step guard tripping in m68k_call_trap) latches
 * c->aborted. We do NOT short-circuit once aborted: the done: cleanup path also
 * calls traps through here, and tearing down the half-built database is worth
 * the bounded cost of letting those run (each either completes or times out
 * once more). The install loop checks c->aborted at its forward-progress points
 * and bails to INSTALLER_ERR_TIMEOUT. */
static uint32_t ix_call(ictx_t *c, uint32_t handler,
                        const m68k_arg_t *args, int nargs, uint32_t *a0)
{
    bool to = false;
    uint32_t d0 = m68k_call_trap(c->cpu, handler, args, nargs, a0, &to);
    if (to) c->aborted = true;
    return d0;
}

/* Scratch sub-offsets within a single MemPtrNew'd block. */
#define SC_NAME      0    /* 32: DB name C-string */
#define SC_AT        32   /*  2: UInt16 atP for DmNewRecord (in/out) */
#define SC_ATTR      34   /*  2: UInt16 db attributes */
#define SC_VER       36   /*  2: UInt16 version */
#define SC_RECATTR   38   /*  2: UInt16 record attributes */
#define SC_CRDATE    40   /*  4 */
#define SC_MODDATE   44   /*  4 */
#define SC_BKDATE    48   /*  4 */
#define SC_MODNUM    52   /*  4 */
#define SC_TYPE      56   /*  4 */
#define SC_CREATOR   60   /*  4 */
#define SC_UID       64   /*  4: record uniqueID */
#define SC_LID       68   /*  4: appInfo/sortInfo LocalID to set */
#define SCRATCH_SZ   128

static inline void sc_w16(ictx_t *c, uint32_t off, uint16_t v) { bus_write16(c->scratch + off, v); }
static inline void sc_w32(ictx_t *c, uint32_t off, uint32_t v) { bus_write32(c->scratch + off, v); }
static inline uint16_t sc_r16(ictx_t *c, uint32_t off)         { return bus_read16(c->scratch + off); }
static inline uint32_t sc_r32(ictx_t *c, uint32_t off)         { return bus_read32(c->scratch + off); }

static bool resolve_traps(ictx_t *c)
{
    c->h_create     = trap_handler(TRAP_DmCreateDatabase);
    c->h_delete     = trap_handler(TRAP_DmDeleteDatabase);
    c->h_find       = trap_handler(TRAP_DmFindDatabase);
    c->h_setinfo    = trap_handler(TRAP_DmSetDatabaseInfo);
    c->h_open       = trap_handler(TRAP_DmOpenDatabase);
    c->h_close      = trap_handler(TRAP_DmCloseDatabase);
    c->h_setrecinfo = trap_handler(TRAP_DmSetRecordInfo);
    c->h_newrec     = trap_handler(TRAP_DmNewRecord);
    c->h_newhandle  = trap_handler(TRAP_DmNewHandle);
    c->h_relrec     = trap_handler(TRAP_DmReleaseRecord);
    c->h_relres     = trap_handler(TRAP_DmReleaseResource);
    c->h_newres     = trap_handler(TRAP_DmNewResource);
    c->h_write      = trap_handler(TRAP_DmWrite);
    c->h_memfree    = trap_handler(TRAP_MemChunkFree);
    c->h_memnew     = trap_handler(TRAP_MemPtrNew);
    c->h_memh2lid   = trap_handler(TRAP_MemHandleToLocalID);
    c->h_memlock    = trap_handler(TRAP_MemHandleLock);
    c->h_memunlock  = trap_handler(TRAP_MemHandleUnlock);
    return c->h_create && c->h_delete && c->h_find && c->h_setinfo && c->h_open &&
           c->h_close && c->h_setrecinfo && c->h_newrec &&
           c->h_newhandle && c->h_relrec && c->h_relres && c->h_newres &&
           c->h_write && c->h_memfree && c->h_memnew && c->h_memh2lid &&
           c->h_memlock && c->h_memunlock;
}

/* ---- thin trap wrappers --------------------------------------------------- */
/* Scalar/Err-returning traps read D0; pointer/handle-returning traps read A0. */

static uint32_t mem_ptr_new(ictx_t *c, uint32_t size)
{
    m68k_arg_t a[] = { { size, M68K_ARG_L } };
    uint32_t a0 = 0;
    ix_call(c, c->h_memnew, a, 1, &a0);
    return a0;
}
static void mem_free(ictx_t *c, uint32_t p)
{
    if (!p) return;
    m68k_arg_t a[] = { { p, M68K_ARG_L } };
    ix_call(c, c->h_memfree, a, 1, NULL);
}
static uint32_t mem_handle_lock(ictx_t *c, uint32_t h)
{
    m68k_arg_t a[] = { { h, M68K_ARG_L } };
    uint32_t a0 = 0;
    ix_call(c, c->h_memlock, a, 1, &a0);
    return a0;
}
static void mem_handle_unlock(ictx_t *c, uint32_t h)
{
    m68k_arg_t a[] = { { h, M68K_ARG_L } };
    ix_call(c, c->h_memunlock, a, 1, NULL);
}
static uint32_t mem_handle_to_localid(ictx_t *c, uint32_t h)
{
    m68k_arg_t a[] = { { h, M68K_ARG_L } };
    return ix_call(c, c->h_memh2lid, a, 1, NULL);
}
static uint32_t dm_find(ictx_t *c, uint16_t card, uint32_t nameP)
{
    m68k_arg_t a[] = { { card, M68K_ARG_W }, { nameP, M68K_ARG_L } };
    return ix_call(c, c->h_find, a, 2, NULL);   /* LocalID in D0 */
}
static uint16_t dm_delete(ictx_t *c, uint16_t card, uint32_t dbID)
{
    m68k_arg_t a[] = { { card, M68K_ARG_W }, { dbID, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_delete, a, 2, NULL);
}
static uint16_t dm_create(ictx_t *c, uint16_t card, uint32_t nameP,
                          uint32_t creator, uint32_t type, bool resDB)
{
    m68k_arg_t a[] = {
        { card, M68K_ARG_W }, { nameP, M68K_ARG_L }, { creator, M68K_ARG_L },
        { type, M68K_ARG_L }, { resDB ? 1u : 0u, M68K_ARG_W },
    };
    return (uint16_t)ix_call(c, c->h_create, a, 5, NULL);
}
static uint32_t dm_open(ictx_t *c, uint16_t card, uint32_t dbID, uint16_t mode)
{
    m68k_arg_t a[] = { { card, M68K_ARG_W }, { dbID, M68K_ARG_L }, { mode, M68K_ARG_W } };
    uint32_t a0 = 0;
    ix_call(c, c->h_open, a, 3, &a0);   /* DmOpenRef (pointer) in A0 */
    return a0;
}
static uint16_t dm_close(ictx_t *c, uint32_t openRef)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_close, a, 1, NULL);
}
static uint16_t dm_set_db_info(ictx_t *c, uint16_t card, uint32_t dbID,
        uint32_t nameP, uint32_t attrP, uint32_t verP, uint32_t crP, uint32_t modP,
        uint32_t bkP, uint32_t modnP, uint32_t appP, uint32_t sortP,
        uint32_t typeP, uint32_t creatorP)
{
    m68k_arg_t a[] = {
        { card, M68K_ARG_W }, { dbID, M68K_ARG_L }, { nameP, M68K_ARG_L },
        { attrP, M68K_ARG_L }, { verP, M68K_ARG_L }, { crP, M68K_ARG_L },
        { modP, M68K_ARG_L }, { bkP, M68K_ARG_L }, { modnP, M68K_ARG_L },
        { appP, M68K_ARG_L }, { sortP, M68K_ARG_L }, { typeP, M68K_ARG_L },
        { creatorP, M68K_ARG_L },
    };
    return (uint16_t)ix_call(c, c->h_setinfo, a, 13, NULL);
}
static uint32_t dm_new_handle(ictx_t *c, uint32_t openRef, uint32_t size)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L }, { size, M68K_ARG_L } };
    uint32_t a0 = 0;
    ix_call(c, c->h_newhandle, a, 2, &a0);
    return a0;
}
static uint32_t dm_new_resource(ictx_t *c, uint32_t openRef, uint32_t type,
                                uint16_t id, uint32_t size)
{
    m68k_arg_t a[] = {
        { openRef, M68K_ARG_L }, { type, M68K_ARG_L }, { id, M68K_ARG_W },
        { size, M68K_ARG_L },
    };
    uint32_t a0 = 0;
    ix_call(c, c->h_newres, a, 4, &a0);
    return a0;
}
static uint16_t dm_release_resource(ictx_t *c, uint32_t resH)
{
    m68k_arg_t a[] = { { resH, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_relres, a, 1, NULL);
}
static uint32_t dm_new_record(ictx_t *c, uint32_t openRef, uint32_t atP, uint32_t size)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L }, { atP, M68K_ARG_L }, { size, M68K_ARG_L } };
    uint32_t a0 = 0;
    ix_call(c, c->h_newrec, a, 3, &a0);
    return a0;
}
static uint16_t dm_set_record_info(ictx_t *c, uint32_t openRef, uint16_t index,
                                   uint32_t attrP, uint32_t uidP)
{
    m68k_arg_t a[] = {
        { openRef, M68K_ARG_L }, { index, M68K_ARG_W }, { attrP, M68K_ARG_L },
        { uidP, M68K_ARG_L },
    };
    return (uint16_t)ix_call(c, c->h_setrecinfo, a, 4, NULL);
}
static uint16_t dm_release_record(ictx_t *c, uint32_t openRef, uint16_t index, bool dirty)
{
    m68k_arg_t a[] = {
        { openRef, M68K_ARG_L }, { index, M68K_ARG_W }, { dirty ? 1u : 0u, M68K_ARG_W },
    };
    return (uint16_t)ix_call(c, c->h_relrec, a, 3, NULL);
}
static uint16_t dm_write(ictx_t *c, uint32_t recordP, uint32_t offset,
                         uint32_t srcP, uint32_t bytes)
{
    m68k_arg_t a[] = {
        { recordP, M68K_ARG_L }, { offset, M68K_ARG_L }, { srcP, M68K_ARG_L },
        { bytes, M68K_ARG_L },
    };
    return (uint16_t)ix_call(c, c->h_write, a, 4, NULL);
}

/* Stage `bytes` from the host image into the (already-locked) storage chunk at
 * `dstP` via DmWrite. Storage RAM is write-protected, so the copy MUST go
 * through DmWrite; the staged source is unprotected dynamic heap, addressable
 * directly in g_bus_ram. We bounce through a SMALL fixed buffer in fixed-size
 * chunks rather than allocating one full-size staging buffer: that bounds the
 * extra dynamic-heap pressure (the storage chunk + this buffer are both live)
 * and lets records/resources near or over the ~64 KB single-chunk ceiling
 * install on the IIIx's modest dynamic heap. */
#define STAGE_CHUNK 4096u
static bool stage_write(ictx_t *c, uint32_t dstP, const uint8_t *src, uint32_t bytes)
{
    if (bytes == 0) return true;
    uint32_t bufsz = (bytes < STAGE_CHUNK) ? bytes : STAGE_CHUNK;
    uint32_t srcBuf = mem_ptr_new(c, bufsz);
    if (!srcBuf || srcBuf >= BUS_RAM_SIZE || srcBuf + bufsz > BUS_RAM_SIZE)
        return false;
    bool ok = true;
    for (uint32_t off = 0; off < bytes; off += bufsz) {
        uint32_t n = (bytes - off < bufsz) ? (bytes - off) : bufsz;
        memcpy(g_bus_ram + srcBuf, src + off, n);
        uint16_t werr = dm_write(c, dstP, off, srcBuf, n);
        if (werr) { g_installer_last_dm_err = werr; ok = false; break; }
    }
    mem_free(c, srcBuf);
    return ok;
}

/* ---- header parse + validate --------------------------------------------- */
typedef struct {
    char     name[HDR_NAME_LEN];
    uint16_t attributes;
    uint16_t version;
    uint32_t crDate, modDate, bkDate, modNum;
    uint32_t appInfoID, sortInfoID;
    uint32_t type, creator;
    uint16_t numRecords;
    bool     isResDB;
} pdb_hdr_t;

static bool parse_header(const uint8_t *img, size_t size, pdb_hdr_t *h)
{
    if (size < HDR_SIZE) return false;

    memcpy(h->name, img, HDR_NAME_LEN);
    h->name[HDR_NAME_LEN - 1] = '\0';   /* force-terminate */
    if (h->name[0] == '\0') return false;

    h->attributes = be16(img + 32);
    h->version    = be16(img + 34);
    h->crDate     = be32(img + 36);
    h->modDate    = be32(img + 40);
    h->bkDate     = be32(img + 44);
    h->modNum     = be32(img + 48);
    h->appInfoID  = be32(img + 52);
    h->sortInfoID = be32(img + 56);
    h->type       = be32(img + 60);
    h->creator    = be32(img + 64);
    /* +68 uniqueIDSeed (unused — DmCreateDatabase seeds it) */
    uint32_t nextListID = be32(img + 72);
    h->numRecords = be16(img + 76);
    h->isResDB    = (h->attributes & DM_HDR_ATTR_RESDB) != 0;

    if (nextListID != 0) return false;   /* chained record lists unsupported */

    uint32_t entrySize = h->isResDB ? RES_ENTRY_SIZE : REC_ENTRY_SIZE;
    uint64_t arrayEnd  = (uint64_t)HDR_SIZE + (uint64_t)h->numRecords * entrySize;
    if (arrayEnd > size) return false;

    /* AppInfo/SortInfo offsets, if present, must fall within the file, and (when
     * both are present) appInfo must precede sortInfo — block_size() assumes that
     * ordering, so a reversed pair would silently produce a 0-byte appInfo block. */
    if (h->appInfoID  && (h->appInfoID  < arrayEnd || h->appInfoID  > size)) return false;
    if (h->sortInfoID && (h->sortInfoID < arrayEnd || h->sortInfoID > size)) return false;
    if (h->appInfoID && h->sortInfoID && h->appInfoID >= h->sortInfoID) return false;

    /* Validate each chunk offset: within [arrayEnd, size] and non-decreasing. */
    uint32_t prev = 0;
    for (uint16_t i = 0; i < h->numRecords; i++) {
        const uint8_t *e = img + HDR_SIZE + (uint32_t)i * entrySize;
        uint32_t chunk = h->isResDB ? be32(e + 6) : be32(e + 0);
        if (chunk < arrayEnd || chunk > size) return false;
        if (chunk < prev) return false;
        prev = chunk;
    }
    return true;
}

/* Offset of record/resource i's data chunk. */
static uint32_t chunk_off(const uint8_t *img, const pdb_hdr_t *h, uint16_t i)
{
    uint32_t entrySize = h->isResDB ? RES_ENTRY_SIZE : REC_ENTRY_SIZE;
    const uint8_t *e = img + HDR_SIZE + (uint32_t)i * entrySize;
    return h->isResDB ? be32(e + 6) : be32(e + 0);
}

/* Size of record/resource i: distance to the next chunk (or EOF). */
static uint32_t chunk_size(const uint8_t *img, size_t size, const pdb_hdr_t *h, uint16_t i)
{
    uint32_t here = chunk_off(img, h, i);
    uint32_t next = (i + 1 < h->numRecords) ? chunk_off(img, h, i + 1) : (uint32_t)size;
    return (next > here) ? (next - here) : 0;
}

/* Size of the appInfo block (offset = h->appInfoID): up to the next thing that
 * follows it in the file (sortInfo, first record, or EOF). */
static uint32_t block_size(const uint8_t *img, size_t size, const pdb_hdr_t *h, bool sortInfo)
{
    uint32_t here = sortInfo ? h->sortInfoID : h->appInfoID;
    uint32_t next = (uint32_t)size;
    if (!sortInfo && h->sortInfoID) next = h->sortInfoID;
    else if (h->numRecords)         next = chunk_off(img, h, 0);
    return (next > here) ? (next - here) : 0;
}

/* Install one appInfo/sortInfo metadata block; returns 0 on success. */
static int install_block(ictx_t *c, const uint8_t *img, uint32_t off, uint32_t sz, bool sortInfo)
{
    uint32_t h = dm_new_handle(c, c->openRef, sz);
    if (!h) return INSTALLER_ERR_OUT_OF_MEMORY;
    uint32_t dst = mem_handle_lock(c, h);
    if (!dst) return INSTALLER_ERR_OUT_OF_MEMORY;
    bool ok = stage_write(c, dst, img + off, sz);
    mem_handle_unlock(c, h);
    if (!ok) return INSTALLER_ERR_CORRUPT;
    uint32_t lid = mem_handle_to_localid(c, h);
    sc_w32(c, SC_LID, lid);
    uint16_t err = sortInfo
        ? dm_set_db_info(c, 0, c->dbID, 0, 0, 0, 0, 0, 0, 0, 0, c->scratch + SC_LID, 0, 0)
        : dm_set_db_info(c, 0, c->dbID, 0, 0, 0, 0, 0, 0, 0, c->scratch + SC_LID, 0, 0, 0);
    if (err) { g_installer_last_dm_err = err; return INSTALLER_ERR_CORRUPT; }
    return 0;
}

installer_result_t installer_install_image(m68k_cpu_t *cpu,
                                           const uint8_t *image, size_t size,
                                           char *name_out)
{
    ictx_t c;
    memset(&c, 0, sizeof(c));
    c.cpu = cpu;
    g_installer_last_dm_err = 0;   /* 0 unless a DM trap reports an error below */

    if (db_is_asleep())
        return INSTALLER_ERR_NOT_READY;
    if (!resolve_traps(&c))
        return INSTALLER_ERR_TRAP_UNRESOLVED;

    pdb_hdr_t h;
    if (!parse_header(image, size, &h))
        return INSTALLER_ERR_BAD_IMAGE;
    if (name_out) { memcpy(name_out, h.name, HDR_NAME_LEN); name_out[HDR_NAME_LEN - 1] = '\0'; }

    installer_result_t result = INSTALLER_ERR_UNKNOWN;
    int step = 0;       /* last stage reached, for failure diagnosis */
    bool created = false;   /* DmCreateDatabase succeeded (for orphan cleanup) */

    step = 1;
    c.scratch = mem_ptr_new(&c, SCRATCH_SZ);
    if (!c.scratch || c.scratch + SCRATCH_SZ > BUS_RAM_SIZE) {
        result = INSTALLER_ERR_OUT_OF_MEMORY;
        goto done;
    }

    /* DB name C-string in emulated memory. */
    for (uint32_t i = 0; i < HDR_NAME_LEN; i++)
        bus_write8(c.scratch + SC_NAME + i, (uint8_t)h.name[i]);

    /* 1. Delete any existing RAM database with this name (tolerate ROM-based;
     *    a DB that is currently open cannot be overwritten). */
    step = 2;
    uint32_t existing = dm_find(&c, 0, c.scratch + SC_NAME);
    if (existing) {
        uint16_t err = dm_delete(&c, 0, existing);
        if (err == DM_ERR_DATABASE_OPEN) { g_installer_last_dm_err = err; result = INSTALLER_ERR_DB_OPEN; goto done; }
        if (err && err != DM_ERR_ROM_BASED) { g_installer_last_dm_err = err; result = INSTALLER_ERR_CORRUPT; goto done; }
    }

    /* 2. Create + locate the new database. Limitation: if this name also exists
     *    as a ROM-based database (a built-in app), the delete above left the ROM
     *    DB in place and this DmFindDatabase may resolve the ROM LocalID rather
     *    than our new RAM one — the subsequent DmSetDatabaseInfo/open-RW then
     *    fails and we report it as an error. Installing custom apps (the common
     *    case) does not collide with ROM names, so this is left as a known edge. */
    step = 3;
    uint16_t cerr = dm_create(&c, 0, c.scratch + SC_NAME, h.creator, h.type, h.isResDB);
    if (cerr) { g_installer_last_dm_err = cerr; result = INSTALLER_ERR_CORRUPT; goto done; }
    created = true;
    step = 4;
    c.dbID = dm_find(&c, 0, c.scratch + SC_NAME);
    if (!c.dbID) { result = INSTALLER_ERR_CORRUPT; goto done; }

    /* 3. Set header info (clear the "not closed properly" bit). */
    sc_w16(&c, SC_ATTR,    (uint16_t)(h.attributes & ~DM_HDR_ATTR_OPEN));
    sc_w16(&c, SC_VER,     h.version);
    sc_w32(&c, SC_CRDATE,  h.crDate);
    sc_w32(&c, SC_MODDATE, h.modDate);
    sc_w32(&c, SC_BKDATE,  h.bkDate);
    sc_w32(&c, SC_MODNUM,  h.modNum);
    sc_w32(&c, SC_TYPE,    h.type);
    sc_w32(&c, SC_CREATOR, h.creator);
    step = 5;
    uint16_t serr = dm_set_db_info(&c, 0, c.dbID, 0,
                       c.scratch + SC_ATTR, c.scratch + SC_VER, c.scratch + SC_CRDATE,
                       c.scratch + SC_MODDATE, c.scratch + SC_BKDATE, c.scratch + SC_MODNUM,
                       0, 0, c.scratch + SC_TYPE, c.scratch + SC_CREATOR);
    if (serr) { g_installer_last_dm_err = serr; result = INSTALLER_ERR_CORRUPT; goto done; }

    /* 4. Open read-write. */
    step = 6;
    c.openRef = dm_open(&c, 0, c.dbID, DM_MODE_READWRITE);
    if (!c.openRef) { result = INSTALLER_ERR_CORRUPT; goto done; }

    /* A driven trap in the create/find/set-info/open prefix never returned. */
    if (c.aborted) { result = INSTALLER_ERR_TIMEOUT; goto done; }

    /* 5. AppInfo / SortInfo blocks (PDBs may have them; PRCs normally don't).
     *    Skip a zero-length block: block_size can legitimately compute 0 (e.g. an
     *    appInfoID that equals the first record offset), and DmNewHandle(0) can
     *    fail — which would abort an otherwise-valid install. A 0-byte block
     *    carries no data, so not creating it (nor setting its LocalID) is correct. */
    step = 7;
    if (h.appInfoID) {
        uint32_t sz = block_size(image, size, &h, false);
        if (sz) {
            int e = install_block(&c, image, h.appInfoID, sz, false);
            if (e) { result = e; goto done; }
        }
    }
    if (h.sortInfoID) {
        uint32_t sz = block_size(image, size, &h, true);
        if (sz) {
            int e = install_block(&c, image, h.sortInfoID, sz, true);
            if (e) { result = e; goto done; }
        }
    }
    if (c.aborted) { result = INSTALLER_ERR_TIMEOUT; goto done; }   /* a block install timed out */

    /* 6. Records / resources. */
    step = 8;
    for (uint16_t i = 0; i < h.numRecords; i++) {
        /* Stop promptly if a prior record's trap timed out — a runaway trap can
         * return a garbage-but-zero value that passes the per-op error checks. */
        if (c.aborted) { result = INSTALLER_ERR_TIMEOUT; goto done; }
        uint32_t off = chunk_off(image, &h, i);
        uint32_t sz  = chunk_size(image, size, &h, i);

        if (h.isResDB) {
            /* A 0-byte resource carries no data and DmNewResource(0) can fail the
             * whole install; skip it, mirroring the nil-record skip below. (Real
             * .prc resources — code/forms/icons — are never legitimately empty.) */
            if (sz == 0) continue;
            const uint8_t *e = image + HDR_SIZE + (uint32_t)i * RES_ENTRY_SIZE;
            uint32_t rtype = be32(e + 0);
            uint16_t rid   = be16(e + 4);
            uint32_t hnd = dm_new_resource(&c, c.openRef, rtype, rid, sz);
            if (!hnd) { result = INSTALLER_ERR_OUT_OF_MEMORY; goto done; }
            uint32_t dst = mem_handle_lock(&c, hnd);
            bool ok = dst && stage_write(&c, dst, image + off, sz);
            mem_handle_unlock(&c, hnd);
            uint16_t rerr = dm_release_resource(&c, hnd);
            if (!ok || rerr) { if (rerr) g_installer_last_dm_err = rerr; result = INSTALLER_ERR_CORRUPT; goto done; }
        } else {
            if (sz == 0) continue;   /* skip nil (deleted) records */
            const uint8_t *e = image + HDR_SIZE + (uint32_t)i * REC_ENTRY_SIZE;
            uint8_t  rattr = e[4];
            uint32_t uid   = ((uint32_t)e[5] << 16) | ((uint32_t)e[6] << 8) | e[7];

            sc_w16(&c, SC_AT, 0xFFFF);   /* append */
            uint32_t hnd = dm_new_record(&c, c.openRef, c.scratch + SC_AT, sz);
            if (!hnd) { result = INSTALLER_ERR_OUT_OF_MEMORY; goto done; }
            uint16_t index = sc_r16(&c, SC_AT);
            uint32_t dst = mem_handle_lock(&c, hnd);
            bool ok = dst && stage_write(&c, dst, image + off, sz);
            mem_handle_unlock(&c, hnd);
            if (!ok) { result = INSTALLER_ERR_CORRUPT; goto done; }

            sc_w16(&c, SC_RECATTR, rattr);
            sc_w32(&c, SC_UID, uid);
            uint16_t sierr = dm_set_record_info(&c, c.openRef, index,
                                   c.scratch + SC_RECATTR, c.scratch + SC_UID);
            if (sierr) { g_installer_last_dm_err = sierr; result = INSTALLER_ERR_CORRUPT; goto done; }
            uint16_t rrerr = dm_release_record(&c, c.openRef, index, true);
            if (rrerr) { g_installer_last_dm_err = rrerr; result = INSTALLER_ERR_CORRUPT; goto done; }
        }

        if ((i & 31) == 31 && g_installer_yield) g_installer_yield();
    }

    /* The last record's trap may have timed out yet returned a garbage-zero that
     * slipped past the per-op checks — don't commit a half-written DB. */
    if (c.aborted) { result = INSTALLER_ERR_TIMEOUT; goto done; }

    /* 7. Restore the modification number the record churn bumped, then close. */
    sc_w32(&c, SC_MODNUM, h.modNum);
    dm_set_db_info(&c, 0, c.dbID, 0, 0, 0, 0, 0, 0, c.scratch + SC_MODNUM, 0, 0, 0, 0);
    dm_close(&c, c.openRef);
    c.openRef = 0;
    c.dbID = 0;   /* keep the installed DB; nothing to roll back */
    mem_free(&c, c.scratch);
    c.scratch = 0;

    return (h.attributes & DM_HDR_ATTR_RESET) ? INSTALLER_NEEDS_RESET : INSTALLER_OK;

done:
    /* If any driven trap hit the runaway guard, the timeout is the root cause —
     * relabel regardless of which 0/garbage-return path triggered the bail. */
    if (c.aborted) result = INSTALLER_ERR_TIMEOUT;
    g_installer_last_fail_step = step;   /* recorded, not logged */
    /* Abort cleanup: tear down the half-built database so the next boot is clean. */
    if (c.openRef) dm_close(&c, c.openRef);
    if (c.dbID)    dm_delete(&c, 0, c.dbID);
    else if (created) {
        /* DmCreateDatabase succeeded but we bailed before capturing the LocalID
         * (the DmFindDatabase that follows it returned 0). Re-find by name so the
         * orphaned database doesn't leak. */
        uint32_t orphan = dm_find(&c, 0, c.scratch + SC_NAME);
        if (orphan) dm_delete(&c, 0, orphan);
    }
    if (c.scratch) mem_free(&c, c.scratch);
    return result;
}

/* ---- launcher refresh + per-tick install driver --------------------------- */

void installer_refresh_launcher(m68k_cpu_t *cpu)
{
    uint32_t globals = bus_read32(0x122);
    if (!globals || (uint32_t)(globals + 0x12D * 4 + 4) >= BUS_RAM_SIZE) return;
    uint32_t h = bus_read32(globals + 0x12D * 4);   /* EvtEnqueueKey (A12D) */
    if (!bus_addr_in_rom(h)) return;
    cpu->regs.stopped = false;                       /* wake to drain the event */
    m68k_key_enqueue(cpu, h, 0x0108 /*vchrLaunch*/, 0, 0x0008 /*commandKeyMask*/);
}

/* Cheap rolling checksum of the Palm 160x160 1bpp framebuffer content (3200 bytes),
 * read straight from emulated RAM. Used only to notice WHEN the content changes (the
 * boot splash giving way to the first drawn UI); the value itself is never matched
 * against anything, so this is ROM/screen-agnostic. Returns 0 if the address is unusable. */
static uint32_t installer_fb_cksum(uint32_t fb)
{
    if (!fb || (uint32_t)(fb + 3200) > BUS_RAM_SIZE) return 0;
    uint32_t s = 2166136261u;                 /* FNV-ish; any mixing is fine here */
    for (int i = 0; i < 3200; i++) s = (s ^ g_bus_ram[fb + i]) * 16777619u;
    return s;
}

installer_pump_result_t installer_pump(install_set_t *set, m68k_cpu_t *cpu,
                                       bool osready, uint32_t current_fb,
                                       bool force, installer_pump_state_t *st)
{
    installer_pump_result_t res = { 0, false, false, NULL };

    /* All images attempted: report completion and what the caller should do.
     * A reset-after-install DB supersedes the launcher kick (the warm reboot
     * repaints everything anyway). */
    if (set->next >= set->count) {
        res.status = 1;
        bool any_ok = false, any_reset = false;
        for (int i = 0; i < set->count; i++) {
            if (set->img[i].status == IMG_INSTALLED) {
                any_ok = true;
                if (set->img[i].result == INSTALLER_NEEDS_RESET) any_reset = true;
            }
        }
        res.need_warm_reset    = any_reset;
        res.need_launcher_kick = any_ok && !any_reset;
        return res;
    }

    /* Cheap pre-gate: don't drive any trap until the OS is settled. The boot->UI
     * framebuffer ADDRESS switch (the Window Manager allocating the UI surface) is
     * NOT enough — it fires a hair too early, while the boot splash is still on
     * screen and the OS is in the narrow post-UIInitialize bring-up window. Driving
     * the install traps then corrupts an AMX system pool; it is cumulative — the
     * second of two back-to-back installs is what trips it (a single install at that
     * instant survives, and spacing the second past the window is clean). So wait
     * until the OS actually DRAWS its first UI over the splash — the framebuffer
     * CONTENT changes — which is necessarily past the bring-up (you can't render a
     * screen until the UI is fully up) and clears the window with margin. We compare
     * the live content against the splash snapshot, not against any known image, so
     * it is ROM/locale/screen agnostic (first boot draws Setup, later boots the
     * launcher — either trips it). Verified on host: a 2-app install at the address
     * switch faults to a wild PC; the same install once the content changes is clean.
     * `force` and an idle STOP still settle directly. */
    if (current_fb && !st->boot_fb) st->boot_fb = current_fb;
    bool ui_up = (current_fb && st->boot_fb && current_fb != st->boot_fb);
    if (ui_up) {
        uint32_t cks = installer_fb_cksum(current_fb);
        if (!st->ui_cksum)            st->ui_cksum = cks ? cks : 1u;  /* snapshot the splash */
        else if (cks != st->ui_cksum) st->settled = true;            /* launcher drawn */
    }
    if (cpu->regs.stopped) st->settled = true;             /* an idle STOP also settles */
    if (!osready || !(st->settled || force)) return res;   /* keep waiting */

    /* Authoritative readiness: wait for the OS to FINISH UIInitialize (sysTrap
     * 0xA1A6) — mirrors CloudpilotEmu's IsUIInitialized() install gate. The
     * framebuffer-switch settle above fires a hair too early, during UI-subsystem
     * init; driving our homebrew Dm/Mem trap sequence then reenters the OS mid-init
     * and corrupts its state (a task stack ends up overlapping an AMX system pool,
     * so the device crashes after the install completes — and the more apps we
     * install in that window, the more reliably it does). This is NOT bypassable by
     * `force`: installing before UIInitialize always corrupts the OS, whoever asked.
     * `force` only skips the idle/settle pre-gate above, never this safety gate. */
    if (!m68k_ui_initialized()) return res;

    /* Authoritative readiness: a MemPtrNew round-trip, retried each tick until it
     * passes. The probe and the install run in the SAME call, so there is no
     * window for the OS to doze in between — once the probe passes we install,
     * once, with no per-image retry. */
    if (!st->heap_confirmed) {
        if (!installer_probe_heap(cpu)) return res;        /* not ready yet */
        st->heap_confirmed = true;
    }

    /* Install one image this tick (a full emulation batch runs between calls so
     * the OS commits each). Past the probe the OS is up, so any failure is a
     * genuine error — there is no retry, and a too-big DB fails once. */
    install_image_t *im = &set->img[set->next];
    char nm[32] = {0};
    installer_result_t r = installer_install_image(cpu, im->data, im->size, nm);
    memcpy(im->dbname, nm, sizeof(im->dbname));
    im->dbname[sizeof(im->dbname) - 1] = '\0';
    im->result = r;
    im->status = (r == INSTALLER_OK || r == INSTALLER_NEEDS_RESET)
                 ? IMG_INSTALLED : IMG_FAILED;
    set->next++;
    res.attempted = im;
    return res;
}

/* ======================== backup / export engine ========================= *
 * The REVERSE of installer_install_image: read a database out of the running OS
 * (driving the Data Manager read/enumerate traps) and serialize it to a standard
 * .prc/.pdb byte stream. Pure C (no SD), so it builds on the host harness; the SD
 * orchestration is in the device-only block below / host_main.c. See backup.h. */

/* Scratch sub-offsets for the backup OUT-param traps (its own block, allocated
 * separately from the installer's — the two never run concurrently). */
#define SCX_NAME     0    /* 32: DB name (DmDatabaseInfo out) */
#define SCX_ATTR     32   /*  2 */
#define SCX_VER      34   /*  2 */
#define SCX_CRDATE   36   /*  4 */
#define SCX_MODDATE  40   /*  4 */
#define SCX_BKDATE   44   /*  4 */
#define SCX_MODNUM   48   /*  4 */
#define SCX_APPID    52   /*  4 */
#define SCX_SORTID   56   /*  4 */
#define SCX_TYPE     60   /*  4 */
#define SCX_CREATOR  64   /*  4 */
#define SCX_RECATTR  68   /*  2: DmRecordInfo attr (UInt16 out) */
#define SCX_UID      70   /*  4: record uniqueID out */
#define SCX_CHUNKID  74   /*  4: record/resource chunk LocalID out */
#define SCX_RESTYPE  78   /*  4: DmResourceInfo type out */
#define SCX_RESID    82   /*  2: DmResourceInfo id out */
#define BACKUP_SCRATCH_SZ 96

#define BACKUP_MAX_ENTRIES 4096   /* per-DB record/resource cap */

/* Per-DB entry gathered in pass 1; the file offset is assigned once all sizes are
 * known, and pass 2 re-acquires each chunk by index to stream its bytes. */
typedef struct {
    uint16_t index;     /* DB record/resource index, for the pass-2 re-acquire */
    uint32_t fileOff;   /* assigned absolute file offset of the data chunk */
    uint32_t size;      /* chunk byte size */
    uint8_t  recAttr;   /* record attribute byte (records only) */
    uint32_t uid;       /* record 24-bit uniqueID (records only) */
    uint32_t resType;   /* resource type (resources only) */
    uint16_t resID;     /* resource id (resources only) */
} bk_entry_t;

static void *bk_alloc(size_t n)
{
#ifdef HOST_BUILD
    return malloc(n);
#else
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
#endif
}
static void bk_free(void *p)
{
#ifdef HOST_BUILD
    free(p);
#else
    heap_caps_free(p);
#endif
}

/* ---- read/enumerate trap wrappers (mirror the installer's thin wrappers) --- */

static uint32_t dm_num_databases(ictx_t *c, uint16_t card)
{
    m68k_arg_t a[] = { { card, M68K_ARG_W } };
    return ix_call(c, c->h_numdb, a, 1, NULL);          /* UInt16 count in D0 */
}
static uint32_t dm_get_database(ictx_t *c, uint16_t card, uint16_t index)
{
    m68k_arg_t a[] = { { card, M68K_ARG_W }, { index, M68K_ARG_W } };
    return ix_call(c, c->h_getdb, a, 2, NULL);          /* LocalID in D0 */
}
/* DmDatabaseInfo: identical 13-arg ABI to DmSetDatabaseInfo, but the pointers are
 * OUT-params (we pass scratch cells and read the results back). Err in D0. */
static uint16_t dm_database_info(ictx_t *c, uint16_t card, uint32_t dbID,
        uint32_t nameP, uint32_t attrP, uint32_t verP, uint32_t crP, uint32_t modP,
        uint32_t bkP, uint32_t modnP, uint32_t appP, uint32_t sortP,
        uint32_t typeP, uint32_t creatorP)
{
    m68k_arg_t a[] = {
        { card, M68K_ARG_W }, { dbID, M68K_ARG_L }, { nameP, M68K_ARG_L },
        { attrP, M68K_ARG_L }, { verP, M68K_ARG_L }, { crP, M68K_ARG_L },
        { modP, M68K_ARG_L }, { bkP, M68K_ARG_L }, { modnP, M68K_ARG_L },
        { appP, M68K_ARG_L }, { sortP, M68K_ARG_L }, { typeP, M68K_ARG_L },
        { creatorP, M68K_ARG_L },
    };
    return (uint16_t)ix_call(c, c->h_dbinfo, a, 13, NULL);
}
static uint16_t dm_num_records(ictx_t *c, uint32_t openRef)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_numrec, a, 1, NULL);
}
static uint16_t dm_record_info(ictx_t *c, uint32_t openRef, uint16_t index,
                               uint32_t attrP, uint32_t uidP, uint32_t chunkP)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L }, { index, M68K_ARG_W },
                       { attrP, M68K_ARG_L }, { uidP, M68K_ARG_L }, { chunkP, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_recinfo, a, 5, NULL);
}
static uint32_t dm_query_record(ictx_t *c, uint32_t openRef, uint16_t index)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L }, { index, M68K_ARG_W } };
    uint32_t a0 = 0;
    ix_call(c, c->h_query, a, 2, &a0);                  /* MemHandle in A0 (no lock) */
    return a0;
}
static uint16_t dm_num_resources(ictx_t *c, uint32_t openRef)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_numres, a, 1, NULL);
}
static uint16_t dm_resource_info(ictx_t *c, uint32_t openRef, uint16_t index,
                                 uint32_t typeP, uint32_t idP, uint32_t chunkP)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L }, { index, M68K_ARG_W },
                       { typeP, M68K_ARG_L }, { idP, M68K_ARG_L }, { chunkP, M68K_ARG_L } };
    return (uint16_t)ix_call(c, c->h_resinfo, a, 5, NULL);
}
static uint32_t dm_get_resource_index(ictx_t *c, uint32_t openRef, uint16_t index)
{
    m68k_arg_t a[] = { { openRef, M68K_ARG_L }, { index, M68K_ARG_W } };
    uint32_t a0 = 0;
    ix_call(c, c->h_getresidx, a, 2, &a0);              /* MemHandle in A0 (loads+locks) */
    return a0;
}
static uint32_t mem_handle_size(ictx_t *c, uint32_t h)
{
    m68k_arg_t a[] = { { h, M68K_ARG_L } };
    return ix_call(c, c->h_handlesize, a, 1, NULL);     /* UInt32 in D0 */
}
static uint32_t mem_ptr_size(ictx_t *c, uint32_t p)
{
    m68k_arg_t a[] = { { p, M68K_ARG_L } };
    return ix_call(c, c->h_ptrsize, a, 1, NULL);        /* UInt32 in D0 */
}
static uint32_t mem_local_id_to_global(ictx_t *c, uint32_t localID, uint16_t card)
{
    m68k_arg_t a[] = { { localID, M68K_ARG_L }, { card, M68K_ARG_W } };
    uint32_t a0 = 0;
    ix_call(c, c->h_lid2g, a, 2, &a0);                  /* MemPtr in A0 (may be a handle) */
    return a0;
}
/* Lock and return the DATA pointer for a LocalID, handling BOTH pointer-type and
 * handle-type local IDs (appInfo/sortInfo from DmDatabaseInfo are handle-type, so
 * MemLocalIDToGlobal would yield the master pointer, not the data). Pair with
 * mem_ptr_unlock once the bytes have been read. */
static uint32_t mem_local_id_to_locked_ptr(ictx_t *c, uint32_t localID, uint16_t card)
{
    m68k_arg_t a[] = { { localID, M68K_ARG_L }, { card, M68K_ARG_W } };
    uint32_t a0 = 0;
    ix_call(c, c->h_lidlocked, a, 2, &a0);              /* locked MemPtr in A0 */
    return a0;
}
static void mem_ptr_unlock(ictx_t *c, uint32_t p)
{
    if (!p) return;
    m68k_arg_t a[] = { { p, M68K_ARG_L } };
    ix_call(c, c->h_ptrunlock, a, 1, NULL);
}

/* Resolve every handler the backup engine drives (read/enumerate traps + the
 * shared open/close/find/lock/release/mem traps). Returns true iff all resolve. */
static bool resolve_export_traps(ictx_t *c)
{
    c->h_numdb      = trap_handler(TRAP_DmNumDatabases);
    c->h_getdb      = trap_handler(TRAP_DmGetDatabase);
    c->h_dbinfo     = trap_handler(TRAP_DmDatabaseInfo);
    c->h_find       = trap_handler(TRAP_DmFindDatabase);
    c->h_open       = trap_handler(TRAP_DmOpenDatabase);
    c->h_close      = trap_handler(TRAP_DmCloseDatabase);
    c->h_numrec     = trap_handler(TRAP_DmNumRecords);
    c->h_recinfo    = trap_handler(TRAP_DmRecordInfo);
    c->h_query      = trap_handler(TRAP_DmQueryRecord);
    c->h_numres     = trap_handler(TRAP_DmNumResources);
    c->h_resinfo    = trap_handler(TRAP_DmResourceInfo);
    c->h_getresidx  = trap_handler(TRAP_DmGetResourceIndex);
    c->h_relres     = trap_handler(TRAP_DmReleaseResource);
    c->h_handlesize = trap_handler(TRAP_MemHandleSize);
    c->h_ptrsize    = trap_handler(TRAP_MemPtrSize);
    c->h_lid2g      = trap_handler(TRAP_MemLocalIDToGlobal);
    c->h_lidlocked  = trap_handler(TRAP_MemLocalIDToLockedPtr);
    c->h_ptrunlock  = trap_handler(TRAP_MemPtrUnlock);
    c->h_memlock    = trap_handler(TRAP_MemHandleLock);
    c->h_memunlock  = trap_handler(TRAP_MemHandleUnlock);
    c->h_memnew     = trap_handler(TRAP_MemPtrNew);
    c->h_memfree    = trap_handler(TRAP_MemChunkFree);
    return c->h_numdb && c->h_getdb && c->h_dbinfo && c->h_find && c->h_open &&
           c->h_close && c->h_numrec && c->h_recinfo && c->h_query && c->h_numres &&
           c->h_resinfo && c->h_getresidx && c->h_relres && c->h_handlesize &&
           c->h_ptrsize && c->h_lid2g && c->h_lidlocked && c->h_ptrunlock &&
           c->h_memlock && c->h_memunlock && c->h_memnew && c->h_memfree;
}

const char *backup_result_str(backup_result_t r)
{
    switch (r) {
    case BACKUP_OK:                  return "ok";
    case BACKUP_ERR_NOT_READY:       return "not ready";
    case BACKUP_ERR_BUSY:            return "i/o busy (retry)";
    case BACKUP_ERR_TRAP_UNRESOLVED: return "trap unresolved";
    case BACKUP_ERR_TIMEOUT:         return "trap timed out";
    case BACKUP_ERR_DM:              return "data manager error";
    case BACKUP_ERR_IO:              return "sd/file io error";
    case BACKUP_ERR_OUT_OF_MEMORY:   return "out of memory";
    case BACKUP_ERR_BAD_DB:          return "db too large";
    case BACKUP_ERR_SKIPPED_ROM:     return "rom-based (skipped)";
    default:                         return "unknown error";
    }
}

const char *backup_ext_for(const backup_db_info_t *info)
{
    if (info->isResDB) return ".prc";
    if (info->creator == 0x636C7072u /* 'clpr' */ ||
        (info->attributes & DM_HDR_ATTR_LAUNCHABLE))
        return ".pqa";
    return ".pdb";
}

void backup_make_filename(const backup_db_info_t *info, char *out, size_t n)
{
    char base[40];
    int j = 0;
    for (int i = 0; i < 31 && info->name[i] && j < (int)sizeof(base) - 1; i++) {
        unsigned char ch = (unsigned char)info->name[i];
        if (ch < 0x20 || strchr("/\\:*?\"<>|", ch)) ch = '_';
        base[j++] = (char)ch;
    }
    while (j > 0 && (base[j - 1] == '.' || base[j - 1] == ' ')) j--;  /* FAT dislikes trailing */
    base[j] = '\0';
    if (j == 0)
        snprintf(base, sizeof(base), "db_%08lX", (unsigned long)info->dbID);
    snprintf(out, n, "%s%s", base, backup_ext_for(info));
}

bool backup_os_ready(void)
{
    static const uint32_t REQ[] = {
        TRAP_DmNumDatabases, TRAP_DmGetDatabase, TRAP_DmDatabaseInfo, TRAP_DmFindDatabase,
        TRAP_DmOpenDatabase, TRAP_DmCloseDatabase, TRAP_DmNumRecords, TRAP_DmRecordInfo,
        TRAP_DmQueryRecord, TRAP_DmNumResources, TRAP_DmResourceInfo, TRAP_DmGetResourceIndex,
        TRAP_DmReleaseResource, TRAP_MemHandleSize, TRAP_MemPtrSize, TRAP_MemLocalIDToGlobal,
        TRAP_MemLocalIDToLockedPtr, TRAP_MemPtrUnlock,
        TRAP_MemHandleLock, TRAP_MemHandleUnlock, TRAP_MemPtrNew, TRAP_MemChunkFree,
    };
    if (db_is_asleep()) return false;
    if (db_lcd_framebuffer_addr() == 0) return false;
    for (size_t i = 0; i < sizeof(REQ) / sizeof(REQ[0]); i++)
        if (!trap_handler(REQ[i])) return false;
    return true;
}

int backup_enumerate(m68k_cpu_t *cpu, backup_db_info_t *out, int max)
{
    ictx_t c;
    memset(&c, 0, sizeof(c));
    c.cpu = cpu;
    if (db_is_asleep())          return -1;
    if (!resolve_export_traps(&c)) return -1;

    c.scratch = mem_ptr_new(&c, BACKUP_SCRATCH_SZ);
    if (!c.scratch || c.scratch + BACKUP_SCRATCH_SZ > BUS_RAM_SIZE) return -1;

    int n = 0;
    uint16_t total = dm_num_databases(&c, 0);
    for (uint16_t i = 0; i < total && n < max; i++) {
        if (c.aborted) break;
        uint32_t dbID = dm_get_database(&c, 0, i);
        if (!dbID) continue;

        for (uint32_t k = 0; k < BACKUP_SCRATCH_SZ; k += 4) sc_w32(&c, k, 0);
        uint16_t err = dm_database_info(&c, 0, dbID,
            c.scratch + SCX_NAME, c.scratch + SCX_ATTR, c.scratch + SCX_VER,
            c.scratch + SCX_CRDATE, c.scratch + SCX_MODDATE, c.scratch + SCX_BKDATE,
            c.scratch + SCX_MODNUM, c.scratch + SCX_APPID, c.scratch + SCX_SORTID,
            c.scratch + SCX_TYPE, c.scratch + SCX_CREATOR);
        if (err) continue;       /* skip DBs whose info we can't read */

        backup_db_info_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        d->dbID = dbID;
        for (int k = 0; k < 31; k++)
            d->name[k] = (char)bus_read8(c.scratch + SCX_NAME + k);
        d->name[31]    = '\0';
        d->attributes  = sc_r16(&c, SCX_ATTR);
        d->version     = sc_r16(&c, SCX_VER);
        d->crDate      = sc_r32(&c, SCX_CRDATE);
        d->modDate     = sc_r32(&c, SCX_MODDATE);
        d->bkDate      = sc_r32(&c, SCX_BKDATE);
        d->modNum      = sc_r32(&c, SCX_MODNUM);
        d->appInfoID   = sc_r32(&c, SCX_APPID);
        d->sortInfoID  = sc_r32(&c, SCX_SORTID);
        d->type        = sc_r32(&c, SCX_TYPE);
        d->creator     = sc_r32(&c, SCX_CREATOR);
        d->isResDB     = (d->attributes & DM_HDR_ATTR_RESDB) != 0;
        uint32_t g     = mem_local_id_to_global(&c, dbID, 0);
        d->romBased    = bus_addr_in_rom(g);
        n++;

        if ((i & 15) == 15 && g_installer_yield) g_installer_yield();
    }

    mem_free(&c, c.scratch);
    return c.aborted ? -1 : n;
}

/* Mask a MemMgr-returned pointer to the 24-bit bus so it indexes g_bus_ram (the
 * 68000 has only 24 address lines; the upper 8 bits are a don't-care tag). The
 * single source of the "tagged 68k ptr -> RAM offset" rule for the backup engine. */
static inline uint32_t bk_ram_addr(uint32_t raw) { return raw & BUS_ADDR_MASK; }

/* Lock the appInfo/sortInfo block named by `localID` and report its still-locked
 * raw pointer (*raw), masked g_bus_ram index (*ptr), and size (*size). A
 * zero-length, out-of-range, or would-overflow block (or a failed lock) is treated
 * as ABSENT: the chunk is unlocked and the three out-params are left 0. The caller
 * unlocks raw in cleanup once the bytes are read. A size above BUS_RAM_SIZE is
 * rejected before the ptr+size add so a garbage size can't wrap the bound. */
static bool bk_lock_block(ictx_t *c, uint32_t localID,
                          uint32_t *raw, uint32_t *ptr, uint32_t *size)
{
    *raw = *ptr = *size = 0;
    if (!localID) return false;
    uint32_t r = mem_local_id_to_locked_ptr(c, localID, 0);
    uint32_t p = bk_ram_addr(r);
    uint32_t s = (r && p < BUS_RAM_SIZE) ? mem_ptr_size(c, r) : 0;
    if (!s || s > BUS_RAM_SIZE || p + s > BUS_RAM_SIZE) {
        mem_ptr_unlock(c, r);                /* no-op if r == 0 */
        return false;
    }
    *raw = r; *ptr = p; *size = s;
    return true;
}

/* Stream `size` bytes of emulated RAM at `ptr` to the sink in STAGE_CHUNK slices
 * (bounds the FatFs/SPI write transaction; yields between slices for the
 * watchdog). The pointer is already range-checked by the caller. */
static bool sink_write_ram(backup_sink_t *sink, uint32_t ptr, uint32_t size)
{
    uint32_t off = 0;
    while (off < size) {
        uint32_t nseg = size - off;
        if (nseg > STAGE_CHUNK) nseg = STAGE_CHUNK;
        if (!sink->write(sink->ctx, g_bus_ram + ptr + off, nseg)) return false;
        off += nseg;
        if (off < size && g_installer_yield) g_installer_yield();
    }
    return true;
}

backup_result_t backup_serialize_db(m68k_cpu_t *cpu, const backup_db_info_t *info,
                                    backup_sink_t *sink)
{
    ictx_t c;
    memset(&c, 0, sizeof(c));
    c.cpu = cpu;
    g_installer_last_dm_err = 0;

    if (db_is_asleep())            return BACKUP_ERR_NOT_READY;
    if (!resolve_export_traps(&c)) return BACKUP_ERR_TRAP_UNRESOLVED;

    backup_result_t result = BACKUP_ERR_UNKNOWN;
    bk_entry_t *ents = NULL;
    int live = 0;
    const uint32_t entrySize = info->isResDB ? RES_ENTRY_SIZE : REC_ENTRY_SIZE;
    uint32_t appPtr = 0, appSize = 0, sortPtr = 0, sortSize = 0;
    uint32_t appRaw = 0, sortRaw = 0;   /* locked data ptrs (unlocked in cleanup) */

    c.scratch = mem_ptr_new(&c, BACKUP_SCRATCH_SZ);
    if (!c.scratch || c.scratch + BACKUP_SCRATCH_SZ > BUS_RAM_SIZE) {
        result = BACKUP_ERR_OUT_OF_MEMORY; goto done;
    }

    c.openRef = dm_open(&c, 0, info->dbID, DM_MODE_READONLY);
    if (!c.openRef) { result = BACKUP_ERR_DM; goto done; }

    uint16_t count = info->isResDB ? dm_num_resources(&c, c.openRef)
                                   : dm_num_records(&c, c.openRef);
    if (c.aborted) { result = BACKUP_ERR_TIMEOUT; goto done; }

    /* Cap the allocation at the live-entry cap (the loop bails at BACKUP_MAX_ENTRIES),
     * so a DB reporting a huge count can't force a multi-MB alloc. */
    uint32_t nalloc = count < BACKUP_MAX_ENTRIES ? count : BACKUP_MAX_ENTRIES;
    ents = bk_alloc(sizeof(bk_entry_t) * (nalloc ? nalloc : 1));
    if (!ents) { result = BACKUP_ERR_OUT_OF_MEMORY; goto done; }

    /* appInfo / sortInfo blocks: bk_lock_block locks each to its data pointer
     * (handling pointer- vs handle-type local IDs, the 24-bit tag mask, the size
     * bound, and unlock-on-reject), reading the bytes directly from emulated RAM —
     * the reverse of the installer's DmWrite. The chunks stay locked across both
     * passes and are unlocked in cleanup. */
    bk_lock_block(&c, info->appInfoID,  &appRaw,  &appPtr,  &appSize);
    bk_lock_block(&c, info->sortInfoID, &sortRaw, &sortPtr, &sortSize);
    if (c.aborted) { result = BACKUP_ERR_TIMEOUT; goto done; }

    /* PASS 1: gather live entries + chunk sizes. */
    for (uint16_t i = 0; i < count; i++) {
        if (c.aborted) { result = BACKUP_ERR_TIMEOUT; goto done; }
        if (live >= BACKUP_MAX_ENTRIES) { result = BACKUP_ERR_BAD_DB; goto done; }
        bk_entry_t *e = &ents[live];
        memset(e, 0, sizeof(*e));
        e->index = i;

        if (info->isResDB) {
            sc_w32(&c, SCX_RESTYPE, 0); sc_w16(&c, SCX_RESID, 0); sc_w32(&c, SCX_CHUNKID, 0);
            uint16_t err = dm_resource_info(&c, c.openRef, i,
                              c.scratch + SCX_RESTYPE, c.scratch + SCX_RESID, c.scratch + SCX_CHUNKID);
            if (err) { g_installer_last_dm_err = err; result = BACKUP_ERR_DM; goto done; }
            e->resType = sc_r32(&c, SCX_RESTYPE);
            e->resID   = sc_r16(&c, SCX_RESID);
            uint32_t hnd = dm_get_resource_index(&c, c.openRef, i);
            if (!hnd) { result = BACKUP_ERR_DM; goto done; }
            e->size = mem_handle_size(&c, hnd);
            dm_release_resource(&c, hnd);
        } else {
            sc_w16(&c, SCX_RECATTR, 0); sc_w32(&c, SCX_UID, 0); sc_w32(&c, SCX_CHUNKID, 0);
            uint16_t err = dm_record_info(&c, c.openRef, i,
                              c.scratch + SCX_RECATTR, c.scratch + SCX_UID, c.scratch + SCX_CHUNKID);
            if (err) { g_installer_last_dm_err = err; result = BACKUP_ERR_DM; goto done; }
            if (sc_r32(&c, SCX_CHUNKID) == 0) continue;   /* deleted/nil record -> drop */
            e->recAttr = (uint8_t)sc_r16(&c, SCX_RECATTR);
            e->uid     = sc_r32(&c, SCX_UID) & 0x00FFFFFFu;
            uint32_t hnd = dm_query_record(&c, c.openRef, i);   /* peek, no release needed */
            if (!hnd) { result = BACKUP_ERR_DM; goto done; }
            e->size = mem_handle_size(&c, hnd);
        }
        /* Reject a garbage size before it feeds the offset/bounds arithmetic, and
         * drop empty live chunks so backup stays symmetric with the installer
         * (which skips size-0 records/resources), keeping the round-trip a fixpoint.
         * The resource handle was already released above, so `continue` is safe. */
        if (e->size > BUS_RAM_SIZE) { result = BACKUP_ERR_BAD_DB; goto done; }
        if (e->size == 0) continue;
        live++;
        if ((i & 31) == 31 && g_installer_yield) g_installer_yield();
    }
    if (c.aborted) { result = BACKUP_ERR_TIMEOUT; goto done; }

    /* Compute file offsets: header | entries | 2-byte gap | appInfo | sortInfo | chunks. */
    uint32_t arrayEnd  = HDR_SIZE + (uint32_t)live * entrySize;
    uint32_t appOff    = appSize  ? arrayEnd + 2 : 0;
    uint32_t sortOff   = sortSize ? (appOff ? appOff + appSize : arrayEnd + 2) : 0;
    uint32_t dataStart = arrayEnd + 2 + appSize + sortSize;
    uint32_t cursor    = dataStart;
    for (int k = 0; k < live; k++) { ents[k].fileOff = cursor; cursor += ents[k].size; }

    /* Output must remain installable by installer_install_image. */
    if (cursor > INSTALL_MAX_FILE_BYTES) { result = BACKUP_ERR_BAD_DB; goto done; }

    /* PASS 2a: header. */
    uint8_t hdr[HDR_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, info->name, 32);
    wb16(hdr + 32, (uint16_t)(info->attributes & ~DM_HDR_ATTR_OPEN));
    wb16(hdr + 34, info->version);
    wb32(hdr + 36, info->crDate);
    wb32(hdr + 40, info->modDate);
    wb32(hdr + 44, info->bkDate);
    wb32(hdr + 48, info->modNum);
    wb32(hdr + 52, appOff);
    wb32(hdr + 56, sortOff);
    wb32(hdr + 60, info->type);
    wb32(hdr + 64, info->creator);
    wb32(hdr + 68, 0);                       /* uniqueIDSeed -> 0 */
    wb32(hdr + 72, 0);                       /* nextRecordListID -> 0 */
    wb16(hdr + 76, (uint16_t)live);
    if (!sink->write(sink->ctx, hdr, HDR_SIZE)) { result = BACKUP_ERR_IO; goto done; }

    /* PASS 2b: entry array. */
    for (int k = 0; k < live; k++) {
        uint8_t eb[RES_ENTRY_SIZE];          /* max(8,10) = 10 */
        if (info->isResDB) {
            wb32(eb + 0, ents[k].resType);
            wb16(eb + 4, ents[k].resID);
            wb32(eb + 6, ents[k].fileOff);
        } else {
            wb32(eb + 0, ents[k].fileOff);
            eb[4] = ents[k].recAttr;
            eb[5] = (uint8_t)(ents[k].uid >> 16);
            eb[6] = (uint8_t)(ents[k].uid >> 8);
            eb[7] = (uint8_t)(ents[k].uid);
        }
        if (!sink->write(sink->ctx, eb, entrySize)) { result = BACKUP_ERR_IO; goto done; }
    }

    /* PASS 2c: 2-byte gap, then appInfo / sortInfo blocks. */
    uint8_t gap[2] = { 0, 0 };
    if (!sink->write(sink->ctx, gap, 2))                       { result = BACKUP_ERR_IO; goto done; }
    if (appSize  && !sink_write_ram(sink, appPtr,  appSize))   { result = BACKUP_ERR_IO; goto done; }
    if (sortSize && !sink_write_ram(sink, sortPtr, sortSize))  { result = BACKUP_ERR_IO; goto done; }

    /* PASS 2d: data chunks — re-acquire each by index and stream from g_bus_ram. */
    for (int k = 0; k < live; k++) {
        if (c.aborted) { result = BACKUP_ERR_TIMEOUT; goto done; }
        uint32_t hnd = info->isResDB ? dm_get_resource_index(&c, c.openRef, ents[k].index)
                                     : dm_query_record(&c, c.openRef, ents[k].index);
        if (!hnd) { result = BACKUP_ERR_DM; goto done; }
        uint32_t ptr = bk_ram_addr(mem_handle_lock(&c, hnd));
        bool okw = ptr && ptr + ents[k].size <= BUS_RAM_SIZE
                   && sink_write_ram(sink, ptr, ents[k].size);
        mem_handle_unlock(&c, hnd);
        if (info->isResDB) dm_release_resource(&c, hnd);
        if (!okw) { result = c.aborted ? BACKUP_ERR_TIMEOUT : BACKUP_ERR_IO; goto done; }
        if ((k & 31) == 31 && g_installer_yield) g_installer_yield();
    }

    result = c.aborted ? BACKUP_ERR_TIMEOUT : BACKUP_OK;

done:
    if (c.aborted && result != BACKUP_ERR_IO) result = BACKUP_ERR_TIMEOUT;
    mem_ptr_unlock(&c, appRaw);             /* no-op if 0 */
    mem_ptr_unlock(&c, sortRaw);
    if (c.openRef) dm_close(&c, c.openRef);
    if (ents)      bk_free(ents);
    if (c.scratch) mem_free(&c, c.scratch);
    return result;
}

/* ---- backup file-sink + filename dedupe (pure C: no SD/ESP deps) ----------
 * These live in the host-buildable section (not the #ifndef HOST_BUILD block)
 * so both the device backup (backup_run_to_sd) and the host -b dump share one
 * implementation. backup_file_sink takes a FILE* via void* ctx — the cast is
 * here, so backup.h stays free of <stdio.h>. */
bool backup_file_sink(void *ctx, const void *data, uint32_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len;
}

void backup_dedupe(char used[][BACKUP_NAME_MAX], int nused, char *fname)
{
    bool clash = false;
    for (int i = 0; i < nused; i++) if (!strcmp(used[i], fname)) { clash = true; break; }
    if (!clash) return;

    char base[BACKUP_NAME_MAX];
    snprintf(base, sizeof(base), "%s", fname);
    char ext[8] = "";
    char *dot = strrchr(base, '.');
    if (dot) { snprintf(ext, sizeof(ext), "%s", dot); *dot = '\0'; }

    for (int k = 1; k < 1000; k++) {
        /* Bounded widths (%.31s base, %.6s ext) keep the result inside fname's
         * BACKUP_NAME_MAX even with a 3-digit suffix — and satisfy -Wformat-truncation. */
        snprintf(fname, BACKUP_NAME_MAX, "%.31s~%d%.6s", base, k, ext);
        bool again = false;
        for (int i = 0; i < nused; i++) if (!strcmp(used[i], fname)) { again = true; break; }
        if (!again) return;
    }
}

/* ===================== device-only: SD scan ============================== */
#ifndef HOST_BUILD

#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "m5_sd.h"

static const char *TAG = "installer";

#define INSTALL_DIR      M5_SD_MOUNT_POINT "/install"

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Scan `dir` for installable files (.prc/.pdb/.pqa via kind_of) into names[], sorted
 * for deterministic order. `name_filter` (NULL/"" = none) additionally restricts to
 * one exact filename. Returns the count collected (0..max), or -1 if the directory is
 * absent (opendir failed) — the caller distinguishes "nothing to do" (>=0) from "no
 * such dir" (-1). The caller owns mount/unmount; this only opens/reads the dir.
 * Shared by installer_scan_sd and backup_restore_from_sd. */
static int scan_sd_names(const char *dir, char names[][INSTALL_NAME_MAX], int max,
                         const char *name_filter)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int nn = 0;
    struct dirent *de;
    while (nn < max && (de = readdir(d)) != NULL) {
        if (kind_of(de->d_name) < 0) continue;   /* skips subdirs / non-.prc/.pdb names */
        if (name_filter && name_filter[0] && strcmp(de->d_name, name_filter) != 0) continue;
        snprintf(names[nn], INSTALL_NAME_MAX, "%.*s", INSTALL_NAME_MAX - 1, de->d_name);
        nn++;
    }
    closedir(d);
    qsort(names, nn, INSTALL_NAME_MAX, name_cmp);
    return nn;
}

/* Read a whole file into a fresh PSRAM buffer: stat + size-check (0 < sz <=
 * INSTALL_MAX_FILE_BYTES) + heap_caps_malloc(SPIRAM) + fopen/fread/fclose. Returns the
 * buffer (caller heap_caps_free's) and sets *out_sz, or NULL on any failure (each logged
 * with the path). Shared by installer_scan_sd and backup_restore_from_sd. */
static uint8_t *read_file_psram(const char *path, size_t *out_sz)
{
    *out_sz = 0;
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    size_t sz = (size_t)st.st_size;
    if (sz == 0 || sz > INSTALL_MAX_FILE_BYTES) {
        ESP_LOGW(TAG, "skip %s (%u bytes, out of range)", path, (unsigned)sz);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGW(TAG, "PSRAM alloc failed for %s", path); return NULL; }
    FILE *f = fopen(path, "rb");
    if (!f) { heap_caps_free(buf); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    if (rd != sz) { ESP_LOGW(TAG, "short read on %s", path); heap_caps_free(buf); return NULL; }
    *out_sz = sz;
    return buf;
}

int installer_scan_sd(install_set_t *set)
{
    sdmmc_card_t *card = NULL;
    if (m5_sd_mount(&card) != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed; no auto-install this boot");
        return 0;
    }

    /* names[] is static (1 KB) to keep it off the boot task's stack; safe because
     * installer_scan_sd runs exactly once, in the single-threaded boot SD window. */
    static char names[INSTALL_MAX_FILES][INSTALL_NAME_MAX];
    int nn = scan_sd_names(INSTALL_DIR, names, INSTALL_MAX_FILES, NULL);
    if (nn < 0) {
        ESP_LOGI(TAG, "no %s directory; skipping auto-install", INSTALL_DIR);
        m5_sd_unmount(card);
        return 0;
    }

    for (int i = 0; i < nn; i++) {
        char path[sizeof(INSTALL_DIR) + INSTALL_NAME_MAX + 2];
        snprintf(path, sizeof(path), "%s/%.*s", INSTALL_DIR, INSTALL_NAME_MAX - 1, names[i]);

        size_t sz;
        uint8_t *buf = read_file_psram(path, &sz);
        if (!buf) continue;   /* out-of-range / open / short-read: skip, try next */
        /* Cumulative budget check stays here (only the auto-installer accumulates into
         * a persistent install_set_t). The one file that trips the cap is read then
         * freed — a single wasted read at boot, negligible. */
        if (set->total_bytes + sz > INSTALL_MAX_TOTAL_BYTES) {
            ESP_LOGW(TAG, "PSRAM budget reached; %s and later files skipped", names[i]);
            heap_caps_free(buf);
            break;
        }
        if (!installer_add_buffer(set, names[i], buf, sz)) { heap_caps_free(buf); break; }
        ESP_LOGI(TAG, "buffered %s (%u bytes)", names[i], (unsigned)sz);
    }

    m5_sd_unmount(card);
    ESP_LOGI(TAG, "auto-install: %d file(s) queued from %s", set->count, INSTALL_DIR);
    return set->count;
}

/* ===================== device-only: backup / restore ===================== */

#define BACKUP_DIR       M5_SD_MOUNT_POINT "/backup"

/* Built-in organizer apps that sync via their own HotSync conduit and therefore
 * do NOT set dmHdrAttrBackup, even though their RAM databases hold the user's data.
 * A bare backup force-includes databases owned by these creators so a memo/contact/
 * appointment/task snapshot isn't silently dropped (the dmHdrAttrBackup filter alone
 * misses them — see the missing-memo investigation). Match is by creator, so an app's
 * auxiliary databases come along too. */
static bool backup_is_user_data_creator(uint32_t creator)
{
    switch (creator) {
    case 0x6D656D6F: /* 'memo' Memo Pad   */
    case 0x61646472: /* 'addr' Address    */
    case 0x64617465: /* 'date' Date Book  */
    case 0x746F646F: /* 'todo' To Do List */
    case 0x6D61696C: /* 'mail' Mail       */
    case 0x65787073: /* 'exps' Expense    */
        return true;
    default:
        return false;
    }
}

backup_result_t backup_run_to_sd(m68k_cpu_t *cpu, const char *name,
                                 int *ok, int *fail, int *skipped)
{
    *ok = *fail = *skipped = 0;
    sdmmc_card_t *card = NULL;
    if (m5_sd_mount(&card) != ESP_OK) {
        ESP_LOGW(TAG, "backup: SD mount failed");
        return BACKUP_ERR_IO;
    }
    mkdir(BACKUP_DIR, 0777);   /* harmless if it already exists */

    /* static (not stack): the cpu task has a bounded stack and these are large. */
    static backup_db_info_t dbs[BACKUP_MAX_DBS];
    static char used[BACKUP_MAX_DBS][BACKUP_NAME_MAX];
    int n = backup_enumerate(cpu, dbs, BACKUP_MAX_DBS);
    if (n < 0) { m5_sd_unmount(card); return BACKUP_ERR_NOT_READY; }

    int nused = 0;
    for (int i = 0; i < n; i++) {
        backup_db_info_t *d = &dbs[i];
        /* A bare backup honors each database's own dmHdrAttrBackup intent (set by
         * the ~all 3rd-party apps that rely on the generic Backup conduit), PLUS a
         * small override for the built-in organizer apps whose data syncs via a
         * dedicated conduit and so does NOT carry the bit. A named backup still
         * grabs that one DB regardless of flag. */
        bool match = (name && name[0])
                       ? (strcmp(d->name, name) == 0)
                       : (((d->attributes & DM_HDR_ATTR_BACKUP) != 0)
                          || backup_is_user_data_creator(d->creator));
        if (!d->romBased)
            ESP_LOGD(TAG, "backup-scan '%s' type=%08lX creator=%08lX attr=%04X -> %s",
                     d->name, (unsigned long)d->type, (unsigned long)d->creator,
                     d->attributes, match ? "INCLUDE" : "skip");
        if (!match) continue;
        if (d->romBased) { (*skipped)++; continue; }

        char fname[BACKUP_NAME_MAX];
        backup_make_filename(d, fname, sizeof(fname));
        backup_dedupe(used, nused, fname);
        if (nused < BACKUP_MAX_DBS) snprintf(used[nused++], BACKUP_NAME_MAX, "%s", fname);

        char path[sizeof(BACKUP_DIR) + BACKUP_NAME_MAX + 2];
        snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, fname);
        FILE *f = fopen(path, "wb");
        if (!f) { (*fail)++; ESP_LOGW(TAG, "backup: cannot create %s", path); continue; }
        backup_sink_t sink = { backup_file_sink, f };
        backup_result_t r = backup_serialize_db(cpu, d, &sink);
        fclose(f);
        if (r == BACKUP_OK) {
            (*ok)++;
            ESP_LOGI(TAG, "backup %s ('%s') -> %s", d->name, fname, path);
        } else {
            (*fail)++;
            remove(path);          /* don't leave a truncated file behind */
            ESP_LOGW(TAG, "backup %s FAILED: %s (dmErr=0x%04X)", d->name,
                     backup_result_str(r), g_installer_last_dm_err);
        }
        if (g_installer_yield) g_installer_yield();
    }

    m5_sd_unmount(card);
    ESP_LOGI(TAG, "backup complete: %d ok, %d failed, %d skipped(ROM)", *ok, *fail, *skipped);
    return BACKUP_OK;
}

backup_result_t backup_restore_from_sd(m68k_cpu_t *cpu, const char *name,
                                       int *ok, int *fail, bool *needs_reset)
{
    *ok = *fail = 0;
    *needs_reset = false;
    sdmmc_card_t *card = NULL;
    if (m5_sd_mount(&card) != ESP_OK) {
        ESP_LOGW(TAG, "restore: SD mount failed");
        return BACKUP_ERR_IO;
    }

    /* names[] static to keep it off the cpu task's bounded stack; safe because restore
     * runs only on the cpu task in the parked SD window (no reentrancy). */
    static char names[INSTALL_MAX_FILES][INSTALL_NAME_MAX];
    int nn = scan_sd_names(BACKUP_DIR, names, INSTALL_MAX_FILES, name);
    if (nn <= 0) {
        /* dir absent (-1) or nothing matching (0): "nothing to restore" is success with
         * zero counts, NOT an I/O error (the old BACKUP_ERR_IO read as "sd/file io error"
         * on the console for a benign empty/absent /backup). */
        ESP_LOGI(TAG, "restore: nothing to restore in %s", BACKUP_DIR);
        m5_sd_unmount(card);
        return BACKUP_OK;
    }

    for (int i = 0; i < nn; i++) {
        char path[sizeof(BACKUP_DIR) + INSTALL_NAME_MAX + 2];
        snprintf(path, sizeof(path), "%s/%.*s", BACKUP_DIR, INSTALL_NAME_MAX - 1, names[i]);

        size_t sz;
        uint8_t *buf = read_file_psram(path, &sz);
        if (!buf) { (*fail)++; continue; }   /* out-of-range / open / short-read */

        char dbname[32] = { 0 };
        installer_result_t r = installer_install_image(cpu, buf, sz, dbname);
        heap_caps_free(buf);
        if (r == INSTALLER_OK || r == INSTALLER_NEEDS_RESET) {
            (*ok)++;
            if (r == INSTALLER_NEEDS_RESET) *needs_reset = true;
            ESP_LOGI(TAG, "restore %s ('%s'): %s", names[i], dbname, installer_result_str(r));
        } else {
            (*fail)++;
            ESP_LOGW(TAG, "restore %s ('%s'): %s (step %d dmErr=0x%04X)", names[i], dbname,
                     installer_result_str(r), g_installer_last_fail_step, g_installer_last_dm_err);
        }
        if (g_installer_yield) g_installer_yield();   /* feed the watchdog between files */
    }

    m5_sd_unmount(card);
    ESP_LOGI(TAG, "restore complete: %d ok, %d failed", *ok, *fail);
    return BACKUP_OK;
}

#endif /* !HOST_BUILD */
