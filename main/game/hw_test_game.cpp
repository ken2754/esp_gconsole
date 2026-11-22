#include "game/hw_test_game.hpp"

#include <stdio.h>

#include "board_pins.h"
#include "esp_psram.h"
#include "hal/audio.hpp"

namespace {

constexpr uint8_t BRIGHTNESS_LEVELS[] = {30, 70, 100};

struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr Rect RECT_DON    = {18, 128, 134, 68};
constexpr Rect RECT_KA     = {168, 128, 134, 68};
constexpr Rect RECT_BRIGHT = {18, 206, 134, 26};
constexpr Rect RECT_MODE   = {168, 206, 134, 26};

const char* mode_label(InputMode m) {
    switch (m) {
        case InputMode::LR_AB:     return "MODE: LR+AB";
        case InputMode::DPAD_4WAY: return "MODE: 4WAY";
        case InputMode::BOTH:      return "MODE: BOTH";
    }
    return "MODE: ?";
}

void draw_key_box(lgfx::LGFX_Sprite& fb, int16_t x, int16_t y, int16_t w, int16_t h,
                  const char* label, bool active, uint16_t active_color) {
    const uint16_t fill   = active ? active_color : TFT_DARKGREY;
    const uint16_t border = active ? TFT_WHITE : TFT_LIGHTGREY;
    const uint16_t text   = active ? TFT_BLACK : TFT_WHITE;
    fb.fillRoundRect(x, y, w, h, 6, fill);
    fb.drawRoundRect(x, y, w, h, 6, border);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(text);
    fb.drawString(label, x + w / 2, y + h / 2);
}

} // namespace

void HwTestGame::init() {
    input_set_mode(InputMode::BOTH);
    display_set_brightness(BRIGHTNESS_LEVELS[_brightness_idx]);
}

void HwTestGame::update(uint32_t dt_ms, const InputState& input) {
    _last_held = input.held;

    if (_ripple_ms > dt_ms)    _ripple_ms -= dt_ms;    else _ripple_ms = 0;
    if (_don_flash_ms > dt_ms) _don_flash_ms -= dt_ms; else _don_flash_ms = 0;
    if (_ka_flash_ms > dt_ms)  _ka_flash_ms -= dt_ms;  else _ka_flash_ms = 0;

    // FPS counter
    ++_frame_acc;
    _time_acc_ms += dt_ms;
    if (_time_acc_ms >= 500) {
        _fps         = (_frame_acc * 1000) / _time_acc_ms;
        _frame_acc   = 0;
        _time_acc_ms = 0;
    }

    // Physical button SFX
    if (input.pressed & BTN_LEFT)  audio_play_tone(523.25f, 90, Waveform::SQUARE, 0.30f);   // C5
    if (input.pressed & BTN_RIGHT) audio_play_tone(587.33f, 90, Waveform::SQUARE, 0.30f);   // D5
    if (input.pressed & (BTN_A | BTN_UP))   audio_play_tone(659.25f, 100, Waveform::SQUARE, 0.30f); // E5
    if (input.pressed & (BTN_B | BTN_DOWN)) audio_play_tone(783.99f, 100, Waveform::SQUARE, 0.30f); // G5
    if (input.pressed & BTN_PAUSE) audio_play_tone(1046.50f, 120, Waveform::TRIANGLE, 0.35f); // C6

    // Capacitive touch interactions
    if (input.touch.down) {
        _last_tx = input.touch.x;
        _last_ty = input.touch.y;
    }

    if (input.touch.just_pressed) {
        _ripple_ms = 220;
        if (RECT_DON.contains(input.touch.x, input.touch.y)) {
            _don_flash_ms = 140;
            audio_play_tone(130.81f, 95, Waveform::TRIANGLE, 0.55f); // Low drum body
            audio_play_tone(90.0f, 60, Waveform::SINE, 0.45f);
        } else if (RECT_KA.contains(input.touch.x, input.touch.y)) {
            _ka_flash_ms = 140;
            audio_play_tone(880.0f, 55, Waveform::NOISE, 0.35f);     // Rim click
            audio_play_tone(1174.66f, 60, Waveform::SQUARE, 0.25f);
        } else if (RECT_BRIGHT.contains(input.touch.x, input.touch.y)) {
            _brightness_idx = (_brightness_idx + 1) % 3;
            display_set_brightness(BRIGHTNESS_LEVELS[_brightness_idx]);
            audio_play_tone(660.0f, 50, Waveform::SINE, 0.25f);
        } else if (RECT_MODE.contains(input.touch.x, input.touch.y)) {
            const InputMode cur = input_get_mode();
            InputMode next = InputMode::BOTH;
            if (cur == InputMode::BOTH)           next = InputMode::LR_AB;
            else if (cur == InputMode::LR_AB)     next = InputMode::DPAD_4WAY;
            input_set_mode(next);
            audio_play_tone(880.0f, 50, Waveform::SINE, 0.25f);
        } else {
            audio_play_tone(440.0f, 40, Waveform::SINE, 0.20f);
        }
    }
}

void HwTestGame::draw(lgfx::LGFX_Sprite& fb) {
    fb.fillScreen(0x1082); // Deep slate blue

    // Header bar
    fb.fillRect(0, 0, SCREEN_WIDTH, 28, 0x18E4);
    fb.setTextSize(1);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("ESP-GCONSOLE ES3C28P TEST", 8, 14);

    char stats[48];
    snprintf(stats, sizeof(stats), "%luFPS %luKB",
             static_cast<unsigned long>(_fps),
             static_cast<unsigned long>(esp_psram_get_size() / 1024));
    fb.setTextDatum(MR_DATUM);
    fb.setTextColor(TFT_CYAN);
    fb.drawString(stats, SCREEN_WIDTH - 8, 14);

    // Physical button status row
    const bool a_or_up   = (_last_held & (BTN_A | BTN_UP)) != 0;
    const bool b_or_down = (_last_held & (BTN_B | BTN_DOWN)) != 0;
    draw_key_box(fb, 18,  40, 62, 36, "LEFT",  (_last_held & BTN_LEFT) != 0,  TFT_GREEN);
    draw_key_box(fb, 88,  40, 62, 36, "RIGHT", (_last_held & BTN_RIGHT) != 0, TFT_GREEN);
    draw_key_box(fb, 158, 40, 68, 36, "A / UP", a_or_up,                    TFT_CYAN);
    draw_key_box(fb, 234, 40, 68, 36, "B / DN", b_or_down,                  TFT_ORANGE);

    // BOOT button + audio clock status
    char info[64];
    snprintf(info, sizeof(info), "BOOT:%s  AUD_CLK:%lums  TP:(%d,%d)",
             (_last_held & BTN_PAUSE) ? "ON" : "--",
             static_cast<unsigned long>(audio_get_playback_ms()),
             _last_tx, _last_ty);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_WHITE);
    fb.drawString(info, SCREEN_WIDTH / 2, 96);

    // Rhythm touch pads (DON / KA)
    draw_key_box(fb, RECT_DON.x, RECT_DON.y, RECT_DON.w, RECT_DON.h,
                 "TOUCH: DON", _don_flash_ms > 0, TFT_RED);
    draw_key_box(fb, RECT_KA.x, RECT_KA.y, RECT_KA.w, RECT_KA.h,
                 "TOUCH: KA", _ka_flash_ms > 0, TFT_BLUE);

    // Footer control buttons
    char bright_lbl[24];
    snprintf(bright_lbl, sizeof(bright_lbl), "BL: %u%%", BRIGHTNESS_LEVELS[_brightness_idx]);
    draw_key_box(fb, RECT_BRIGHT.x, RECT_BRIGHT.y, RECT_BRIGHT.w, RECT_BRIGHT.h, bright_lbl, false, TFT_WHITE);
    draw_key_box(fb, RECT_MODE.x, RECT_MODE.y, RECT_MODE.w, RECT_MODE.h, mode_label(input_get_mode()), false, TFT_WHITE);

    // Touch crosshair / ripple
    if (_ripple_ms > 0 && _last_tx >= 0 && _last_ty >= 0) {
        const int r = static_cast<int>((220 - _ripple_ms) / 10) + 4;
        fb.drawCircle(_last_tx, _last_ty, r, TFT_YELLOW);
    }
}
