/* Exercise the real SD ROM-loader file/validation/cleanup path with a fake
 * mount and PSRAM allocator. The synthetic vector-valid ROM is not Palm OS. */
#include "rom_loader.h"
#include "m5_sd.h"
#include "bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#ifdef _WIN32
#include <direct.h>
#endif

static sdmmc_card_t card;
static int no_sd, fail_alloc, mounts, unmounts, allocations;
static const char *path = TEST_SD_DIR "/palm.rom";

esp_err_t m5_sd_mount(sdmmc_card_t **out)
{
    *out = NULL;
    mounts++;
    if (no_sd) return ESP_FAIL;
    card.mounted = 1;
    *out = &card;
    return ESP_OK;
}
void m5_sd_unmount(sdmmc_card_t *c)
{
    assert(c == &card && card.mounted);
    card.mounted = 0;
    unmounts++;
}
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    assert(caps == 1);
    if (fail_alloc) return NULL;
    void *ptr = malloc(size);
    if (ptr) allocations++;
    return ptr;
}
void heap_caps_free(void *ptr) { if (ptr) allocations--; free(ptr); }

static void write_rom(size_t size, uint32_t ssp, uint32_t pc)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    unsigned char vectors[8];
    wb32(vectors, ssp); wb32(vectors + 4, pc);
    assert(fwrite(vectors, 1, 8, f) == 8);
    for (size_t i = 8; i < size; i++) assert(fputc(0xff, f) != EOF);
    assert(fclose(f) == 0);
}

static void expect(rom_load_result_t result)
{
    uint8_t *data = (uint8_t *)1;
    size_t size = 42;
    int previous_unmounts = unmounts;
    assert(rom_load_from_sd(&data, &size) == result);
    assert(unmounts == previous_unmounts + !no_sd);
    assert(!card.mounted);
    if (result == ROM_LOAD_OK) {
        assert(data && size == 0x40000 && allocations == 1);
        assert(be32(data) == BUS_RAM_SIZE);
        assert(be32(data + 4) == BUS_ROM_BASE + 8);
        heap_caps_free(data);
    } else {
        assert(!data && !size);
    }
    assert(!allocations);
}

int main(void)
{
#ifdef _WIN32
    _mkdir(TEST_SD_DIR);
#else
    mkdir(TEST_SD_DIR, 0700);
#endif
    remove(path);
    no_sd = 1; expect(ROM_LOAD_NO_SD);
    no_sd = 0; expect(ROM_LOAD_NO_FILE);
    write_rom(8, BUS_RAM_SIZE, BUS_ROM_BASE + 8); expect(ROM_LOAD_BAD_ROM);
    write_rom(BUS_ROM_MAX_SIZE + 1, BUS_RAM_SIZE, BUS_ROM_BASE + 8); expect(ROM_LOAD_BAD_ROM);
    write_rom(0x40000, 0, BUS_ROM_BASE + 8); expect(ROM_LOAD_BAD_ROM);
    write_rom(0x40000, BUS_RAM_SIZE + 1, BUS_ROM_BASE + 8); expect(ROM_LOAD_BAD_ROM);
    write_rom(0x40000, BUS_RAM_SIZE, BUS_ROM_BASE - 1); expect(ROM_LOAD_BAD_ROM);
    write_rom(0x40000, BUS_RAM_SIZE, BUS_ROM_BASE + 0x40000); expect(ROM_LOAD_BAD_ROM);
    write_rom(0x40000, BUS_RAM_SIZE, BUS_ROM_BASE + 8);
    fail_alloc = 1; expect(ROM_LOAD_BAD_ROM);
    fail_alloc = 0; expect(ROM_LOAD_OK);
    expect(ROM_LOAD_OK); /* remount after success */
    remove(path);
    assert(mounts == 11 && unmounts == 10);
    puts("ALL ROM LOADER TESTS PASSED");
    return 0;
}
