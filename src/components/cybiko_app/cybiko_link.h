#pragma once
#include <stdint.h>
#include "cybiko.h"

/* Radio link between boards over ESP-NOW (experimental, off by default).
 *
 * The real Cybiko talks to its radio through a co-processor on a serial
 * port, using a protocol that is not documented yet. Until it is, this link
 * only carries the raw bytes of one SCI channel between boards, which is
 * enough to study the traffic but not to make CyOS chat work. Enable it in
 * /Cybiko/cybiko.cfg:
 *   rf = espnow        # default: off
 *   rf.port = 0        # SCI channel to carry (0-1; 2 is the debug console)
 *   rf.channel = 1     # Wi-Fi channel, the same on every board */
void cybiko_link_init(void);
/* CPU task: a byte the emulated CPU sent on SCI `port`. */
void cybiko_link_serial_tx(int port, uint8_t b);
/* CPU task: send what is buffered and hand received bytes to the machine. */
void cybiko_link_poll(cybiko_t *m);
