#include "web_proxy.h"
#include "portal_protocol.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool copy(char *out, size_t cap, const char *src, size_t len)
{
    if (len >= cap) return false;
    memcpy(out, src, len); out[len] = 0; return true;
}

bool web_proxy_parse_url(const char *url, web_proxy_url_t *out)
{
    if (!strncasecmp(url, "https://", 8)) { out->https = true; url += 8; }
    else if (!strncasecmp(url, "http://", 7)) { out->https = false; url += 7; }
    else return false;
    const char *end = url + strcspn(url, "/?#");
    const char *colon = memchr(url, ':', (size_t)(end - url));
    size_t host_len = (size_t)((colon ? colon : end) - url);
    if (!host_len || host_len >= sizeof(out->host)) return false;
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (!isalnum(c) && c != '-' && c != '.' && c != '_') return false;
        out->host[i] = (char)tolower(c);
    }
    out->host[host_len] = 0;
    out->port = out->https ? 443 : 80;
    if (colon) {
        unsigned port = 0;
        if (colon + 1 == end) return false;
        for (const char *p = colon + 1; p < end; p++) {
            if (!isdigit((unsigned char)*p)) return false;
            port = port * 10 + (unsigned)(*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
        out->port = (uint16_t)port;
    }
    const char *fragment = strchr(end, '#');
    size_t len = fragment ? (size_t)(fragment - end) : strlen(end);
    size_t prefix = !len || *end != '/' ? 1 : 0;
    if (len + prefix >= sizeof(out->path)) return false;
    if (prefix) out->path[0] = '/';
    memcpy(out->path + prefix, end, len); out->path[len + prefix] = 0;
    for (size_t i = 0; i < len + prefix; i++)
        if ((unsigned char)out->path[i] <= 32 || out->path[i] == 127 || out->path[i] == '\\') return false;
    return true;
}

bool web_proxy_format_url(const web_proxy_url_t *url, bool palm_http, char *out, size_t cap)
{
    char port[8] = "";
    if (url->port != (url->https ? 443 : 80)) snprintf(port, sizeof(port), ":%u", url->port);
    int n = snprintf(out, cap, "%s://%s%s%s", url->https && !palm_http ? "https" : "http", url->host, port, url->path);
    return n >= 0 && (size_t)n < cap;
}

/* Normalize relative redirect paths without touching percent-encoded bytes. */
static void normalize_path(char *path)
{
    char result[WEB_PROXY_URL_MAX];
    const char *query = strchr(path, '?');
    size_t end = query ? (size_t)(query - path) : strlen(path), n = 1;
    result[0] = '/';
    size_t pos = 1;
    bool trailing = end && path[end - 1] == '/';
    while (pos < end) {
        size_t start = pos;
        while (pos < end && path[pos] != '/') pos++;
        size_t len = pos - start;
        if (len == 1 && path[start] == '.') trailing = true;
        else if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
            while (n > 1 && result[n - 1] != '/') n--;
            if (n > 1) n--;
            trailing = true;
        } else if (len) {
            if (n > 1) result[n++] = '/';
            memcpy(result + n, path + start, len); n += len;
            trailing = pos < end;
        }
        pos++;
    }
    if (trailing && n > 1 && result[n - 1] != '/') result[n++] = '/';
    if (query) { size_t len = strlen(query); memcpy(result + n, query, len); n += len; }
    result[n] = 0;
    memcpy(path, result, n + 1);
}

bool web_proxy_resolve(const char *location, const web_proxy_url_t *base, char *url, size_t cap)
{
    web_proxy_url_t next;
    if (!strncasecmp(location, "http://", 7) || !strncasecmp(location, "https://", 8)) {
        if (!web_proxy_parse_url(location, &next)) return false;
    } else if (!strncmp(location, "//", 2)) {
        char absolute[WEB_PROXY_URL_MAX];
        int n = snprintf(absolute, sizeof(absolute), "%s:%s", base->https ? "https" : "http", location);
        if (n < 0 || n >= (int)sizeof(absolute) || !web_proxy_parse_url(absolute, &next)) return false;
    } else {
        if (strchr(location, ':') && strcspn(location, ":") < strcspn(location, "/?#")) return false;
        next = *base;
        char path[WEB_PROXY_URL_MAX];
        size_t base_len = strcspn(base->path, "?#");
        if (*location == '/') base_len = 0;
        else if (*location != '?' && *location != '#' && *location) {
            while (base_len && base->path[base_len - 1] != '/') base_len--;
        }
        size_t loc_len = strcspn(location, "#");
        if (base_len + loc_len >= sizeof(path)) return false;
        memcpy(path, base->path, base_len);
        memcpy(path + base_len, location, loc_len); path[base_len + loc_len] = 0;
        if (!path[0]) strcpy(path, "/");
        if (!copy(next.path, sizeof(next.path), path, strlen(path))) return false;
    }
    normalize_path(next.path);
    return web_proxy_format_url(&next, false, url, cap) && web_proxy_parse_url(url, &next);
}

int web_proxy_request_url(const char *target, const char *host, char *out, size_t cap)
{
    char url[WEB_PROXY_URL_MAX];
    web_proxy_url_t parsed;
    if (!strncasecmp(target, "http://", 7) || !strncasecmp(target, "https://", 8)) {
        if (!copy(url, sizeof(url), target, strlen(target))) return -1;
    } else {
        if (*target != '/') return -1;
        if (!*host) return 0; /* Old HTTP/0.9 portal requests have no Host header. */
        int n = snprintf(url, sizeof(url), "http://%s%s", host, target);
        if (n < 0 || n >= (int)sizeof(url)) return -1;
    }
    if (!web_proxy_parse_url(url, &parsed)) return -1;
    if (!strcmp(parsed.host, PALM_PORTAL_HOST) || !strcmp(parsed.host, PALM_GATEWAY_IP)) return 0;
    if (!strcmp(parsed.host, WEB_PROXY_XIINO_HOST)) {
        const char *real = strchr(parsed.path, '?');
        if (!real) return 0;
        if (!web_proxy_parse_url(real + 1, &parsed)) return -1;
    }
    return web_proxy_format_url(&parsed, false, out, cap) ? 1 : -1;
}

bool web_proxy_resource_supported(const char *path)
{
    size_t n = strcspn(path, "?#");
    static const char *const suffixes[] = {".gif", ".jpg", ".jpeg", ".png", ".bmp", ".ico", ".webp", ".svg",
        ".css", ".js", ".woff", ".woff2", ".ttf", ".mp3", ".mp4"};
    for (size_t i = 0; i < sizeof(suffixes)/sizeof(suffixes[0]); i++) {
        size_t k = strlen(suffixes[i]);
        if (n >= k && !strncasecmp(path + n - k, suffixes[i], k)) return false;
    }
    return true;
}

enum { TEXT, NAME, KEEP_TAG, DROP_TAG, DROP_ELEMENT, COMMENT, DROP_OPEN };

static bool flush(web_proxy_filter_t *f)
{
    if (f->output_len && !f->sink(f->ctx, f->output, f->output_len)) f->failed = true;
    f->output_len = 0;
    return !f->failed;
}

static void output_byte(web_proxy_filter_t *f, char c)
{
    if (f->truncated || f->failed) return;
    if (f->total == WEB_PROXY_PAGE_MAX) { f->truncated = true; return; }
    f->output[f->output_len++] = c; f->total++;
    if (f->output_len == sizeof(f->output)) flush(f);
}

static void rewrite(web_proxy_filter_t *f, char c)
{
    static const char pattern[] = "https://";
    if (!f->html) { output_byte(f, c); return; }
    if (tolower((unsigned char)c) == pattern[f->link_len]) {
        f->link[f->link_len++] = c;
        if (f->link_len == 8) {
            for (const char *p = "http://"; *p; p++) output_byte(f, *p);
            f->link_len = 0;
        }
    } else {
        for (size_t i = 0; i < f->link_len; i++) output_byte(f, f->link[i]);
        f->link_len = 0;
        if (tolower((unsigned char)c) == 'h') f->link[f->link_len++] = c;
        else output_byte(f, c);
    }
}

static void character(web_proxy_filter_t *f, unsigned char c)
{
    if (!f->utf8) { rewrite(f, (char)c); return; }
    if (f->pending) {
        if ((c & 0xc0) != 0x80) { f->pending = 0; rewrite(f, '?'); character(f, c); return; }
        f->code = (f->code << 6) | (c & 0x3f);
        if (--f->pending) return;
        uint32_t code = f->code;
        if (code >= 32 && code < 256 && code != 127) rewrite(f, (char)code);
        else if (code == 0x2018 || code == 0x2019) rewrite(f, '\'');
        else if (code == 0x201c || code == 0x201d) rewrite(f, '"');
        else if (code == 0x2013 || code == 0x2014) rewrite(f, '-');
        else if (code == 0x2022) rewrite(f, '*');
        else if (code == 0x2026) { rewrite(f, '.'); rewrite(f, '.'); rewrite(f, '.'); }
        else rewrite(f, '?');
    } else if (c < 128) rewrite(f, (char)c);
    else if (c >= 0xc2 && c <= 0xf4) {
        f->pending = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        f->code = c & (0x3f >> f->pending);
    } else rewrite(f, '?');
}

static void markup(web_proxy_filter_t *f, unsigned char c)
{
    switch (f->state) {
    case TEXT:
        if (c == '<') { f->state = NAME; strcpy(f->held, "<"); f->held_len = 1; }
        else character(f, c);
        break;
    case NAME:
        if ((isalnum(c) || c == '!' || c == '-') && f->held_len < sizeof(f->held) - 1) {
            f->held[f->held_len++] = (char)c; f->held[f->held_len] = 0;
            if (!strcmp(f->held, "<!--")) { f->state = COMMENT; f->matched = 0; }
            break;
        }
        if (!strcasecmp(f->held + 1, "script") || !strcasecmp(f->held + 1, "style") || !strcasecmp(f->held + 1, "svg")) {
            snprintf(f->closing, sizeof(f->closing), "</%s", f->held + 1);
            for (char *p = f->closing; *p; p++) *p = (char)tolower((unsigned char)*p);
            f->state = DROP_OPEN; f->matched = 0; f->quote = 0; f->self_closing = false;
        } else if (!strcasecmp(f->held + 1, "img") || !strcasecmp(f->held + 1, "link") || !strcasecmp(f->held + 1, "meta")) {
            f->state = DROP_TAG; f->quote = 0;
        } else {
            for (size_t i = 0; i < f->held_len; i++) character(f, (unsigned char)f->held[i]);
            f->state = KEEP_TAG; f->quote = 0;
        }
        markup(f, c);
        break;
    case KEEP_TAG: case DROP_TAG:
        if (f->state == KEEP_TAG) character(f, c);
        if (f->quote) { if (c == f->quote) f->quote = 0; }
        else if (c == '\'' || c == '"') f->quote = c;
        else if (c == '>') f->state = TEXT;
        break;
    case DROP_OPEN:
        if (f->quote) { if (c == f->quote) f->quote = 0; }
        else if (c == '\'' || c == '"') f->quote = c;
        else if (c == '>') f->state = f->self_closing ? TEXT : DROP_ELEMENT;
        else if (!isspace(c)) f->self_closing = c == '/';
        break;
    case DROP_ELEMENT: {
        size_t n = strlen(f->closing);
        if (f->matched == n) {
            if (c == '>') { f->state = TEXT; f->matched = 0; }
            else if (isspace(c) || c == '/') { f->state = DROP_TAG; f->quote = 0; }
            else f->matched = c == '<' ? 1 : 0;
        } else if (tolower(c) == f->closing[f->matched]) f->matched++;
        else f->matched = c == '<' ? 1 : 0;
        break;
    }
    case COMMENT:
        if (c == '-') { if (f->matched < 2) f->matched++; }
        else if (c == '>' && f->matched == 2) f->state = TEXT;
        else f->matched = 0;
        break;
    }
}

void web_proxy_filter_init(web_proxy_filter_t *f, bool html, bool utf8, web_proxy_sink_t sink, void *ctx)
{
    memset(f, 0, sizeof(*f)); f->html = html; f->utf8 = utf8; f->sink = sink; f->ctx = ctx;
}

bool web_proxy_filter_feed(web_proxy_filter_t *f, const void *data, size_t len)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < len && !f->truncated && !f->failed; i++) {
        if (f->html) markup(f, bytes[i]); else character(f, bytes[i]);
    }
    return flush(f) && !f->truncated;
}

bool web_proxy_filter_finish(web_proxy_filter_t *f)
{
    if (f->pending) { f->pending = 0; rewrite(f, '?'); }
    for (size_t i = 0; i < f->link_len; i++) output_byte(f, f->link[i]);
    f->link_len = 0;
    return flush(f) && !f->truncated;
}
