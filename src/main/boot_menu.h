#pragma once

typedef enum {
    BOOT_SYSTEM_PALM = 0,
    BOOT_SYSTEM_CYBIKO,
    BOOT_SYSTEM_COUNT
} boot_system_t;

/* Show the system chooser on the panel and block until a system is picked.
 * Needs display_init() and input_init(). Returns BOOT_SYSTEM_PALM straight
 * away when no SD card mounts, so the Palm path can report it. */
boot_system_t boot_menu_run(void);
