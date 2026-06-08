#pragma once

#include <stdbool.h>
#include "m68k.h"

/* Resolve EvtEnqueuePenPoint / PenScreenToRaw from the live trap table and inject
 * one pen point via m68k_pen_tap, then wake the CPU to drain it. Returns false if
 * the trap table isn't ready yet (globals==0 or the handler isn't in ROM) — early
 * taps before the OS boots are silently dropped. Pen-up: pass x=y=-1, pen_up=true
 * (do NOT route the sentinel through layout_panel_to_palm). Shared by the UI mouse
 * path and any scripted-tap path; compiled unconditionally (no SDL dependency). */
bool host_pen_inject(m68k_cpu_t *cpu, int x, int y, bool pen_up);

#ifdef HOST_UI
/* Advance-the-emulator callback: runs one batch (execute + timer + irq + the
 * install/backup pumps host_main owns) and returns the cycles advanced, or 0 to
 * stop. host_main passes its per-batch body so the UI reuses all orchestration. */
typedef int (*host_step_fn)(m68k_cpu_t *cpu, void *ctx);

/* Run the interactive SDL3 UI: a window sized to the active layout panel, the
 * render-on-change + mouse->pen loop, and real-time-paced emulation via `step`.
 * Returns the process exit code. */
int host_ui_run(m68k_cpu_t *cpu, host_step_fn step, void *ctx);
#endif
