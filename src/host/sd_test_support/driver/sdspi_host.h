#pragma once
#include "esp_err.h"
#define SPI2_HOST 2
#define SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC (1 << 7)
#define SPI_DMA_CH_AUTO 3
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
typedef struct { int slot, max_freq_khz, flags; } sdmmc_host_t;
typedef struct { int gpio_cs, host_id; } sdspi_device_config_t;
#define SDSPI_HOST_DEFAULT() {0}
#define SDSPI_DEVICE_CONFIG_DEFAULT() {0}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma);
