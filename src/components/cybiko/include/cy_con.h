/* A PC talking to CyOS 1.2's serial console on a Cybiko Classic, the way
 * Cybiko's CyberLoad and AutoUpdate did: commands ("ver", "boot X",
 * "rm -f X", "reboot") and files ("rcv NAME SIZE", sent in 512-byte blocks).
 *
 * The protocol is ReBiko's, worked out from CyOS 1.2.46's code (ReBiko,
 * cybiko-emu/rebiko_apps.c, MIT, Copyright 2026 Dan Berkowitz):
 *  - a line from the PC is its text, "\n", then a 16-bit sum, low byte first:
 *    per character sum = ((sum ^ c) << 1) | bit 16 of sum, from 0. CyOS
 *    answers 0x02 (taken) or 0x01 (bad sum) and echoes ">> line".
 *  - "rcv NAME SIZE": CyOS asks for block N with "sendN" on its own line; a
 *    block is 'C', N and the length (16 bits each, low byte first), up to 512
 *    bytes, then zlib's CRC-32 of N, the length and the data. "receive
 *    successfull" ends it, "cancel" or "* abort" fails it.
 *    CyOS waits about 300 ms for a block and then asks again; what is left of
 *    the late copy is dropped then (drop), or CyOS reads it as the start of
 *    the next one.
 *  - every command ends with the prompt "Ready >" (except "reboot"). */
#ifndef CY_CON_H
#define CY_CON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { CY_CON_IDLE, CY_CON_BUSY, CY_CON_DONE, CY_CON_FAILED } cy_con_state_t;

typedef struct {
    int (*put)(void *ctx, const uint8_t *data, int n);   /* bytes to CyOS; how many fit */
    void (*drop)(void *ctx);   /* forget bytes put but not yet taken (may be NULL) */
    void *ctx;
    cy_con_state_t state;
    uint8_t op, step, tries;
    uint32_t idle_ms;
    char line[96];
    const uint8_t *data;
    uint32_t size, blocks_sent;
    uint8_t pending[8 + 512 + 4];
    int pending_n, pending_off;
    char rx[2048];
    int rx_n;
    char result[1024];
} cy_con_t;

void cy_con_init(cy_con_t *c, int (*put)(void *, const uint8_t *, int), void *ctx);
void cy_con_rx(cy_con_t *c, uint8_t byte);             /* each byte CyOS sends */
void cy_con_poll(cy_con_t *c, uint32_t elapsed_ms);    /* every few ms */
bool cy_con_command(cy_con_t *c, const char *line);
bool cy_con_send_file(cy_con_t *c, const char *name, const uint8_t *data, uint32_t size);

#endif
