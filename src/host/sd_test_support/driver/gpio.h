#pragma once
#include <stdint.h>
#include "esp_err.h"
#define GPIO_MODE_OUTPUT 1
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *cfg);
int gpio_set_level(int pin, int level);
