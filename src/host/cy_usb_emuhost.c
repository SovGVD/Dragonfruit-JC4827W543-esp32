/* A USB host controller for the emulated Cybiko: the PC side of the
 * Xtreme's USB link (cybiko-browser's usb/cyusb.h) runs in its own thread
 * and makes blocking transfers; the emulator's thread runs one
 * transaction of it at each 1 ms frame (cy_usbhc_tick). Like a PC, it
 * enumerates the device first (cy_usbhc_enumerate), and like Windows it
 * also asks for Microsoft OS descriptors and logs them. When the device
 * drops off the bus (its pull-up off, MCNTRL.NAT) that is noted
 * (cy_usbhc_detached) and the client enumerates it again. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_usb_emuhost.h"

enum { T_SETUP, T_OUT, T_IN, T_RESET, T_WAIT };

struct cy_usbhc {
    cybiko_t *m;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool pending, done, stopped, parked;
    bool was_attached, detached;
    int kind, ep, len, max, toggle;
    uint8_t data[64];
    int res, rlen, rtoggle;
    uint64_t ms;
    int addr;
    uint8_t tog_out[16], tog_in[16];
    bool verbose;
    cyusb_dev_t dev;
};

static void logf_(cy_usbhc_t *h, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[usb %7.3f] %s\n", (double)h->ms / 1000, buf);
}

cy_usbhc_t *cy_usbhc_new(cybiko_t *m, bool verbose)
{
    cy_usbhc_t *h = calloc(1, sizeof *h);
    h->m = m;
    h->verbose = verbose;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    return h;
}

void cy_usbhc_tick(cy_usbhc_t *h)
{
    pthread_mutex_lock(&h->mu);
    /* Lockstep: every frame waits for the client's next transaction (or
     * a wait), so emulated time runs at the client's pace. */
    while (!h->pending && !h->parked && !h->stopped) pthread_cond_wait(&h->cv, &h->mu);
    h->ms++;
    cybiko_usb_frame(h->m);
    bool att = cybiko_usb_attached(h->m);
    if (h->was_attached && !att) {
        h->detached = true;
        logf_(h, "the device left the bus");
    }
    h->was_attached = att;
    if (h->pending) {
        h->rlen = 0;
        h->rtoggle = 0;
        switch (h->kind) {
        case T_SETUP: h->res = cybiko_usb_setup(h->m, h->addr, h->data); break;
        case T_OUT: h->res = cybiko_usb_out(h->m, h->addr, h->ep, h->data, h->len, h->toggle); break;
        case T_IN: h->res = cybiko_usb_in(h->m, h->addr, h->ep, h->data, h->max, &h->rlen, &h->rtoggle); break;
        case T_RESET: cybiko_usb_bus_reset(h->m); h->res = CY_USB_ACK; break;
        default: h->res = CY_USB_ACK; break;
        }
        h->pending = false;
        h->done = true;
        pthread_cond_broadcast(&h->cv);
    }
    pthread_mutex_unlock(&h->mu);
}

void cy_usbhc_stop(cy_usbhc_t *h)
{
    pthread_mutex_lock(&h->mu);
    h->stopped = true;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

uint64_t cy_usbhc_ms(cy_usbhc_t *h) { return h->ms; }

bool cy_usbhc_detached(cy_usbhc_t *h)
{
    pthread_mutex_lock(&h->mu);
    bool d = h->detached;
    pthread_mutex_unlock(&h->mu);
    return d;
}

void cy_usbhc_park(cy_usbhc_t *h, bool parked)
{
    pthread_mutex_lock(&h->mu);
    h->parked = parked;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

/* One transaction at the next frame. */
static int xact(cy_usbhc_t *h, int kind, int ep, uint8_t *data, int len, int toggle, int *rlen, int *rtoggle)
{
    pthread_mutex_lock(&h->mu);
    if (h->stopped) { pthread_mutex_unlock(&h->mu); return -1; }
    h->kind = kind;
    h->ep = ep;
    h->len = len;
    h->max = 64;
    h->toggle = toggle;
    if (kind != T_IN && data) memcpy(h->data, data, (size_t)len);
    h->pending = true;
    h->done = false;
    pthread_cond_broadcast(&h->cv);
    while (!h->done && !h->stopped) pthread_cond_wait(&h->cv, &h->mu);
    int res = h->done ? h->res : -1;
    if (kind == T_IN && h->done) {
        memcpy(data, h->data, (size_t)h->rlen);
        if (rlen) *rlen = h->rlen;
        if (rtoggle) *rtoggle = h->rtoggle;
    }
    h->done = false;
    pthread_mutex_unlock(&h->mu);
    return res;
}

void cy_usbhc_wait_ms(cy_usbhc_t *h, unsigned ms)
{
    for (unsigned i = 0; i < ms; i++)
        if (xact(h, T_WAIT, 0, NULL, 0, 0, NULL, NULL) < 0) return;
}

static const char *res_name(int r)
{
    switch (r) {
    case CY_USB_ACK: return "ACK";
    case CY_USB_NAK: return "NAK";
    case CY_USB_STALL: return "STALL";
    case CY_USB_TIMEOUT: return "no answer";
    }
    return "stopped";
}

/* Retries NAKs (and silence) until timeout_ms frames have passed. */
static int retry(cy_usbhc_t *h, int kind, int ep, uint8_t *data, int len, int toggle,
                 int *rlen, int *rtoggle, unsigned timeout_ms)
{
    uint64_t until = h->ms + timeout_ms;
    for (;;) {
        int r = xact(h, kind, ep, data, len, toggle, rlen, rtoggle);
        if (r == CY_USB_ACK || r == CY_USB_STALL || r < 0) return r;
        if (h->ms >= until) return r;
    }
}

static int emu_control(cyusb_dev_t *d, uint8_t type, uint8_t req, uint16_t value,
                       uint16_t index, uint8_t *data, uint16_t len, unsigned timeout_ms)
{
    cy_usbhc_t *h = d->ctx;
    uint8_t s[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
                     (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    int r = retry(h, T_SETUP, 0, s, 8, 0, NULL, NULL, timeout_ms);
    if (r != CY_USB_ACK) {
        logf_(h, "SETUP %02x %02x: %s", type, req, res_name(r));
        return r == CY_USB_STALL ? CYUSB_ESTALL : CYUSB_ETIMEOUT;
    }
    int done = 0, tog = 1;
    bool in = type & 0x80;
    while (done < len) {
        int n = len - done < 8 ? len - done : 8, got = 0, rt = 0;
        if (in) {
            r = retry(h, T_IN, 0, data + done, 8, 0, &got, &rt, timeout_ms);
            if (r != CY_USB_ACK) break;
            if (rt != tog) logf_(h, "control IN: DATA%d, expected DATA%d", rt, tog);
            done += got;
            tog ^= 1;
            if (got < 8) break;
        } else {
            r = retry(h, T_OUT, 0, data + done, n, tog, NULL, NULL, timeout_ms);
            if (r != CY_USB_ACK) break;
            done += n;
            tog ^= 1;
        }
    }
    if (r != CY_USB_ACK) {
        logf_(h, "control %02x %02x data stage: %s", type, req, res_name(r));
        return r == CY_USB_STALL ? CYUSB_ESTALL : CYUSB_ETIMEOUT;
    }
    /* status stage: a zero-length packet the other way, DATA1 */
    int got = 0, rt = 0;
    uint8_t zl[8];
    r = in ? retry(h, T_OUT, 0, zl, 0, 1, NULL, NULL, timeout_ms)
           : retry(h, T_IN, 0, zl, 0, 0, &got, &rt, timeout_ms);
    if (r != CY_USB_ACK) {
        logf_(h, "control %02x %02x status stage: %s", type, req, res_name(r));
        return r == CY_USB_STALL ? CYUSB_ESTALL : CYUSB_ETIMEOUT;
    }
    if (!in && got) logf_(h, "control status stage carried %d bytes", got);
    return done;
}

static int emu_bulk(cyusb_dev_t *d, uint8_t ep, uint8_t *data, int len, unsigned timeout_ms, int *timed_out)
{
    cy_usbhc_t *h = d->ctx;
    int n = ep & 0x0f, done = 0;
    uint64_t until = h->ms + timeout_ms;
    *timed_out = 0;
    if (ep & 0x80) {
        while (done < len) {
            uint8_t pkt[64];
            int got = 0, rt = 0;
            int r = xact(h, T_IN, n, pkt, 0, 0, &got, &rt);
            if (r == CY_USB_NAK || r == CY_USB_TIMEOUT) {
                if (h->ms >= until) { *timed_out = 1; break; }
                continue;
            }
            if (r == CY_USB_STALL) return CYUSB_ESTALL;
            if (r < 0) return CYUSB_EIO;
            if (rt != h->tog_in[n]) {
                logf_(h, "bulk IN %d: DATA%d again, dropped as a repeat (%d bytes)", n, rt, got);
                continue;
            }
            h->tog_in[n] ^= 1;
            int c = got < len - done ? got : len - done;
            if (c < got) logf_(h, "bulk IN %d: %d bytes did not fit the buffer", n, got - c);
            memcpy(data + done, pkt, (size_t)c);
            done += c;
            if (h->verbose) logf_(h, "IN %d: %d bytes", n, got);
            if (got < 64) break;
        }
    } else {
        do {
            int c = len - done < 64 ? len - done : 64;
            int r = xact(h, T_OUT, n, data + done, c, h->tog_out[n], NULL, NULL);
            if (r == CY_USB_NAK || r == CY_USB_TIMEOUT) {
                if (h->ms >= until) { *timed_out = 1; break; }
                continue;
            }
            if (r == CY_USB_STALL) return CYUSB_ESTALL;
            if (r < 0) return CYUSB_EIO;
            h->tog_out[n] ^= 1;
            if (h->verbose) logf_(h, "OUT %d: %d bytes", n, c);
            done += c;
        } while (done < len);
    }
    return done;
}

static void emu_log(cyusb_dev_t *d, const char *msg) { logf_(d->ctx, "%s", msg); }

static unsigned emu_now_ms(cyusb_dev_t *d)
{
    return (unsigned)cy_usbhc_ms((cy_usbhc_t *)d->ctx);
}

cyusb_dev_t *cy_usbhc_dev(cy_usbhc_t *h)
{
    h->dev.control = emu_control;
    h->dev.bulk = emu_bulk;
    h->dev.log = emu_log;
    h->dev.now_ms = emu_now_ms;
    h->dev.ctx = h;
    return &h->dev;
}

static void hexline(cy_usbhc_t *h, const char *what, const uint8_t *p, int n)
{
    char buf[400];
    int o = snprintf(buf, sizeof buf, "%s:", what);
    for (int i = 0; i < n && o < (int)sizeof buf - 4; i++) o += snprintf(buf + o, sizeof buf - (size_t)o, " %02x", p[i]);
    logf_(h, "%s", buf);
}

static void utf16(const uint8_t *p, int bytes, char *out, int max)
{
    int o = 0;
    for (int j = 0; j + 1 < bytes && o < max - 1 && (p[j] || p[j + 1]); j += 2)
        out[o++] = p[j] >= 32 && p[j] < 127 && !p[j + 1] ? (char)p[j] : '.';
    out[o] = 0;
}

/* What Windows asks a new device for before it picks a driver: a USB 2.0
 * device's other-speed descriptor (a full speed one stalls it), then, only
 * from a USB 2.0 device, string descriptor ee ("MSFT100" and a vendor
 * request code), and with that request the compatible ID (wIndex 4) and
 * the extended properties (wIndex 5). Logged the way they came. */
static void ms_os_descriptors(cy_usbhc_t *h, cyusb_dev_t *d, unsigned bcd_usb)
{
    uint8_t b[256];
    char a[128];
    if (bcd_usb < 0x0200) {
        logf_(h, "Microsoft OS descriptor: not asked for (USB %x.%02x; Windows asks USB 2.0 devices only)", bcd_usb >> 8, bcd_usb & 0xff);
        return;
    }
    int n = d->control(d, 0x80, 6, 0x0600, 0, b, 10, 300);
    logf_(h, "device qualifier: %s", n == CYUSB_ESTALL ? "stalled (full speed only)" : n < 0 ? "failed" : "answered (a high speed device?)");
    n = d->control(d, 0x80, 6, 0x03ee, 0, b, 0x12, 300);
    if (n < 18 || b[1] != 3 || memcmp(b + 2, "M\0S\0F\0T\0" "1\0" "0\0" "0\0", 14)) {
        if (n < 0) logf_(h, "Microsoft OS descriptor: none (string ee failed)");
        else hexline(h, "Microsoft OS descriptor: none; string ee", b, n);
        return;
    }
    uint8_t code = b[16];
    logf_(h, "Microsoft OS descriptor: MSFT100, vendor request %02x", code);
    n = d->control(d, 0xc0, code, 0, 4, b, 16, 300);
    int total = n >= 4 ? (int)(b[0] | b[1] << 8) : 0;
    if (total > (int)sizeof b) total = sizeof b;
    if (total >= 16) n = d->control(d, 0xc0, code, 0, 4, b, (uint16_t)total, 300);
    if (n < 16 || total != n || b[6] != 4) hexline(h, "compatible ID descriptor: bad", b, n > 0 ? n : 0);
    else
        for (int i = 0, k = b[8]; i < k && 16 + 24 * i + 24 <= n; i++) {
            const uint8_t *f = b + 16 + 24 * i;
            logf_(h, "compatible ID: interface %d \"%.8s\" sub \"%.8s\"", f[0], (const char *)f + 2, (const char *)f + 10);
        }
    n = d->control(d, 0xc1, code, 0, 5, b, 10, 300);
    total = n >= 4 ? (int)(b[0] | b[1] << 8) : 0;
    if (total > (int)sizeof b) total = sizeof b;
    if (total >= 10) n = d->control(d, 0xc1, code, 0, 5, b, (uint16_t)total, 300);
    if (n < 10 || total != n || b[6] != 5) { hexline(h, "extended properties: none or bad", b, n > 0 ? n : 0); return; }
    for (int i = 0, o = 10, k = b[8]; i < k && o + 14 <= n; i++) {
        int size = b[o] | b[o + 1] << 8, type = b[o + 4], nl = b[o + 8] | b[o + 9] << 8;
        if (size < 14 + nl || o + size > n) { logf_(h, "extended property %d: bad size", i); return; }
        char name[64];
        utf16(b + o + 10, nl, name, sizeof name);
        int dl = b[o + 10 + nl] | b[o + 11 + nl] << 8;
        utf16(b + o + 14 + nl, dl, a, sizeof a);
        logf_(h, "extended property: %s (type %d) = %s", name, type, a);
        o += size;
    }
}

int cy_usbhc_enumerate(cy_usbhc_t *h, unsigned timeout_ms)
{
    cyusb_dev_t *d = cy_usbhc_dev(h);
    uint64_t until = h->ms + timeout_ms;
    pthread_mutex_lock(&h->mu);
    h->detached = false;
    pthread_mutex_unlock(&h->mu);
    cybiko_usb_vbus(h->m, true);
    while (!cybiko_usb_attached(h->m)) {
        if (h->ms >= until) { logf_(h, "the device never attached (no pull-up)"); return -1; }
        cy_usbhc_wait_ms(h, 1);
    }
    logf_(h, "device attached");
    cy_usbhc_wait_ms(h, 100);
    xact(h, T_RESET, 0, NULL, 0, 0, NULL, NULL);
    cy_usbhc_wait_ms(h, 20);
    h->addr = 0;
    memset(h->tog_in, 0, sizeof h->tog_in);
    memset(h->tog_out, 0, sizeof h->tog_out);
    uint8_t desc[256];
    int n = d->control(d, 0x80, 6, 0x0100, 0, desc, 18, 1000);
    if (n < 0) { logf_(h, "GET_DESCRIPTOR(device) failed"); return -1; }
    hexline(h, "device descriptor", desc, n);
    uint8_t dev_desc[18] = { 0 };
    memcpy(dev_desc, desc, n < 18 ? (size_t)n : 18);
    if (d->control(d, 0x00, 5, 7, 0, NULL, 0, 1000) < 0) { logf_(h, "SET_ADDRESS failed"); return -1; }
    h->addr = 7;
    cy_usbhc_wait_ms(h, 5);
    n = d->control(d, 0x80, 6, 0x0200, 0, desc, 9, 1000);
    if (n < 9) { logf_(h, "GET_DESCRIPTOR(config) failed"); return -1; }
    int total = desc[2] | desc[3] << 8;
    if (total > (int)sizeof desc) total = sizeof desc;
    n = d->control(d, 0x80, 6, 0x0200, 0, desc, (uint16_t)total, 1000);
    hexline(h, "configuration", desc, n);
    for (int i = 1; i <= 4; i++) {
        uint8_t s[64];
        int k = d->control(d, 0x80, 6, (uint16_t)(0x0300 | i), 0x0409, s, sizeof s, 300);
        if (k > 2) {
            char a[33] = { 0 };
            for (int j = 2, o = 0; j + 1 < k && o < 32; j += 2) a[o++] = (char)s[j];
            logf_(h, "string %d: \"%s\"", i, a);
        }
    }
    ms_os_descriptors(h, d, (unsigned)(dev_desc[2] | dev_desc[3] << 8));
    if (d->control(d, 0x00, 9, 1, 0, NULL, 0, 1000) < 0) { logf_(h, "SET_CONFIGURATION failed"); return -1; }
    logf_(h, "configured");
    return 0;
}
