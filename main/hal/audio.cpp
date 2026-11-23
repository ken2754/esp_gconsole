#include "hal/audio.hpp"

#include <math.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/display.hpp"

static const char* TAG = "audio";

namespace {

constexpr uint32_t SAMPLE_RATE_HZ   = 32000;
constexpr size_t   FRAMES_PER_BUF   = 256; // 8 ms per DMA chunk at 32kHz
constexpr int      MAX_VOICES       = 8;
constexpr float    TWO_PI           = 6.28318530718f;
constexpr uint32_t ATTACK_FRAMES    = 48;  // 1.5 ms soft attack to avoid pops

// ES8311 Register Addresses
constexpr uint8_t ES8311_RESET_REG00       = 0x00;
constexpr uint8_t ES8311_CLK_MANAGER_REG01 = 0x01;
constexpr uint8_t ES8311_CLK_MANAGER_REG02 = 0x02;
constexpr uint8_t ES8311_CLK_MANAGER_REG03 = 0x03;
constexpr uint8_t ES8311_CLK_MANAGER_REG04 = 0x04;
constexpr uint8_t ES8311_CLK_MANAGER_REG05 = 0x05;
constexpr uint8_t ES8311_CLK_MANAGER_REG06 = 0x06;
constexpr uint8_t ES8311_CLK_MANAGER_REG07 = 0x07;
constexpr uint8_t ES8311_CLK_MANAGER_REG08 = 0x08;
constexpr uint8_t ES8311_SDPIN_REG09       = 0x09;
constexpr uint8_t ES8311_SDPOUT_REG0A      = 0x0A;
constexpr uint8_t ES8311_SYSTEM_REG0D      = 0x0D;
constexpr uint8_t ES8311_SYSTEM_REG0E      = 0x0E;
constexpr uint8_t ES8311_SYSTEM_REG12      = 0x12;
constexpr uint8_t ES8311_SYSTEM_REG13      = 0x13;
constexpr uint8_t ES8311_SYSTEM_REG14      = 0x14;
constexpr uint8_t ES8311_ADC_REG17         = 0x17;
constexpr uint8_t ES8311_ADC_REG1C         = 0x1C;
constexpr uint8_t ES8311_DAC_REG31         = 0x31;
constexpr uint8_t ES8311_DAC_REG32         = 0x32;
constexpr uint8_t ES8311_DAC_REG37         = 0x37;

struct SynthVoice {
    bool     active         = false;
    bool     is_bgm         = false;
    Waveform wave           = Waveform::SINE;
    float    phase          = 0.0f;
    float    phase_inc      = 0.0f;
    float    phase_inc_step = 0.0f;
    float    gain           = 0.0f;
    uint32_t frames_left    = 0;
    uint32_t total_frames   = 1;
    uint32_t lfsr           = 0xACE1u;
};

i2s_chan_handle_t     s_tx_chan    = nullptr;
i2c_master_dev_handle_t s_es_dev   = nullptr;
SemaphoreHandle_t     s_voice_mtx  = nullptr;
SynthVoice            s_voices[MAX_VOICES];
uint8_t               s_volume_pct = 65;

portMUX_TYPE s_clock_mux     = portMUX_INITIALIZER_UNLOCKED;
uint64_t     s_played_frames = 0;

// BGM Sequencer state
const BgmNote* s_bgm_notes        = nullptr;
size_t         s_bgm_count        = 0;
size_t         s_bgm_next_idx     = 0;
uint64_t       s_bgm_frame        = 0;
uint64_t       s_bgm_loop_frames  = 0;
uint64_t       s_bgm_clock_frames = 0;
bool           s_bgm_loop         = false;
bool           s_bgm_playing      = false;
bool           s_bgm_paused       = false;

esp_err_t es8311_write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es_dev, buf, sizeof(buf), 1000);
}

esp_err_t es8311_read_reg(uint8_t reg, uint8_t* val) {
    return i2c_master_transmit_receive(s_es_dev, &reg, 1, val, 1, 1000);
}

esp_err_t es8311_set_dac_volume(uint8_t percent) {
    if (!s_es_dev) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;
    const uint8_t reg32 = (percent == 0) ? 0 : static_cast<uint8_t>(((static_cast<uint16_t>(percent) * 256U) / 100U) - 1U);
    return es8311_write_reg(ES8311_DAC_REG32, reg32);
}

int alloc_voice_slot(bool prefer_bgm) {
    int free_slot = -1;
    int best_steal = 0;
    uint32_t min_left = UINT32_MAX;

    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!s_voices[i].active) {
            free_slot = i;
            break;
        }
        if (prefer_bgm && !s_voices[i].is_bgm) {
            continue;
        }
        if (s_voices[i].frames_left < min_left) {
            min_left   = s_voices[i].frames_left;
            best_steal = i;
        }
    }
    return (free_slot >= 0) ? free_slot : best_steal;
}

void start_voice_locked(float start_freq_hz, float end_freq_hz, uint32_t duration_ms,
                        Waveform wave, float gain, bool is_bgm) {
    const uint32_t frames = (SAMPLE_RATE_HZ * duration_ms) / 1000;
    if (frames == 0) return;

    const int slot = alloc_voice_slot(is_bgm);
    SynthVoice& v  = s_voices[slot];

    const float inc_start = start_freq_hz / static_cast<float>(SAMPLE_RATE_HZ);
    const float inc_end   = (end_freq_hz > 0.0f ? end_freq_hz : start_freq_hz) / static_cast<float>(SAMPLE_RATE_HZ);

    v.active         = true;
    v.is_bgm         = is_bgm;
    v.wave           = wave;
    v.phase          = 0.0f;
    v.phase_inc      = inc_start;
    v.phase_inc_step = (inc_end - inc_start) / static_cast<float>(frames);
    v.gain           = gain;
    v.frames_left    = frames;
    v.total_frames   = frames;
}

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
    v.phase_inc += v.phase_inc_step;
    if (v.phase_inc < 0.0f) {
        v.phase_inc = 0.0f;
    }

    const uint32_t elapsed = v.total_frames - v.frames_left;
    float env = static_cast<float>(v.frames_left) / static_cast<float>(v.total_frames);
    if (elapsed < ATTACK_FRAMES) {
        env *= static_cast<float>(elapsed) / static_cast<float>(ATTACK_FRAMES);
    }
    return out * v.gain * env;
}

void step_bgm_locked() {
    if (!s_bgm_playing || s_bgm_paused || !s_bgm_notes) {
        return;
    }

    while (s_bgm_next_idx < s_bgm_count) {
        const BgmNote& note = s_bgm_notes[s_bgm_next_idx];
        const uint64_t note_frame = (static_cast<uint64_t>(note.time_ms) * SAMPLE_RATE_HZ) / 1000ULL;
        if (s_bgm_frame < note_frame) {
            break;
        }
        if (note.freq_hz > 0 && note.duration_ms > 0) {
            const float gain = static_cast<float>(note.gain_pct) * 0.005f;
            start_voice_locked(static_cast<float>(note.freq_hz),
                               static_cast<float>(note.end_freq_hz),
                               note.duration_ms,
                               note.wave,
                               gain,
                               true);
        }
        ++s_bgm_next_idx;
    }

    ++s_bgm_frame;
    if (s_bgm_loop_frames > 0 && s_bgm_frame >= s_bgm_loop_frames) {
        if (s_bgm_loop) {
            s_bgm_frame    = 0;
            s_bgm_next_idx = 0;
        } else {
            s_bgm_playing = false;
        }
    }
}

void audio_task(void* /*arg*/) {
    int16_t pcm_buf[FRAMES_PER_BUF * 2];

    while (true) {
        xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
        for (size_t i = 0; i < FRAMES_PER_BUF; ++i) {
            step_bgm_locked();

            float mix = 0.0f;
            for (int v = 0; v < MAX_VOICES; ++v) {
                if (!s_voices[v].active) continue;
                if (s_voices[v].is_bgm && s_bgm_paused) continue;
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
        const uint64_t bgm_frame_snapshot = s_bgm_frame;
        xSemaphoreGive(s_voice_mtx);

        size_t bytes_written = 0;
        if (i2s_channel_write(s_tx_chan, pcm_buf, sizeof(pcm_buf), &bytes_written, portMAX_DELAY) == ESP_OK) {
            const uint64_t frames_written = bytes_written / (sizeof(int16_t) * 2);
            portENTER_CRITICAL(&s_clock_mux);
            s_played_frames += frames_written;
            s_bgm_clock_frames = bgm_frame_snapshot;
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
    std_cfg.clk_cfg               = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg              = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk         = static_cast<gpio_num_t>(PIN_I2S_MCLK);
    std_cfg.gpio_cfg.bclk         = static_cast<gpio_num_t>(PIN_I2S_BCLK);
    std_cfg.gpio_cfg.ws           = static_cast<gpio_num_t>(PIN_I2S_LRCK);
    std_cfg.gpio_cfg.dout         = static_cast<gpio_num_t>(PIN_I2S_DOUT);
    std_cfg.gpio_cfg.din          = GPIO_NUM_NC;

    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        return false;
    }
    if (i2s_channel_enable(s_tx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // Allow MCLK to stabilize before ES8311 I2C init
    return true;
}

bool init_es8311() {
    i2c_master_bus_handle_t bus = display_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "Shared I2C bus not initialized");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address      = I2C_ADDR_ES8311;
    dev_cfg.scl_speed_hz        = 100000;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_es_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device for ES8311 failed");
        return false;
    }

    // Soft reset & power-on CSM
    auto wr = [](uint8_t reg, uint8_t val) -> bool {
        if (es8311_write_reg(reg, val) != ESP_OK) {
            ESP_LOGE(TAG, "ES8311 write reg 0x%02X failed", reg);
            return false;
        }
        return true;
    };
    auto rd = [](uint8_t reg, uint8_t* val) -> bool {
        if (es8311_read_reg(reg, val) != ESP_OK) {
            ESP_LOGE(TAG, "ES8311 read reg 0x%02X failed", reg);
            return false;
        }
        return true;
    };

    if (!wr(ES8311_RESET_REG00, 0x1F)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!wr(ES8311_RESET_REG00, 0x00)) return false;
    if (!wr(ES8311_RESET_REG00, 0x80)) return false;

    // Clock config for 32kHz with MCLK=256*Fs (8.192 MHz):
    // pre_div=1, pre_multi=0, adc_div=1, dac_div=1, fs_mode=0, lrck=0x00FF, bclk_div=4, osr=0x10
    if (!wr(ES8311_CLK_MANAGER_REG01, 0x3F)) return false;
    uint8_t regv = 0;
    if (!rd(ES8311_CLK_MANAGER_REG06, &regv)) return false;
    if (!wr(ES8311_CLK_MANAGER_REG06, static_cast<uint8_t>((regv & 0xC0) | 0x03))) return false;

    if (!rd(ES8311_CLK_MANAGER_REG02, &regv)) return false;
    if (!wr(ES8311_CLK_MANAGER_REG02, static_cast<uint8_t>(regv & 0x07))) return false;
    if (!wr(ES8311_CLK_MANAGER_REG03, 0x10)) return false;
    if (!wr(ES8311_CLK_MANAGER_REG04, 0x10)) return false;
    if (!wr(ES8311_CLK_MANAGER_REG05, 0x00)) return false;
    if (!rd(ES8311_CLK_MANAGER_REG07, &regv)) return false;
    if (!wr(ES8311_CLK_MANAGER_REG07, static_cast<uint8_t>(regv & 0xC0))) return false;
    if (!wr(ES8311_CLK_MANAGER_REG08, 0xFF)) return false;

    // Slave mode & 16-bit I2S format
    if (!rd(ES8311_RESET_REG00, &regv)) return false;
    if (!wr(ES8311_RESET_REG00, static_cast<uint8_t>(regv & 0xBF))) return false;
    if (!wr(ES8311_SDPIN_REG09, 0x0C)) return false;
    if (!wr(ES8311_SDPOUT_REG0A, 0x0C)) return false;

    // Analog & DAC power-up
    if (!wr(ES8311_SYSTEM_REG0D, 0x01)) return false;
    if (!wr(ES8311_SYSTEM_REG0E, 0x02)) return false;
    if (!wr(ES8311_SYSTEM_REG12, 0x00)) return false;
    if (!wr(ES8311_SYSTEM_REG13, 0x10)) return false;
    if (!wr(ES8311_ADC_REG1C, 0x6A)) return false;
    if (!wr(ES8311_DAC_REG37, 0x08)) return false;

    // Analog mic config + DAC unmute & volume
    if (!wr(ES8311_ADC_REG17, 0xC8)) return false;
    if (!wr(ES8311_SYSTEM_REG14, 0x1A)) return false;
    if (!wr(ES8311_DAC_REG31, 0x00)) return false;
    if (es8311_set_dac_volume(s_volume_pct) != ESP_OK) return false;
    return true;
}

} // namespace

bool audio_init() {
    gpio_config_t amp_io = {};
    amp_io.pin_bit_mask  = (1ULL << PIN_AMP_EN);
    amp_io.mode          = GPIO_MODE_OUTPUT;
    gpio_config(&amp_io);
    gpio_set_level(static_cast<gpio_num_t>(PIN_AMP_EN), 1);

    s_voice_mtx = xSemaphoreCreateMutex();
    if (!s_voice_mtx) {
        return false;
    }

    if (!init_i2s() || !init_es8311()) {
        return false;
    }

    gpio_set_level(static_cast<gpio_num_t>(PIN_AMP_EN), 0);

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, nullptr, configMAX_PRIORITIES - 3, nullptr, 1);
    ESP_LOGI(TAG, "Audio initialized (%lu Hz, ES8311 + FM8002E enabled)", static_cast<unsigned long>(SAMPLE_RATE_HZ));
    return true;
}

void audio_set_volume(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_volume_pct = percent;
    es8311_set_dac_volume(percent);
}

uint8_t audio_get_volume() {
    return s_volume_pct;
}

void audio_play_tone(float freq_hz, uint32_t duration_ms, Waveform wave, float gain) {
    audio_play_sweep(freq_hz, freq_hz, duration_ms, wave, gain);
}

void audio_play_sweep(float start_freq_hz, float end_freq_hz, uint32_t duration_ms,
                      Waveform wave, float gain) {
    if (!s_voice_mtx || duration_ms == 0) return;

    xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
    start_voice_locked(start_freq_hz, end_freq_hz, duration_ms, wave, gain, false);
    xSemaphoreGive(s_voice_mtx);
}

void audio_bgm_play(const BgmNote* notes, size_t count, uint32_t loop_len_ms, bool loop) {
    if (!s_voice_mtx) return;

    xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (s_voices[i].is_bgm) {
            s_voices[i].active = false;
        }
    }
    s_bgm_notes       = notes;
    s_bgm_count       = count;
    s_bgm_next_idx    = 0;
    s_bgm_frame       = 0;
    s_bgm_loop_frames = (static_cast<uint64_t>(loop_len_ms) * SAMPLE_RATE_HZ) / 1000ULL;
    s_bgm_loop        = loop;
    s_bgm_paused      = false;
    s_bgm_playing     = (notes != nullptr && count > 0);
    xSemaphoreGive(s_voice_mtx);

    portENTER_CRITICAL(&s_clock_mux);
    s_bgm_clock_frames = 0;
    portEXIT_CRITICAL(&s_clock_mux);
}

void audio_bgm_stop() {
    if (!s_voice_mtx) return;

    xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
    s_bgm_playing  = false;
    s_bgm_paused   = false;
    s_bgm_notes    = nullptr;
    s_bgm_count    = 0;
    s_bgm_next_idx = 0;
    s_bgm_frame    = 0;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (s_voices[i].is_bgm) {
            s_voices[i].active = false;
        }
    }
    xSemaphoreGive(s_voice_mtx);

    portENTER_CRITICAL(&s_clock_mux);
    s_bgm_clock_frames = 0;
    portEXIT_CRITICAL(&s_clock_mux);
}

void audio_bgm_set_paused(bool paused) {
    if (!s_voice_mtx) return;
    xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
    s_bgm_paused = paused;
    xSemaphoreGive(s_voice_mtx);
}

bool audio_bgm_is_playing() {
    if (!s_voice_mtx) return false;
    xSemaphoreTake(s_voice_mtx, portMAX_DELAY);
    const bool playing = s_bgm_playing;
    xSemaphoreGive(s_voice_mtx);
    return playing;
}

uint32_t audio_bgm_get_time_ms() {
    portENTER_CRITICAL(&s_clock_mux);
    const uint64_t frames = s_bgm_clock_frames;
    portEXIT_CRITICAL(&s_clock_mux);
    return static_cast<uint32_t>((frames * 1000ULL) / SAMPLE_RATE_HZ);
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
