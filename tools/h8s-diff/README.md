# H8S core differential tester

Compares `src/components/h8s` with the GNU H8S simulator (`sim/h8300` from
binutils-gdb) one random instruction at a time: random registers, CCR and
memory, one step on each, then registers, PC, CCR and every byte written.
Opcode patterns come from MAME's `h8.lst` (BSD-3-Clause) in `patterns.h`.

The sim is not a perfect reference. The tester leaves out what it gets wrong,
and `src/host/test_h8s.c` covers those cases against the manual instead:

- the half-carry flag (the sim never computes H)
- V for INC/DEC, NEG, SUBX and SHAL #2, Z for ADDX/SUBX, and DAA/DAS carries
- `@aa:8` and 0xff0000+ accesses (the sim models on-chip I/O there)
- odd word addresses (the real bus drops bit 0, the sim does not)
- RTE, LDM/STM, LDC/STC.W, EEPMOV and DIVXU/DIVXS (sim frame format and
  decoding differ, or the result is undefined on overflow)

## Build

```sh
apt-get source gdb                 # or any gdb release tarball
mkdir build && cd build
../gdb-*/configure --target=h8300-elf --disable-gdb --disable-gdbserver \
    --disable-ld --disable-gas --disable-binutils --disable-gprof \
    --disable-nls --disable-werror --enable-sim
make all-sim
GDB_SRC=$PWD/../gdb-15.1 GDB_BUILD=$PWD /path/to/tools/h8s-diff/build.sh
./h8s_diff 300000 1                # cases, seed [, pattern substring]
```

On 2026-10-21 four seeds of 300,000 cases each (about 230,000 compared per
seed) ran with no mismatches.
