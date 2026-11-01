/* Installing Cybiko's System Pack 57 (CyOS 1.2.57) on an emulated Cybiko
 * Classic that runs CyOS 1.2.46, as Cybiko's AutoUpdate did: the files go in
 * through CyOS's serial console (cy_con.h), then "boot comp1.2.57r.boot" and
 * a restart. The files come straight out of Cybiko-AutoUpdate.exe, a Windows
 * program that keeps them as resources (type FILES_TO_LOAD: resource 2000
 * lists the names, 2001 + i holds file i; its autoupdate.ini says which go
 * to which model). Only the Classic's "[114]" part is done: the "[119]"
 * part (CyOS 1.3.57) is for a Classic that runs CyOS from a parallel ROM
 * at 100000, which the emulator does not have. The last step of the real
 * update, "avr write avr19c6.bin" (the radio chip's firmware), is left out:
 * the radio chip is emulated. ReBiko's tools/rebiko_cyos_update.py (MIT,
 * Dan Berkowitz) was the model for the order: desktop.app last, since CyOS
 * cannot replace the one it runs before it is deleted. */
#ifndef CY_SYSUPDATE_H
#define CY_SYSUPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cy_con.h"

struct cybiko;

#define CY_SU_MAX_FILES 32

typedef struct {
    char name[40];
    const uint8_t *data;       /* inside the .exe the caller keeps */
    uint32_t size;
} cy_su_file_t;

typedef enum {
    CY_SU_IDLE,
    CY_SU_WAITING,             /* for CyOS's console prompt */
    CY_SU_RUNNING,
    CY_SU_DONE,                /* CyOS 1.2.57 answered after the restart */
    CY_SU_FAILED,
} cy_su_state_t;

typedef struct {
    struct cybiko *m;
    cy_con_t con;
    cy_su_file_t files[CY_SU_MAX_FILES];
    int nfiles;
    int step, nsteps;          /* ver, files, rm, desktop.app, boot, reboot, ver */
    bool already;              /* done: CyOS was 1.2.57 before */
    int tries;                 /* of this step; a lost block can make CyOS give up a file */
    int retries;               /* steps done again, all told */
    cy_su_state_t state;
    uint32_t wait_ms;          /* how long the prompt has been awaited */
    char seen[8];              /* the last bytes CyOS sent, to spot "Ready >" */
    bool ready;
    char message[160];         /* what is happening, or what went wrong */
} cy_su_t;

/* Pick the Classic's System Pack files out of Cybiko-AutoUpdate.exe; returns
 * how many (0: not that program), the names into out. */
int cy_su_read_exe(const uint8_t *exe, size_t len, cy_su_file_t *out, int max);

/* Start an update of the running machine m (a Classic) with the files of exe
 * (kept by the caller until done). False if exe is not the updater. */
bool cy_su_start(cy_su_t *u, struct cybiko *m, const uint8_t *exe, size_t len);
/* Every byte CyOS sends on SCI2 (the host's serial_tx for port 2) */
void cy_su_rx(cy_su_t *u, uint8_t byte);
/* Every few milliseconds of emulated time */
cy_su_state_t cy_su_poll(cy_su_t *u, uint32_t elapsed_ms);
/* Steps done and all, for a progress bar */
static inline int cy_su_progress(const cy_su_t *u, int *total) { *total = u->nsteps; return u->step; }

#endif
