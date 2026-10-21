#pragma once
/* Shared driver for the 68k emulator benchmark (host harness and the ESP-IDF
 * bench app). Boots the bundled workload image (bench_rom.h) as if it were the
 * Palm ROM and runs it to completion in m68k_execute batches. */
#include <stdint.h>
#include <stdbool.h>

/* Checksum bench.c produces on a correct 68000. Update together with
 * bench_rom.h whenever the workload changes. */
#define BENCH_EXPECTED_CHECKSUM 0x6339CF9Au

typedef struct {
    uint32_t checksum;      /* D0 at the final STOP */
    uint64_t cycles;        /* emulated 68k cycles to finish */
    bool     finished;      /* reached the final STOP before the cycle cap */
} bench_result_t;

/* Run the workload once from a fresh reset. The caller times the call. */
bench_result_t bench_run(uint64_t max_cycles);
