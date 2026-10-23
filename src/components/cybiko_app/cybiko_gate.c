/* The CyWIG on the board (see cybiko_gate.h). */
#include <string.h>
#include "cybiko_gate.h"
#include "cybiko_link.h"
#include "cy_gate.h"
#include "cyb_net_esp.h"
#include "palm_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "cywig";

/* Pages are parsed on the gate's task: it needs the browser's deep stack.
 * The stack may be in PSRAM (the gate never writes flash). */
#define GATE_STACK 16384

typedef struct {
    uint8_t len, channel;
    uint8_t data[CY_RF_LONG];
} packet_t;

static cy_gate_t *gate;
static QueueHandle_t to_gate, to_machine;
static volatile bool pause_req, paused;
static char home[256];
static const uint8_t *fonts_from;      /* the CyOS flash image */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void gate_rf(void *user, const uint8_t *pkt, size_t len, uint8_t ch)
{
    (void)user;
    packet_t p = { .len = (uint8_t)len, .channel = ch };
    memcpy(p.data, pkt, len);
    if (xQueueSend(to_machine, &p, pdMS_TO_TICKS(20)) != pdTRUE) ESP_LOGW(TAG, "packet for the Cybiko dropped");
}

static void gate_log(void *user, const char *line)
{
    (void)user;
    ESP_LOGI(TAG, "%s", line);
}

static void gate_task(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    static cyb_net_t net;
    cyb_net_esp(&net);
    cy_gate_host_t host = { .rf_send = gate_rf, .log = gate_log };
    gate = cy_gate_create(id, &host, &net);
    if (!gate) { ESP_LOGE(TAG, "out of memory"); vTaskDelete(NULL); return; }
    if (home[0]) cy_gate_set_home(gate, home);
    if (fonts_from) cy_gate_set_fonts(gate, fonts_from, CY_FLASH_SIZE);   /* for the Browser app's pages */
    ESP_LOGI(TAG, "gate %08lx on the radio", (unsigned long)id);
    uint32_t wait = 0;
    for (;;) {
        if (pause_req) {
            cy_gate_cancel(gate);
            paused = true;
            while (pause_req) vTaskDelay(pdMS_TO_TICKS(20));
            paused = false;
        }
        packet_t p;
        if (xQueueReceive(to_gate, &p, pdMS_TO_TICKS(wait > 50 ? 50 : wait)) == pdTRUE) {
            cy_gate_rf_in(gate, p.data, p.len, p.channel);
            while (xQueueReceive(to_gate, &p, 0) == pdTRUE) cy_gate_rf_in(gate, p.data, p.len, p.channel);
        }
        wait = cy_gate_poll(gate, now_ms());
        if (!wait) wait = 1;
    }
}

void cybiko_gate_init(cybiko_t *m, const uint8_t *flash, const char *cfg)
{
    fonts_from = flash;
    char value[16] = "on";
    if (cfg) config_get_value(cfg, "wap.gate", value, sizeof value);
    if (!strcmp(value, "off")) { ESP_LOGI(TAG, "no gate (wap.gate = off)"); return; }
    if (cfg) config_get_value(cfg, "wap.home", home, sizeof home);
    to_gate = xQueueCreate(8, sizeof(packet_t));
    to_machine = xQueueCreate(8, sizeof(packet_t));
    if (!to_gate || !to_machine) { ESP_LOGE(TAG, "out of memory"); return; }
    /* an ID of its own: the machine's with another first byte */
    uint32_t id = cybiko_unit_id(m) ^ 0x01000000u;
    TaskHandle_t task;
    if (xTaskCreatePinnedToCore(gate_task, "cywig", GATE_STACK, (void *)(uintptr_t)id, 3, &task, 0) != pdPASS &&
        xTaskCreatePinnedToCoreWithCaps(gate_task, "cywig", GATE_STACK, (void *)(uintptr_t)id, 3, &task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "no memory for the gate task");
}

void cybiko_gate_send(void *user, const uint8_t *pkt, size_t len, uint8_t channel)
{
    cybiko_link_send(user, pkt, len, channel);
    if (!to_gate) return;
    packet_t p = { .len = (uint8_t)len, .channel = channel };
    memcpy(p.data, pkt, len);
    xQueueSend(to_gate, &p, 0);
}

void cybiko_gate_poll(cybiko_t *m)
{
    if (!to_machine) return;
    cyb_net_esp_save();     /* networks set on the gate's Wi-Fi page (needs this internal stack) */
    packet_t p;
    while (xQueuePeek(to_machine, &p, 0) == pdTRUE) {
        if (!cybiko_rf_receive(m, p.data, p.len, p.channel)) break;    /* full: next time */
        xQueueReceive(to_machine, &p, 0);
    }
}

void cybiko_gate_pause(bool pause)
{
    if (!to_gate) return;
    pause_req = pause;
    if (!pause) return;
    for (int i = 0; i < 100 && !paused && gate; i++) vTaskDelay(pdMS_TO_TICKS(10));
}
