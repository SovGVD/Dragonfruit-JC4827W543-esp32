#pragma once

/* Backup / restore of Palm databases (.prc / .pdb / .pqa) to/from the SD card.
 *
 * This is the REVERSE of the auto-installer (installer.h): backup reads databases
 * out of the running Palm OS and serializes them to standard PDB/PRC files on the
 * SD `/backup` directory; restore reads them back in. It is clean-room — the
 * Palm database file format is a documented public spec, and CloudpilotEmu's
 * DbExporter is consulted for algorithm understanding only, never copied.
 *
 * The export engine drives the Data Manager's *read/enumerate* traps
 * (DmNumDatabases/DmGetDatabase/DmDatabaseInfo/DmNumRecords/DmRecordInfo/
 * DmQueryRecord/DmNumResources/DmResourceInfo/DmGetResourceIndex, plus
 * MemHandleLock/Size and MemLocalIDToGlobal) via the same host->ROM trap-call
 * machinery the installer uses (m68k_call_trap, the ictx_t/ix_call funnel, the
 * scratch block). Chunk bytes are read directly from emulated RAM (g_bus_ram) —
 * the reverse of the installer's DmWrite copy. It reuses installer_install_image
 * for restore verbatim (only the byte source differs: /backup instead of /install).
 *
 * Normalization on export (so a backup is a clean, installable snapshot — NOT a
 * byte-for-byte copy of a vendor file): the dmHdrAttrOpen ("not closed properly")
 * bit is cleared, uniqueIDSeed and nextRecordListID are forced to 0, and deleted/
 * nil records (localChunkID == 0) are DROPPED (a compacting export — a literal 0
 * offset would also fail the installer's own validation, breaking the round-trip).
 * The strong correctness invariant is therefore the FIXPOINT, not vendor-equality:
 * install -> backup -> install -> backup is byte-stable. The offsets we emit
 * satisfy installer.c's parse_header (chunk offsets >= arrayEnd, non-decreasing,
 * with the 2-byte gap), so our backup round-trips through our own restore.
 *
 * The engine (backup_enumerate / backup_serialize_db / backup_os_ready and the
 * helpers) is pure C with no SD/ESP dependencies, so it builds and runs on the
 * host harness (dragonfruit -b <dir>). Only the SD orchestration (backup_run_to_sd /
 * backup_restore_from_sd) is device-only. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "m68k.h"          /* m68k_cpu_t — backup drives the CPU to call ROM traps */
#include "installer.h"     /* installer_install_image (restore reuses it) */

typedef enum {
    BACKUP_OK = 0,
    BACKUP_ERR_NOT_READY,        /* OS not booted / asleep when called */
    BACKUP_ERR_BUSY,             /* I/O tasks didn't park; SD bus not safe — retry */
    BACKUP_ERR_TRAP_UNRESOLVED,  /* a needed trap handler wasn't in ROM range */
    BACKUP_ERR_TIMEOUT,          /* a driven trap never returned (runaway guard) */
    BACKUP_ERR_DM,               /* a Data Manager trap reported an error */
    BACKUP_ERR_IO,               /* SD/file write failed */
    BACKUP_ERR_OUT_OF_MEMORY,    /* scratch / entry-table allocation failed */
    BACKUP_ERR_BAD_DB,           /* too many records, or output exceeds the cap */
    BACKUP_ERR_SKIPPED_ROM,      /* a named DB resolved to a ROM-based database */
    BACKUP_ERR_UNKNOWN,
} backup_result_t;

/* Forward-write sink: the serializer never seeks (all file offsets are computed
 * up front), so a pure forward writer suffices. write() returns false on IO
 * failure, which aborts the serialize with BACKUP_ERR_IO. ctx is a FILE* on both
 * device (/backup) and host (-b dir). */
typedef struct backup_sink {
    bool (*write)(void *ctx, const void *data, uint32_t len);
    void *ctx;
} backup_sink_t;

/* One card-0 database's header metadata, as read by backup_enumerate. */
typedef struct {
    uint32_t dbID;                /* LocalID on card 0 */
    char     name[32];            /* NUL-terminated DB name */
    uint16_t attributes;
    uint16_t version;
    uint32_t crDate, modDate, bkDate, modNum;
    uint32_t appInfoID, sortInfoID;   /* in-RAM LocalIDs (0 = none) */
    uint32_t type, creator;
    bool     isResDB;             /* attributes & dmHdrAttrResDB */
    bool     romBased;            /* MemLocalIDToGlobal(dbID) lands in ROM -> built-in */
} backup_db_info_t;

#define BACKUP_MAX_DBS    256     /* enumerate cap (a IIIx has well under this) */
#define BACKUP_NAME_MAX   48      /* FAT-safe backup filename budget (name + ext + dedupe ~k suffix) */

/* Enumerate every database on card 0 into out[] (metadata only — no DB is opened
 * or locked). Returns the count written (0..max), or -1 on not-ready / trap
 * timeout. MUST run on the cpu task. */
int backup_enumerate(m68k_cpu_t *cpu, backup_db_info_t *out, int max);

/* Serialize one database (described by info, from backup_enumerate) to sink as a
 * standard .prc/.pdb byte stream. Two passes: gather record/resource sizes to
 * compute file offsets, then forward-write header + entry table + gap + appInfo/
 * sortInfo + data chunks (streamed from g_bus_ram). MUST run on the cpu task. */
backup_result_t backup_serialize_db(m68k_cpu_t *cpu, const backup_db_info_t *info,
                                    backup_sink_t *sink);

/* Cheap read-only precondition: awake, LCD up, and every read/enumerate trap the
 * exporter needs resolves into ROM. Like installer_os_ready, this is necessary
 * but not sufficient — drive backup only after a settle signal + a heap probe. */
bool backup_os_ready(void);

/* File extension for a DB: ".prc" (resource DB), ".pqa" (Clipper/launchable
 * data), else ".pdb". */
const char *backup_ext_for(const backup_db_info_t *info);

/* Build a FAT-safe filename ("<sanitized-name><ext>") for a DB into out[n]. */
void backup_make_filename(const backup_db_info_t *info, char *out, size_t n);

const char *backup_result_str(backup_result_t r);

/* Forward-write sink over a FILE* (ctx is a FILE*). Shared by the device backup
 * (/backup) and the host -b dump; the FILE* cast lives in installer.c, so this
 * header needs no <stdio.h>. Returns false (aborting the serialize) on short write. */
bool backup_file_sink(void *ctx, const void *data, uint32_t len);

/* Make `fname` unique against the first `nused` entries of used[][] by appending
 * "~k" before the extension on a collision (two DB names can sanitize equal). The
 * device backup loop and the host -b dump both use this to avoid silent overwrites. */
void backup_dedupe(char used[][BACKUP_NAME_MAX], int nused, char *fname);

/* ---- console/integration runtime (mirrors install_runtime_t) -------------
 * One struct holds the backup/restore runtime. The cpu task is the sole writer of
 * every field except `req` + `arg`, which the console publishes to it. The handoff
 * is the same release/acquire pattern db_uart_rx_push uses (uart.c): the console
 * writes `arg`, issues a __sync_synchronize() barrier, THEN sets `req`; the cpu
 * task reads `req`, barriers, and snapshots `arg` into a local. A non-zero `req` is
 * the single "busy" lock — the console rejects a new request until the cpu clears
 * it after the op completes. The console reads phase/ok/fail/skipped/last racily
 * for `backup status`. */
typedef enum { BR_IDLE = 0, BR_RUNNING, BR_DONE } br_phase_t;
typedef enum { BR_REQ_NONE = 0, BR_REQ_BACKUP, BR_REQ_RESTORE } br_req_t;

typedef struct {
    volatile int    req;          /* console -> cpu: br_req_t (0 = idle/free) */
    char            arg[64];      /* optional DB-name filter ("" = all); see above */
    br_phase_t      phase;
    int             ok, fail, skipped;
    backup_result_t last;
} backup_runtime_t;

#ifndef HOST_BUILD
/* Device: mount the SD, back up matching card-0 RAM databases to /sdcard/backup
 * (overwriting same-named files), unmount. name==NULL/"" backs up every RAM DB that
 * either carries the backup flag (dmHdrAttrBackup) OR belongs to a built-in PIM app
 * (Memo/Address/Date Book/To Do/Mail/Expense, which sync via their own conduit and so
 * omit the flag); a non-NULL name backs up just that DB regardless of the flag.
 * ROM-based DBs are skipped (counted in skipped). Tallies per-DB results into the
 * ok/fail/skipped out-params. MUST run on the cpu task with the display+input tasks
 * already parked (the SD shares the display bus). */
backup_result_t backup_run_to_sd(m68k_cpu_t *cpu, const char *name,
                                 int *ok, int *fail, int *skipped);

/* Device: restore .prc/.pdb/.pqa files from /sdcard/backup into the running OS
 * via installer_install_image, one file at a time. name==NULL/"" restores all;
 * non-NULL restores just /backup/<name>. *needs_reset is set if any installed DB
 * requested reset-after-install. (The caller warm-resets after any restore that
 * touched storage anyway — see run_br_engine — so there is no separate launcher-kick
 * out-param.) MUST run on the cpu task with the I/O tasks parked. */
backup_result_t backup_restore_from_sd(m68k_cpu_t *cpu, const char *name,
                                       int *ok, int *fail, bool *needs_reset);
#endif
