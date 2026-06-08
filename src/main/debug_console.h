#pragma once

/* Minimal line-protocol debug console over the ESP32 console UART (UART0 — the
 * same port flash.ps1 -Monitor opens). Lets a human or a pyserial script drive
 * and interrogate the emulator without physical interaction: input injection
 * and state queries. Responses are single lines prefixed with "= " so a script
 * can pick them out of the interleaved ESP_LOG stream. Type "help" for the
 * command list. Starts one task on core 0; safe to call once from app_main. The
 * console task is the single owner of UART0 reads: in passthrough it forwards
 * bytes to the emulated serial port, otherwise it dispatches console commands. */
void debug_console_start(void);
