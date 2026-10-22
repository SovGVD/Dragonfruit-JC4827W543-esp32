#pragma once
#include <stdint.h>
#include "cybiko.h"

/* The Cybiko radio between boards, over ESP-NOW.
 *
 * The emulated radio co-processor (cy_rf.c) hands over each packet CyOS
 * sends; this broadcasts it to every board on the same Wi-Fi channel and
 * passes the packets other boards send back in, so CyOS on each board sees
 * the others (chat, the people nearby counter and so on). Each board also
 * gets its own Cybiko ID, made from its MAC address, since CyOS ignores
 * packets that carry its own ID and every copy of a flash dump has the same
 * one. Keys in /Cybiko/cybiko.cfg:
 *   rf = espnow        # default; "off" switches the radio link off
 *   rf.channel = 1     # Wi-Fi channel, the same on every board (1-13)
 *   rf.id = auto       # auto (from the MAC), keep (the dump's ID) or 8 hex
 *                      # digits */
/* cfg is the text of /Cybiko/cybiko.cfg, or NULL when there is none. Call
 * after cybiko_init and before the machine runs. */
void cybiko_link_init(cybiko_t *m, const char *cfg);
/* The machine's rf_send hook (CPU task). */
void cybiko_link_send(void *user, const uint8_t *pkt, size_t len, uint8_t channel);
/* CPU task: hand packets heard from other boards to the machine. */
void cybiko_link_poll(cybiko_t *m);
/* Back on the radio's own channel after the web browser used Wi-Fi. */
void cybiko_link_resume(void);
