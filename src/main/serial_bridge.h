#pragma once

#include <stdbool.h>

/*
 * Serial pass-through bridge.
 *
 * Connects the emulated MC68EZ328 UART byte pipe (components/dragonball/uart.c)
 * to the M5Paper's USB-C serial so a desktop HotSync can sync natively. The
 * M5Paper is a classic ESP32 with no native USB: its "USB serial" is a CP2104
 * bridge on UART0 — the SAME line the debug console and ESP_LOG output use. So
 * the bridge does not open a new pipe; it takes UART0 over for the duration of a
 * serial session and hands it back afterward.
 *
 * Mode is resolved from /palm.cfg (serial.mode):
 *   AUTO        - debug console by default; switch to passthrough while the ROM
 *                 holds its serial port open (USTCNT_UARTEN set), back to console
 *                 when it closes. This is the default.
 *   CONSOLE     - force the console; never bridge.
 *   PASSTHROUGH - force the raw bridge; the console is parked for the whole run.
 */

enum {
    SERIAL_MODE_AUTO = 0,
    SERIAL_MODE_CONSOLE,
    SERIAL_MODE_PASSTHROUGH,
};

/* Create the bridge task. `baud` is the UART0 line rate used while bridging
 * (e.g. 57600 for HotSync); the console rate is restored on exit. A `baud` <= 0
 * keeps the built-in 57600 default. Call AFTER debug_console_start() — the bridge
 * shares its UART0 driver and re-routes its reads while active. */
void serial_bridge_init(int mode, int baud);

/* Ask the bridge to enter (active=true) or leave (active=false) passthrough.
 * Honored only in AUTO mode; FORCE_* modes ignore it. Cheap and lock-free: it
 * only flips a flag and notifies the bridge task, which performs the actual
 * UART0/log/console reconfiguration on its own (core-0) context. Safe to call
 * from the cpu task. */
void serial_bridge_request(bool active);

/* Runtime override for the `serial` console command. */
void serial_bridge_set_mode(int mode);
int  serial_bridge_mode(void);
bool serial_bridge_active(void);

/* "auto"/"console"/"passthrough" -> SERIAL_MODE_*; -1 if unrecognized. */
int  serial_bridge_parse_mode(const char *s);
const char *serial_bridge_mode_str(int mode);
