#include "game/hw_test_game.hpp"

#include <stdio.h>

#include "board_pins.h"
#include "esp_psram.h"
#include "hal/audio.hpp"
#include "hal/ble_hid.hpp"
#include "hal/save_data.hpp"

namespace {

constexpr uint8_t BRIGHTNESS_LEVELS[] = {30, 70, 100};
constexpr uint32_t BOOT_EXIT_HOLD_MS = 1000;

struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr Rect RECT_MENU   = {186, 4, 48, 20};
constexpr Rect RECT_DON    = {18, 136, 134, 60};
constexpr Rect RECT_KA     = {168, 136, 134, 60};
constexpr Rect RECT_BRIGHT = {18, 206, 134, 26};
constexpr Rect RECT_REPAIR = {168, 206, 134, 26};

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
    _wants_exit   = false;
    _boot_hold_ms = 0;
    _ripple_ms    = 0;
    _don_flash_ms = 0;
    _ka_flash_ms  = 0;
    input_set_mode(InputMode::BOTH);
    const uint8_t cur_bri = save_data_get().brightness_pct;
    if (cur_bri <= 40)      _brightness_idx = 0;
    else if (cur_bri <= 80) _brightness_idx = 1;
    else                    _brightness_idx = 2;
    display_set_brightness(BRIGHTNESS_LEVELS[_brightness_idx]);
}

void HwTestGame::on_exit() {
    audio_bgm_stop();
}

void HwTestGame::update(uint32_t dt_ms, const InputState& input) {
    _last_held = input.held;

    if (_ripple_ms > dt_ms)    _ripple_ms -= dt_ms;    else _ripple_ms = 0;
    if (_don_flash_ms > dt_ms) _don_flash_ms -= dt_ms; else _don_flash_ms = 0;
    if (_ka_flash_ms > dt_ms)  _ka_flash_ms -= dt_ms;  else _ka_flash_ms = 0;

    // Hold BOOT / START for 1 second to return to Launcher
    if (input.held & BTN_PAUSE) {
        _boot_hold_ms += dt_ms;
        if (_boot_hold_ms >= BOOT_EXIT_HOLD_MS) {
            audio_play_tone(880.0f, 70, Waveform::SQUARE, 0.25f);
            _wants_exit = true;
            return;
        }
    } else {
        _boot_hold_ms = 0;
    }

    // FPS counter
    ++_frame_acc;
    _time_acc_ms += dt_ms;
    if (_time_acc_ms >= 500) {
        _fps         = (_frame_acc * 1000) / _time_acc_ms;
        _frame_acc   = 0;
        _time_acc_ms = 0;
    }

    // Button SFX
    if (input.pressed & BTN_LEFT)   audio_play_tone(523.25f, 70, Waveform::SQUARE, 0.25f);
    if (input.pressed & BTN_RIGHT)  audio_play_tone(587.33f, 70, Waveform::SQUARE, 0.25f);
    if (input.pressed & BTN_UP)     audio_play_tone(659.25f, 70, Waveform::SQUARE, 0.25f);
    if (input.pressed & BTN_DOWN)   audio_play_tone(698.46f, 70, Waveform::SQUARE, 0.25f);
    if (input.pressed & BTN_A)      audio_play_tone(783.99f, 80, Waveform::SQUARE, 0.28f);
    if (input.pressed & BTN_B)      audio_play_tone(880.00f, 80, Waveform::SQUARE, 0.28f);
    if (input.pressed & BTN_X)      audio_play_tone(987.77f, 80, Waveform::SQUARE, 0.28f);
    if (input.pressed & BTN_Y)      audio_play_tone(1046.5f, 80, Waveform::SQUARE, 0.28f);
    if (input.pressed & BTN_L)      audio_play_sweep(170.0f, 65.0f, 95, Waveform::TRIANGLE, 0.50f);
    if (input.pressed & BTN_R)      audio_play_tone(1240.0f, 55, Waveform::SQUARE, 0.25f);
    if (input.pressed & (BTN_PAUSE | BTN_SELECT)) audio_play_tone(1174.7f, 90, Waveform::TRIANGLE, 0.30f);

    if (input.touch.down) {
        _last_tx = input.touch.x;
        _last_ty = input.touch.y;
    }

    if (input.touch.just_pressed) {
        _ripple_ms = 220;
        if (RECT_MENU.contains(input.touch.x, input.touch.y)) {
            audio_play_tone(783.99f, 60, Waveform::SQUARE, 0.25f);
            _wants_exit = true;
        } else if (RECT_DON.contains(input.touch.x, input.touch.y)) {
            _don_flash_ms = 140;
            audio_play_sweep(170.0f, 65.0f, 95, Waveform::TRIANGLE, 0.55f);
        } else if (RECT_KA.contains(input.touch.x, input.touch.y)) {
            _ka_flash_ms = 140;
            audio_play_tone(880.0f, 55, Waveform::NOISE, 0.35f);
            audio_play_tone(1174.66f, 60, Waveform::SQUARE, 0.25f);
        } else if (RECT_BRIGHT.contains(input.touch.x, input.touch.y)) {
            _brightness_idx = (_brightness_idx + 1) % 3;
            display_set_brightness(BRIGHTNESS_LEVELS[_brightness_idx]);
            save_data_set_brightness(BRIGHTNESS_LEVELS[_brightness_idx]);
            audio_play_tone(660.0f, 50, Waveform::SINE, 0.25f);
        } else if (RECT_REPAIR.contains(input.touch.x, input.touch.y)) {
            ble_hid_clear_bonds();
            audio_play_tone(523.25f, 60, Waveform::TRIANGLE, 0.25f);
        } else {
            audio_play_tone(440.0f, 40, Waveform::SINE, 0.20f);
        }
    }
}

void HwTestGame::draw(lgfx::LGFX_Sprite& fb) {
    fb.fillScreen(0x1082);

    // Header bar
    fb.fillRect(0, 0, SCREEN_WIDTH, 28, 0x18E4);
    fb.setTextSize(1);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("ES3C28P + BLE HID TEST", 8, 14);

    draw_key_box(fb, RECT_MENU.x, RECT_MENU.y, RECT_MENU.w, RECT_MENU.h, "MENU", false, TFT_WHITE);

    char stats[48];
    snprintf(stats, sizeof(stats), "%luFPS %luMB",
             static_cast<unsigned long>(_fps),
             static_cast<unsigned long>(esp_psram_get_size() / (1024 * 1024)));
    fb.setTextDatum(MR_DATUM);
    fb.setTextColor(TFT_CYAN);
    fb.drawString(stats, SCREEN_WIDTH - 8, 14);

    // Row 1: D-Pad + Shoulders
    draw_key_box(fb, 12,  34, 46, 28, "L",   (_last_held & BTN_LEFT)  != 0, TFT_GREEN);
    draw_key_box(fb, 62,  34, 46, 28, "U",   (_last_held & BTN_UP)    != 0, TFT_GREEN);
    draw_key_box(fb, 112, 34, 46, 28, "D",   (_last_held & BTN_DOWN)  != 0, TFT_GREEN);
    draw_key_box(fb, 162, 34, 46, 28, "R",   (_last_held & BTN_RIGHT) != 0, TFT_GREEN);
    draw_key_box(fb, 214, 34, 46, 28, "L1",  (_last_held & BTN_L)     != 0, TFT_YELLOW);
    draw_key_box(fb, 264, 34, 44, 28, "R1",  (_last_held & BTN_R)     != 0, TFT_YELLOW);

    // Row 2: Face buttons + Start/Select
    draw_key_box(fb, 12,  68, 46, 28, "A",   (_last_held & BTN_A)      != 0, TFT_CYAN);
    draw_key_box(fb, 62,  68, 46, 28, "B",   (_last_held & BTN_B)      != 0, TFT_ORANGE);
    draw_key_box(fb, 112, 68, 46, 28, "X",   (_last_held & BTN_X)      != 0, TFT_CYAN);
    draw_key_box(fb, 162, 68, 46, 28, "Y",   (_last_held & BTN_Y)      != 0, TFT_ORANGE);
    draw_key_box(fb, 214, 68, 46, 28, "SEL", (_last_held & BTN_SELECT) != 0, TFT_MAGENTA);
    draw_key_box(fb, 264, 68, 44, 28, "ST",  (_last_held & BTN_PAUSE)  != 0, TFT_MAGENTA);

    // BLE HID + Raw HID Usage diagnostic line
    const BleHidStatus ble = ble_hid_get_status();
    char info[80];
    snprintf(info, sizeof(info), "BT:%.14s  KEY:0x%02X  TP:(%d,%d)",
             ble.connected ? ble.device_name : (ble.scanning ? "SCAN" : "WAIT"),
             static_cast<unsigned>(ble.last_keycode),
             _last_tx, _last_ty);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(ble.connected ? TFT_GREEN : TFT_WHITE);
    fb.drawString(info, SCREEN_WIDTH / 2, 108);

    if (_boot_hold_ms > 0) {
        const int bar_w = static_cast<int>((_boot_hold_ms * 120) / BOOT_EXIT_HOLD_MS);
        fb.fillRect(100, 120, 120, 6, TFT_DARKGREY);
        fb.fillRect(100, 120, bar_w > 120 ? 120 : bar_w, 6, TFT_YELLOW);
    } else {
        fb.setTextColor(TFT_LIGHTGREY);
        fb.drawString("Hold START/BOOT or tap MENU to exit", SCREEN_WIDTH / 2, 123);
    }

    // Rhythm touch pads (DON / KA)
    draw_key_box(fb, RECT_DON.x, RECT_DON.y, RECT_DON.w, RECT_DON.h,
                 "TOUCH: DON", _don_flash_ms > 0, TFT_RED);
    draw_key_box(fb, RECT_KA.x, RECT_KA.y, RECT_KA.w, RECT_KA.h,
                 "TOUCH: KA", _ka_flash_ms > 0, TFT_BLUE);

    // Footer control buttons
    char bright_lbl[24];
    snprintf(bright_lbl, sizeof(bright_lbl), "BL: %u%%", BRIGHTNESS_LEVELS[_brightness_idx]);
    draw_key_box(fb, RECT_BRIGHT.x, RECT_BRIGHT.y, RECT_BRIGHT.w, RECT_BRIGHT.h, bright_lbl, false, TFT_WHITE);
    draw_key_box(fb, RECT_REPAIR.x, RECT_REPAIR.y, RECT_REPAIR.w, RECT_REPAIR.h, "BT RE-PAIR", false, TFT_WHITE);

    if (_ripple_ms > 0 && _last_tx >= 0 && _last_ty >= 0) {
        const int r = static_cast<int>((220 - _ripple_ms) / 10) + 4;
        fb.drawCircle(_last_tx, _last_ty, r, TFT_YELLOW);
    }
}
