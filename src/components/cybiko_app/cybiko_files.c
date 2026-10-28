#include "cybiko_app.h"
#include "m5_sd.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *const rom_names[] = { "bootrom.bin", "cyrom150.bin", "xtreme-rom.bin" };
static const char *const flash_names[] = { "flash.bin", "cyos_v1508.bin", "xtreme-flash.bin" };

static const char *const classic_rom_names[] = { "cyrom112.bin", "boot.bin", "bootrom.bin" };
static const char *const classic_flash_names[] = { "flash_saved.bin", "flash_v1246.bin", "flash.bin" };

static bool find_in(const char *dir, const char *const *names, int count, char *out, size_t cap)
{
    char path[96];
    struct stat st;
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (out && cap) snprintf(out, cap, "%s", path);
            return true;
        }
    }
    return false;
}

static bool find(const char *const *names, int count, char *out, size_t cap)
{
    return find_in(CYBIKO_SD_DIR, names, count, out, cap);
}

bool cybiko_app_find_classic(char *rom_path, char *flash_path, size_t cap)
{
    bool rom = find_in(CYBIKO_CLASSIC_DIR, classic_rom_names, 3, rom_path, cap);
    bool flash = find_in(CYBIKO_CLASSIC_DIR, classic_flash_names, 3, flash_path, cap);
    return rom && flash;
}

bool cybiko_app_find_files(char *rom_path, char *flash_path, size_t cap)
{
    bool rom = find(rom_names, 3, rom_path, cap);
    bool flash = find(flash_names, 3, flash_path, cap);
    return rom && flash;
}

bool cybiko_app_find_dump(char *flash_path, size_t cap)
{
    return find(flash_names + 1, 2, flash_path, cap);
}

bool cybiko_app_find_update(char *path, size_t cap)
{
    DIR *d = opendir(CYBIKO_SD_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcasecmp(e->d_name + n - 5, ".boot")) continue;
        char p[300];
        struct stat st;
        snprintf(p, sizeof p, CYBIKO_SD_DIR "/%s", e->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        /* CyOS images are about 500 KB; the small ones (avr*.boot, 7 KB)
         * update the Xtreme's AVR co-processor, which is not emulated. */
        if (st.st_size < CYBIKO_UPDATE_MIN || st.st_size > CYBIKO_UPDATE_MAX) continue;
        snprintf(path, cap, "%s", p);
        found = true;
    }
    closedir(d);
    return found;
}
