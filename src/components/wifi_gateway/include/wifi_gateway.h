#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Initialize once before starting the emulation task. Failure leaves UART0 usable. */
bool wifi_gateway_init(void);
bool wifi_gateway_enabled(void);

/* CPU-task handoff. Each UART reset supplies a new generation. Only this task
 * transfers UART bytes; lwIP and Wi-Fi never access emulated registers/rings. */
void wifi_gateway_request(bool open, uint32_t generation);
bool wifi_gateway_ready(void);
size_t wifi_gateway_write_space(void);
size_t wifi_gateway_write(const void *data, size_t len);
size_t wifi_gateway_read(void *data, size_t len);
