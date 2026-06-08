#include "serial_bridge.h"

#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "dragonball.h"

static const char *TAG = "serbridge";

#define BRIDGE_UART   CONFIG_ESP_CONSOLE_UART_NUM        /* UART0 == the CP2104/USB-C line */
#define CONSOLE_BAUD  CONFIG_ESP_CONSOLE_UART_BAUDRATE   /* restore this when bridging ends */

/* Bridge state. s_mode/s_baud are set once at init. s_desired is the policy input
 * (set by the cpu task's auto-trigger or the `serial` command); s_active is the
 * actual state, owned solely by the bridge task. The task reconciles the two — so
 * all UART0 / log / console-task manipulation happens on one context, avoiding
 * cross-core hazards even though requests arrive from the cpu task. */
static volatile int   s_mode  = SERIAL_MODE_AUTO;   /* read on the cpu task (core 1), written on core 0 */
static int            s_baud  = 57600;
static volatile bool  s_desired = false;
static volatile bool  s_active  = false;
static TaskHandle_t   s_task    = NULL;

/* Saved while passthrough is active so logging can be restored on exit. */
static vprintf_like_t s_prev_vprintf = NULL;

/* No-op log sink: while bridging, every ESP_LOGx byte would corrupt the HotSync
 * stream on the shared wire, so we swallow them all (including the per-batch
 * "EMU MHz" telemetry, which is an ESP_LOGI). */
static int sink_vprintf(const char *fmt, va_list ap)
{
    (void)fmt; (void)ap;
    return 0;
}

/* Enter/leave passthrough. We do NOT suspend the console task here: the console
 * task is the single owner of UART0 reads and simply re-routes incoming bytes to
 * the emulated UART once s_active is set (see debug_console.c). Suspending it
 * while it was blocked inside uart_read_bytes — holding the driver's rx_mux —
 * deadlocked the bridge's own reads, so the bridge is now TX-only and never
 * contends for the read mutex. */
static void bridge_enter(void)
{
    ESP_LOGI(TAG, "passthrough ON (UART%d @ %d baud)", BRIDGE_UART, s_baud);
    db_uart_set_loopback(false);             /* loopback would add a 2nd RX-ring producer (echo) */
    uart_wait_tx_done(BRIDGE_UART, pdMS_TO_TICKS(100));  /* flush pending console output */
    s_prev_vprintf = esp_log_set_vprintf(sink_vprintf);  /* gag all logging on the shared wire */
    uart_set_baudrate(BRIDGE_UART, s_baud);
    uart_flush_input(BRIDGE_UART);           /* drop stale console-typed bytes */
    s_active = true;                         /* console now forwards RX to the emulated UART */
}

static void bridge_exit(void)
{
    s_active = false;                        /* console returns to command mode */
    uart_wait_tx_done(BRIDGE_UART, pdMS_TO_TICKS(100));
    uart_set_baudrate(BRIDGE_UART, CONSOLE_BAUD);
    if (s_prev_vprintf) { esp_log_set_vprintf(s_prev_vprintf); s_prev_vprintf = NULL; }
    uart_flush_input(BRIDGE_UART);
    ESP_LOGI(TAG, "passthrough OFF — console restored (UART%d @ %d baud)",
             BRIDGE_UART, CONSOLE_BAUD);
}

/* One pump pass while active: TX only. The console task handles RX (wire ->
 * emulated UART) as the sole reader. The driver's TX buffer is zero-length, so
 * uart_write_bytes() would block until every byte clocks out — long enough to
 * miss an s_desired clear. uart_tx_chars() writes only what fits in the HW FIFO
 * and returns immediately; we re-check s_desired between chunks so a revert is
 * always honored within a pass (bytes unsent at revert are abandoned, which is
 * correct when tearing down). */
static void bridge_pump(void)
{
    uint8_t buf[128];
    uint32_t txn = db_uart_tx_pop(buf, sizeof buf);
    if (txn == 0) {
        vTaskDelay(1);                       /* nothing to send: yield, then re-check s_desired */
        return;
    }
    for (uint32_t off = 0; off < txn && s_desired; ) {
        int w = uart_tx_chars(BRIDGE_UART, (const char *)buf + off, txn - off);
        if (w > 0) off += (uint32_t)w;
        else       vTaskDelay(1);            /* FIFO full: yield, then re-check s_desired */
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Idle until a session is wanted. CONSOLE mode is a permanent no-op. Only
         * write s_desired in the CONSOLE case — a self-assign in AUTO/PASSTHROUGH
         * would be a read-modify-write that can clobber a concurrent
         * serial_bridge_request()/set_mode() store and drop the wakeup. */
        while (!s_desired || s_mode == SERIAL_MODE_CONSOLE) {
            if (s_mode == SERIAL_MODE_CONSOLE) s_desired = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        bridge_enter();
        while (s_desired)
            bridge_pump();
        bridge_exit();
    }
}

void serial_bridge_init(int mode, int baud)
{
    s_mode = mode;
    if (baud > 0) s_baud = baud;
    s_desired = (mode == SERIAL_MODE_PASSTHROUGH);   /* forced bridge starts active */
    xTaskCreatePinnedToCore(bridge_task, "serbridge", 4096, NULL, 3, &s_task, 0);
    ESP_LOGI(TAG, "init mode=%s baud=%d", serial_bridge_mode_str(mode), s_baud);
}

void serial_bridge_request(bool active)
{
    if (s_mode != SERIAL_MODE_AUTO) return;          /* FORCE_* ignores the auto-trigger */
    if (s_desired == active) return;                 /* self-dedup: cheap to call every batch */
    s_desired = active;
    if (s_task) xTaskNotifyGive(s_task);
}

void serial_bridge_set_mode(int mode)
{
    s_mode = mode;
    switch (mode) {
    case SERIAL_MODE_CONSOLE:     s_desired = false;     break;
    case SERIAL_MODE_PASSTHROUGH: s_desired = true;      break;
    case SERIAL_MODE_AUTO:        s_desired = s_active;  break;  /* keep current; next UARTEN edge re-decides */
    }
    if (s_task) xTaskNotifyGive(s_task);
}

int  serial_bridge_mode(void)   { return s_mode; }
bool serial_bridge_active(void) { return s_active; }

int serial_bridge_parse_mode(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "auto"))        return SERIAL_MODE_AUTO;
    if (!strcmp(s, "console"))     return SERIAL_MODE_CONSOLE;
    if (!strcmp(s, "passthrough")) return SERIAL_MODE_PASSTHROUGH;
    return -1;
}

const char *serial_bridge_mode_str(int mode)
{
    switch (mode) {
    case SERIAL_MODE_AUTO:        return "auto";
    case SERIAL_MODE_CONSOLE:     return "console";
    case SERIAL_MODE_PASSTHROUGH: return "passthrough";
    default:                      return "?";
    }
}
