#pragma once
#include <stdbool.h>
#include "cybiko.h"

/* Screen and on-screen keyboard for Cybiko mode (UI task on core 0). */
void cybiko_ui_start(void);

/* Provided by cybiko_app.c for the UI task. */
void cybiko_app_key(int key, bool down);
cybiko_t *cybiko_app_machine(void);
/* Write the RAM disk (if it changed, or always with force), reprogrammed
 * flash and the clock to /Cybiko. Pauses the CPU task briefly. */
void cybiko_app_save(bool force);
