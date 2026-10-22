#!/usr/bin/env bash
# Build the differential tester. GDB_SRC is an unpacked gdb source tree and
# GDB_BUILD a build of it configured with --target=h8300-elf --enable-sim
# (make all-sim). See README.md.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
: "${GDB_SRC:?set GDB_SRC to the gdb source tree}"
: "${GDB_BUILD:?set GDB_BUILD to the h8300-elf sim build}"
out="${1:-$here/h8s_diff}"
gcc -O2 -g -DH8S_TEST_HOOKS -DPACKAGE='"h8s_diff"' \
    -I"$here/../../src/components/h8s/include" -I"$GDB_SRC/include" \
    -I"$GDB_BUILD/bfd" -I"$GDB_SRC/bfd" \
    "$here/h8s_diff.c" "$here/../../src/components/h8s/h8s.c" \
    "$GDB_BUILD/sim/h8300/libsim.a" "$GDB_BUILD/bfd/.libs/libbfd.a" \
    "$GDB_BUILD/opcodes/.libs/libopcodes.a" "$GDB_BUILD/libiberty/libiberty.a" \
    "$GDB_BUILD/gnulib/import/libgnu.a" -lz -ltermcap -lm -o "$out"
