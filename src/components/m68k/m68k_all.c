/* Unity (single-translation-unit) build of the 68k interpreter.
 *
 * Why: the per-instruction hot path threads through several small functions
 * that live in separate .c files — m68k_resolve_ea / m68k_read_ea /
 * m68k_write_ea (m68k_ea.c) and the m68k_set_flags_* helpers (m68k.c) — each
 * called once or more per executed instruction from every opcode group. With
 * the files compiled separately the compiler cannot see across them, so every
 * one of those is an out-of-line call. On the ESP32's Xtensa windowed-register
 * ABI a call is not free (ENTRY/RETW + window spill under pressure), and at
 * ~16M instructions/sec that fixed tax dominates. Compiling the whole core as
 * one TU lets -O2 inline the EA/flag chain into the handlers and the handlers
 * toward the dispatch, collapsing the call depth.
 *
 * This is a build-structure change only — no behavior change. The individual
 * m68k_*.c files are intentionally NOT listed in CMakeLists.txt; they are
 * pulled in here. Each is independently compilable and uses unique file-scope
 * symbol names, so aggregating them into one TU is safe. (Verified: no static
 * function or file-scope variable name collides across the set.)
 *
 * If this ever needs to be reverted, restore the per-file SRCS list in
 * components/m68k/CMakeLists.txt and delete this file.
 */

#include "m68k.c"
#include "m68k_ea.c"
#include "m68k_alu.c"
#include "m68k_move.c"
#include "m68k_branch.c"
#include "m68k_shift.c"
#include "m68k_bcd.c"
#include "m68k_system.c"
#include "m68k_decode.c"
#include "m68k_palm_hle.c"
