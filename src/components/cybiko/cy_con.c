/* CyOS 1.2's serial console from the PC side; see cy_con.h. Follows
 * ReBiko's rb_con (cybiko-emu/rebiko_apps.c, MIT, Copyright 2026 Dan
 * Berkowitz). */
#include "cy_con.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_CMD = 1, OP_PUT, OP_REBOOT };
enum { STEP_SEND, STEP_ACK, STEP_RUN, STEP_PROMPT };
#define ACK_MS 3000
#define IDLE_MS 20000

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int frame(const char *text, uint8_t *out, int cap)
{
    uint32_t sum = 0;
    int n = 0;
    for (const char *p = text; *p && n < cap - 3; p++) {
        uint8_t ch = (uint8_t)*p;
        sum = ((sum ^ ch) << 1) | ((sum >> 16) & 1);
        out[n++] = ch;
    }
    out[n++] = '\n';
    out[n++] = (uint8_t)sum;
    out[n++] = (uint8_t)(sum >> 8);
    return n;
}

void cy_con_init(cy_con_t *c, int (*put)(void *, const uint8_t *, int), void *ctx)
{
    memset(c, 0, sizeof *c);
    c->put = put;
    c->ctx = ctx;
}

static void flush(cy_con_t *c)
{
    if (c->pending_off < c->pending_n)
        c->pending_off += c->put(c->ctx, c->pending + c->pending_off, c->pending_n - c->pending_off);
}

static void finish(cy_con_t *c, cy_con_state_t st, const char *msg)
{
    c->state = st;
    if (msg) snprintf(c->result, sizeof c->result, "%s", msg);
}

static bool start(cy_con_t *c, int op, const char *line)
{
    if (c->state == CY_CON_BUSY) return false;
    c->state = CY_CON_BUSY;
    c->op = (uint8_t)op;
    c->step = STEP_SEND;
    c->tries = 0;
    c->idle_ms = 0;
    c->rx_n = 0;
    c->rx[0] = 0;
    c->result[0] = 0;
    c->blocks_sent = 0;
    snprintf(c->line, sizeof c->line, "%s", line);
    return true;
}

bool cy_con_command(cy_con_t *c, const char *line)
{
    return start(c, strcmp(line, "reboot") ? OP_CMD : OP_REBOOT, line);
}

bool cy_con_send_file(cy_con_t *c, const char *name, const uint8_t *data, uint32_t size)
{
    char line[96];
    snprintf(line, sizeof line, "rcv %s %lu", name, (unsigned long)size);
    if (!start(c, OP_PUT, line)) return false;
    c->data = data;
    c->size = size;
    return true;
}

void cy_con_rx(cy_con_t *c, uint8_t byte)
{
    if (c->state != CY_CON_BUSY) return;
    c->idle_ms = 0;
    if (c->step == STEP_ACK && (byte == 0x01 || byte == 0x02)) {
        if (byte == 0x02) {
            c->step = STEP_RUN;
            c->rx_n = 0;
            c->rx[0] = 0;
        } else {
            c->step = STEP_SEND;                          /* bad sum: again */
        }
        return;
    }
    if (byte == '\r' || c->step == STEP_ACK) return;
    if (c->rx_n >= (int)sizeof c->rx - 1) {              /* keep the newest half */
        memmove(c->rx, c->rx + sizeof c->rx / 2, sizeof c->rx / 2);
        c->rx_n -= (int)sizeof c->rx / 2;
    }
    c->rx[c->rx_n++] = (char)byte;
    c->rx[c->rx_n] = 0;
}

/* "sendN" at the start of a line (not inside the echo of the rcv line) */
static char *find_send(char *rx)
{
    for (char *m = strstr(rx, "send"); m; m = strstr(m + 1, "send"))
        if ((m == rx || m[-1] == '\n') && m[4] >= '0' && m[4] <= '9') return m;
    return NULL;
}

static void send_block(cy_con_t *c, uint32_t n)
{
    uint32_t off = n * 512, len = off < c->size ? c->size - off : 0;
    if (len > 512) len = 512;
    uint8_t *b = c->pending;
    b[0] = 'C';
    b[1] = (uint8_t)n; b[2] = (uint8_t)(n >> 8);
    b[3] = (uint8_t)len; b[4] = (uint8_t)(len >> 8);
    memcpy(b + 5, c->data + off, len);
    uint32_t crc = crc32(0, b + 1, 4 + len);
    b[5 + len] = (uint8_t)crc; b[6 + len] = (uint8_t)(crc >> 8);
    b[7 + len] = (uint8_t)(crc >> 16); b[8 + len] = (uint8_t)(crc >> 24);
    c->pending_n = (int)(9 + len);
    c->pending_off = 0;
    if (n + 1 > c->blocks_sent) c->blocks_sent = n + 1;
}

void cy_con_poll(cy_con_t *c, uint32_t elapsed_ms)
{
    if (c->state != CY_CON_BUSY) return;
    flush(c);
    c->idle_ms += elapsed_ms;
    if (c->step == STEP_SEND) {
        if (++c->tries > 3) { finish(c, CY_CON_FAILED, "CyOS did not take the command"); return; }
        c->step = STEP_ACK;
        c->idle_ms = 0;
        c->pending_n = frame(c->line, c->pending, (int)sizeof c->pending);
        c->pending_off = 0;
        flush(c);
        return;
    }
    if (c->step == STEP_ACK) {
        if (c->idle_ms > ACK_MS) c->step = STEP_SEND;
        return;
    }
    if (c->op == OP_REBOOT) { finish(c, CY_CON_DONE, "rebooting"); return; }
    char *m;
    if (c->step == STEP_RUN && c->op == OP_PUT) {
        while ((m = find_send(c->rx)) != NULL) {
            char *nl = strchr(m, '\n');
            if (!nl) break;
            uint32_t n = (uint32_t)strtoul(m + 4, NULL, 10);
            int k = (int)(nl + 1 - c->rx);
            memmove(c->rx, nl + 1, (size_t)(c->rx_n - k + 1));
            c->rx_n -= k;
            if (n * 512 > c->size && !(n == 0 && c->size == 0)) {
                finish(c, CY_CON_FAILED, "CyOS asked for a block past the end");
                return;
            }
            if (c->drop) c->drop(c->ctx);
            send_block(c, n);
            flush(c);
        }
        if (strstr(c->rx, "receive successfull")) c->step = STEP_PROMPT;
        else if (strstr(c->rx, "cancel\n") || strstr(c->rx, "* abort")) {
            finish(c, CY_CON_FAILED, "CyOS cancelled: no room, or a bad name");
            return;
        }
    }
    if ((c->step == STEP_RUN && c->op == OP_CMD) || c->step == STEP_PROMPT) {
        if ((m = strstr(c->rx, "Ready >")) != NULL) {
            *m = 0;
            const char *out = c->rx, *echo = strstr(c->rx, ">> ");
            if (echo) {
                const char *nl = strchr(echo, '\n');
                out = nl ? nl + 1 : echo + strlen(echo);
            }
            while (*out == '\n') out++;
            size_t k = strlen(out);
            if (k > sizeof c->result - 1) k = sizeof c->result - 1;
            memcpy(c->result, out, k);
            c->result[k] = 0;
            while (k && c->result[k - 1] == '\n') c->result[--k] = 0;
            finish(c, CY_CON_DONE, NULL);
            return;
        }
    }
    if (c->idle_ms > IDLE_MS) finish(c, CY_CON_FAILED, "CyOS stopped answering");
}
