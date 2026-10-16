#include "wifi_gateway.h"
#include "sdkconfig.h"

#if CONFIG_PALM_WIFI_GATEWAY
#include "portal_protocol.h"
#include "web_proxy.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"
#include "lwip/netifapi.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SAVED_MAX 5
#define SCAN_MAX 10
#define STREAM_SIZE 8192
#define DNS_PENDING 4
static const char *TAG = "palm_wifi";
static bool enabled;
static esp_netif_t *station;
static ppp_pcb *ppp;
static struct netif ppp_netif;
static struct netif slip_netif;
static uint8_t slip_rx_buf[1500];
static size_t slip_rx_len = 0;
static bool slip_rx_esc = false;
static portal_framing_t active_framing = PORTAL_FRAMING_NONE;
static StreamBufferHandle_t to_palm, from_palm;
static portMUX_TYPE handoff_lock = portMUX_INITIALIZER_UNLOCKED;
/* Protected by handoff_lock. The generation prevents queued bytes from a reset
 * or closed connection being delivered into a new Palm serial session. */
static uint32_t requested_generation, ready_generation;
static bool requested_open, ready;
static atomic_uint palm_ip;
static atomic_uint link_epoch;
static atomic_bool wifi_up, scan_done, disconnected;
static atomic_int disconnect_reason;
static atomic_bool proxy_enabled = true;

typedef struct { char ssid[33], password[65]; } saved_network_t;
typedef struct { uint32_t version, count; saved_network_t nets[SAVED_MAX]; } saved_store_t;
static saved_store_t saved;
static char outcomes[SAVED_MAX][48];
static wifi_ap_record_t scanned[SCAN_MAX];
static uint16_t scanned_count;
static nvs_handle_t settings;
static enum { IDLE, SCANNING, CONNECTING, CONNECTED, WAITING } phase;
static uint32_t phase_at;
static int queue[SAVED_MAX], queue_count, queue_pos, current = -1, preferred = -1;
static bool rescan = true, scanning;
static atomic_bool ppp_dead;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

static atomic_uint ppp_drops, ppp_waits, ppp_tx_bytes, ppp_rx_bytes;

static u32_t ppp_output(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    (void)pcb; (void)ctx;
    /* A dropped frame costs a full TCP RTO (>=1.5 s) on this link, so wait a
     * little for the Palm to drain the stream first. Bounded so lwIP is never
     * stalled for long (the CPU task drains up to 128 bytes per ~1 ms batch). */
    if (xStreamBufferSpacesAvailable(to_palm) < len) {
        atomic_fetch_add(&ppp_waits, 1);
        for (int i = 0; i < 50 && xStreamBufferSpacesAvailable(to_palm) < len; i++)
            vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
        if (xStreamBufferSpacesAvailable(to_palm) < len) {
            atomic_fetch_add(&ppp_drops, 1);
            return 0;
        }
    }
    size_t n = xStreamBufferSend(to_palm, data, len, 0);
    atomic_fetch_add(&ppp_tx_bytes, n);
    return n;
}

/* Diagnostic TCP trace of everything crossing the PPP link. */
static netif_output_fn ppp_ip_output;
static netif_input_fn ppp_ip_input;

static void tcp_trace(const char *dir, struct pbuf *p)
{
    uint8_t h[60];
    u16_t got = pbuf_copy_partial(p, h, sizeof(h), 0);
    if (got < 20 || (h[0] >> 4) != 4) return;
    unsigned ihl = (h[0] & 15) * 4, total = (h[2] << 8) | h[3];
    if (h[9] != 6 || got < ihl + 20) { ESP_LOGI(TAG, "%s proto=%u len=%u", dir, h[9], total); return; }
    const uint8_t *t = h + ihl;
    unsigned doff = (t[12] >> 4) * 4, fl = t[13];
    unsigned mss = 0;
    if ((fl & 0x02) && doff > 20 && got >= ihl + 24 && t[20] == 2) mss = (t[22] << 8) | t[23];
    if (mss) {
        ESP_LOGI(TAG, "%s %u>%u %s%s%s%s seq=%lu ack=%lu win=%u len=%u mss=%u",
                 dir, (t[0] << 8) | t[1], (t[2] << 8) | t[3],
                 fl & 2 ? "S" : "", fl & 1 ? "F" : "", fl & 4 ? "R" : "", fl & 16 ? "." : "",
                 (unsigned long)((uint32_t)t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7]),
                 (unsigned long)((uint32_t)t[8] << 24 | t[9] << 16 | t[10] << 8 | t[11]),
                 (t[14] << 8) | t[15], total - ihl - doff, mss);
    } else {
        ESP_LOGI(TAG, "%s %u>%u %s%s%s%s seq=%lu ack=%lu win=%u len=%u",
                 dir, (t[0] << 8) | t[1], (t[2] << 8) | t[3],
                 fl & 2 ? "S" : "", fl & 1 ? "F" : "", fl & 4 ? "R" : "", fl & 16 ? "." : "",
                 (unsigned long)((uint32_t)t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7]),
                 (unsigned long)((uint32_t)t[8] << 24 | t[9] << 16 | t[10] << 8 | t[11]),
                 (t[14] << 8) | t[15], total - ihl - doff);
    }
}

static err_t ppp_trace_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst)
{
    tcp_trace("->palm", p);
    return ppp_ip_output(nif, p, dst);
}

static err_t ppp_trace_input(struct pbuf *p, struct netif *nif)
{
    tcp_trace("<-palm", p);
    return ppp_ip_input(p, nif);
}

static void ppp_status(ppp_pcb *pcb, int error, void *ctx)
{
    (void)pcb; (void)ctx;
    atomic_fetch_add(&link_epoch, 1);
    if (error == PPPERR_NONE) {
        ip_napt_enable(ip4_addr_get_u32(netif_ip4_addr(&ppp_netif)), 1);
        atomic_store(&palm_ip, ip4_addr_get_u32(netif_ip4_gw(&ppp_netif)));
        ESP_LOGI(TAG, "PPP up: our=%s", ip4addr_ntoa(netif_ip4_addr(&ppp_netif)));
        ESP_LOGI(TAG, "PPP up: palm=%s", ip4addr_ntoa(netif_ip4_gw(&ppp_netif)));
    } else {
        ip_napt_enable_no(ppp_netif.num, 0);
        atomic_store(&palm_ip, 0);
        atomic_store(&ppp_dead, true);
    }
}

static err_t slip_netif_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    (void)netif; (void)ipaddr;
    tcp_trace("->palm", p);
    uint8_t out[1500 * 2 + 2];
    size_t out_len = 0;
    out[out_len++] = SLIP_END;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *src = (const uint8_t *)q->payload;
        for (u16_t i = 0; i < q->len; i++) {
            uint8_t b = src[i];
            if (b == SLIP_END) {
                if (out_len + 2 >= sizeof(out)) return ERR_MEM;
                out[out_len++] = SLIP_ESC;
                out[out_len++] = SLIP_ESC_END;
            } else if (b == SLIP_ESC) {
                if (out_len + 2 >= sizeof(out)) return ERR_MEM;
                out[out_len++] = SLIP_ESC;
                out[out_len++] = SLIP_ESC_ESC;
            } else {
                if (out_len + 1 >= sizeof(out)) return ERR_MEM;
                out[out_len++] = b;
            }
        }
    }
    if (out_len + 1 > sizeof(out)) return ERR_MEM;
    out[out_len++] = SLIP_END;

    if (xStreamBufferSpacesAvailable(to_palm) < out_len) {
        atomic_fetch_add(&ppp_waits, 1);
        for (int i = 0; i < 50 && xStreamBufferSpacesAvailable(to_palm) < out_len; i++)
            vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
        if (xStreamBufferSpacesAvailable(to_palm) < out_len) {
            atomic_fetch_add(&ppp_drops, 1);
            return ERR_MEM;
        }
    }
    size_t n = xStreamBufferSend(to_palm, out, out_len, 0);
    atomic_fetch_add(&ppp_tx_bytes, n);
    return n == out_len ? ERR_OK : ERR_MEM;
}

static err_t slip_netif_init(struct netif *netif)
{
    netif->name[0] = 's';
    netif->name[1] = 'l';
    netif->output = slip_netif_output;
    netif->mtu = 1500;
    netif->flags = 0;
    return ERR_OK;
}

static void slip_rx_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t pkt_len = 0;
        if (portal_slip_decode_byte(data[i], slip_rx_buf, sizeof(slip_rx_buf),
                                    &slip_rx_len, &slip_rx_esc, &pkt_len)) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)pkt_len, PBUF_POOL);
            if (p) {
                pbuf_take(p, slip_rx_buf, (u16_t)pkt_len);
                tcp_trace("<-palm", p);
                if (slip_netif.input(p, &slip_netif) != ERR_OK) {
                    pbuf_free(p);
                }
            }
        }
    }
}

static void ppp_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool running = false;
    char at_buf[64] = {0};
    size_t at_len = 0;
    uint32_t generation = 0;
    uint8_t input[128];
    size_t pending = 0;
    for (;;) {
        portENTER_CRITICAL(&handoff_lock);
        bool want_open = requested_open;
        uint32_t want_generation = requested_generation;
        bool change = running && (!want_open || generation != want_generation || atomic_load(&ppp_dead));
        if (change) ready = false;
        portEXIT_CRITICAL(&handoff_lock);
        if (change) {
            if (active_framing == PORTAL_FRAMING_PPP) {
                pppapi_close(ppp, 1);
            } else if (active_framing == PORTAL_FRAMING_SLIP) {
                netifapi_netif_set_link_down(&slip_netif);
                netifapi_netif_set_down(&slip_netif);
                ip_napt_enable_no(slip_netif.num, 0);
            }
            atomic_store(&palm_ip, 0);
            portENTER_CRITICAL(&handoff_lock);
            xStreamBufferReset(to_palm);
            xStreamBufferReset(from_palm);
            portEXIT_CRITICAL(&handoff_lock);
            pending = 0;
            running = false;
            active_framing = PORTAL_FRAMING_NONE;
            at_len = 0;
            slip_rx_len = 0;
            slip_rx_esc = false;
        }
        if (!running && want_open) {
            atomic_store(&ppp_dead, false);
            generation = want_generation;
            running = true;
            active_framing = PORTAL_FRAMING_NONE;
            at_len = 0;
            slip_rx_len = 0;
            slip_rx_esc = false;
            portENTER_CRITICAL(&handoff_lock);
            ready_generation = generation;
            ready = true;
            portEXIT_CRITICAL(&handoff_lock);
        }
        if (running) {
            if (!pending) pending = xStreamBufferReceive(from_palm, input, sizeof(input), 0);
            if (pending) {
                if (active_framing == PORTAL_FRAMING_NONE) {
                    char resp[64];
                    size_t resp_len = 0;
                    portal_framing_t found = PORTAL_FRAMING_NONE;
                    size_t pre = portal_ppp_preamble(input, pending,
                                                     at_buf, sizeof(at_buf), &at_len,
                                                     resp, sizeof(resp), &resp_len,
                                                     &found);
                    if (resp_len) {
                        xStreamBufferSend(to_palm, resp, resp_len, 0);
                    }
                    if (found == PORTAL_FRAMING_PPP) {
                        active_framing = PORTAL_FRAMING_PPP;
                        pppapi_listen(ppp);
                        size_t ppp_bytes = pending - pre;
                        if (ppp_bytes > 0 && pppos_input_tcpip(ppp, input + pre, (int)ppp_bytes) == ERR_OK) {
                            pending = 0;
                        } else if (ppp_bytes > 0) {
                            memmove(input, input + pre, ppp_bytes);
                            pending = ppp_bytes;
                        } else {
                            pending = 0;
                        }
                    } else if (found == PORTAL_FRAMING_SLIP) {
                        active_framing = PORTAL_FRAMING_SLIP;
                        netifapi_netif_set_link_up(&slip_netif);
                        netifapi_netif_set_up(&slip_netif);
                        ip_napt_enable(ip4_addr_get_u32(netif_ip4_addr(&slip_netif)), 1);
                        atomic_store(&palm_ip, inet_addr(PALM_CLIENT_IP));
                        atomic_fetch_add(&link_epoch, 1);
                        ESP_LOGI(TAG, "SLIP link up: our=%s palm=%s",
                                 ip4addr_ntoa(netif_ip4_addr(&slip_netif)), PALM_CLIENT_IP);
                        size_t slip_bytes = pending - pre;
                        slip_rx_feed(input + pre, slip_bytes);
                        pending = 0;
                    } else {
                        pending = 0;
                    }
                } else if (active_framing == PORTAL_FRAMING_PPP) {
                    if (pppos_input_tcpip(ppp, input, (int)pending) == ERR_OK) pending = 0;
                } else if (active_framing == PORTAL_FRAMING_SLIP) {
                    slip_rx_feed(input, pending);
                    pending = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void wifi_gateway_request(bool open, uint32_t generation)
{
    portENTER_CRITICAL(&handoff_lock);
    requested_open = open;
    requested_generation = generation;
    portEXIT_CRITICAL(&handoff_lock);
}

static bool handoff_ready(void)
{
    return enabled && requested_open && ready && ready_generation == requested_generation;
}

bool wifi_gateway_ready(void)
{
    portENTER_CRITICAL(&handoff_lock);
    bool ok = handoff_ready();
    portEXIT_CRITICAL(&handoff_lock);
    return ok;
}

size_t wifi_gateway_write_space(void)
{
    portENTER_CRITICAL(&handoff_lock);
    size_t n = handoff_ready() ? xStreamBufferSpacesAvailable(from_palm) : 0;
    portEXIT_CRITICAL(&handoff_lock);
    return n;
}

size_t wifi_gateway_write(const void *data, size_t len)
{
    portENTER_CRITICAL(&handoff_lock);
    size_t n = handoff_ready() ? xStreamBufferSend(from_palm, data, len, 0) : 0;
    portEXIT_CRITICAL(&handoff_lock);
    atomic_fetch_add(&ppp_rx_bytes, n);
    return n;
}

size_t wifi_gateway_read(void *data, size_t len)
{
    portENTER_CRITICAL(&handoff_lock);
    size_t n = handoff_ready() ? xStreamBufferReceive(to_palm, data, len, 0) : 0;
    portEXIT_CRITICAL(&handoff_lock);
    return n;
}

static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* PPP must never become the upstream default route. */
        esp_netif_set_default_netif(station);
        atomic_store(&wifi_up, true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&wifi_up, false);
        atomic_store(&disconnect_reason, ((wifi_event_sta_disconnected_t *)data)->reason);
        atomic_store(&disconnected, true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        atomic_store(&scan_done, true);
    }
}

static bool personal(wifi_auth_mode_t auth)
{
    return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA_PSK ||
           auth == WIFI_AUTH_WPA2_PSK || auth == WIFI_AUTH_WPA_WPA2_PSK ||
           auth == WIFI_AUTH_WPA3_PSK || auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

static bool store_networks(const saved_store_t *next)
{
    if (nvs_set_blob(settings, "networks", next, sizeof(*next)) != ESP_OK ||
        nvs_commit(settings) != ESP_OK) return false;
    saved = *next;
    return true;
}

static void restart_wifi(void)
{
    if (scanning) { esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); scanning = false; }
    esp_wifi_disconnect();
    atomic_store(&wifi_up, false);
    phase = WAITING;
    phase_at = now_ms();
    rescan = true;
    current = -1;
}

static void try_next_network(void)
{
    if (queue_pos >= queue_count) {
        phase = WAITING;
        phase_at = now_ms();
        return;
    }
    current = queue[queue_pos++];
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, saved.nets[current].ssid, strlen(saved.nets[current].ssid));
    memcpy(cfg.sta.password, saved.nets[current].password, strlen(saved.nets[current].password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    atomic_store(&disconnected, false);
    snprintf(outcomes[current], sizeof(outcomes[current]), "connecting");
    phase = CONNECTING;
    phase_at = now_ms();
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        snprintf(outcomes[current], sizeof(outcomes[current]), "could not start connection");
        phase = WAITING;
    }
}

static void manage_wifi(void)
{
    uint32_t now = now_ms();
    if (atomic_exchange(&scan_done, false)) {
        scanned_count = SCAN_MAX;
        if (esp_wifi_scan_get_ap_records(&scanned_count, scanned) != ESP_OK) scanned_count = 0;
        scanning = false;
        if (phase == SCANNING) {
            queue_count = queue_pos = 0;
            bool queued[SAVED_MAX] = {0};
            /* A just-entered network may be hidden. Otherwise strongest first. */
            if (preferred >= 0 && preferred < (int)saved.count) {
                queue[queue_count++] = preferred;
                queued[preferred] = true;
            }
            preferred = -1;
            for (int i = 0; i < scanned_count; i++) {
                if (!personal(scanned[i].authmode)) continue;
                for (unsigned j = 0; j < saved.count; j++) {
                    if (!queued[j] && !strcmp((char *)scanned[i].ssid, saved.nets[j].ssid)) {
                        queue[queue_count++] = (int)j;
                        queued[j] = true;
                    }
                }
            }
            for (unsigned j = 0; j < saved.count; j++)
                if (!queued[j]) snprintf(outcomes[j], sizeof(outcomes[j]), "not in range");
            try_next_network();
        }
    }
    if (atomic_load(&wifi_up)) {
        phase = CONNECTED;
        if (current >= 0) snprintf(outcomes[current], sizeof(outcomes[current]), "connected");
    } else if (phase == CONNECTED) {
        phase = WAITING;
        phase_at = now;
    } else if (phase == CONNECTING &&
               (atomic_load(&disconnected) || now - phase_at >= 15000)) {
        int reason = atomic_load(&disconnected) ? atomic_load(&disconnect_reason) : 0;
        snprintf(outcomes[current], sizeof(outcomes[current]),
                 reason == 202 || reason == 15 || reason == 204 ? "password refused (%d)" : "connection failed (%d)", reason);
        esp_wifi_disconnect();
        /* Let a deliberate disconnect event drain before the next attempt. */
        phase = IDLE;
        phase_at = now;
    }
    if (phase == IDLE && !rescan && now - phase_at >= 300) try_next_network();
    bool scan_wanted = rescan || (phase == WAITING && saved.count && now - phase_at >= 20000);
    if (!scanning && scan_wanted && phase != CONNECTING &&
        (phase != WAITING || now - phase_at >= 300)) {
        atomic_store(&scan_done, false);
        if (esp_wifi_scan_start(NULL, false) == ESP_OK) {
            scanning = true;
            rescan = false;
            if (!atomic_load(&wifi_up)) phase = SCANNING;
        }
    }
}

static bool send_all(int sock, const char *data, size_t len)
{
    uint32_t since = now_ms();
    while (len) {
        int n = send(sock, data, len, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && now_ms() - since < 30000) continue;
        if (n <= 0) return false;
        data += n; len -= (size_t)n;
    }
    return true;
}

static void page(int sock, const char *status, const char *body)
{
    char header[240];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.0 %s\r\nContent-Type: text/html; charset=ISO-8859-1\r\n"
        "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, (unsigned)strlen(body));
    uint32_t t0 = now_ms();
    bool ok = send_all(sock, header, (size_t)n) && send_all(sock, body, strlen(body));
    ESP_LOGD(TAG, "HTTP tx '%s' body=%u ok=%d errno=%d in %u ms", status, (unsigned)strlen(body), ok, ok ? 0 : errno,
             (unsigned)(now_ms() - t0));
}

static void append(char *out, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(out);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
}

static void serve_http(int sock)
{
    /* Static because this single-client server does not need per-request stacks. */
    static char request[2048], body[10240];
    size_t used = 0, content_len = 0;
    int header_len = 0;
    uint32_t since = now_ms();
    while (used < sizeof(request) - 1 && now_ms() - since < 20000) {
        int n = recv(sock, request + used, sizeof(request) - 1 - used, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;
        used += (size_t)n;
        request[used] = 0;
        header_len = portal_http_headers(request, used, &content_len);
        if (header_len < 0) break;
        if (header_len && used >= (size_t)header_len + content_len) break;
    }
    {
        const char *eol = memchr(request, '\r', used);
        ESP_LOGD(TAG, "HTTP rx used=%u hdr=%d body=%u after %u ms: %.*s", (unsigned)used, header_len,
                 (unsigned)content_len, (unsigned)(now_ms() - since),
                 (int)(eol ? eol - request : (used > 80 ? 80 : used)), request);
    }
    if (header_len <= 0 || used < (size_t)header_len + content_len) {
        page(sock, "400 Bad Request", "<html><body>Invalid or oversized request.</body></html>");
        return;
    }
    request[header_len + content_len] = 0;
    char host[260], agent[160], type[128], remote[WEB_PROXY_URL_MAX];
    if (portal_http_header_value(request, (size_t)header_len, "Host", host, sizeof(host)) < 0 ||
        portal_http_header_value(request, (size_t)header_len, "User-Agent", agent, sizeof(agent)) < 0 ||
        portal_http_header_value(request, (size_t)header_len, "Content-Type", type, sizeof(type)) < 0) {
        page(sock, "400 Bad Request", "Invalid or oversized header"); return;
    }
    bool post = !strncmp(request, "POST ", 5);
    char *target = request + (post ? 5 : 4);
    char *end = strpbrk(target, " \r\n");
    if (!end) { page(sock, "400 Bad Request", "Invalid request"); return; }
    *end = 0;
    int route = web_proxy_request_url(target, host, remote, sizeof(remote));
    if (route < 0) { page(sock, "400 Bad Request", "Invalid address"); return; }
    if (route > 0) {
        if (!atomic_load(&proxy_enabled)) page(sock, "503 Unavailable", "Web proxy is off. Open http://wifi.setup/ to enable it.");
        else if (!atomic_load(&wifi_up)) page(sock, "503 Unavailable", "Wi-Fi is disconnected. Open http://wifi.setup/ to configure it.");
        else web_proxy_fetch(sock, remote, post, request + header_len, type, agent);
        memset(request, 0, sizeof(request));
        return;
    }
    if (!strncasecmp(target, "http://", 7) || !strncasecmp(target, "https://", 8)) {
        web_proxy_url_t local;
        if (web_proxy_parse_url(target, &local)) memmove(target, local.path, strlen(local.path) + 1);
    }
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    char *form = post ? request + header_len : query;
    const char *message = "";
    if (!strcmp(target, "/proxy/on") || !strcmp(target, "/proxy/off")) {
        bool on = !strcmp(target, "/proxy/on");
        if (nvs_set_u8(settings, "proxy", on) != ESP_OK || nvs_commit(settings) != ESP_OK) {
            page(sock, "500 Storage Error", "Could not save proxy setting."); return;
        }
        atomic_store(&proxy_enabled, on);
        message = "Proxy setting saved. Disconnect and reconnect PPP to refresh cached DNS addresses.";
    } else if (!strcmp(target, "/save") || !strcmp(target, "/forget")) {
        /* Accept GET too for Palm browsers whose form support uses query fields. */
        char ssid[33], password[65];
        if (!form || !portal_form_get(form, "s", ssid, sizeof(ssid)) || !ssid[0]) {
            page(sock, "400 Bad Request", "Invalid network name"); return;
        }
        int index = -1;
        for (unsigned i = 0; i < saved.count; i++)
            if (!strcmp(saved.nets[i].ssid, ssid)) index = (int)i;
        saved_store_t next = saved;
        if (!strcmp(target, "/save")) {
            if (!portal_form_get(form, "p", password, sizeof(password)) || !portal_password_valid(password)) {
                page(sock, "400 Bad Request", "Password: empty for open Wi-Fi, 8-63 ASCII characters, or 64 hex digits."); return;
            }
            if (index < 0) {
                if (next.count == SAVED_MAX) {
                    page(sock, "400 Bad Request", "Five networks saved. Forget one first."); return;
                }
                index = (int)next.count++;
            }
            snprintf(next.nets[index].ssid, sizeof(next.nets[index].ssid), "%s", ssid);
            snprintf(next.nets[index].password, sizeof(next.nets[index].password), "%s", password);
        } else if (index >= 0) {
            memmove(&next.nets[index], &next.nets[index + 1], (next.count - (unsigned)index - 1) * sizeof(next.nets[0]));
            memset(&next.nets[--next.count], 0, sizeof(next.nets[0]));
        }
        if (!store_networks(&next)) { page(sock, "500 Storage Error", "Could not save Wi-Fi settings."); return; }
        memset(outcomes, 0, sizeof(outcomes));
        preferred = !strcmp(target, "/save") ? index : -1;
        restart_wifi();
        message = "Settings saved. Refresh after a few seconds.";
    } else if (!strcmp(target, "/scan")) {
        rescan = true;
        message = "Scan requested. Refresh after a few seconds.";
    } else if (strcmp(target, "/")) {
        page(sock, "404 Not Found", "<html><body><a href=\"/\">Wi-Fi setup</a></body></html>"); return;
    }
    snprintf(body, sizeof(body), "<html><head><title>Wi-Fi</title></head><body><h3>Palm Wi-Fi</h3><p>%s</p>", message);
    if (atomic_load(&wifi_up)) {
        wifi_ap_record_t ap;
        esp_netif_ip_info_t ip;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && esp_netif_get_ip_info(station, &ip) == ESP_OK) {
            char escaped[200];
            portal_html_escape((char *)ap.ssid, escaped, sizeof(escaped));
            append(body, sizeof(body), "<p>Connected: %s<br>Address: " IPSTR "</p>", escaped, IP2STR(&ip.ip));
        }
    } else append(body, sizeof(body), "<p>Wi-Fi: %s</p>", phase == CONNECTING ? "connecting" : phase == SCANNING ? "scanning" : "disconnected");
    append(body, sizeof(body), "<p><a href=\"/\">Refresh</a> <a href=\"/scan\">Scan</a></p>");
    append(body, sizeof(body), "<p>Web proxy: %s. HTTPS pages are fetched by ESP32 and sent to Palm over HTTP.<br>"
        "<a href=\"/proxy/%s\">Turn proxy %s</a></p>",
        atomic_load(&proxy_enabled) ? "on" : "off", atomic_load(&proxy_enabled) ? "off" : "on", atomic_load(&proxy_enabled) ? "off" : "on");
    for (unsigned i = 0; i < saved.count; i++) {
        char escaped[200];
        portal_html_escape(saved.nets[i].ssid, escaped, sizeof(escaped));
        append(body, sizeof(body), "<p>Saved: %s %s</p><form action=\"/forget\" method=\"post\">"
            "<input type=\"hidden\" name=\"s\" value=\"%s\"><input type=\"submit\" value=\"Forget\"></form>",
            escaped, outcomes[i], escaped);
    }
    append(body, sizeof(body), "<h4>Add or update Wi-Fi</h4><form action=\"/save\" method=\"post\">SSID:<br>"
        "<input name=\"s\" maxlength=\"32\"><br>Password:<br><input type=\"password\" name=\"p\" maxlength=\"64\">"
        "<br><input type=\"submit\" value=\"Save and connect\"></form><p>Empty password for open Wi-Fi.</p>");
    if (scanned_count) {
        append(body, sizeof(body), "<h4>Nearby networks</h4><form action=\"/save\" method=\"post\"><select name=\"s\">");
        for (unsigned i = 0; i < scanned_count; i++) {
            if (!scanned[i].ssid[0] || !personal(scanned[i].authmode)) continue;
            char escaped[200];
            portal_html_escape((char *)scanned[i].ssid, escaped, sizeof(escaped));
            append(body, sizeof(body), "<option value=\"%s\">%s (%s, %d dBm)</option>", escaped, escaped,
                   scanned[i].authmode == WIFI_AUTH_OPEN ? "open" : "personal", scanned[i].rssi);
        }
        append(body, sizeof(body), "</select><br>Password:<br><input type=\"password\" name=\"p\" maxlength=\"64\">"
               "<br><input type=\"submit\" value=\"Save and connect\"></form>");
    }
    append(body, sizeof(body), "<p>Palm: " PALM_CLIENT_IP "<br>Gateway: " PALM_GATEWAY_IP "</p></body></html>");
    page(sock, "200 OK", body);
    memset(request, 0, sizeof(request));
}

static int listen_socket(int type, int port)
{
    int fd = socket(AF_INET, type, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = inet_addr(PALM_GATEWAY_IP)};
    if (fd >= 0 && bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

static void http_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int listener = -1;
    for (;;) {
        manage_wifi();
        if (listener < 0) {
            listener = listen_socket(SOCK_STREAM, 80);
            if (listener < 0 || listen(listener, 2) < 0) {
                if (listener >= 0) close(listener);
                listener = -1;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        fd_set fds;
        FD_ZERO(&fds); FD_SET(listener, &fds);
        struct timeval wait = {.tv_usec = 200000};
        if (select(listener + 1, &fds, NULL, NULL, &wait) <= 0) continue;
        struct sockaddr_in peer;
        socklen_t size = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &size);
        if (client < 0) continue;
        ESP_LOGD(TAG, "HTTP accept from " IPSTR, IP2STR((esp_ip4_addr_t *)&peer.sin_addr.s_addr));
        if (peer.sin_addr.s_addr == atomic_load(&palm_ip) && atomic_load(&palm_ip)) {
            struct timeval timeout = {.tv_sec = 2};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            serve_http(client);
            shutdown(client, SHUT_WR);
        }
        close(client);
    }
}

typedef struct {
    int sock;
    struct sockaddr_in peer;
    uint32_t since;
    uint32_t epoch;
    size_t len;
    uint8_t query[PORTAL_DNS_MAX];
} lookup_t;

static void dns_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    static lookup_t pending[DNS_PENDING];
    for (int i = 0; i < DNS_PENDING; i++) pending[i].sock = -1;
    static uint8_t pkt[PORTAL_DNS_MAX + 16];
    static const uint8_t gateway[] = {192,168,99,1};
    int listener = -1;
    uint32_t stats_at = now_ms();
    for (;;) {
        if (now_ms() - stats_at >= 2000) {
            unsigned tx = atomic_exchange(&ppp_tx_bytes, 0), rx = atomic_exchange(&ppp_rx_bytes, 0);
            unsigned waits = atomic_exchange(&ppp_waits, 0), drops = atomic_exchange(&ppp_drops, 0);
            uint32_t span = now_ms() - stats_at;
            if (tx || rx || drops)
                ESP_LOGI(TAG, "PPP to_palm %u B/s, from_palm %u B/s, waits=%u drops=%u, queued=%u",
                         (unsigned)(tx * 1000ULL / span), (unsigned)(rx * 1000ULL / span), waits, drops,
                         (unsigned)(STREAM_SIZE - xStreamBufferSpacesAvailable(to_palm)));
            stats_at = now_ms();
        }
        if (listener < 0) {
            listener = listen_socket(SOCK_DGRAM, 53);
            if (listener < 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        }
        fd_set fds;
        FD_ZERO(&fds); FD_SET(listener, &fds);
        int top = listener;
        for (int i = 0; i < DNS_PENDING; i++) if (pending[i].sock >= 0) {
            FD_SET(pending[i].sock, &fds);
            if (pending[i].sock > top) top = pending[i].sock;
        }
        struct timeval wait = {.tv_usec = 200000};
        int result = select(top + 1, &fds, NULL, NULL, &wait);
        if (result > 0 && FD_ISSET(listener, &fds)) {
            struct sockaddr_in peer;
            socklen_t size = sizeof(peer);
            int len = recvfrom(listener, pkt, PORTAL_DNS_MAX + 1, 0, (struct sockaddr *)&peer, &size);
            char name[254] = "";
            size_t qend = len > 0 ? portal_dns_question(pkt, (size_t)len, name, sizeof(name)) : 0;
            ESP_LOGD(TAG, "DNS rx len=%d from=" IPSTR " palm=" IPSTR " flags=%02x%02x qd=%d parsed=%u name='%s' wifi=%d proxy=%d",
                     len, IP2STR((esp_ip4_addr_t *)&peer.sin_addr.s_addr),
                     IP2STR((esp_ip4_addr_t *)&(uint32_t){atomic_load(&palm_ip)}),
                     len > 3 ? pkt[2] : 0, len > 3 ? pkt[3] : 0, len > 5 ? (pkt[4] << 8 | pkt[5]) : -1,
                     (unsigned)qend, name, (int)atomic_load(&wifi_up), (int)atomic_load(&proxy_enabled));
            if (len > 0) ESP_LOG_BUFFER_HEX_LEVEL(TAG, pkt, len > 64 ? 64 : len, ESP_LOG_DEBUG);
            if (len > 0 && peer.sin_addr.s_addr == atomic_load(&palm_ip) && atomic_load(&palm_ip) && qend) {
                if (!strcmp(name, PALM_PORTAL_HOST) || !atomic_load(&wifi_up) || atomic_load(&proxy_enabled)) {
                    size_t n = portal_dns_answer(pkt, (size_t)len, sizeof(pkt), gateway, false);
                    int sent = n ? sendto(listener, pkt, n, 0, (struct sockaddr *)&peer, size) : -1;
                    ESP_LOGD(TAG, "DNS tx local n=%u sent=%d", (unsigned)n, sent);
                    if (n) ESP_LOG_BUFFER_HEX_LEVEL(TAG, pkt, n, ESP_LOG_DEBUG);
                } else {
                    lookup_t *slot = NULL;
                    for (int i = 0; i < DNS_PENDING; i++) if (pending[i].sock < 0) { slot = &pending[i]; break; }
                    esp_netif_dns_info_t dns;
                    uint32_t server = inet_addr("1.1.1.1");
                    if (esp_netif_get_dns_info(station, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
                        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr) server = dns.ip.u_addr.ip4.addr;
                    int upstream = slot ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1;
                    struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server};
                    if (upstream >= 0 && connect(upstream, (struct sockaddr *)&dest, sizeof(dest)) == 0 &&
                        send(upstream, pkt, (size_t)len, 0) == len) {
                        slot->sock = upstream; slot->peer = peer; slot->since = now_ms(); slot->len = (size_t)len;
                        slot->epoch = atomic_load(&link_epoch);
                        memcpy(slot->query, pkt, (size_t)len);
                    } else {
                        if (upstream >= 0) close(upstream);
                        size_t n = portal_dns_answer(pkt, (size_t)len, sizeof(pkt), gateway, true);
                        if (n) sendto(listener, pkt, n, 0, (struct sockaddr *)&peer, size);
                    }
                }
            }
        }
        for (int i = 0; i < DNS_PENDING; i++) {
            lookup_t *slot = &pending[i];
            if (slot->sock < 0) continue;
            if (slot->epoch != atomic_load(&link_epoch) || !atomic_load(&palm_ip)) {
                close(slot->sock); slot->sock = -1; continue;
            }
            bool done = false;
            if (result > 0 && FD_ISSET(slot->sock, &fds)) {
                int len = recv(slot->sock, pkt, PORTAL_DNS_MAX + 1, 0);
                if (len > 0 && portal_dns_reply_matches(slot->query, slot->len, pkt, (size_t)len)) {
                    sendto(listener, pkt, (size_t)len, 0, (struct sockaddr *)&slot->peer, sizeof(slot->peer));
                    done = true;
                }
            }
            if (!done && now_ms() - slot->since >= 3000) {
                memcpy(pkt, slot->query, slot->len);
                size_t n = portal_dns_answer(pkt, slot->len, sizeof(pkt), gateway, true);
                if (n) sendto(listener, pkt, n, 0, (struct sockaddr *)&slot->peer, sizeof(slot->peer));
                done = true;
            }
            if (done || !atomic_load(&palm_ip)) { close(slot->sock); slot->sock = -1; }
        }
    }
}

bool wifi_gateway_init(void)
{
    if (enabled) return true;
    TaskHandle_t tasks[3] = {0};
    bool wifi_initialized = false, wifi_handler = false, ip_handler = false;
    /* Do not erase NVS on initialization failures: it may contain other settings. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) goto fail;
    err = nvs_open("palm_wifi", NVS_READWRITE, &settings);
    if (err != ESP_OK) goto fail;
    size_t len = sizeof(saved);
    if (nvs_get_blob(settings, "networks", &saved, &len) != ESP_OK || len != sizeof(saved) ||
        saved.version != 1 || saved.count > SAVED_MAX) memset(&saved, 0, sizeof(saved));
    saved.version = 1;
    uint8_t proxy = 1;
    nvs_get_u8(settings, "proxy", &proxy);
    atomic_store(&proxy_enabled, proxy != 0);
    for (unsigned i = 0; i < saved.count; i++) {
        if (!memchr(saved.nets[i].ssid, 0, sizeof(saved.nets[i].ssid)) || !saved.nets[i].ssid[0] ||
            !memchr(saved.nets[i].password, 0, sizeof(saved.nets[i].password)) ||
            !portal_password_valid(saved.nets[i].password)) {
            saved.count = 0;
            break;
        }
    }
    err = esp_netif_init();
    if (err != ESP_OK) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    station = esp_netif_create_default_wifi_sta();
    if (!station) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;
    wifi_initialized = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) goto fail;
    wifi_handler = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (err != ESP_OK) goto fail;
    ip_handler = true;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    to_palm = xStreamBufferCreate(STREAM_SIZE, 1);
    from_palm = xStreamBufferCreate(STREAM_SIZE, 1);
    if (!to_palm || !from_palm) { err = ESP_ERR_NO_MEM; goto fail; }
    ppp = pppapi_pppos_create(&ppp_netif, ppp_output, ppp_status, NULL);
    if (!ppp) { err = ESP_ERR_NO_MEM; goto fail; }
    ppp_ip_output = ppp_netif.output; ppp_netif.output = ppp_trace_output;
    ppp_ip_input = ppp_netif.input; ppp_netif.input = ppp_trace_input;
    /* The newly created PCB is dormant; configure before publishing it to tasks. */
    ip4_addr_t addr;
    ip4addr_aton(PALM_GATEWAY_IP, &addr);
    ppp_set_ipcp_ouraddr(ppp, &addr);
    ppp_set_ipcp_dnsaddr(ppp, 0, &addr);
    ip4addr_aton(PALM_CLIENT_IP, &addr);
    ppp_set_ipcp_hisaddr(ppp, &addr);
    ppp_set_usepeerdns(ppp, 0);
#if VJ_SUPPORT
    ppp->ipcp_wantoptions.neg_vj = 0;
    ppp->ipcp_allowoptions.neg_vj = 0;
#endif
    /* Authentication is disabled in this build; Palm can use any user name. */
    /* Listener mode responds to Palm's LCP; it does not send modem commands. */
    ip4_addr_t our_ip, palm_ip_addr, netmask;
    ip4addr_aton(PALM_GATEWAY_IP, &our_ip);
    ip4addr_aton(PALM_CLIENT_IP, &palm_ip_addr);
    ip4addr_aton("255.255.255.0", &netmask);
    err = netifapi_netif_add(&slip_netif, &our_ip, &netmask, &palm_ip_addr, NULL, slip_netif_init, tcpip_input);
    if (err != ESP_OK) goto fail;
    if (xTaskCreatePinnedToCore(ppp_task, "palm_ppp", 4096, NULL, 3, &tasks[0], 0) != pdPASS ||
        xTaskCreatePinnedToCore(http_task, "wifi_portal", 16384, NULL, 1, &tasks[1], 0) != pdPASS ||
        xTaskCreatePinnedToCore(dns_task, "palm_dns", 4096, NULL, 2, &tasks[2], 0) != pdPASS) {
        /* Tasks wait at a startup gate until all allocations have succeeded. */
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    enabled = true;
    for (int i = 0; i < 3; i++) xTaskNotifyGive(tasks[i]);
    ESP_LOGI(TAG, "PPP/SLIP Wi-Fi ready; portal http://" PALM_PORTAL_HOST "/");
    return true;
fail:
    for (int i = 0; i < 3; i++) if (tasks[i]) vTaskDelete(tasks[i]);
    if (slip_netif.input) netifapi_netif_remove(&slip_netif);
    if (ppp) { pppapi_free(ppp); ppp = NULL; }
    if (to_palm) { vStreamBufferDelete(to_palm); to_palm = NULL; }
    if (from_palm) { vStreamBufferDelete(from_palm); from_palm = NULL; }
    if (wifi_handler) esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
    if (ip_handler) esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event);
    if (wifi_initialized) { esp_wifi_stop(); esp_wifi_deinit(); }
    if (station) { esp_netif_destroy_default_wifi(station); station = NULL; }
    if (settings) { nvs_close(settings); settings = 0; }
    ESP_LOGE(TAG, "Gateway unavailable: %s", esp_err_to_name(err));
    return false;
}

bool wifi_gateway_enabled(void) { return enabled; }
#else
bool wifi_gateway_init(void) { return false; }
bool wifi_gateway_enabled(void) { return false; }
void wifi_gateway_request(bool open, uint32_t generation) { (void)open; (void)generation; }
bool wifi_gateway_ready(void) { return false; }
size_t wifi_gateway_write_space(void) { return 0; }
size_t wifi_gateway_write(const void *data, size_t len) { (void)data; (void)len; return 0; }
size_t wifi_gateway_read(void *data, size_t len) { (void)data; (void)len; return 0; }
#endif
