#pragma once
#include <stdbool.h>
#include "cybiko.h"

/* Screen and on-screen keyboard for Cybiko mode (UI task on core 0). */
void cybiko_ui_start(void);

/* Provided by cybiko_app.c for the UI task. */
void cybiko_app_key(int key, bool down);
cybiko_t *cybiko_app_machine(void);
/* Emulation speed over the last second, in percent of the real machine's;
 * -1 until the first second has run. */
int cybiko_app_speed(void);
/* Share of the last second the CPU task spent running the emulation, in
 * percent. */
int cybiko_app_busy(void);
/* Write the RAM disk (if it changed, or always with force), reprogrammed
 * flash and the clock to /Cybiko. Pauses the CPU task briefly. */
void cybiko_app_save(bool force);
/* Whole frames (the boot menu's VSYNC): true when the CPU task takes the
 * frames, and then the newest whole frame (CY_LCD_W x CY_LCD_H luminance)
 * since the last call, or NULL. */
bool cybiko_app_vsync_on(void);
const uint8_t *cybiko_app_frame(void);
