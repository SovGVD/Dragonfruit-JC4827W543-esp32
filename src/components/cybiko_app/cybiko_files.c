#include "cybiko_app.h"
#include "m5_sd.h"
#include <stdio.h>
#include <sys/stat.h>

static const char *const rom_names[] = { "bootrom.bin", "cyrom150.bin", "xtreme-rom.bin" };
static const char *const flash_names[] = { "flash.bin", "cyos_v1508.bin", "xtreme-flash.bin" };

static bool find(const char *const *names, int count, char *out, size_t cap)
{
    char path[96];
    struct stat st;
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof path, CYBIKO_SD_DIR "/%s", names[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (out && cap) snprintf(out, cap, "%s", path);
            return true;
        }
    }
    return false;
}

bool cybiko_app_find_files(char *rom_path, char *flash_path, size_t cap)
{
    bool rom = find(rom_names, 3, rom_path, cap);
    bool flash = find(flash_names, 3, flash_path, cap);
    return rom && flash;
}
