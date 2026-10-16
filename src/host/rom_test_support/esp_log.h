#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) ((void)(tag), fprintf(stderr, fmt "\n", ##__VA_ARGS__))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW ESP_LOGE
