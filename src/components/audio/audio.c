#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "nvs.h"

bool audio_saved_volume(uint8_t *volume)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("audio", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100;
    nvs_close(h);
    if (ok) *volume = v;
    return ok;
}

void audio_save_volume(uint8_t volume)
{
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "volume", volume > 100 ? 100 : volume);
    nvs_commit(h);
    nvs_close(h);
}

#if BOARD_HAS_AUDIO

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* 16-bit stereo Philips I2S, as the esp32-pipboy reference drives this amp.
 * Both slots carry the same mono sample. */
#define SAMPLE_RATE  32000
#define CHUNK_FRAMES 256        /* 8 ms per write */
#define DMA_DESC     4

static i2s_chan_handle_t tx;
static TaskHandle_t task;
static volatile uint32_t phase_inc;   /* Q32 cycles per sample; 0 = silent */
static volatile uint32_t duty_q32;    /* fraction of each cycle spent high */
static volatile int32_t level = 0;    /* peak-to-peak amplitude */
static uint8_t cur_volume;

static int32_t volume_level(uint8_t volume)
{
    if (volume > 100) volume = 100;
    return (int32_t)32767 * volume / 100;
}

void audio_set_volume(uint8_t volume)
{
    cur_volume = volume > 100 ? 100 : volume;
    level = volume_level(cur_volume);
}

void audio_tone(float freq_hz, float duty)
{
    if (!task) return;
    if (freq_hz <= 0.0f || freq_hz >= SAMPLE_RATE / 2 || duty <= 0.0f || duty >= 1.0f) {
        phase_inc = 0;
        return;
    }
    bool was_silent = phase_inc == 0;
    duty_q32 = (uint32_t)(duty * 4294967295.0f);
    phase_inc = (uint32_t)(freq_hz * (4294967296.0f / SAMPLE_RATE));
    if (was_silent) xTaskNotifyGive(task);
}

static void audio_task(void *arg)
{
    static int16_t buf[CHUNK_FRAMES * 2];
    uint32_t phase = 0;
    bool enabled = false;
    size_t written;

    for (;;) {
        uint32_t inc = phase_inc;
        if (!inc) {
            if (enabled) {
                /* Push the queued tone out with silence, then stop the clocks. */
                for (int i = 0; i < CHUNK_FRAMES * 2; i++) buf[i] = 0;
                for (int i = 0; i < DMA_DESC; i++)
                    i2s_channel_write(tx, buf, sizeof(buf), &written, portMAX_DELAY);
                i2s_channel_disable(tx);
                enabled = false;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!enabled) {
            phase = 0;
            if (i2s_channel_enable(tx) != ESP_OK) { phase_inc = 0; continue; }
            enabled = true;
        }

        /* Rectangle wave with its DC removed, so duty sets loudness the way a
         * piezo behaves: 50% is loudest, narrow pulses are quiet. */
        uint32_t duty = duty_q32;
        float d = (float)duty / 4294967296.0f;
        int16_t hi = (int16_t)(level * (1.0f - d));
        int16_t lo = (int16_t)(-level * d);
        for (int i = 0; i < CHUNK_FRAMES; i++) {
            int16_t s = phase < duty ? hi : lo;
            buf[2 * i] = buf[2 * i + 1] = s;
            phase += inc;
        }
        i2s_channel_write(tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

esp_err_t audio_init(uint8_t volume)
{
    audio_set_volume(volume);
    if (task) return ESP_OK;                 /* already running (boot menu) */
    if (!cur_volume) {
        ESP_LOGI(TAG, "sound disabled");
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC;
    chan_cfg.dma_frame_num = CHUNK_FRAMES;
    chan_cfg.auto_clear_after_cb = true;   /* underrun plays silence, not a loop */
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_LRCLK,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(tx);
        tx = NULL;
        return err;
    }

    /* Core 0, below input/display: a late tone chunk costs a click, not a
     * missed pen event. */
    if (xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 2, &task, 0) != pdPASS) {
        i2s_del_channel(tx);
        tx = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2S speaker ready, volume %u%%", cur_volume);
    return ESP_OK;
}

#else  /* !BOARD_HAS_AUDIO */

esp_err_t audio_init(uint8_t volume) { return ESP_OK; }
void audio_set_volume(uint8_t volume) {}
void audio_tone(float freq_hz, float duty) {}

#endif
