#include "hal/audio.hpp"

#include <math.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "audio";

namespace {

constexpr uint32_t SAMPLE_RATE_HZ   = 32000;
constexpr size_t   FRAMES_PER_BUF   = 256; // 8 ms per DMA chunk at 32kHz
constexpr int      MAX_VOICES       = 4;
constexpr float    TWO_PI           = 6.28318530718f;

struct SynthVoice {
    bool     active        = false;
    Waveform wave          = Waveform::SINE;
    float    phase         = 0.0f;
    float    phase_inc     = 0.0f;
    float    gain          = 0.0f;
    uint32_t frames_left   = 0;
    uint32_t total_frames  = 1;
    uint32_t lfsr          = 0xACE1u;
};

i2s_chan_handle_t s_tx_chan   = nullptr;
es8311_handle_t   s_es_handle = nullptr;
SemaphoreHandle_t s_voice_mtx = nullptr;
SynthVoice        s_voices[MAX_VOICES];

portMUX_TYPE s_clock_mux   = portMUX_INITIALIZER_UNLOCKED;
uint64_t     s_played_frames = 0;

inline float sample_voice(SynthVoice& v) {
    float out = 0.0f;
    switch (v.wave) {
        case Waveform::SINE:
            out = sinf(v.phase * TWO_PI);
            break;
        case Waveform::SQUARE:
            out = (v.phase < 0.5f) ? 1.0f : -1.0f;
            break;
        case Waveform::TRIANGLE:
            out = 4.0f * fabsf(v.phase - 0.5f) - 1.0f;
            break;
        case Waveform::NOISE: {
            // Advance Galois LFSR once per cycle wrap or each sample for crisp percussion
            const uint32_t lsb = v.lfsr & 1u;
            v.lfsr >>= 1;
            if (lsb) v.lfsr ^= 0xB400u;
            out = (v.lfsr & 1u) ? 1.0f : -1.0f;
            break;
        }
    }

    v.phase += v.phase_inc;
    if (v.phase >= 1.0f) {
        v.phase -= floorf(v.phase);
    }

    // Simple linear decay envelope to avoid clicks at note end
    const float env = static_cast<float>(v.frames_left) / static_cast<float>(v.total_frames);
    return out * v.gain * env;
}

void audio_task(void* /*arg*/) {
    int16_t pcm_buf[FRAMES_PER_BUF * 2];

    while (true) {
        xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
        for (size_t i = 0; i < FRAMES_PER_BUF; ++i) {
            float mix = 0.0f;
            for (int v = 0; v < MAX_VOICES; ++v) {
                if (!s_voices[v].active) continue;
                mix += sample_voice(s_voices[v]);
                if (--s_voices[v].frames_left == 0) {
                    s_voices[v].active = false;
                }
            }
            if (mix > 1.0f) mix = 1.0f;
            if (mix < -1.0f) mix = -1.0f;
            const int16_t s = static_cast<int16_t>(mix * 28000.0f);
            pcm_buf[i * 2]     = s;
            pcm_buf[i * 2 + 1] = s;
        }
        xSemaphoreGive(s_voice_mtx);

        size_t bytes_written = 0;
        if (i2s_channel_write(s_tx_chan, pcm_buf, sizeof(pcm_buf), &bytes_written, portMAX_DELAY) == ESP_OK) {
            const uint64_t frames_written = bytes_written / (sizeof(int16_t) * 2);
            portENTER_CRITICAL(&s_clock_mux);
            s_played_frames += frames_written;
            portEXIT_CRITICAL(&s_clock_mux);
        }
    }
}

bool init_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear        = true;
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg          = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg         = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk    = static_cast<gpio_num_t>(PIN_I2S_MCLK);
    std_cfg.gpio_cfg.bclk    = static_cast<gpio_num_t>(PIN_I2S_BCLK);
    std_cfg.gpio_cfg.ws      = static_cast<gpio_num_t>(PIN_I2S_LRCK);
    std_cfg.gpio_cfg.dout    = static_cast<gpio_num_t>(PIN_I2S_DOUT);
    std_cfg.gpio_cfg.din     = GPIO_NUM_NC;

    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        return false;
    }
    if (i2s_channel_enable(s_tx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        return false;
    }
    return true;
}

bool init_es8311() {
    s_es_handle = es8311_create(I2C_SHARED_PORT, ES8311_ADDRRES_0);
    if (!s_es_handle) {
        ESP_LOGE(TAG, "es8311_create failed");
        return false;
    }

    const es8311_clock_config_t clk_cfg = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = static_cast<int>(SAMPLE_RATE_HZ * 256),
        .sample_frequency   = static_cast<int>(SAMPLE_RATE_HZ),
    };

    if (es8311_init(s_es_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        ESP_LOGE(TAG, "es8311_init failed");
        return false;
    }
    ESP_ERROR_CHECK(es8311_sample_frequency_config(s_es_handle, clk_cfg.mclk_frequency, clk_cfg.sample_frequency));
    ESP_ERROR_CHECK(es8311_microphone_config(s_es_handle, false));
    ESP_ERROR_CHECK(es8311_voice_volume_set(s_es_handle, 65, nullptr));
    return true;
}

} // namespace

bool audio_init() {
    // Keep FM8002E muted during I2S/codec startup to avoid pops
    gpio_config_t amp_io = {};
    amp_io.pin_bit_mask  = (1ULL << PIN_AMP_EN);
    amp_io.mode          = GPIO_MODE_OUTPUT;
    gpio_config(&amp_io);
    gpio_set_level(static_cast<gpio_num_t>(PIN_AMP_EN), 1);

    s_voice_mtx = xSemaphoreCreateMutex();
    if (!s_voice_mtx) {
        return false;
    }

    // Start I2S (and MCLK) BEFORE es8311_init, because ES8311 needs an active MCLK
    if (!init_i2s() || !init_es8311()) {
        return false;
    }

    // Enable FM8002E speaker amplifier (Active LOW)
    gpio_set_level(static_cast<gpio_num_t>(PIN_AMP_EN), 0);

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, nullptr, configMAX_PRIORITIES - 3, nullptr, 1);
    ESP_LOGI(TAG, "Audio initialized (%lu Hz, ES8311 + FM8002E enabled)", static_cast<unsigned long>(SAMPLE_RATE_HZ));
    return true;
}

void audio_set_volume(uint8_t percent) {
    if (!s_es_handle) return;
    if (percent > 100) percent = 100;
    es8311_voice_volume_set(s_es_handle, percent, nullptr);
}

void audio_play_tone(float freq_hz, uint32_t duration_ms, Waveform wave, float gain) {
    if (!s_voice_mtx || duration_ms == 0) return;

    const uint32_t frames = (SAMPLE_RATE_HZ * duration_ms) / 1000;
    if (frames == 0) return;

    xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
    int slot = 0;
    uint32_t min_left = UINT32_MAX;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!s_voices[i].active) {
            slot = i;
            break;
        }
        if (s_voices[i].frames_left < min_left) {
            min_left = s_voices[i].frames_left;
            slot     = i;
        }
    }

    SynthVoice& v  = s_voices[slot];
    v.active       = true;
    v.wave         = wave;
    v.phase        = 0.0f;
    v.phase_inc    = freq_hz / static_cast<float>(SAMPLE_RATE_HZ);
    v.gain         = gain;
    v.frames_left  = frames;
    v.total_frames = frames;
    xSemaphoreGive(s_voice_mtx);
}

uint64_t audio_get_played_frames() {
    portENTER_CRITICAL(&s_clock_mux);
    const uint64_t frames = s_played_frames;
    portEXIT_CRITICAL(&s_clock_mux);
    return frames;
}

uint32_t audio_get_playback_ms() {
    return static_cast<uint32_t>((audio_get_played_frames() * 1000ULL) / SAMPLE_RATE_HZ);
}
