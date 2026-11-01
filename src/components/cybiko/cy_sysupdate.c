/* System Pack 57 for an emulated Cybiko Classic; see cy_sysupdate.h. */
#include "cy_sysupdate.h"

#include <stdio.h>
#include <string.h>
#include "cybiko.h"

#define PROMPT_MS 120000           /* CyOS starting up, or restarting */
#define TRIES 5                    /* for each step */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- the resources of a Windows program (PE32) ---- */

typedef struct {
    const uint8_t *exe;
    size_t len;
    const uint8_t *sec;        /* section table */
    int nsec;
    uint32_t rsrc;             /* file offset of the resource directory */
    uint32_t rsrc_len;
} pe_t;

static bool in(const pe_t *pe, uint32_t off, uint32_t n) { return off <= pe->len && n <= pe->len - off; }

static uint32_t rva_to_off(const pe_t *pe, uint32_t rva, uint32_t n)
{
    for (int i = 0; i < pe->nsec; i++) {
        const uint8_t *s = pe->sec + i * 40;
        uint32_t va = le32(s + 12), size = le32(s + 16), raw = le32(s + 20);
        if (rva >= va && rva - va + n <= size) return raw + (rva - va);
    }
    return UINT32_MAX;
}

static bool pe_open(pe_t *pe, const uint8_t *exe, size_t len)
{
    memset(pe, 0, sizeof *pe);
    pe->exe = exe;
    pe->len = len;
    if (len < 0x40 || exe[0] != 'M' || exe[1] != 'Z') return false;
    uint32_t h = le32(exe + 0x3c);
    if (!in(pe, h, 24) || memcmp(exe + h, "PE\0\0", 4)) return false;
    pe->nsec = le16(exe + h + 6);
    uint32_t opt = h + 24, optlen = le16(exe + h + 20);
    if (!in(pe, opt, optlen) || optlen < 96 + 3 * 8 || le16(exe + opt) != 0x10b) return false;
    if (!in(pe, opt + optlen, (uint32_t)pe->nsec * 40)) return false;
    pe->sec = exe + opt + optlen;
    uint32_t rva = le32(exe + opt + 96 + 16), size = le32(exe + opt + 96 + 20);
    pe->rsrc = rva_to_off(pe, rva, size);
    pe->rsrc_len = size;
    return pe->rsrc != UINT32_MAX && size >= 16;
}

/* Directory entry i of the directory at rsrc offset dir */
static bool dir_entry(const pe_t *pe, uint32_t dir, int i, uint32_t *name, uint32_t *to)
{
    if (dir + 16 > pe->rsrc_len) return false;
    const uint8_t *d = pe->exe + pe->rsrc + dir;
    int n = le16(d + 12) + le16(d + 14);
    if (i >= n || dir + 16 + (uint32_t)(i + 1) * 8 > pe->rsrc_len) return false;
    *name = le32(d + 16 + i * 8);
    *to = le32(d + 20 + i * 8);
    return true;
}

static bool name_is(const pe_t *pe, uint32_t name, const char *want)
{
    if (!(name & 0x80000000u)) return false;
    uint32_t off = name & 0x7fffffffu;
    if (off + 2 > pe->rsrc_len) return false;
    const uint8_t *p = pe->exe + pe->rsrc + off;
    size_t n = le16(p);
    if (n != strlen(want) || off + 2 + n * 2 > pe->rsrc_len) return false;
    for (size_t k = 0; k < n; k++)
        if (le16(p + 2 + k * 2) != (uint8_t)want[k]) return false;
    return true;
}

/* Resource id of the type directory at tdir: its data, any language */
static const uint8_t *resource(const pe_t *pe, uint32_t tdir, uint32_t id, uint32_t *size)
{
    uint32_t name, to;
    for (int i = 0; dir_entry(pe, tdir, i, &name, &to); i++) {
        if (name != id || !(to & 0x80000000u)) continue;
        uint32_t lname, lto;
        if (!dir_entry(pe, to & 0x7fffffffu, 0, &lname, &lto) || (lto & 0x80000000u)) return NULL;
        if (lto + 16 > pe->rsrc_len) return NULL;
        const uint8_t *e = pe->exe + pe->rsrc + lto;
        uint32_t n = le32(e + 4), off = rva_to_off(pe, le32(e), n);
        if (off == UINT32_MAX || !in(pe, off, n)) return NULL;
        *size = n;
        return pe->exe + off;
    }
    return NULL;
}

static bool starts(const char *s, size_t n, const char *prefix)
{
    size_t k = strlen(prefix);
    return n >= k && !memcmp(s, prefix, k);
}

int cy_su_read_exe(const uint8_t *exe, size_t len, cy_su_file_t *out, int max)
{
    pe_t pe;
    if (!pe_open(&pe, exe, len)) return 0;
    uint32_t name, to, tdir = UINT32_MAX;
    for (int i = 0; dir_entry(&pe, 0, i, &name, &to); i++)
        if (name_is(&pe, name, "FILES_TO_LOAD") && (to & 0x80000000u)) tdir = to & 0x7fffffffu;
    if (tdir == UINT32_MAX) return 0;
    uint32_t list_len;
    const char *list = (const char *)resource(&pe, tdir, 2000, &list_len);
    if (!list) return 0;
    static const char *const packs[] = { "common\\System Pack\\", "114\\System Pack\\" };
    int n = 0;
    uint32_t i = 0;
    for (size_t at = 0; at < list_len;) {
        size_t end = at;
        while (end < list_len && list[end] != '\r' && list[end] != '\n') end++;
        size_t ln = end - at;
        if (ln) {
            for (size_t p = 0; p < 2; p++) {
                if (!starts(list + at, ln, packs[p])) continue;
                size_t k = strlen(packs[p]);
                uint32_t size;
                const uint8_t *data = resource(&pe, tdir, 2001 + i, &size);
                if (!data || n >= max || ln - k >= sizeof out[n].name) return 0;
                memcpy(out[n].name, list + at + k, ln - k);
                out[n].name[ln - k] = 0;
                out[n].data = data;
                out[n].size = size;
                n++;
            }
            i++;
        }
        at = end;
        while (at < list_len && (list[at] == '\r' || list[at] == '\n')) at++;
    }
    return n;
}

/* ---- the update ---- */

static int con_put(void *ctx, const uint8_t *data, int n)
{
    int k = 0;
    while (k < n && cybiko_serial_rx(ctx, 2, data[k])) k++;
    return k;
}

static void con_drop(void *ctx)
{
    struct cybiko *m = ctx;
    m->sci[2].rx_count = 0;
}

static int desktop(const cy_su_t *u)
{
    for (int i = 0; i < u->nfiles; i++)
        if (!strcmp(u->files[i].name, "desktop.app")) return i;
    return -1;
}

bool cy_su_start(cy_su_t *u, struct cybiko *m, const uint8_t *exe, size_t len)
{
    memset(u, 0, sizeof *u);
    u->m = m;
    u->nfiles = cy_su_read_exe(exe, len, u->files, CY_SU_MAX_FILES);
    bool boot = false;
    for (int i = 0; i < u->nfiles; i++) boot |= !strcmp(u->files[i].name, "comp1.2.57r.boot");
    if (!boot || desktop(u) < 0) {
        snprintf(u->message, sizeof u->message, "not Cybiko's System Pack 57 updater");
        u->state = CY_SU_FAILED;
        return false;
    }
    /* ver, the other files, rm desktop.app, desktop.app, boot, reboot, ver */
    u->nsteps = u->nfiles - 1 + 6;
    cy_con_init(&u->con, con_put, m);
    u->con.drop = con_drop;
    u->state = CY_SU_WAITING;
    snprintf(u->message, sizeof u->message, "waiting for CyOS");
    return true;
}

bool cy_su_start_files(cy_su_t *u, struct cybiko *m, const cy_su_file_t *files, int n)
{
    memset(u, 0, sizeof *u);
    u->m = m;
    u->plain = true;
    if (n <= 0 || n > CY_SU_MAX_FILES) {
        snprintf(u->message, sizeof u->message, "%d files: 1 to %d can go in at once", n, CY_SU_MAX_FILES);
        u->state = CY_SU_FAILED;
        return false;
    }
    memcpy(u->files, files, (size_t)n * sizeof *files);
    u->nfiles = n;
    u->nsteps = n + 1;                     /* ver, then each file */
    cy_con_init(&u->con, con_put, m);
    u->con.drop = con_drop;
    u->state = CY_SU_WAITING;
    snprintf(u->message, sizeof u->message, "waiting for CyOS");
    return true;
}

void cy_su_rx(cy_su_t *u, uint8_t byte)
{
    if (u->state != CY_SU_WAITING && u->state != CY_SU_RUNNING) return;
    memmove(u->seen, u->seen + 1, sizeof u->seen - 2);
    u->seen[sizeof u->seen - 2] = (char)byte;
    if (!memcmp(u->seen, "Ready >", 7)) u->ready = true;
    cy_con_rx(&u->con, byte);
}

static void fail(cy_su_t *u, const char *what)
{
    u->state = CY_SU_FAILED;
    snprintf(u->message, sizeof u->message, "%.150s", what);
}

/* Start step u->step: "ver" (nothing to do if CyOS is 1.2.57 already), the
 * files but desktop.app, rm desktop.app, desktop.app, boot, reboot, ver */
static void begin(cy_su_t *u)
{
    if (u->plain) {                        /* cy_su_start_files: ver, the files */
        if (u->step == 0) {
            snprintf(u->message, sizeof u->message, "asking CyOS its version");
            cy_con_command(&u->con, "ver");
        } else {
            const cy_su_file_t *f = &u->files[u->step - 1];
            snprintf(u->message, sizeof u->message, "sending %s", f->name);
            cy_con_send_file(&u->con, f->name, f->data, f->size);
        }
        return;
    }
    int d = desktop(u), nother = u->nfiles - 1, s = u->step - 1;
    if (s < 0) {
        snprintf(u->message, sizeof u->message, "asking CyOS its version");
        cy_con_command(&u->con, "ver");
    } else if (s < nother) {
        const cy_su_file_t *f = &u->files[s >= d ? s + 1 : s];
        snprintf(u->message, sizeof u->message, "sending %s", f->name);
        cy_con_send_file(&u->con, f->name, f->data, f->size);
    } else if (s == nother) {
        snprintf(u->message, sizeof u->message, "replacing desktop.app");
        cy_con_command(&u->con, "rm -f desktop.app");
    } else if (s == nother + 1) {
        cy_con_send_file(&u->con, "desktop.app", u->files[d].data, u->files[d].size);
    } else if (s == nother + 2) {
        snprintf(u->message, sizeof u->message, "making CyOS 1.2.57 the one that starts");
        cy_con_command(&u->con, "boot comp1.2.57r.boot");
    } else if (s == nother + 3) {
        snprintf(u->message, sizeof u->message, "restarting");
        u->ready = false;
        cy_con_command(&u->con, "reboot");
    } else {
        snprintf(u->message, sizeof u->message, "checking the version");
        cy_con_command(&u->con, "ver");
    }
}

cy_su_state_t cy_su_poll(cy_su_t *u, uint32_t elapsed_ms)
{
    if (u->state == CY_SU_WAITING) {
        if (!u->ready) {
            /* The prompt may have gone by already: ask "ver" now and then */
            u->wait_ms += elapsed_ms;
            cy_con_poll(&u->con, elapsed_ms);
            if (u->con.state == CY_CON_DONE) u->ready = true;
            else if (u->con.state != CY_CON_BUSY) {
                u->con.state = CY_CON_IDLE;
                if (u->wait_ms > PROMPT_MS) { fail(u, "CyOS's console did not answer"); return u->state; }
                if (u->wait_ms > 5000) cy_con_command(&u->con, "ver");
            }
            if (!u->ready) return u->state;
        }
        u->con.state = CY_CON_IDLE;
        u->state = CY_SU_RUNNING;
        u->wait_ms = 0;
        begin(u);
    }
    if (u->state != CY_SU_RUNNING) return u->state;
    cy_con_poll(&u->con, elapsed_ms);
    if (u->con.state == CY_CON_BUSY) return u->state;
    if (u->con.state == CY_CON_FAILED && ++u->tries < TRIES && (u->plain || u->step < u->nsteps - 2)) {
        /* CyOS gave up on a file (it waits only so long for each block) or
         * missed a command: once it is at its prompt again, the same step */
        u->retries++;
        u->con.state = CY_CON_IDLE;
        u->ready = false;
        u->state = CY_SU_WAITING;
        u->wait_ms = 0;
        return u->state;                   /* begin() runs u->step again after the wait */
    }
    if (u->con.state == CY_CON_FAILED) {
        char what[200];
        snprintf(what, sizeof what, "%.60s: %.80s (tried %d times)", u->con.line, u->con.result, u->tries);
        fail(u, what);
        return u->state;
    }
    int last = u->nsteps - 1;
    if (u->plain && u->step == last) {
        u->state = CY_SU_DONE;
        snprintf(u->message, sizeof u->message, "%d file%s installed", u->nfiles, u->nfiles == 1 ? "" : "s");
        return u->state;
    }
    if (u->plain) {
        u->con.state = CY_CON_IDLE;
        u->step++;
        u->tries = 0;
        begin(u);
        return u->state;
    }
    if (u->step == 0 && strstr(u->con.result, "1.2.57")) {
        u->state = CY_SU_DONE;
        u->already = true;
        snprintf(u->message, sizeof u->message, "CyOS 1.2.57 was installed already");
        return u->state;
    }
    if (u->step == last) {
            if (strstr(u->con.result, "1.2.57")) {
            u->state = CY_SU_DONE;
            snprintf(u->message, sizeof u->message, "CyOS 1.2.57 is installed");
        } else {
            fail(u, "CyOS did not come back as 1.2.57");
        }
        return u->state;
    }
    u->con.state = CY_CON_IDLE;
    u->step++;
    u->tries = 0;
    if (u->step == last) {                 /* after the restart: wait for the prompt */
        u->state = CY_SU_WAITING;
        u->wait_ms = 0;
        return u->state;
    }
    begin(u);
    return u->state;
}
