/* Native ESP-IDF NV3041A QSPI driver for the JC4827W543.
 * Register settings and wire protocol follow Arduino_GFX 1.4.9, as used by
 * esp32-pipboy. See docs/third-party-notices.md for attribution and license.
 */
#include "nv3041a.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static spi_device_handle_t lcd;
static const char *TAG = "nv3041a";

static const uint8_t init_regs[][2] = {
    {0xff, 0xa5},
    {0x36, 0xc0},
    {0x3A, 0x01},
    {0x41, 0x03},
    {0x44, 0x15},
    {0x45, 0x15},
    {0x7d, 0x03},
    {0xc1, 0xbb},
    {0xc2, 0x05},
    {0xc3, 0x10},
    {0xc6, 0x3e},
    {0xc7, 0x25},
    {0xc8, 0x11},
    {0x7a, 0x5f},
    {0x6f, 0x44},
    {0x78, 0x70},
    {0xc9, 0x00},
    {0x67, 0x21},
    {0x51, 0x0a},
    {0x52, 0x76},
    {0x53, 0x0a},
    {0x54, 0x76},
    {0x46, 0x0a},
    {0x47, 0x2a},
    {0x48, 0x0a},
    {0x49, 0x1a},
    {0x56, 0x43},
    {0x57, 0x42},
    {0x58, 0x3c},
    {0x59, 0x64},
    {0x5a, 0x41},
    {0x5b, 0x3c},
    {0x5c, 0x02},
    {0x5d, 0x3c},
    {0x5e, 0x1f},
    {0x60, 0x80},
    {0x61, 0x3f},
    {0x62, 0x21},
    {0x63, 0x07},
    {0x64, 0xe0},
    {0x65, 0x02},
    {0xca, 0x20},
    {0xcb, 0x52},
    {0xcc, 0x10},
    {0xcD, 0x42},
    {0xD0, 0x20},
    {0xD1, 0x52},
    {0xD2, 0x10},
    {0xD3, 0x42},
    {0xD4, 0x0a},
    {0xD5, 0x32},
    {0x80, 0x00},
    {0xA0, 0x00},
    {0x81, 0x07},
    {0xA1, 0x06},
    {0x82, 0x02},
    {0xA2, 0x01},
    {0x86, 0x11},
    {0xA6, 0x10},
    {0x87, 0x27},
    {0xA7, 0x27},
    {0x83, 0x37},
    {0xA3, 0x37},
    {0x84, 0x35},
    {0xA4, 0x35},
    {0x85, 0x3f},
    {0xA5, 0x3f},
    {0x88, 0x0b},
    {0xA8, 0x0b},
    {0x89, 0x14},
    {0xA9, 0x14},
    {0x8a, 0x1a},
    {0xAa, 0x1a},
    {0x8b, 0x0a},
    {0xAb, 0x0a},
    {0x8c, 0x14},
    {0xAc, 0x08},
    {0x8d, 0x17},
    {0xAd, 0x07},
    {0x8e, 0x16},
    {0xAe, 0x06},
    {0x8f, 0x1B},
    {0xAf, 0x07},
    {0x90, 0x04},
    {0xB0, 0x04},
    {0x91, 0x0A},
    {0xB1, 0x0A},
    {0x92, 0x16},
    {0xB2, 0x15},
    {0xff, 0x00},
};

static esp_err_t command(uint8_t reg, const uint8_t *data, size_t size)
{
    spi_transaction_t t = {
        .cmd = 0x02,
        .addr = (uint32_t)reg << 8,
        .length = size * 8,
        .tx_buffer = data,
    };
    /* Command and address are single-lane; only pixel payloads use four lanes. */
    return spi_device_polling_transmit(lcd, &t);
}

void nv3041a_window(int x, int y, int w, int h)
{
    uint8_t col[] = { x >> 8, x, (x + w - 1) >> 8, x + w - 1 };
    uint8_t row[] = { y >> 8, y, (y + h - 1) >> 8, y + h - 1 };
    ESP_ERROR_CHECK(command(0x2a, col, sizeof(col)));
    ESP_ERROR_CHECK(command(0x2b, row, sizeof(row)));
    ESP_ERROR_CHECK(command(0x2c, NULL, 0));
}

void nv3041a_write_row(const uint8_t *rgb565, int width)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_MODE_QIO,
        .cmd = 0x32,
        .addr = 0x003c00,  /* RAM write continue; a fresh header for each row */
        .length = width * 16,
        .tx_buffer = rgb565,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &t));
}

void nv3041a_backlight(bool on)
{
    gpio_set_level(BOARD_LCD_BL, on);
}

esp_err_t nv3041a_init(void)
{
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight GPIO");
    nv3041a_backlight(false);
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_D0,
        .miso_io_num = BOARD_LCD_D1,
        .sclk_io_num = BOARD_LCD_CLK,
        .quadwp_io_num = BOARD_LCD_D2,
        .quadhd_io_num = BOARD_LCD_D3,
        .max_transfer_sz = BOARD_PANEL_W * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "QSPI bus init");
    spi_device_interface_config_t dev = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,
        .clock_speed_hz = 32000000,
        .spics_io_num = BOARD_LCD_CS,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(SPI3_HOST, &dev, &lcd);
    if (err != ESP_OK) {
        spi_bus_free(SPI3_HOST);
        return err;
    }
    err = command(0x01, NULL, 0); /* software reset; panel has no reset GPIO */
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));
        for (size_t i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++) {
            err = command(init_regs[i][0], &init_regs[i][1], 1);
            if (err != ESP_OK) break;
        }
    }
    if (err == ESP_OK) err = command(0x11, NULL, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));
        err = command(0x29, NULL, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        err = command(0x21, NULL, 0); /* IPS inversion, matching the Pip-Boy */
    }
    if (err != ESP_OK) {
        spi_bus_remove_device(lcd);
        lcd = NULL;
        spi_bus_free(SPI3_HOST);
    }
    return err;
}
