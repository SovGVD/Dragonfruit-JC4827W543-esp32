#pragma once

/* Auto-installer for Palm databases (.prc / .pdb / .pqa) dropped on the SD card.
 *
 * Files placed in /install on the SD are buffered into PSRAM at boot, then
 * installed into the running Palm OS by driving Data Manager traps from host C
 * (the clean-room equivalent of CloudpilotEmu's "homebrew" install path — we
 * rebuild each database record-by-record via DmCreateDatabase/DmNewResource/
 * DmNewRecord/DmWrite rather than the OS-callback ExgDBRead path, which our
 * single-step trap-call model can't drive).
 *
 * Source files are LEFT IN /install and re-installed on every ESP32 boot. This
 * is the right model here because the emulated Palm RAM is volatile: bus_init
 * zeroes it on every cold boot and nothing persists it to flash, so an installed
 * database survives an emulated power-off + ESP32 light sleep (PSRAM retained,
 * app_main does not re-run) but is wiped by a full power-cycle/reflash — exactly
 * when re-installing from /install is wanted. installer_install_image is
 * idempotent (it deletes any existing same-named RAM database before recreating
 * it), so re-installing over a fresh boot is clean.
 *
 * The engine (installer_install_image / installer_pump / installer_os_ready /
 * installer_add_buffer) is pure C with no SD/ESP dependencies, so it builds and
 * runs on the host harness. Only the SD scan (installer_scan_sd) is
 * device-only. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "m68k.h"   /* m68k_cpu_t — install drives the CPU to call ROM traps */

/* Conservative caps for a 2 MB-RAM Palm IIIx. */
#define INSTALL_MAX_FILES        16
#define INSTALL_MAX_FILE_BYTES   (512u * 1024u)
#define INSTALL_MAX_TOTAL_BYTES  (1024u * 1024u)
#define INSTALL_NAME_MAX         64

typedef enum {
    INSTALLER_OK = 0,
    INSTALLER_NEEDS_RESET,         /* installed, but DB has dmHdrAttrResetAfterInstall */
    INSTALLER_ERR_NOT_READY,       /* OS not booted / asleep when called */
    INSTALLER_ERR_BAD_IMAGE,       /* header validation failed */
    INSTALLER_ERR_DB_OPEN,         /* existing DB is open; cannot overwrite */
    INSTALLER_ERR_OUT_OF_MEMORY,   /* storage/dynamic-heap allocation failed */
    INSTALLER_ERR_CORRUPT,         /* a Data Manager trap reported an error */
    INSTALLER_ERR_TRAP_UNRESOLVED, /* a needed trap handler wasn't in ROM range */
    INSTALLER_ERR_TIMEOUT,         /* a driven trap never returned (runaway guard) */
    INSTALLER_ERR_UNKNOWN,
} installer_result_t;

typedef enum { IMG_PRC = 0, IMG_PDB, IMG_PQA } install_kind_t;

typedef enum {
    IMG_PENDING = 0,   /* buffered, not yet attempted */
    IMG_INSTALLED,     /* installed OK (or needs reset) */
    IMG_FAILED,        /* attempted, engine returned an error */
} install_img_status_t;

typedef struct {
    char     name[INSTALL_NAME_MAX]; /* basename, e.g. "MemoPad.prc" */
    char     dbname[32];             /* parsed Palm DB name (valid once attempted) */
    uint8_t *data;                   /* PSRAM (device) / malloc (host); owned by set */
    size_t   size;
    install_kind_t       kind;
    install_img_status_t status;
    installer_result_t   result;     /* last engine result (valid once attempted) */
} install_image_t;

typedef struct {
    install_image_t img[INSTALL_MAX_FILES];
    int    count;
    int    next;            /* index of the next IMG_PENDING image to install */
    size_t total_bytes;
} install_set_t;

/* Optional hook the engine calls every few records during a long install
 * (NULL = no-op). On device, main.c points this at a watchdog-feed/yield
 * wrapper so a large .pdb doesn't starve the idle task past the task-watchdog
 * window. Left NULL on the host. */
extern void (*g_installer_yield)(void);

/* Stage (1..8) at which the most recent failed install gave up — for logging a
 * give-up reason without spamming a line per retry. 1=scratch alloc,
 * 2=delete-existing, 3=create, 4=find, 5=set-info, 6=open, 7=appinfo/sortinfo,
 * 8=records. */
extern int g_installer_last_fail_step;

/* Raw Data Manager error (Err) behind the most recent INSTALLER_ERR_CORRUPT, or
 * 0 if the failure wasn't a DM error (e.g. a bad image or out-of-memory). Lets a
 * failure log show *which* DM error occurred instead of just "data manager
 * error". Paired with g_installer_last_fail_step. */
extern uint16_t g_installer_last_dm_err;

/* Append an already-buffered image to the set (takes ownership of `data` on
 * success). Returns false — and does NOT take ownership — if the set is full,
 * the file is empty/too large, or the total PSRAM budget would be exceeded.
 * `name` is copied; the extension selects the kind. */
bool installer_add_buffer(install_set_t *set, const char *name,
                          uint8_t *data, size_t size);

/* Install one image into the running Palm OS (card 0). MUST be called on the
 * cpu task with the OS booted, awake, and past the readiness gate (use the
 * installer_pump driver, which probes the heap first). name_out, if non-NULL,
 * must point to a buffer of EXACTLY 32 bytes (the Palm DB-name field width); it
 * receives the parsed DB name (NUL-terminated) for logging. */
installer_result_t installer_install_image(m68k_cpu_t *cpu,
                                           const uint8_t *image, size_t size,
                                           char *name_out);

/* Cheap, read-only precondition for installing: the device is awake, the LCD is
 * up, and every trap the engine needs resolves into ROM. NECESSARY but NOT
 * sufficient — the dispatch table is populated before the memory manager is
 * initialised, so calling a trap this early crashes the OS. The installer_pump
 * driver therefore additionally waits for a cheap settle signal (the boot->UI
 * framebuffer switch, i.e. the Window Manager is up well past MemInit; or an
 * idle STOP on builds that doze) and then a functional heap probe
 * (installer_probe_heap) before driving any real install. */
bool installer_os_ready(void);

/* Functional readiness probe: actually round-trips MemPtrNew + MemPtrFree to
 * prove the dynamic heap (and thus the memory/data managers) is live. This is
 * the AUTHORITATIVE "OS is up" signal. It DRIVES TRAPS, so it must only be
 * called once installer_os_ready() passes AND a cheap settle signal says we are
 * past MemInit (calling MemPtrNew before MemInit crashes). Returns true on a
 * clean alloc+free round-trip. */
bool installer_probe_heap(m68k_cpu_t *cpu);

/* Refresh the Launcher so a freshly-installed app appears: inject vchrLaunch
 * (0x0108) + commandKeyMask via the ROM's EvtEnqueueKey trap. No-op if the trap
 * table isn't resolvable yet. */
void installer_refresh_launcher(m68k_cpu_t *cpu);

const char *installer_result_str(installer_result_t r);

/* ---- per-tick install driver (shared by main.c and host_main.c) ----------
 * The pump owns the whole orchestration so the device and host harnesses don't
 * duplicate it: settle detection (boot->UI framebuffer switch, or an idle STOP),
 * the functional heap probe, and the per-image install loop. The caller holds a
 * zero-initialised installer_pump_state_t across calls and supplies the readiness
 * inputs each tick; it performs only the side effects it owns (logging, the
 * launcher kick, the warm reset). MUST be called on the cpu task (it drives
 * traps). */
typedef struct {
    uint32_t boot_fb;        /* first framebuffer seen (the boot FB) */
    uint32_t ui_cksum;       /* content checksum of the UI surface when it first appeared
                              * (the boot splash); settle waits for it to change (launcher drawn) */
    bool     settled;        /* cheap pre-gate satisfied (launcher drawn or idle STOP) */
    bool     heap_confirmed; /* installer_probe_heap has succeeded at least once */
} installer_pump_state_t;

typedef struct {
    int  status;             /* 0 = still working, 1 = all images attempted */
    bool need_launcher_kick; /* (status==1) caller should refresh the Launcher */
    bool need_warm_reset;    /* (status==1) an installed DB requested reset-after-install */
    const install_image_t *attempted;  /* image installed/failed THIS call, else NULL */
} installer_pump_result_t;

/* Coarse phase the host/device harness reports over the console (`install
 * status`). The cpu task is the sole writer; console reads are racy diagnostics. */
typedef enum {
    INST_IDLE = 0,   /* nothing queued */
    INST_WAIT,       /* queued, waiting for the OS to settle */
    INST_RUN,        /* installing one image per batch */
    INST_DONE,       /* finished (launcher refreshed / warm reset) */
} install_phase_t;

/* Recommended container that bundles all of an integrator's install runtime
 * state (main.c uses one of these as g_install; the console reads it). The cpu
 * task is the sole writer of every field except `force`, so only `force` is
 * volatile: `set`/`pump` are touched on the cpu hot path and must not be, and
 * phase/ok/fail/last have a single cpu-task writer that the console reads as a
 * racy diagnostic. The host harness rolls its own locals instead — it doesn't
 * need the console-visible status — so this is optional sugar, not engine API. */
typedef struct {
    install_set_t          set;    /* buffered images (large; NOT volatile)      */
    installer_pump_state_t pump;   /* settle/probe state across batches          */
    install_phase_t        phase;  /* INST_*; cpu writes, console reads (racy)   */
    int                    ok, fail;
    installer_result_t     last;
    volatile bool          force;  /* console -> cpu: "install now"              */
} install_runtime_t;

/* osready  = installer_os_ready() result; current_fb = db_lcd_framebuffer_addr();
 * force    = bypass the cheap settle pre-gate (the probe still runs). */
installer_pump_result_t installer_pump(install_set_t *set, m68k_cpu_t *cpu,
                                       bool osready, uint32_t current_fb,
                                       bool force, installer_pump_state_t *state);

#ifndef HOST_BUILD
/* Mount the SD, scan /sdcard/install for .prc/.pdb/.pqa files at the top level,
 * buffer each into PSRAM (honoring the caps), unmount. Returns the number
 * buffered (0 if the directory is absent — not an error). MUST run in the boot
 * SD window (display SPI bus free, before the tasks start). Files are left in
 * place and re-scanned/re-installed every boot (see the file header). */
int installer_scan_sd(install_set_t *set);
#endif
