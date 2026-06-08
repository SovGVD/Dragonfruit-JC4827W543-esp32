#pragma once
/* Minimal shim replacing ESP-IDF headers for host builds */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* GCC/Clang provide __sync_synchronize() as a builtin full memory barrier;
 * MSVC does not. The host build runs the emulator effectively single-threaded
 * (the UART rings are exercised from one thread in the test harness), so a
 * compiler reordering barrier is sufficient here. */
#if defined(_MSC_VER)
#include <intrin.h>
#define __sync_synchronize() _ReadWriteBarrier()
#endif

/* esp_log.h replacements. D/V are no-ops here, mirroring IDF where they are
 * compiled out below the default (INFO) log level — so debug-only traces stay
 * silent on the host harness just as they do on device. */
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

/* esp_heap_caps.h replacements */
#define MALLOC_CAP_SPIRAM    0
#define MALLOC_CAP_INTERNAL  0
#define MALLOC_CAP_8BIT      0
static inline void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
static inline size_t esp_get_free_heap_size(void) { return 0; }
