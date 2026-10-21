/* ESP-NOW transport for the Cybiko radio (see cybiko_link.h). Frames are
 * broadcast: "CY", a version byte, a sequence byte, then up to 240 payload
 * bytes. */
#include <string.h>
#include <stdlib.h>
#include "cybiko_link.h"
#include "cybiko_app.h"
#include "palm_config.h"
#include "m5_sd.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "cybiko_rf";

#define CFG_PATH CYBIKO_SD_DIR "/cybiko.cfg"
#define FRAME_PAYLOAD 240

static bool enabled;
static int rf_port;
static uint8_t seq;
static uint8_t tx_buf[FRAME_PAYLOAD];
static size_t tx_len;
static StreamBufferHandle_t rx_stream;
static const uint8_t broadcast[ESP_NOW_ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (len < 4 || data[0] != 'C' || data[1] != 'Y' || data[2] != 1) return;
    xStreamBufferSend(rx_stream, data + 4, (size_t)len - 4, 0);
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

void cybiko_link_init(void)
{
    static char text[1024];
    char value[16];
    size_t len;
    if (config_read_file(CFG_PATH, text, sizeof text, &len) != CONFIG_OK) return;
    if (!config_get_value(text, "rf", value, sizeof value) || strcmp(value, "espnow") != 0) return;
    int channel = 1;
    if (config_get_value(text, "rf.port", value, sizeof value)) rf_port = atoi(value);
    if (config_get_value(text, "rf.channel", value, sizeof value)) channel = atoi(value);
    if (rf_port < 0 || rf_port > 1 || channel < 1 || channel > 13) {
        ESP_LOGW(TAG, "bad rf.port/rf.channel in %s", CFG_PATH);
        return;
    }
    rx_stream = xStreamBufferCreate(2048, 1);
    if (!rx_stream || !start_radio(channel)) return;
    enabled = true;
    ESP_LOGI(TAG, "ESP-NOW link on SCI%d, Wi-Fi channel %d (experimental)", rf_port, channel);
}

void cybiko_link_serial_tx(int port, uint8_t b)
{
    if (!enabled || port != rf_port) return;
    if (tx_len < sizeof tx_buf) tx_buf[tx_len++] = b;
}

void cybiko_link_poll(cybiko_t *m)
{
    if (!enabled) return;
    if (tx_len) {
        uint8_t frame[4 + FRAME_PAYLOAD] = { 'C', 'Y', 1, seq++ };
        memcpy(frame + 4, tx_buf, tx_len);
        esp_now_send(broadcast, frame, 4 + tx_len);
        tx_len = 0;
    }
    uint8_t b;
    /* Only take bytes the SCI queue can hold, so none are dropped. */
    while (m->sci[rf_port].rx_count < sizeof m->sci[rf_port].rx_fifo &&
           xStreamBufferReceive(rx_stream, &b, 1, 0) == 1)
        cybiko_serial_rx(m, rf_port, b);
}
