#pragma once

#include "m68k.h"

/* One directory-backed Palm OS 4 VFS volume. All entry points run on the CPU
 * task. No Palm ROM, filesystem image, driver PRC or periodic RAM save needed.
 * root must be a directory on an already mounted filesystem, kept mounted for
 * this component's lifetime. Paths exposed to Palm are relative to that root. */
bool palm_card_init(const char *root, bool read_only);
void palm_card_close(void);
void palm_card_reset(void); /* close guest handles on every emulated reset */
void palm_card_pump(m68k_cpu_t *cpu); /* register features once OS 4 UI is ready */
bool palm_card_ready(void);
bool palm_card_sync(void); /* flush open writable files before safe power-off */
bool palm_card_trap(m68k_cpu_t *cpu, uint16_t trap);

/* Optional board capacity query: used bytes / total bytes, capped at UINT32_MAX
 * (Palm's 32-bit volume API). Host builds use statvfs by default. */
extern bool (*palm_card_space)(uint32_t *used, uint32_t *total);
