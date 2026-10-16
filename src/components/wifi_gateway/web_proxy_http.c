#include "sdkconfig.h"
#include "web_proxy.h"

#if CONFIG_PALM_WIFI_GATEWAY
#include "portal_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define ORIGIN_TIMEOUT_MS 12000
#define ORIGIN_INPUT_MAX (256 * 1024)
typedef struct {
    char url[WEB_PROXY_URL_MAX], location[WEB_PROXY_URL_MAX];
    char content_type[128], encoding[48];
    bool bad_header;
    web_proxy_url_t target;
    web_proxy_filter_t filter;
    char input[512];
} exchange_t;

static bool socket_sink(void *ctx, const char *data, size_t len)
{
    int fd = *(int *)ctx;
    while (len) {
        int n = send(fd, data, len, 0);
        if (n <= 0) return false;
        data += n; len -= (size_t)n;
    }
    return true;
}

static void error_page(int fd, const char *status, const char *reason)
{
    char response[512];
    int n = snprintf(response, sizeof(response),
        "HTTP/1.0 %s\r\nContent-Type: text/html; charset=ISO-8859-1\r\nConnection: close\r\n\r\n"
        "<html><body><h3>Cannot open page</h3><p>%s</p><a href=\"http://" PALM_PORTAL_HOST "/\">Wi-Fi settings</a></body></html>",
        status, reason);
    if (n > 0 && n < (int)sizeof(response)) socket_sink(&fd, response, (size_t)n);
}

static esp_err_t origin_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    exchange_t *s = event->user_data;
    char *dest = NULL; size_t cap = 0;
    if (!strcasecmp(event->header_key, "Location")) { dest = s->location; cap = sizeof(s->location); }
    else if (!strcasecmp(event->header_key, "Content-Type")) { dest = s->content_type; cap = sizeof(s->content_type); }
    else if (!strcasecmp(event->header_key, "Content-Encoding")) { dest = s->encoding; cap = sizeof(s->encoding); }
    if (dest) {
        size_t n = strlen(event->header_value);
        if (n >= cap) s->bad_header = true;
        else memcpy(dest, event->header_value, n + 1);
    }
    return ESP_OK;
}

static esp_http_client_handle_t open_origin(exchange_t *s, bool post, const char *body,
                                            const char *content_type, const char *agent)
{
    s->location[0] = s->content_type[0] = s->encoding[0] = 0;
    s->bad_header = false;
    esp_http_client_config_t cfg = {
        .url = s->url, .timeout_ms = ORIGIN_TIMEOUT_MS,
        .disable_auto_redirect = true, .event_handler = origin_event, .user_data = s,
        .user_agent = agent && *agent ? agent : "Mozilla/3.0",
        .buffer_size = 1024, .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_http_client_set_method(client, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "text/html, text/plain");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Connection", "close");
    if (post) esp_http_client_set_header(client, "Content-Type", content_type && *content_type ? content_type : "application/x-www-form-urlencoded");
    size_t length = post ? strlen(body) : 0;
    if (esp_http_client_open(client, (int)length) != ESP_OK) goto fail;
    for (size_t sent = 0; sent < length;) {
        int n = esp_http_client_write(client, body + sent, (int)(length - sent));
        if (n <= 0) goto fail;
        sent += (size_t)n;
    }
    if (esp_http_client_fetch_headers(client) < 0 || s->bad_header) goto fail;
    return client;
fail:
    esp_http_client_cleanup(client);
    return NULL;
}

void web_proxy_fetch(int fd, const char *url, bool post, const char *body,
                      const char *content_type, const char *agent)
{
    exchange_t *s = calloc(1, sizeof(*s));
    if (!s) { error_page(fd, "503 Unavailable", "Not enough memory to fetch this page."); return; }
    if (strlen(url) >= sizeof(s->url)) { error_page(fd, "400 Bad Request", "Address too long."); free(s); return; }
    strcpy(s->url, url);
    TickType_t since = xTaskGetTickCount();
    for (int hop = 0; hop <= 4; hop++) {
        if (!web_proxy_parse_url(s->url, &s->target)) { error_page(fd, "400 Bad Request", "Invalid address."); break; }
        if (!web_proxy_resource_supported(s->target.path)) { error_page(fd, "404 Not Found", "This resource is not relayed."); break; }
        esp_http_client_handle_t client = open_origin(s, post, body, content_type, agent);
        if (!client && !s->target.https && !post) {
            /* Some sites have removed their HTTP listener altogether. A POST is
             * never replayed after a failed exchange because it may have applied. */
            s->target.https = true;
            if (s->target.port == 80) s->target.port = 443;
            if (web_proxy_format_url(&s->target, false, s->url, sizeof(s->url)))
                client = open_origin(s, false, "", content_type, agent);
        }
        if (!client) { error_page(fd, "502 Bad Gateway", "Could not connect to the site or verify its certificate."); break; }
        int status = esp_http_client_get_status_code(client);
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && s->location[0]) {
            esp_http_client_cleanup(client);
            if (hop == 4 || xTaskGetTickCount() - since > pdMS_TO_TICKS(45000)) {
                error_page(fd, "502 Bad Gateway", "Too many redirects."); break;
            }
            if (!web_proxy_resolve(s->location, &s->target, s->url, sizeof(s->url))) {
                error_page(fd, "502 Bad Gateway", "Invalid redirect address."); break;
            }
            if (status == 303 || status == 301 || status == 302) post = false;
            continue;
        }
        for (char *p = s->content_type; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
        bool html = !s->content_type[0] || strstr(s->content_type, "html");
        if ((!html && strncmp(s->content_type, "text/", 5)) ||
            (s->encoding[0] && strcasecmp(s->encoding, "identity"))) {
            esp_http_client_cleanup(client);
            error_page(fd, "415 Unsupported Media Type", "Only uncompressed text and HTML pages are relayed."); break;
        }
        char header[200];
        int n = snprintf(header, sizeof(header),
            "HTTP/1.0 %d %s\r\nContent-Type: %s; charset=ISO-8859-1\r\nConnection: close\r\n\r\n",
            status, status == 200 ? "OK" : "Response", html ? "text/html" : "text/plain");
        bool listening = n > 0 && n < (int)sizeof(header) && socket_sink(&fd, header, (size_t)n);
        /* A redirect may have changed both host and directory. Preserve relative
         * links by giving the Palm the final URL as its document base. */
        if (html && listening && web_proxy_format_url(&s->target, true, s->url, sizeof(s->url))) {
            size_t cap = strlen(s->url) * 6 + 1;
            char *escaped = malloc(cap);
            if (escaped) {
                portal_html_escape(s->url, escaped, cap);
                listening = socket_sink(&fd, "<base href=\"", 12) && socket_sink(&fd, escaped, strlen(escaped)) && socket_sink(&fd, "\">\r\n", 4);
                free(escaped);
            }
        }
        /* Missing charset is common on old pages; UTF-8 is the modern default. */
        bool utf8 = !strstr(s->content_type, "charset=") || strstr(s->content_type, "utf-8") || strstr(s->content_type, "utf8");
        web_proxy_filter_init(&s->filter, html, utf8, socket_sink, &fd);
        size_t received = 0;
        bool limited = false, read_error = false;
        while (listening) {
            if (received >= ORIGIN_INPUT_MAX || xTaskGetTickCount() - since > pdMS_TO_TICKS(45000)) { limited = true; break; }
            n = esp_http_client_read(client, s->input, sizeof(s->input));
            if (n <= 0) { read_error = n < 0 || !esp_http_client_is_complete_data_received(client); break; }
            received += (size_t)n;
            listening = web_proxy_filter_feed(&s->filter, s->input, (size_t)n);
        }
        web_proxy_filter_finish(&s->filter);
        if (limited || s->filter.truncated || read_error) {
            const char *notice = html ? "<p>[Page cut off]</p>" : "\r\n[Page cut off]\r\n";
            if (!s->filter.failed) socket_sink(&fd, notice, strlen(notice));
        }
        esp_http_client_cleanup(client);
        break;
    }
    free(s);
}
#endif
