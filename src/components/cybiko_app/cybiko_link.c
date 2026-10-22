/* ESP-NOW transport for the Cybiko radio (see cybiko_link.h). Frames are
 * broadcast: "CY", a version byte (2), the Cybiko radio channel, then the
 * 50- or 200-byte packet. */
#include <string.h>
#include <stdlib.h>
#include "cybiko_link.h"
#include "palm_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "cybiko_rf";

#define VERSION 2
#define HEADER 4

typedef struct {
    uint8_t len, channel;
    uint8_t data[CY_RF_LONG];
} packet_t;

static bool enabled;
static QueueHandle_t rx_queue;
static uint32_t dropped;
static const uint8_t broadcast[ESP_NOW_ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (len < HEADER || data[0] != 'C' || data[1] != 'Y' || data[2] != VERSION) return;
    int n = len - HEADER;
    if (n != CY_RF_SHORT && n != CY_RF_LONG) return;
    packet_t p = { .len = (uint8_t)n, .channel = data[3] };
    memcpy(p.data, data + HEADER, (size_t)n);
    if (xQueueSend(rx_queue, &p, 0) != pdTRUE) dropped++;
}

static bool start_radio(int channel)
{
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (err == ESP_OK) err = esp_wifi_init(&cfg);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) err = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) err = esp_now_init();
    if (err == ESP_OK) err = esp_now_register_recv_cb(on_recv);
    if (err == ESP_OK) {
        esp_now_peer_info_t peer = { .channel = (uint8_t)channel, .ifidx = WIFI_IF_STA, .encrypt = false };
        memcpy(peer.peer_addr, broadcast, sizeof broadcast);
        err = esp_now_add_peer(&peer);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "ESP-NOW start failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

/* The board's own Cybiko ID: the dump's first byte, then the last three
 * bytes of the Wi-Fi MAC, unique per board. */
static void set_id(cybiko_t *m, const char *cfg)
{
    char value[16] = "auto";
    if (cfg) config_get_value(cfg, "rf.id", value, sizeof value);
    uint32_t id = 0;
    if (!strcmp(value, "keep")) {
        id = 0;
    } else if (!strcmp(value, "auto")) {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
            id = (cybiko_unit_id(m) & 0xff000000u) | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5];
    } else {
        id = (uint32_t)strtoul(value, NULL, 16);
    }
    cybiko_set_unit_id(m, id);
    ESP_LOGI(TAG, "Cybiko ID %08lx", (unsigned long)cybiko_unit_id(m));
}

void cybiko_link_init(cybiko_t *m, const char *cfg)
{
    char value[16];
    set_id(m, cfg);
    if (cfg && config_get_value(cfg, "rf", value, sizeof value) && strcmp(value, "espnow") != 0) {
        ESP_LOGI(TAG, "radio link off (rf = %s)", value);
        return;
    }
    int channel = 1;
    if (cfg && config_get_value(cfg, "rf.channel", value, sizeof value)) channel = atoi(value);
    if (channel < 1 || channel > 13) {
        ESP_LOGW(TAG, "bad rf.channel in cybiko.cfg; using 1");
        channel = 1;
    }
    rx_queue = xQueueCreate(8, sizeof(packet_t));
    if (!rx_queue || !start_radio(channel)) return;
    enabled = true;
    ESP_LOGI(TAG, "radio over ESP-NOW, Wi-Fi channel %d", channel);
}

void cybiko_link_send(void *user, const uint8_t *pkt, size_t len, uint8_t channel)
{
    (void)user;
    if (!enabled || len > CY_RF_LONG) return;
    uint8_t frame[HEADER + CY_RF_LONG] = { 'C', 'Y', VERSION, channel };
    memcpy(frame + HEADER, pkt, len);
    esp_now_send(broadcast, frame, HEADER + len);
}

void cybiko_link_poll(cybiko_t *m)
{
    if (!enabled) return;
    packet_t p;
    /* Leave a packet in the queue while the machine's own queue is full. */
    while (m->rf.queued < CY_RF_QUEUE && xQueueReceive(rx_queue, &p, 0) == pdTRUE)
        cybiko_rf_receive(m, p.data, p.len, p.channel);
    static uint32_t reported;
    if (dropped != reported) {
        reported = dropped;
        ESP_LOGW(TAG, "%lu radio packet(s) dropped (queue full)", (unsigned long)dropped);
    }
}
