#pragma once
#include <stdint.h>
/* Render into a caller-owned 160x160, MSB-first 1bpp buffer (3200 bytes). */
void display_message_bitmap(const char *msg, uint8_t *msgfb);
