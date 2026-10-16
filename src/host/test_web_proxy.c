#include "web_proxy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { char text[WEB_PROXY_PAGE_MAX + 1]; size_t len; } capture_t;
static bool collect(void *ctx, const char *data, size_t len)
{
    capture_t *c = ctx;
    assert(c->len + len <= WEB_PROXY_PAGE_MAX);
    memcpy(c->text + c->len, data, len); c->len += len; c->text[c->len] = 0;
    return true;
}

static void filter(const char *input, const char *expected, size_t chunk, bool html, bool utf8)
{
    capture_t c = {0}; web_proxy_filter_t f;
    web_proxy_filter_init(&f, html, utf8, collect, &c);
    for (size_t off = 0; off < strlen(input);) {
        size_t n = strlen(input) - off;
        if (n > chunk) n = chunk;
        assert(web_proxy_filter_feed(&f, input + off, n)); off += n;
    }
    assert(web_proxy_filter_finish(&f));
    if (strcmp(c.text, expected)) fprintf(stderr, "filter result: %s\nexpected: %s\n", c.text, expected);
    assert(!strcmp(c.text, expected));
}

int main(void)
{
    web_proxy_url_t u;
    assert(web_proxy_parse_url("HTTPS://EXAMPLE.COM:8443/a/b?x=1#frag", &u));
    assert(u.https && u.port == 8443 && !strcmp(u.host, "example.com") && !strcmp(u.path, "/a/b?x=1"));
    char url[WEB_PROXY_URL_MAX];
    assert(web_proxy_resolve("../next?q=2", &u, url, sizeof(url)) && !strcmp(url, "https://example.com:8443/next?q=2"));
    assert(web_proxy_resolve("?new=1", &u, url, sizeof(url)) && !strcmp(url, "https://example.com:8443/a/b?new=1"));
    assert(web_proxy_resolve("//other.example/p", &u, url, sizeof(url)) && !strcmp(url, "https://other.example/p"));
    assert(web_proxy_resolve("/new/./path/../", &u, url, sizeof(url)) && !strcmp(url, "https://example.com:8443/new/"));
    assert(!web_proxy_resolve("javascript:bad", &u, url, sizeof(url)));
    assert(web_proxy_parse_url("https://example.com?q=1", &u) && !strcmp(u.path, "/?q=1"));
    assert(web_proxy_format_url(&u, true, url, sizeof(url)) && !strcmp(url, "http://example.com/?q=1"));
    const char *bad[] = {"ftp://example.com/", "http:///", "http://user:pass@example.com/", "http://host:65536/",
        "http://host:0/", "http://host:abc/", "http://host:/", "http://host/line\r\nInjected:yes", "http://host/a\\b", "http://host name/"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) assert(!web_proxy_parse_url(bad[i], &u));
    assert(web_proxy_request_url("/news?q=1", "example.com", url, sizeof(url)) == 1 && !strcmp(url, "http://example.com/news?q=1"));
    assert(web_proxy_request_url("https://example.com/a", "wifi.setup", url, sizeof(url)) == 1 && !strcmp(url, "https://example.com/a"));
    assert(web_proxy_request_url("http://WIFI.SETUP/save", "example.com", url, sizeof(url)) == 0);
    assert(web_proxy_request_url("/save", "192.168.99.1:80", url, sizeof(url)) == 0);
    assert(web_proxy_request_url("/save", "", url, sizeof(url)) == 0);
    assert(web_proxy_request_url("/<settings>/?https://example.com/p", WEB_PROXY_XIINO_HOST, url, sizeof(url)) == 1 && !strcmp(url, "https://example.com/p"));
    assert(web_proxy_request_url("/save", "bad\r\nHost:evil", url, sizeof(url)) == -1);
    assert(!web_proxy_resource_supported("/img.PNG?q=1"));
    assert(web_proxy_resource_supported("/news?pic=img.png"));

    const char *html = "<html><head><STYLE>x{}</STYLE><!-- no --><link href='x'></head>"
        "<body><script>secret();</script><svg><path/></svg><img alt='>bad'>"
        "<a title=\"<script> literal\" href=\"HTTPS://example.com/x\">caf\xc3\xa9 \xe2\x80\x94 \xe2\x80\xa6</a></body></html>";
    const char *expected = "<html><head></head><body><a title=\"<script> literal\" href=\"http://example.com/x\">caf\xe9 - ...</a></body></html>";
    for (size_t chunk = 1; chunk <= 33; chunk++) filter(html, expected, chunk, true, true);
    filter("hhttps://x httpS:/ partial htt", "hhttp://x httpS:/ partial htt", 1, true, false);
    filter("https://plain caf\xc3\xa9", "https://plain caf\xe9", 1, false, true);
    filter("<script>fake </scripture> still hidden</script>shown", "shown", 1, true, true);
    filter("<svg/>shown<svg data='</svg>'><path/></svg>after", "shownafter", 1, true, true);
    capture_t c = {0}; web_proxy_filter_t f;
    web_proxy_filter_init(&f, false, false, collect, &c);
    char block[1024]; memset(block, 'x', sizeof(block));
    while (web_proxy_filter_feed(&f, block, sizeof(block))) {}
    assert(f.truncated && c.len == WEB_PROXY_PAGE_MAX);
    puts("web proxy routing, redirects, streaming HTML and limits passed");
    return 0;
}
