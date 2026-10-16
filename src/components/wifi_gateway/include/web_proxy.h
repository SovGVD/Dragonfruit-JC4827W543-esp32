#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_PROXY_URL_MAX 1536
#define WEB_PROXY_PAGE_MAX 48000
#define WEB_PROXY_XIINO_HOST "pds.mobirus.com"

typedef struct {
    bool https;
    char host[254];
    uint16_t port;
    char path[WEB_PROXY_URL_MAX];
} web_proxy_url_t;

bool web_proxy_parse_url(const char *url, web_proxy_url_t *out);
bool web_proxy_resolve(const char *location, const web_proxy_url_t *base, char *url, size_t cap);
bool web_proxy_format_url(const web_proxy_url_t *url, bool palm_http, char *out, size_t cap);
/* 0=portal, 1=remote site, -1=invalid. Validated URL returned for remote sites. */
int web_proxy_request_url(const char *target, const char *host, char *out, size_t cap);
bool web_proxy_resource_supported(const char *path);

typedef bool (*web_proxy_sink_t)(void *ctx, const char *data, size_t len);
typedef struct {
    bool html, utf8, truncated, failed, self_closing;
    unsigned state, matched, pending;
    uint32_t code;
    unsigned char quote;
    char held[16], closing[16], link[8], output[768];
    size_t held_len, link_len, output_len, total;
    web_proxy_sink_t sink;
    void *ctx;
} web_proxy_filter_t;

void web_proxy_filter_init(web_proxy_filter_t *f, bool html, bool utf8,
                           web_proxy_sink_t sink, void *ctx);
bool web_proxy_filter_feed(web_proxy_filter_t *f, const void *data, size_t len);
bool web_proxy_filter_finish(web_proxy_filter_t *f);

/* ESP-IDF adapter. Only the PPP HTTP listener calls this function. */
void web_proxy_fetch(int socket, const char *url, bool post,
                      const char *body, const char *content_type, const char *agent);
