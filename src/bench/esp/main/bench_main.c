/* m68k emulator benchmark app. Runs the bundled 68000 workload with the pure
 * interpreter and then with the JIT (when the target has one), several times
 * each, and prints emulated MHz against the Palm IIIx's 16.58 MHz. Every run is
 * checked against the workload's known checksum. */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "m68k.h"
#include "bench_core.h"

#define RUNS      3
#define REAL_MHZ  16.58

static void run_mode(const char *name, int accel)
{
    g_m68k_accel_mode = accel;
    for (int i = 0; i < RUNS; i++) {
        int64_t t0 = esp_timer_get_time();
        bench_result_t r = bench_run(400000000ull);
        int64_t us = esp_timer_get_time() - t0;
        double mhz = us > 0 ? (double)r.cycles / (double)us : 0;
        bool ok = r.finished && r.checksum == BENCH_EXPECTED_CHECKSUM;
        printf("BENCH %s run %d: %" PRIu64 " cycles in %" PRId64 " us = %.2f MHz (%.0f%% of real) checksum %08" PRIX32 " %s\n",
               name, i, r.cycles, us, mhz, mhz / REAL_MHZ * 100.0, r.checksum, ok ? "OK" : "MISMATCH");
        vTaskDelay(1);
    }
}

static void bench_task(void *arg)
{
    (void)arg;
    printf("BENCH internal free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    run_mode("interp", 0);
    m68k_jit_init();
    run_mode("jit", 1);
    printf("BENCH done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* Same placement as the firmware: the emulated CPU runs alone on core 1, the
     * JIT's async compiler on core 0. */
    xTaskCreatePinnedToCore(bench_task, "bench", 16384, NULL, 5, NULL, 1);
}
