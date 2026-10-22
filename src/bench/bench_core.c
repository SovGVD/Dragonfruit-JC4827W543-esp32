#include "bench_core.h"
#include "bench_rom.h"
#include "m68k.h"
#include "bus.h"
#include <string.h>

static m68k_cpu_t s_cpu;
static bool       s_bus_ready;

bench_result_t bench_run(uint64_t max_cycles)
{
    bench_result_t r = { 0, 0, false };
    if (!s_bus_ready) {
        bus_init(bench_rom, sizeof bench_rom);
        s_bus_ready = true;
    }
    memset(bus_ram_ptr(), 0, BUS_RAM_SIZE);
    memcpy(bus_ram_ptr(), bench_rom, 8);      /* reset SSP/PC, as bus_init does */

    m68k_cpu_t *cpu = &s_cpu;
    m68k_init(cpu);
    m68k_reset(cpu);
    while (r.cycles < max_cycles) {
        m68k_execute(cpu, 4000);
        r.cycles += (uint32_t)cpu->regs.cycles_executed;
        if (cpu->regs.stopped) { r.finished = true; break; }
    }
    r.checksum = cpu->regs.d[0];
    return r;
}
