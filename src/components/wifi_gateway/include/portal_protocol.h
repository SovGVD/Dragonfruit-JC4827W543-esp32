#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PALM_GATEWAY_IP "192.168.99.1"
#define PALM_CLIENT_IP  "192.168.99.2"
#define PALM_PORTAL_HOST "wifi.setup"
#define PORTAL_DNS_MAX 512

/* Strict, bounded form decoding; missing/duplicate/malformed fields fail. */
bool portal_form_get(const char *form, const char *key, char *out, size_t cap);
size_t portal_html_escape(const char *src, char *out, size_t cap);
bool portal_password_valid(const char *password);

/* Return end of a single uncompressed DNS question, or zero. */
size_t portal_dns_question(const uint8_t *pkt, size_t len, char *name, size_t cap);
/* Answer an A/IN question, or a no-data response for other question types.
 * IPv4 address is supplied as four bytes in network order. */
size_t portal_dns_answer(uint8_t *pkt, size_t len, size_t cap,
                         const uint8_t address[4], bool failed);
bool portal_dns_reply_matches(const uint8_t *query, size_t qlen,
                              const uint8_t *reply, size_t rlen);

/* Header boundary/body size. Zero=incomplete, -1=invalid, positive=header size.
 * Accept HTTP/1.0, HTTP/1.1 and old HTTP/0.9 GET; reject transfer encoding,
 * ambiguous length, oversized bodies and unsupported methods. */
int portal_http_headers(const char *request, size_t len, size_t *body_len);
/* 1=present, 0=missing, -1=duplicate, invalid or too long. */
int portal_http_header_value(const char *request, size_t header_len,
                              const char *name, char *out, size_t cap);

/* Pre-PPP modem/handshake emulation for Palm OS network connections.
 * Inspects incoming bytes prior to PPP framing (0x7E):
 * - If 0x7E is found, sets *found_hdlc=true and returns offset of the 0x7E flag.
 * - Non-PPP ASCII commands (AT..., ATD..., CLIENT) generate synthetic modem
 *   responses (OK, CONNECT, CLIENTSERVER) in resp. */
size_t portal_ppp_preamble(const uint8_t *src, size_t len,
                           char *at_buf, size_t at_cap, size_t *at_len,
                           char *resp, size_t resp_cap, size_t *resp_len,
                           bool *found_hdlc);
