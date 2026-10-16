#include "portal_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char out[200];
    assert(portal_form_get("s=Home+Wi%26Fi&p=a%2Bb%25%3Dc", "s", out, 33));
    assert(!strcmp(out, "Home Wi&Fi"));
    assert(portal_form_get("s=Home+Wi%26Fi&p=a%2Bb%25%3Dc", "p", out, 65));
    assert(!strcmp(out, "a+b%=c"));
    assert(portal_form_get("p=", "p", out, 65) && !out[0]);
    assert(!portal_form_get("p=one&p=two", "p", out, 65));
    assert(!portal_form_get("s=home", "p", out, 65));
    assert(!portal_form_get("p=%", "p", out, 65));
    assert(!portal_form_get("p=%xz", "p", out, 65));
    assert(!portal_form_get("p=%00", "p", out, 65));
    assert(!portal_form_get("p=%0D", "p", out, 65));
    assert(!portal_form_get("p=12345678", "p", out, 8));
    assert(portal_form_get("p=12345678", "p", out, 9));
    portal_html_escape("\"'><&", out, sizeof(out));
    assert(!strcmp(out, "&quot;&#39;&gt;&lt;&amp;"));
    assert(portal_html_escape("<&", out, 5) == 4 && !strcmp(out, "&lt;"));
    assert(portal_password_valid(""));
    assert(!portal_password_valid("short"));
    assert(portal_password_valid("12345678"));
    memset(out, 'a', 64); out[64] = 0;
    assert(portal_password_valid(out));
    out[63] = 'z';
    assert(!portal_password_valid(out));

    const char *post = "POST /save HTTP/1.0\r\nContent-Length: 14\r\n\r\ns=Home&p=hello";
    size_t length;
    int h = portal_http_headers(post, strlen(post), &length);
    assert(h == (int)(strstr(post, "\r\n\r\n") - post + 4) && length == 14);
    for (int i = 0; i < h; i++) assert(portal_http_headers(post, (size_t)i, &length) == 0);
    const char *get = "GET / HTTP/1.0\r\n\r\n";
    assert(portal_http_headers(get, strlen(get), &length) == (int)strlen(get) && length == 0);
    get = "GET /\r\n";
    assert(portal_http_headers(get, strlen(get), &length) == (int)strlen(get));
    const char *bad[] = {
        "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 513\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 2x\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "PUT / HTTP/1.1\r\n\r\n"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
        assert(portal_http_headers(bad[i], strlen(bad[i]), &length) == -1);
    const char *headers = "GET / HTTP/1.0\r\nHost: Example.com:80 \r\nUser-Agent: Xiino\r\n\r\n";
    assert(portal_http_header_value(headers, strlen(headers), "host", out, sizeof(out)) == 1 && !strcmp(out, "Example.com:80"));
    assert(portal_http_header_value(headers, strlen(headers), "User-Agent", out, sizeof(out)) == 1 && !strcmp(out, "Xiino"));
    assert(portal_http_header_value(headers, strlen(headers), "Cookie", out, sizeof(out)) == 0 && !out[0]);
    assert(portal_http_header_value(headers, strlen(headers), "Host", out, 4) == -1);
    headers = "GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n";
    assert(portal_http_header_value(headers, strlen(headers), "Host", out, sizeof(out)) == -1);
    headers = "GET / HTTP/1.0\r\nHost: a\rBad\r\n\r\n";
    assert(portal_http_header_value(headers, strlen(headers), "Host", out, sizeof(out)) == -1);

    const uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,
                            4,'W','i','F','i',5,'S','e','t','u','p',0,0,1,0,1};
    uint8_t pkt[528], reply[528];
    char name[254];
    size_t qlen = sizeof(query);
    assert(portal_dns_question(query, qlen, name, sizeof(name)) == qlen);
    assert(!strcmp(name, "wifi.setup"));
    for (size_t i = 0; i < qlen; i++) assert(!portal_dns_question(query, i, name, sizeof(name)));
    const uint8_t ip[] = {192,168,99,1};
    memcpy(pkt, query, qlen);
    size_t n = portal_dns_answer(pkt, qlen, sizeof(pkt), ip, false);
    assert(n == qlen + 16 && pkt[7] == 1 && pkt[2] == 0x81 && pkt[3] == 0x80);
    assert(!memcmp(pkt + n - 4, ip, 4));
    assert(portal_dns_reply_matches(query, qlen, pkt, n));
    memcpy(reply, pkt, n); reply[1]++;
    assert(!portal_dns_reply_matches(query, qlen, reply, n));
    memcpy(reply, pkt, n); reply[14] = 'X';
    assert(!portal_dns_reply_matches(query, qlen, reply, n));
    memcpy(reply, pkt, n); reply[qlen - 3] = 28;
    assert(!portal_dns_reply_matches(query, qlen, reply, n));
    memcpy(pkt, query, qlen); pkt[qlen - 3] = 28;
    assert(portal_dns_answer(pkt, qlen, sizeof(pkt), ip, false) == qlen && pkt[7] == 0);
    memcpy(pkt, query, qlen);
    assert(portal_dns_answer(pkt, qlen, sizeof(pkt), ip, true) == qlen && (pkt[3] & 15) == 2);
    memcpy(pkt, query, qlen);
    assert(!portal_dns_answer(pkt, qlen, qlen + 15, ip, false));
    memcpy(pkt, query, qlen); pkt[12] = 0xc0;
    assert(!portal_dns_question(pkt, qlen, name, sizeof(name)));
    memcpy(pkt, query, qlen); pkt[5] = 2;
    assert(!portal_dns_question(pkt, qlen, name, sizeof(name)));
    memcpy(pkt, query, qlen); pkt[2] |= 0x80;
    assert(!portal_dns_question(pkt, qlen, name, sizeof(name)));
    /* Every two-byte prefix and every label size exercises malformed-packet
     * boundaries under ASan/UBSan as well as ordinary CTest. */
    for (unsigned i = 0; i <= 255; i++) {
        memcpy(pkt, query, qlen); pkt[12] = (uint8_t)i;
        for (size_t j = 0; j <= qlen; j++)
            portal_dns_question(pkt, j, name, sizeof(name));
    }
    puts("portal form, HTTP framing and DNS tests passed");
    return 0;
}
