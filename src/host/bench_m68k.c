/* Host benchmark for the 68k interpreter: runs the bundled Palm-like workload
 * (src/bench) N times and reports emulated MHz and the checksum. The absolute
 * speed is the host's, not the ESP32's; use it to check correctness and to
 * compare interpreter changes. The firmware-side equivalent is src/bench/esp.
 *
 * Usage: bench_m68k [runs]   (default 5) */
#include "bench_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 5;
    double best = 0;
    int bad = 0;
    for (int i = 0; i < runs; i++) {
        double t0 = now_s();
        bench_result_t r = bench_run(2000000000ull);
        double dt = now_s() - t0;
        double mhz = r.cycles / dt / 1e6;
        if (mhz > best) best = mhz;
        bool ok = r.finished && r.checksum == BENCH_EXPECTED_CHECKSUM;
        if (!ok) bad++;
        printf("run %d: %llu cycles in %.3f s = %.1f MHz, checksum %08lX %s\n", i,
               (unsigned long long)r.cycles, dt, mhz, (unsigned long)r.checksum,
               ok ? "OK" : "MISMATCH");
    }
    printf("best: %.1f emulated MHz\n", best);
    return bad ? 1 : 0;
}
