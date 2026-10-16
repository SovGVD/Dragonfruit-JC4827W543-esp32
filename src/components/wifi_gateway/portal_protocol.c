#include "portal_protocol.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int unhex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool portal_form_get(const char *form, const char *key, char *out, size_t cap)
{
    if (!cap) return false;
    out[0] = 0;
    bool found = false;
    size_t keylen = strlen(key);
    for (const char *p = form; *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == keylen && !memcmp(p, key, keylen)) {
            if (found) return false;
            found = true;
            size_t n = 0;
            for (const char *v = eq + 1; v < end; v++) {
                unsigned char c = (unsigned char)*v;
                if (c == '%') {
                    if (end - v < 3 || unhex(v[1]) < 0 || unhex(v[2]) < 0) return false;
                    c = (unsigned char)((unhex(v[1]) << 4) | unhex(v[2]));
                    v += 2;
                } else if (c == '+') c = ' ';
                if (c < 32 || c == 127 || n + 1 >= cap) return false;
                out[n++] = (char)c;
            }
            out[n] = 0;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}

size_t portal_html_escape(const char *src, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    for (; *src; src++) {
        const char *rep = NULL;
        switch (*src) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        }
        size_t k = rep ? strlen(rep) : 1;
        if (n + k >= cap) break;
        if (rep) memcpy(out + n, rep, k);
        else out[n] = *src;
        n += k;
    }
    out[n] = 0;
    return n;
}

bool portal_password_valid(const char *p)
{
    size_t n = strlen(p);
    if (!n) return true;
    if (n == 64) {
        for (size_t i = 0; i < n; i++) if (unhex(p[i]) < 0) return false;
        return true;
    }
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126) return false;
    return true;
}

size_t portal_dns_question(const uint8_t *pkt, size_t len, char *name, size_t cap)
{
    if (len < 17 || len > PORTAL_DNS_MAX || !cap ||
        (pkt[2] & 0xF8) || pkt[4] || pkt[5] != 1) return 0;
    size_t pos = 12, n = 0;
    while (pos < len && pkt[pos]) {
        size_t k = pkt[pos++];
        if (k > 63 || pos + k >= len || n + k + (n ? 1 : 0) >= cap) return 0;
        if (n) name[n++] = '.';
        for (size_t i = 0; i < k; i++) {
            unsigned char c = pkt[pos++];
            if (c <= 32 || c >= 127 || c == '.') return 0;
            name[n++] = (char)tolower(c);
        }
    }
    if (pos + 5 > len) return 0;
    name[n] = 0;
    return pos + 5;
}

size_t portal_dns_answer(uint8_t *pkt, size_t len, size_t cap,
                         const uint8_t address[4], bool failed)
{
    char name[254];
    size_t end = portal_dns_question(pkt, len, name, sizeof(name));
    if (!end || end > cap) return 0;
    bool a = !failed && pkt[end - 4] == 0 && pkt[end - 3] == 1 &&
                          pkt[end - 2] == 0 && pkt[end - 1] == 1;
    if (a && end + 16 > cap) return 0;
    pkt[2] = 0x80 | (pkt[2] & 1); /* response, preserve recursion desired */
    pkt[3] = 0x80 | (failed ? 2 : 0);
    memset(pkt + 6, 0, 6);
    if (!a) return end;
    pkt[7] = 1;
    static const uint8_t rr[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
    memcpy(pkt + end, rr, sizeof(rr));
    memcpy(pkt + end + sizeof(rr), address, 4);
    return end + 16;
}

bool portal_dns_reply_matches(const uint8_t *q, size_t qlen,
                              const uint8_t *r, size_t rlen)
{
    if (qlen < 12 || rlen < 12 || !(r[2] & 0x80) || (r[2] & 0x78) ||
        r[0] != q[0] || r[1] != q[1] || r[4] || r[5] != 1) return false;
    uint8_t copy[PORTAL_DNS_MAX];
    if (rlen > sizeof(copy)) return false;
    memcpy(copy, r, rlen);
    copy[2] &= 0x07;
    char qname[254], rname[254];
    size_t qe = portal_dns_question(q, qlen, qname, sizeof(qname));
    size_t re = portal_dns_question(copy, rlen, rname, sizeof(rname));
    return qe && re && !strcmp(qname, rname) && !memcmp(q + qe - 4, r + re - 4, 4);
}

int portal_http_headers(const char *r, size_t len, size_t *body_len)
{
    *body_len = 0;
    const char *line = memchr(r, '\n', len);
    if (!line) return 0;
    if (memchr(r, 0, len)) return -1;
    bool post = len >= 5 && !memcmp(r, "POST ", 5);
    bool get = len >= 4 && !memcmp(r, "GET ", 4);
    if (!post && !get) return -1;
    const char *version = memchr(r + (post ? 5 : 4), ' ', (size_t)(line - r) - (post ? 5 : 4));
    if (!version) return get ? (int)(line - r + 1) : -1;
    if (line - version < 9 || (memcmp(version, " HTTP/1.0", 9) && memcmp(version, " HTTP/1.1", 9))) return -1;
    bool have_length = false;
    const char *p = line + 1, *end = r + len;
    while (p < end) {
        line = memchr(p, '\n', (size_t)(end - p));
        if (!line) return 0;
        size_t n = (size_t)(line - p);
        if (n && p[n - 1] == '\r') n--;
        if (!n) return (int)(line - r + 1);
        if (n >= 18 && !strncasecmp(p, "Transfer-Encoding:", 18)) return -1;
        if (n >= 15 && !strncasecmp(p, "Content-Length:", 15)) {
            if (have_length) return -1;
            have_length = true;
            const char *v = p + 15;
            while (v < p + n && (*v == ' ' || *v == '\t')) v++;
            if (v == p + n || !isdigit((unsigned char)*v)) return -1;
            size_t size = 0;
            for (; v < p + n && isdigit((unsigned char)*v); v++) {
                size = size * 10 + (unsigned)(*v - '0');
                if (size > 512) return -1;
            }
            while (v < p + n && (*v == ' ' || *v == '\t')) v++;
            if (v != p + n) return -1;
            *body_len = size;
        }
        p = line + 1;
    }
    return 0;
}

int portal_http_header_value(const char *r, size_t len, const char *name, char *out, size_t cap)
{
    if (!cap) return -1;
    out[0] = 0;
    const char *end = r + len, *p = memchr(r, '\n', len);
    if (!p) return 0;
    p++;
    int found = 0;
    size_t keylen = strlen(name);
    while (p < end) {
        const char *next = memchr(p, '\n', (size_t)(end - p));
        if (!next) return -1;
        const char *last = next;
        if (last > p && last[-1] == '\r') last--;
        if (last == p) break;
        if ((size_t)(last - p) > keylen && p[keylen] == ':' && !strncasecmp(p, name, keylen)) {
            if (found) return -1;
            const char *v = p + keylen + 1;
            while (v < last && (*v == ' ' || *v == '\t')) v++;
            while (last > v && (last[-1] == ' ' || last[-1] == '\t')) last--;
            size_t n = (size_t)(last - v);
            if (n >= cap) return -1;
            for (size_t i = 0; i < n; i++) if ((unsigned char)v[i] < 32 || v[i] == 127) return -1;
            memcpy(out, v, n); out[n] = 0; found = 1;
        }
        p = next + 1;
    }
    return found;
}
