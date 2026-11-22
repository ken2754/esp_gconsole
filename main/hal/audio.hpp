#pragma once

#include <stdint.h>

enum class Waveform : uint8_t {
    SINE = 0,
    SQUARE,
    TRIANGLE,
    NOISE,
};

/**
 * @brief Initialize I2S0 (MCLK+BCLK+LRCK+DOUT), ES8311 codec (via I2C_NUM_0),
 *        FM8002E speaker amp (GPIO 1), and start the audio mixing task.
 *        Must be called AFTER display_init() so I2C_NUM_0 is already installed.
 * @return true on success.
 */
bool audio_init();

/**
 * @brief Set ES8311 DAC output volume (0..100). Safe to call at runtime.
 */
void audio_set_volume(uint8_t percent);

/**
 * @brief Trigger a synthesized tone/SFX on an available mixer voice.
 */
void audio_play_tone(float freq_hz, uint32_t duration_ms, Waveform wave = Waveform::SQUARE, float gain = 0.35f);

/**
 * @brief Returns total stereo frames written to I2S DMA since boot.
 *        Use this as the master clock for rhythm games to prevent audio-visual drift.
 */
uint64_t audio_get_played_frames();

/**
 * @brief Returns elapsed audio playback time in milliseconds.
 */
uint32_t audio_get_playback_ms();
