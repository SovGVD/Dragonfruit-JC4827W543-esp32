#pragma once
#include <stdint.h>
/* Render into a caller-owned 160x160, MSB-first 1bpp buffer (3200 bytes). */
void display_message_bitmap(const char *msg, uint8_t *msgfb);
/* Operation screen: phase text, percentage, progress bar and power reminder.
 * percent<0 means unknown (e.g. waiting for filesystem sync). */
void display_progress_bitmap(const char *title, const char *phase, int percent, uint8_t *fb);
