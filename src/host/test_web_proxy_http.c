#include "web_proxy.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    const char *url, *location, *type, *encoding, *data, *post_body;
    int status, open_error;
} fixture_t;
struct mock_client {
    esp_http_client_config_t config;
    const fixture_t *fixture;
    esp_http_client_method_t method;
    size_t offset, written;
    char body[513];
};
static const fixture_t *fixtures;
static size_t fixture_count, opened, cleaned;
static TickType_t ticks;
TickType_t xTaskGetTickCount(void) { return ticks++; }
int esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(cfg->disable_auto_redirect && cfg->crt_bundle_attach == esp_crt_bundle_attach);
    assert(cfg->timeout_ms > 0 && cfg->user_agent);
    struct mock_client *c = calloc(1, sizeof(*c)); assert(c);
    c->config = *cfg;
    for (size_t i = 0; i < fixture_count; i++)
        if (!strcmp(fixtures[i].url, cfg->url)) c->fixture = &fixtures[i];
    if (!c->fixture) fprintf(stderr, "unexpected origin: %s\n", cfg->url);
    assert(c->fixture); opened++;
    return c;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m)
{ c->method = m; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{ (void)c; assert(!strchr(key, '\r') && !strchr(value, '\n')); return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    assert(c->method == (c->fixture->post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET));
    assert(write_len == (int)(c->fixture->post_body ? strlen(c->fixture->post_body) : 0));
    return c->fixture->open_error;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int len)
{
    if (len > 3) len = 3; /* A write need not consume the whole form at once. */
    assert(c->written + (size_t)len < sizeof(c->body));
    memcpy(c->body + c->written, data, (size_t)len); c->written += (size_t)len;
    return len;
}
static void header(esp_http_client_handle_t c, const char *key, const char *value)
{
    if (!value) return;
    esp_http_client_event_t event = {.event_id = HTTP_EVENT_ON_HEADER,
        .user_data = c->config.user_data, .header_key = (char *)key, .header_value = (char *)value};
    assert(c->config.event_handler(&event) == ESP_OK);
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    if (c->fixture->post_body) assert(!strcmp(c->body, c->fixture->post_body));
    header(c, "Location", c->fixture->location);
    header(c, "Content-Type", c->fixture->type);
    header(c, "Content-Encoding", c->fixture->encoding);
    return (int64_t)strlen(c->fixture->data ? c->fixture->data : "");
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->fixture->status; }
int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int cap)
{
    size_t len = strlen(c->fixture->data ? c->fixture->data : "") - c->offset;
    if (len > (size_t)cap) len = (size_t)cap;
    if (len > 7) len = 7; /* Deliberately split tags, UTF-8 and link schemes. */
    if (len) memcpy(buffer, c->fixture->data + c->offset, len);
    c->offset += len; return (int)len;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{ return c->offset == strlen(c->fixture->data ? c->fixture->data : ""); }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { cleaned++; free(c); return ESP_OK; }

static char result[WEB_PROXY_PAGE_MAX + 4096];
static void run(const fixture_t *f, size_t count, const char *url, const char *post)
{
    fixtures = f; fixture_count = count; opened = cleaned = 0; ticks = 0;
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    web_proxy_fetch(pair[0], url, post != NULL, post ? post : "", "application/x-www-form-urlencoded", "Xiino");
    close(pair[0]);
    size_t n = 0; int got;
    while ((got = (int)read(pair[1], result + n, sizeof(result) - n - 1)) > 0) n += (size_t)got;
    result[n] = 0; close(pair[1]);
    assert(opened == cleaned);
}

int main(void)
{
    const fixture_t redirect[] = {
        {.url="http://example.com/start", .status=302, .location="https://example.com/dir/page"},
        {.url="https://example.com/dir/page", .status=200, .type="text/html; charset=UTF-8",
         .data="<html><script>hidden()</script><img src='bad'><a href=\"https://next.example/\">caf\xc3\xa9</a></html>"}
    };
    run(redirect, 2, redirect[0].url, NULL);
    if (!strstr(result, "<base href=\"http://example.com/dir/page\">")) fprintf(stderr, "proxy response: %s\n", result);
    assert(strstr(result, "HTTP/1.0 200 OK") && strstr(result, "<base href=\"http://example.com/dir/page\">"));
    assert(strstr(result, "href=\"http://next.example/\"") && strstr(result, "caf\xe9"));
    assert(!strstr(result, "hidden") && !strstr(result, "<img") && opened == 2);
    const fixture_t fallback[] = {
        {.url="http://example.com/", .open_error=-1},
        {.url="https://example.com/", .status=200, .type="text/plain", .data="plain https://unmodified"}
    };
    run(fallback, 2, fallback[0].url, NULL);
    assert(opened == 2 && strstr(result, "plain https://unmodified"));
    const fixture_t custom_port[] = {
        {.url="http://example.com:8443/", .open_error=-1},
        {.url="https://example.com:8443/", .status=200, .type="text/plain", .data="custom TLS port"}
    };
    run(custom_port, 2, custom_port[0].url, NULL);
    assert(opened == 2 && strstr(result, "custom TLS port"));
    const fixture_t posts[] = {
        {.url="https://example.com/form", .status=307, .location="../submit", .post_body="a=hello%20world"},
        {.url="https://example.com/submit", .status=303, .location="?done=1", .post_body="a=hello%20world"},
        {.url="https://example.com/submit?done=1", .status=200, .data="done"}
    };
    run(posts, 3, posts[0].url, posts[0].post_body);
    assert(opened == 3 && strstr(result, "done"));
    const fixture_t failed_post[] = {{.url="http://example.com/form", .post_body="a=1", .open_error=-1}};
    run(failed_post, 1, failed_post[0].url, "a=1");
    assert(opened == 1 && strstr(result, "502 Bad Gateway"));
    const fixture_t bad_certificate[] = {{.url="https://example.com/", .open_error=-1}};
    run(bad_certificate, 1, bad_certificate[0].url, NULL);
    assert(strstr(result, "502 Bad Gateway") && strstr(result, "verify its certificate"));
    const fixture_t loop[] = {{.url="http://example.com/", .status=302, .location="/"}};
    run(loop, 1, loop[0].url, NULL);
    assert(opened == 5 && strstr(result, "Too many redirects"));
    const fixture_t compressed[] = {{.url="https://example.com/", .status=200, .type="text/html", .encoding="gzip", .data="not readable"}};
    run(compressed, 1, compressed[0].url, NULL);
    assert(strstr(result, "415 Unsupported Media Type") && !strstr(result, "not readable"));
    const fixture_t binary[] = {{.url="https://example.com/image", .status=200, .type="image/png", .data="binary"}};
    run(binary, 1, binary[0].url, NULL);
    assert(strstr(result, "415 Unsupported Media Type"));
    run(binary, 1, "https://example.com/image.png", NULL);
    assert(!opened && strstr(result, "404 Not Found"));
    puts("proxy HTTP adapter: redirects, fallback, POST, errors and filtering passed");
    return 0;
}
