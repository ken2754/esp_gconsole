#include <memory>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "game/action_game.hpp"
#include "game/hw_test_game.hpp"
#include "game/launcher_game.hpp"
#include "game/rhythm_game.hpp"
#include "hal/audio.hpp"
#include "hal/display.hpp"
#include "hal/input.hpp"
#include "hal/save_data.hpp"

static const char* TAG = "main";

namespace {

std::unique_ptr<GameApp> create_game_app(AppId id) {
    switch (id) {
        case AppId::ACTION:  return std::make_unique<ActionGame>();
        case AppId::RHYTHM:  return std::make_unique<RhythmGame>();
        case AppId::HW_TEST: return std::make_unique<HwTestGame>();
        default:             return nullptr;
    }
}

} // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting esp_gconsole on ES3C28P...");

    // 0. Load persisted console settings & high scores from NVS
    save_data_init();
    const auto& saved = save_data_get();

    // 1. Initialize display + FT6336G touch (installs I2C_NUM_0)
    if (!display_init()) {
        ESP_LOGE(TAG, "Display init failed, halting");
        return;
    }
    display_set_brightness(saved.brightness_pct);

    // 2. Initialize 4-pin 6-button + BOOT GPIOs
    input_init();

    // 3. Initialize I2S + ES8311 (shares I2C_NUM_0) + FM8002E speaker amp
    if (!audio_init()) {
        ESP_LOGW(TAG, "Audio init failed; continuing without sound");
    } else {
        audio_set_volume(saved.volume_pct);
        // Play cheerful startup chime
        audio_play_tone(523.25f, 80, Waveform::SQUARE, 0.25f);
        vTaskDelay(pdMS_TO_TICKS(70));
        audio_play_tone(659.25f, 80, Waveform::SQUARE, 0.25f);
        vTaskDelay(pdMS_TO_TICKS(70));
        audio_play_tone(783.99f, 140, Waveform::SQUARE, 0.28f);
    }

    auto launcher = std::make_unique<LauncherGame>();
    std::unique_ptr<GameApp> current_game;

    GameApp* active_game = launcher.get();
    active_game->init();

    constexpr int64_t FRAME_PERIOD_US = 16667; // ~60 FPS
    int64_t last_us = esp_timer_get_time();

    while (true) {
        const int64_t frame_start_us = esp_timer_get_time();
        uint32_t dt_ms = static_cast<uint32_t>((frame_start_us - last_us) / 1000);
        if (dt_ms == 0) dt_ms = 1;
        if (dt_ms > 100) dt_ms = 100;
        last_us = frame_start_us;

        const InputState input = input_poll();
        active_game->update(dt_ms, input);

        // Handle transitions between Launcher and games
        if (active_game == launcher.get()) {
            const AppId req = launcher->take_launch_request();
            if (req != AppId::NONE) {
                launcher->on_exit();
                current_game = create_game_app(req);
                if (current_game) {
                    active_game = current_game.get();
                    active_game->init();
                }
            }
        } else if (active_game->wants_exit()) {
            active_game->on_exit();
            current_game.reset();
            active_game = launcher.get();
            active_game->init();
        }

        auto& fb = display_framebuffer();
        active_game->draw(fb);
        display_present();

        const int64_t elapsed_us = esp_timer_get_time() - frame_start_us;
        uint32_t sleep_ms = 1; // Always yield >=1ms so IDLE0 resets task_wdt
        if (elapsed_us < FRAME_PERIOD_US) {
            const uint32_t rem_ms = static_cast<uint32_t>((FRAME_PERIOD_US - elapsed_us) / 1000);
            if (rem_ms > 1) {
                sleep_ms = rem_ms;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}
