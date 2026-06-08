#include "bus.h"
#include "m68k.h"
#include "dragonball.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "bus";

/* Published to the inline fast path in bus.h. */
uint8_t       *g_bus_ram   = NULL;
const uint8_t *g_bus_rom   = NULL;
uint32_t       g_bus_rom_end = BUS_ROM_LO;   /* BUS_ROM_LO + rom_size */
uint32_t       g_bus_periph_seq = 0;         /* bumped on each peripheral access */

static size_t rom_size;

volatile int dispatch_table_populated = 0;

void bus_on_dispatch_table_set(uint32_t table_base)
{
    if (!g_bus_ram) return;
    static int call_count = 0;
    call_count++;
    if (call_count <= 2)
        ESP_LOGI(TAG, "Dispatch table base set to %06lX", (unsigned long)table_base);
    dispatch_table_populated = call_count;
}

void bus_init(const uint8_t *rom_data, size_t rom_sz)
{
    g_bus_rom = rom_data;
    rom_size = rom_sz;
    g_bus_rom_end = BUS_ROM_LO + (uint32_t)rom_sz;

    g_bus_ram = heap_caps_calloc(1, BUS_RAM_SIZE, MALLOC_CAP_SPIRAM);
    if (!g_bus_ram) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for RAM", BUS_RAM_SIZE);
        return;
    }
    ESP_LOGI(TAG, "Emulated RAM: %u KB at %p", BUS_RAM_SIZE / 1024, g_bus_ram);

    /*
     * Copy reset vectors from ROM into RAM address 0.
     * At power-on the MC68EZ328 maps ROM across the whole address space
     * so the CPU reads SSP from $000000 and PC from $000004, both hitting ROM.
     * Rather than emulating the full chip-select remap, we pre-populate the
     * 8-byte reset vector area in RAM from the ROM image.
     */
    if (g_bus_rom && rom_sz >= 8)
        memcpy(g_bus_ram, g_bus_rom, 8);
}

uint8_t *bus_ram_ptr(void) { return g_bus_ram; }

/* --- Slow path: peripherals + the MC68EZ328 "ROM mapped across the whole
 * address space" default (Palm OS local IDs are direct addresses that rely on
 * it).  Reached only when the address is neither RAM nor inside the ROM
 * window. RAM/ROM are handled inline in bus.h. --- */

uint8_t bus_read8_slow(uint32_t addr)
{
    if (addr >= BUS_PERIPH_BASE) {
        g_bus_periph_seq++;
        return dragonball_read8(addr - BUS_PERIPH_BASE);
    }
    if (g_bus_rom && rom_size > 0)
        return g_bus_rom[addr % rom_size];
    return 0xFF;
}

uint16_t bus_read16_slow(uint32_t addr)
{
    if (addr >= BUS_PERIPH_BASE) {
        g_bus_periph_seq++;
        return dragonball_read16(addr - BUS_PERIPH_BASE);
    }
    if (g_bus_rom && rom_size > 0) {
        const uint8_t *p = g_bus_rom + (addr % rom_size);
        return (uint16_t)((p[0] << 8) | p[1]);
    }
    return 0xFFFF;
}

uint32_t bus_read32_slow(uint32_t addr)
{
    if (addr >= BUS_PERIPH_BASE) {
        g_bus_periph_seq++;
        return dragonball_read32(addr - BUS_PERIPH_BASE);
    }
    if (g_bus_rom && rom_size > 0) {
        const uint8_t *p = g_bus_rom + (addr % rom_size);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    return 0xFFFFFFFF;
}

void bus_write8_slow(uint32_t addr, uint8_t val)
{
    if (addr >= BUS_PERIPH_BASE) {
        g_bus_periph_seq++;
        dragonball_write8(addr - BUS_PERIPH_BASE, val);
    }
}

void bus_write16_slow(uint32_t addr, uint16_t val)
{
    if (addr >= BUS_PERIPH_BASE) {
        g_bus_periph_seq++;
        dragonball_write16(addr - BUS_PERIPH_BASE, val);
    }
}

void bus_write32_slow(uint32_t addr, uint32_t val)
{
    if (addr >= BUS_PERIPH_BASE) {
        g_bus_periph_seq++;
        dragonball_write32(addr - BUS_PERIPH_BASE, val);
    }
}
