#pragma once

#include "esp_err.h"
#include <stdint.h>

/* Initialize the IT8951 hardware (SPI bus, panel clear). Does NOT draw the
 * silkscreen — the caller establishes the layout first, then calls
 * display_draw_static(). The SD card (ROM + /palm.cfg) shares this SPI bus, so
 * display_init() must run before any SD access. */
esp_err_t display_init(void);

/* Real panel dimensions from the IT8951 (valid after display_init succeeds).
 * The caller uses these to size the layout. */
void display_get_panel_size(uint16_t *w, uint16_t *h);

/* Build + draw the static silkscreen overlay for the active layout. Call once
 * after the layout is established (default or loaded from SD config). */
void display_draw_static(void);

void display_refresh(void);

/* Render a short status/error message centered on the Palm-screen area and push
 * it with a full GC16 refresh. '\n' starts a new line; text is drawn uppercase
 * in a small built-in font. Used for the fatal "no ROM on SD" screens at boot,
 * so it must be safe to call before any tasks start (display_init must have
 * succeeded first — it shares the SPI bus). Keep each line <= ~13 chars. */
void display_show_message(const char *msg);

/* Same as display_show_message but pushed with the fast 2-level (A2) waveform
 * (~155 ms vs GC16's ~465 ms on this panel — see docs/m5paper-it8951-refresh-timings.md)
 * — used for the backup/restore confirm countdown where the digits update roughly once a
 * second. May leave light ghosting that a following GC16 (e.g. display_resume_after_wake)
 * clears. */
void display_show_message_fast(const char *msg);

/* Sleep coordination (called from the CPU task around esp_light_sleep_start,
 * while the display task is parked so there is no concurrent SPI). */
void display_blank_for_sleep(void);
void display_resume_after_wake(void);

/* Measure and log (over serial, as "= WAVE ..." lines) how long each IT8951
 * waveform mode takes to complete, for the full panel and the live screen
 * region. MUST be called from the display task (sole IT8951 owner) — main.c
 * services the debug-console `wavetime` request from there. Flashes the panel
 * and restores the live screen + silkscreen when done. */
void display_run_waveform_benchmark(void);
