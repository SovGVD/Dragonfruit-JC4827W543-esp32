#include "input.h"
#include "input_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Pen events flow input_poll() (touch task) -> CPU task, which injects them
 * into Palm OS via the EvtEnqueuePenPoint ROM trap. We do NOT drive the raw
 * pen-IRQ digitizer path (db_spi_set_touch): on the IIIx ROM that re-enters
 * the AMXHardwareEZ pen handler and fatal-errors ("Pen down int. invalid"). */
/* Deep enough to hold a whole Graffiti stroke's worth of sampled points if the
 * CPU task briefly falls behind draining them (it normally drains far faster
 * than the 100 Hz sampling). */
#define PEN_QUEUE_LEN 64
static QueueHandle_t pen_queue = NULL;

#define KEY_QUEUE_LEN 8
static QueueHandle_t key_queue = NULL;

void input_inject_pen(int16_t x, int16_t y, bool pen_up)
{
    if (!pen_queue)
        return;
    input_pen_event_t ev = { x, y, pen_up };
    xQueueSend(pen_queue, &ev, 0);   /* drop if full rather than block */
}

bool input_pop_pen_event(input_pen_event_t *out)
{
    if (!pen_queue)
        return false;
    return xQueueReceive(pen_queue, out, 0) == pdTRUE;
}

void input_inject_key(uint16_t ascii, uint16_t keycode, uint16_t modifiers)
{
    if (!key_queue)
        return;
    input_key_event_t ev = { ascii, keycode, modifiers };
    xQueueSend(key_queue, &ev, 0);   /* drop if full rather than block */
}

bool input_pop_key(input_key_event_t *out)
{
    if (!key_queue)
        return false;
    return xQueueReceive(key_queue, out, 0) == pdTRUE;
}


esp_err_t input_events_init(void)
{
    pen_queue = xQueueCreate(PEN_QUEUE_LEN, sizeof(input_pen_event_t));
    key_queue = xQueueCreate(KEY_QUEUE_LEN, sizeof(input_key_event_t));
    if (pen_queue && key_queue) return ESP_OK;
    if (pen_queue) vQueueDelete(pen_queue);
    if (key_queue) vQueueDelete(key_queue);
    pen_queue = key_queue = NULL;
    return ESP_ERR_NO_MEM;
}
