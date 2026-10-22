#pragma once
#include <stdbool.h>
/* The Cybiko Wireless Internet Gate (CyWIG) on the board, for the Xtreme's
 * own WAP browser. The gate itself is in the cybiko-browser project
 * (cy_gate.h): one more Cybiko on the emulated radio that fetches pages
 * over the board's Wi-Fi (the networks set up in the Browser) and hands
 * them to the WAP browser as WML. It runs in a task of its own and talks
 * to the board's emulated Cybiko only (not over ESP-NOW). Keys in
 * /Cybiko/cybiko.cfg:
 *   wap.gate = on      # default; "off": no gate
 *   wap.home = URL     # the page the WAP browser's Home opens (default:
 *                      # the gate's own, with a search box and a few sites) */
#include <stddef.h>
#include <stdint.h>
#include "cybiko.h"

/* After cybiko_link_init (the gate takes an ID next to the machine's). */
void cybiko_gate_init(cybiko_t *m, const char *cfg);
/* The machine's rf_send hook, with cybiko_link_send (CPU task). */
void cybiko_gate_send(void *user, const uint8_t *pkt, size_t len, uint8_t channel);
/* CPU task: hand the gate's packets to the machine. */
void cybiko_gate_poll(cybiko_t *m);
/* Stop using the network (the web browser needs it) or go on again. */
void cybiko_gate_pause(bool pause);
