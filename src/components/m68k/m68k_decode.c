#include "m68k.h"

/* The 16-way group dispatch is now a direct switch in m68k_execute (m68k.c),
 * not an indirect call through a function-pointer table — that lets the hot
 * handlers inline into the loop (they can't if their address is taken here).
 * The old m68k_group_handlers[] table and its populator are gone; this stub
 * keeps the m68k_init() call site (and the m68k.h prototype) valid. */
void m68k_init_decode_table(void)
{
}
