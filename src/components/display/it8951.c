#include "it8951.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "it8951";

/* M5 Paper GPIO assignments */
#define PIN_CS        15
#define PIN_SCK       14
#define PIN_MOSI      12
#define PIN_MISO      13
#define PIN_BUSY      27
#define PIN_EPD_PWR   23
#define PIN_EXT_PWR    5
#define PIN_MAIN_PWR   2
#define PIN_SD_CS      4

/* IT8951 commands */
#define CMD_SYS_RUN       0x0001
#define CMD_REG_RD        0x0010
#define CMD_REG_WR        0x0011
#define CMD_LD_IMG_AREA   0x0021
#define CMD_LD_IMG_END    0x0022
#define CMD_DPY_AREA      0x0034
#define CMD_GET_DEV_INFO  0x0302
#define CMD_DPY_BUF_AREA  0x0037
#define CMD_VCOM          0x0039

/* IT8951 registers */
#define REG_I80CPCR  0x0004
#define REG_LISAR    0x0208
#define REG_LUTAFSR  0x1224

#define PIX_4BPP      2
#define ENDIAN_BIG    1
#define ROTATE_0      0

/* Cap for waiting on a busy display engine (it8951_wait_ready / it8951_display_timed).
 * The slowest real waveform is a full-panel INIT at ~1.1 s and GC16 at ~0.5 s, so this
 * only trips on a genuinely wedged engine; bailing then frees the display task instead of
 * freezing it. Both wait sites derive from this one value so they can't drift apart. */
#define IT8951_BUSY_TIMEOUT_MS  6000

static spi_device_handle_t spi;

/* --- Low-level SPI (matches pvginkel/it8951-esp32 exactly) --- */

static void cs_low(void)  { gpio_set_level(PIN_CS, 0); }
static void cs_high(void) { gpio_set_level(PIN_CS, 1); }

static void wait_busy(void)
{
    uint32_t start = xTaskGetTickCount();
    while (!gpio_get_level(PIN_BUSY)) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(3000)) {
            /* HRDY never came back: the controller is wedged. We return so the task
             * isn't stuck forever, but the in-flight SPI command stream is now
             * desynced — log loud so a corrupted-panel symptom is traceable here. */
            ESP_LOGE(TAG, "HRDY timeout (3s) — SPI stream may be desynced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void spi_write16(uint16_t val)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,
        .tx_data = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF), 0, 0 },
    };
    spi_device_transmit(spi, &t);
}

static uint16_t spi_read16(void)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_RXDATA,
        .length = 16,
    };
    spi_device_transmit(spi, &t);
    return ((uint16_t)t.rx_data[0] << 8) | t.rx_data[1];
}

static void spi_write_buf(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_transmit(spi, &t);
}

/* --- IT8951 protocol (matches pvginkel pattern) --- */

static void write_command(uint16_t cmd)
{
    cs_low();
    wait_busy();
    spi_write16(0x6000);
    wait_busy();
    spi_write16(cmd);
    cs_high();
}

static void write_data(uint16_t data)
{
    cs_low();
    wait_busy();
    spi_write16(0x0000);
    wait_busy();
    spi_write16(data);
    cs_high();
}

static uint16_t read_data(void)
{
    cs_low();
    wait_busy();
    spi_write16(0x1000);
    wait_busy();
    spi_read16();
    wait_busy();
    uint16_t val = spi_read16();
    cs_high();
    return val;
}

static void read_data_buf(uint8_t *buf, size_t len)
{
    cs_low();
    wait_busy();
    spi_write16(0x1000);
    wait_busy();
    spi_read16();
    wait_busy();
    spi_transaction_t t = {
        .length = len * 8,
        .rx_buffer = buf,
    };
    spi_device_transmit(spi, &t);
    /* byte-swap 16-bit words; `i + 1 < len` so an odd trailing byte (no pair to
     * swap) is never read out of bounds. */
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t tmp = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = tmp;
    }
    cs_high();
}

static void write_reg(uint16_t reg, uint16_t val)
{
    write_command(CMD_REG_WR);
    write_data(reg);
    write_data(val);
}

static uint16_t read_reg(uint16_t reg)
{
    write_command(CMD_REG_RD);
    write_data(reg);
    return read_data();
}

esp_err_t it8951_init(it8951_dev_info_t *info)
{
    /* Use gpio_config for proper IO_MUX setup (not just gpio_set_direction) */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_MAIN_PWR) | (1ULL << PIN_EXT_PWR) |
                        (1ULL << PIN_EPD_PWR)  | (1ULL << PIN_SD_CS) |
                        (1ULL << PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    /* Power on sequence (from M5EPD.cpp) */
    gpio_set_level(PIN_MAIN_PWR, 1);
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(PIN_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(PIN_EXT_PWR, 1);

    /* SPI bus init — matches pvginkel driver exactly. Done once; the IT8951
     * is (re)reset by power-cycling its rail (EPD_PWR) in the retry loop. */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_MASTER_FREQ_10M,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI3_HOST, &dev_cfg, &spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Bring up the IT8951 with retries.  A warm ESP32 reset can leave the
     * controller mid-SPI-transaction, so the device-info read comes back
     * corrupted (garbled FW string, bogus buffer address) and HRDY hangs.
     * Power-cycling its rail (EPD_PWR low->high) forces a clean cold start;
     * validate the readback and retry with a longer reset pulse until sane. */
    const int MAX_TRIES = 6;
    bool ok = false;
    for (int attempt = 1; attempt <= MAX_TRIES && !ok; attempt++) {
        /* Fully discharge the IT8951 rail (longer pulse each retry) so it
         * cold-starts cleanly, then give it ample time to boot before SPI. */
        gpio_set_level(PIN_EPD_PWR, 0);
        vTaskDelay(pdMS_TO_TICKS(400 + attempt * 300));
        gpio_set_level(PIN_EPD_PWR, 1);
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGI(TAG, "Waiting for IT8951... (try %d/%d)", attempt, MAX_TRIES);
        wait_busy();
        write_command(CMD_SYS_RUN);

        /* Read device info */
        uint16_t dev_buf[20] = {0};
        write_command(CMD_GET_DEV_INFO);
        read_data_buf((uint8_t *)dev_buf, sizeof(dev_buf));

        info->panel_w = dev_buf[0];
        info->panel_h = dev_buf[1];
        info->img_buf_addr = ((uint32_t)dev_buf[3] << 16) | dev_buf[2];
        memcpy(info->fw_version, &dev_buf[4], 16);
        memcpy(info->lut_version, &dev_buf[12], 16);

        ESP_LOGI(TAG, "Panel: %dx%d  buf=0x%08lX",
                 info->panel_w, info->panel_h,
                 (unsigned long)info->img_buf_addr);
        ESP_LOGI(TAG, "FW: %.16s  LUT: %.16s",
                 info->fw_version, info->lut_version);

        /* M5Paper panel is 960x540, the image buffer lives low in IT8951 RAM
         * (~0x1236E0), and the firmware string starts with "5mtsca". Checking
         * the FW string is essential: a partially-desynced chip can return
         * correct dims+buffer but a garbled, repeating FW ("5m5m5m...") and
         * then time out on every subsequent HRDY (display never refreshes). */
        ok = (info->panel_w == 960 && info->panel_h == 540 &&
              info->img_buf_addr >= 0x10000 && info->img_buf_addr < 0x1000000 &&
              memcmp(info->fw_version, "5mtsca", 6) == 0);
        if (!ok)
            ESP_LOGW(TAG, "IT8951 readback invalid; power-cycling and retrying");
    }

    if (!ok) {
        ESP_LOGE(TAG, "IT8951 did not come up cleanly after %d tries; "
                      "continuing with default panel params", MAX_TRIES);
        info->panel_w = 960;
        info->panel_h = 540;
        info->img_buf_addr = 0x001236E0;
    }

    /* Enable pack write */
    write_reg(REG_I80CPCR, 0x0001);

    /* Set VCOM */
    write_command(CMD_VCOM);
    write_data(0x0001);
    write_data(2300);

    /* Set target memory address */
    write_reg(REG_LISAR + 2, (uint16_t)(info->img_buf_addr >> 16));
    write_reg(REG_LISAR, (uint16_t)(info->img_buf_addr & 0xFFFF));

    return ESP_OK;
}

void it8951_wait_ready(void)
{
    /* IT8951_BUSY_TIMEOUT_MS cap (shared with it8951_display_timed). The 10 ms poll body
     * means the loop runs TIMEOUT_MS / 10 times. Was 30 s, which froze the display task far
     * too long on a stuck panel. */
    for (int i = 0; i < IT8951_BUSY_TIMEOUT_MS / 10; i++) {
        if (read_reg(REG_LUTAFSR) == 0)
            return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "Display engine timeout (%ds)", IT8951_BUSY_TIMEOUT_MS / 1000);
}

void it8951_load_area_start(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    write_command(CMD_LD_IMG_AREA);
    write_data((ENDIAN_BIG << 8) | (PIX_4BPP << 4) | ROTATE_0);
    write_data(x);
    write_data(y);
    write_data(w);
    write_data(h);

    cs_low();
    wait_busy();
    spi_write16(0x0000);
    wait_busy();
}

void it8951_load_pixels(const uint8_t *data_4bpp, size_t len)
{
    while (len > 0) {
        size_t n = (len > 2048) ? 2048 : len;
        spi_write_buf(data_4bpp, n);
        data_4bpp += n;
        len -= n;
    }
}

void it8951_load_end(void)
{
    cs_high();
    write_command(CMD_LD_IMG_END);
}

void it8951_display_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode)
{
    write_command(CMD_DPY_AREA);
    write_data(x);
    write_data(y);
    write_data(w);
    write_data(h);
    write_data(mode);
}

void it8951_clear(uint16_t mode)
{
    uint8_t white[480] __attribute__((aligned(4)));
    memset(white, 0xFF, sizeof(white));

    it8951_load_area_start(0, 0, 960, 540);
    for (int y = 0; y < 540; y++)
        it8951_load_pixels(white, 480);
    it8951_load_end();

    it8951_display_area(0, 0, 960, 540, mode);
    it8951_wait_ready();
}

void it8951_fill_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t pix4)
{
    /* One max-width 4bpp row (2 px/byte; long panel axis <= 960 px -> 480 B). */
    static uint8_t fill[960 / 2] __attribute__((aligned(4)));
    size_t bytes = (size_t)w / 2;
    if (bytes > sizeof(fill)) bytes = sizeof(fill);
    memset(fill, pix4, bytes);

    it8951_load_area_start(x, y, w, h);
    for (int row = 0; row < h; row++)
        it8951_load_pixels(fill, bytes);
    it8951_load_end();
}

int64_t it8951_display_timed(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode)
{
    int64_t t0 = esp_timer_get_time();
    it8951_display_area(x, y, w, h, mode);

    /* Tight LUTAFSR poll (each read_reg is several SPI exchanges, ~tens of us, so
     * this is not a no-op hot spin) for microsecond-accurate completion timing.
     * To keep the lower-priority core-0 idle task / task-watchdog fed during a
     * long wait (a full-panel INIT can run a couple of seconds), drop a single
     * 1-tick delay every ~100 ms; that costs at most ~1 tick of detection latency
     * and never affects the elapsed total (esp_timer measures real wall-clock).
     * The IT8951_BUSY_TIMEOUT_MS guard only trips on a stuck engine. */
    int64_t last_feed = t0;
    while (read_reg(REG_LUTAFSR) != 0) {
        int64_t now = esp_timer_get_time();
        if (now - t0 > (int64_t)IT8951_BUSY_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "display_timed: engine busy >%ds (mode %u)",
                     IT8951_BUSY_TIMEOUT_MS / 1000, mode);
            break;
        }
        if (now - last_feed > 100000) {
            vTaskDelay(1);
            last_feed = esp_timer_get_time();
        }
    }
    return esp_timer_get_time() - t0;
}
