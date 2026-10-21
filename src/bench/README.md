# 68k emulator benchmark

A fixed 68000 workload plus two runners, for measuring the speed of the m68k
core (interpreter and JIT) before and after a change.

- `workload/bench.c` is a Palm-OS-flavoured program (MemMove/MemSet, strings,
  record sort, chunk-list walk, CRC, 1bpp blits, MULU/DIVU, recursive calls,
  a switch-dispatched event loop) built for a plain 68000 with no libgcc. It
  is linked at the Palm ROM address, so the JIT treats it as ROM code, and it
  ends with its checksum in D0 and a `STOP`. The checksum `0x6339CF9A` was
  cross-checked against the Musashi 68000 core.
- `bench_rom.h` is the built image, committed so no 68k toolchain is needed.
  Rebuild it with `workload/build.sh` (needs `gcc-m68k-linux-gnu`) and update
  `BENCH_EXPECTED_CHECKSUM` in `bench_core.h` if the workload changes.
- `bench_core.c` boots the image on the real `bus` and `m68k` code.

## Host

```sh
cmake -S src/host -B build-host && cmake --build build-host --target bench_m68k
./build-host/bench_m68k 5
```

It is also a ctest (`bench_m68k`), as a correctness check of the interpreter.
Host MHz figures say nothing about the ESP32.

## On the board

```sh
idf.py -C src/bench/esp -B build-bench build flash monitor
```

This is a separate app (it does not touch the Palm firmware's settings or SD
card). It prints three interpreter runs and three JIT runs:

```
BENCH interp run 2: 50037366 cycles in ... us = ... MHz (...% of real) checksum 6339CF9A OK
BENCH jit run 2: ...
```

"% of real" is relative to the Palm IIIx's 16.58 MHz. JIT runs report slightly
more cycles than interpreter runs because the JIT's cycle accounting is coarser.

## Under QEMU

Espressif's QEMU (`idf_tools.py install qemu-xtensa`) runs the same app. Build
with `-DBENCH_QEMU=1`: QEMU does not alias the S3's IRAM and DRAM windows, so
the JIT must write its code cache through the instruction address there. Run
QEMU with `-icount shift=0`; each guest instruction then takes 1 ns of virtual
time, so the reported microseconds are thousands of Xtensa instructions
executed. That is a proxy for speed: QEMU does not model the PSRAM, caches or
branch penalties, so check the real figures on the board.
