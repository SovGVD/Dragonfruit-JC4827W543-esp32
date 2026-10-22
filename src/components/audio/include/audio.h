#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Speaker output, independent of the emulated machine. A low-priority task
 * synthesizes a rectangle wave and streams it to the board's I2S amplifier;
 * while silent the I2S channel is disabled and the task sleeps.
 *
 * Any emulator that drives a piezo/PWM tone (Palm DragonBall PWM, Cybiko) maps
 * its hardware to audio_tone(). Boards without BOARD_HAS_AUDIO get stubs. */

/* Start the I2S channel and synthesis task. volume is 0..100; 0 leaves audio
 * off and returns ESP_OK without touching the I2S pins. */
esp_err_t audio_init(uint8_t volume);

/* 0..100, applied to the next tone. */
void audio_set_volume(uint8_t volume);

/* Play a rectangle wave at freq_hz with the given duty (0..1), replacing any
 * current tone. freq_hz <= 0, or duty 0 or 1 (no AC component), is silence.
 * Cheap and non-blocking: safe to call from an emulator CPU task on every
 * register write. Matches db_pwm_sink_t. */
void audio_tone(float freq_hz, float duty);

/* Sampled sound (the Cybiko MP3 cartridge): queue mono 16-bit samples at
 * AUDIO_RATE Hz, mixed with any tone and scaled by the volume. Blocks until
 * they fit, so a decoder calling it runs at playback speed. audio_pcm_stop()
 * drops what is queued and lets the speaker go quiet. */
#define AUDIO_RATE 32000
void audio_pcm_write(const int16_t *samples, size_t n);
void audio_pcm_stop(void);

/* The volume chosen in the boot menu, kept in NVS (nvs_flash_init() must have
 * run). It is the default for every emulated system; audio_saved_volume()
 * returns false while it has never been set. */
bool audio_saved_volume(uint8_t *volume);
void audio_save_volume(uint8_t volume);
