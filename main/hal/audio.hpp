#pragma once

#include <stddef.h>
#include <stdint.h>

enum class Waveform : uint8_t {
    SINE = 0,
    SQUARE,
    TRIANGLE,
    NOISE,
};

/**
 * @brief Single note event for the sample-accurate BGM sequencer.
 *        Array of notes passed to audio_bgm_play() must be sorted by time_ms
 *        and remain valid while playing (typically static constexpr tables).
 */
struct BgmNote {
    uint32_t time_ms;
    uint16_t freq_hz;
    uint16_t end_freq_hz; // 0 => constant freq_hz
    uint16_t duration_ms;
    Waveform wave;
    uint8_t  gain_pct;    // 0..100
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
 * @brief Get current ES8311 DAC output volume (0..100).
 */
uint8_t audio_get_volume();

/**
 * @brief Trigger a synthesized tone/SFX on an available mixer voice.
 */
void audio_play_tone(float freq_hz, uint32_t duration_ms, Waveform wave = Waveform::SQUARE, float gain = 0.35f);

/**
 * @brief Trigger a frequency-swept tone/SFX (e.g., jump, stomp, drum drop).
 */
void audio_play_sweep(float start_freq_hz, float end_freq_hz, uint32_t duration_ms,
                      Waveform wave = Waveform::SQUARE, float gain = 0.35f);

/**
 * @brief Start sample-accurate BGM playback synchronized to the I2S DMA clock.
 * @param notes Pointer to sorted BgmNote array (must outlive playback).
 * @param count Number of notes in array.
 * @param loop_len_ms Total song/loop duration in ms.
 * @param loop Whether to loop continuously when reaching loop_len_ms.
 */
void audio_bgm_play(const BgmNote* notes, size_t count, uint32_t loop_len_ms, bool loop);

/**
 * @brief Stop BGM playback and silence active BGM voices.
 */
void audio_bgm_stop();

/**
 * @brief Pause or resume BGM playback without resetting song position.
 */
void audio_bgm_set_paused(bool paused);

/**
 * @brief Returns true if BGM is currently active (even if paused).
 */
bool audio_bgm_is_playing();

/**
 * @brief Returns current BGM position in milliseconds based on I2S sample frames.
 */
uint32_t audio_bgm_get_time_ms();

/**
 * @brief Returns total stereo frames written to I2S DMA since boot.
 *        Use this as the master clock for rhythm games to prevent audio-visual drift.
 */
uint64_t audio_get_played_frames();

/**
 * @brief Returns elapsed audio playback time in milliseconds.
 */
uint32_t audio_get_playback_ms();
