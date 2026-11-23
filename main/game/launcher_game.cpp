#include "game/launcher_game.hpp"

#include <math.h>
#include <stdio.h>

#include "board_pins.h"
#include "hal/audio.hpp"
#include "hal/ble_hid.hpp"
#include "hal/save_data.hpp"

namespace {

constexpr uint8_t VOL_STEPS[] = {30, 50, 70, 90};
constexpr uint8_t BRI_STEPS[] = {30, 70, 100};

struct Rect {
    int16_t x, y, w, h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr Rect CARD_RECTS[3] = {
    {12,  46, 94, 142},
    {113, 46, 94, 142},
    {214, 46, 94, 142},
};
constexpr Rect RECT_BT  = {194, 4, 118, 26};
constexpr Rect RECT_VOL = {12,  200, 94, 30};
constexpr Rect RECT_BRI = {113, 200, 94, 30};
constexpr Rect RECT_GO  = {214, 200, 94, 30};

uint8_t next_in_cycle(uint8_t cur, const uint8_t* table, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (cur <= table[i] + 4) {
            return table[(i + 1) % n];
        }
    }
    return table[0];
}

void draw_card_icon(lgfx::LGFX_Sprite& fb, int idx, int cx, int cy, uint32_t anim_ms) {
    const int bob = static_cast<int>(sinf(anim_ms * 0.005f + idx) * 2.5f);
    const int y = cy + bob;

    if (idx == 0) {
        // Hoppy Star hero + coin block
        fb.fillRoundRect(cx - 18, y - 16, 36, 32, 6, 0xFE60);
        fb.drawRoundRect(cx - 18, y - 16, 36, 32, 6, TFT_WHITE);
        fb.fillCircle(cx, y - 2, 9, TFT_RED);
        fb.fillRect(cx - 7, y + 1, 14, 9, 0xFDE0);
        fb.fillRect(cx - 4, y + 3, 2, 3, TFT_BLACK);
        fb.fillRect(cx + 2, y + 3, 2, 3, TFT_BLACK);
    } else if (idx == 1) {
        // Taiko drum icon
        fb.fillCircle(cx, y, 18, TFT_RED);
        fb.drawCircle(cx, y, 18, TFT_WHITE);
        fb.fillCircle(cx, y, 13, 0xFDE0);
        fb.fillRect(cx - 5, y - 4, 3, 4, TFT_BLACK);
        fb.fillRect(cx + 2, y - 4, 3, 4, TFT_BLACK);
        fb.fillRect(cx - 4, y + 3, 8, 2, TFT_BLACK);
    } else {
        // Hardware chip icon
        fb.fillRoundRect(cx - 14, y - 14, 28, 28, 4, 0x2589);
        fb.drawRoundRect(cx - 14, y - 14, 28, 28, 4, TFT_CYAN);
        for (int p = -8; p <= 8; p += 8) {
            fb.fillRect(cx + p - 2, y - 19, 4, 5, TFT_LIGHTGREY);
            fb.fillRect(cx + p - 2, y + 14, 4, 5, TFT_LIGHTGREY);
            fb.fillRect(cx - 19, y + p - 2, 5, 4, TFT_LIGHTGREY);
            fb.fillRect(cx + 14, y + p - 2, 5, 4, TFT_LIGHTGREY);
        }
        fb.fillCircle(cx, y, 5, TFT_GREEN);
    }
}

} // namespace

void LauncherGame::init() {
    _anim_ms    = 0;
    _launch_req = AppId::NONE;
    input_set_mode(InputMode::LR_AB);
    audio_bgm_stop();
}

AppId LauncherGame::take_launch_request() {
    const AppId req = _launch_req;
    _launch_req = AppId::NONE;
    return req;
}

void LauncherGame::update(uint32_t dt_ms, const InputState& input) {
    _anim_ms += dt_ms;

    if (input.pressed & (BTN_LEFT | BTN_L)) {
        _selected = (_selected + 2) % 3;
        audio_play_tone(659.25f, 45, Waveform::SQUARE, 0.22f);
    } else if (input.pressed & (BTN_RIGHT | BTN_R)) {
        _selected = (_selected + 1) % 3;
        audio_play_tone(659.25f, 45, Waveform::SQUARE, 0.22f);
    } else if (input.pressed & (BTN_A | BTN_X)) {
        audio_play_tone(987.77f, 70, Waveform::SQUARE, 0.28f);
        _launch_req = static_cast<AppId>(_selected);
        return;
    } else if (input.pressed & (BTN_B | BTN_Y)) {
        const uint8_t next_v = next_in_cycle(save_data_get().volume_pct, VOL_STEPS, sizeof(VOL_STEPS));
        audio_set_volume(next_v);
        save_data_set_volume(next_v);
        audio_play_tone(783.99f, 55, Waveform::TRIANGLE, 0.28f);
    } else if (input.pressed & BTN_SELECT) {
        const uint8_t next_b = next_in_cycle(save_data_get().brightness_pct, BRI_STEPS, sizeof(BRI_STEPS));
        display_set_brightness(next_b);
        save_data_set_brightness(next_b);
        audio_play_tone(660.0f, 45, Waveform::SINE, 0.22f);
    }

    if (input.touch.just_pressed) {
        const int16_t tx = input.touch.x;
        const int16_t ty = input.touch.y;
        if (RECT_BT.contains(tx, ty)) {
            ble_hid_clear_bonds();
            audio_play_tone(523.25f, 60, Waveform::TRIANGLE, 0.25f);
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (CARD_RECTS[i].contains(tx, ty)) {
                _selected = i;
                audio_play_tone(987.77f, 70, Waveform::SQUARE, 0.28f);
                _launch_req = static_cast<AppId>(i);
                return;
            }
        }
        if (RECT_VOL.contains(tx, ty)) {
            const uint8_t next_v = next_in_cycle(save_data_get().volume_pct, VOL_STEPS, sizeof(VOL_STEPS));
            audio_set_volume(next_v);
            save_data_set_volume(next_v);
            audio_play_tone(783.99f, 55, Waveform::TRIANGLE, 0.28f);
        } else if (RECT_BRI.contains(tx, ty)) {
            const uint8_t next_b = next_in_cycle(save_data_get().brightness_pct, BRI_STEPS, sizeof(BRI_STEPS));
            display_set_brightness(next_b);
            save_data_set_brightness(next_b);
            audio_play_tone(660.0f, 45, Waveform::SINE, 0.22f);
        } else if (RECT_GO.contains(tx, ty)) {
            audio_play_tone(987.77f, 70, Waveform::SQUARE, 0.28f);
            _launch_req = static_cast<AppId>(_selected);
        }
    }
}

void LauncherGame::draw(lgfx::LGFX_Sprite& fb) {
    fb.fillScreen(0x1064);

    // Header
    fb.fillRect(0, 0, SCREEN_WIDTH, 34, 0x18C7);
    fb.drawFastHLine(0, 34, SCREEN_WIDTH, 0x4A6F);
    fb.setTextDatum(ML_DATUM);
    fb.setTextSize(2);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("ESP-GCONSOLE", 10, 17);

    // BLE HID status pill
    const BleHidStatus ble = ble_hid_get_status();
    const uint16_t bt_bg  = ble.connected ? 0x13A4 : (ble.scanning ? 0x8340 : 0x2945);
    const uint16_t bt_brd = ble.connected ? TFT_GREEN : (ble.scanning ? TFT_YELLOW : TFT_LIGHTGREY);
    fb.fillRoundRect(RECT_BT.x, RECT_BT.y, RECT_BT.w, RECT_BT.h, 6, bt_bg);
    fb.drawRoundRect(RECT_BT.x, RECT_BT.y, RECT_BT.w, RECT_BT.h, 6, bt_brd);
    fb.setTextDatum(MC_DATUM);
    fb.setTextSize(1);
    fb.setTextColor(TFT_WHITE);
    char bt_lbl[24];
    if (ble.connected) {
        if (ble.battery_pct >= 0) {
            snprintf(bt_lbl, sizeof(bt_lbl), "BT:OK %d%%", ble.battery_pct);
        } else {
            snprintf(bt_lbl, sizeof(bt_lbl), "BT:8BitDo OK");
        }
    } else if (ble.scanning) {
        snprintf(bt_lbl, sizeof(bt_lbl), "BT:SCAN...");
    } else {
        snprintf(bt_lbl, sizeof(bt_lbl), "BT:PAIR/WAIT");
    }
    fb.drawString(bt_lbl, RECT_BT.x + RECT_BT.w / 2, RECT_BT.y + RECT_BT.h / 2);

    // Game Cards
    static constexpr const char* TITLES[3] = {"HOPPY STAR", "TAIKO BEAT", "HW TEST"};
    static constexpr const char* SUBS[3]   = {"Action Game", "Rhythm Game", "Diagnostics"};
    static constexpr uint16_t ACCENTS[3]   = {TFT_YELLOW, TFT_RED, TFT_CYAN};

    const auto& sd = save_data_get();
    const uint32_t best_rhythm = (sd.rhythm_best[0] > sd.rhythm_best[1])
                               ? ((sd.rhythm_best[0] > sd.rhythm_best[2]) ? sd.rhythm_best[0] : sd.rhythm_best[2])
                               : ((sd.rhythm_best[1] > sd.rhythm_best[2]) ? sd.rhythm_best[1] : sd.rhythm_best[2]);

    for (int i = 0; i < 3; ++i) {
        const Rect& r = CARD_RECTS[i];
        const bool sel = (_selected == i);
        const uint16_t bg     = sel ? 0x294B : 0x18A6;
        const uint16_t border = sel ? ACCENTS[i] : 0x4228;

        fb.fillRoundRect(r.x, r.y, r.w, r.h, 10, bg);
        fb.drawRoundRect(r.x, r.y, r.w, r.h, 10, border);
        if (sel) {
            fb.drawRoundRect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 11, border);
        }

        draw_card_icon(fb, i, r.x + r.w / 2, r.y + 42, _anim_ms);

        fb.setTextDatum(MC_DATUM);
        fb.setTextSize(1);
        fb.setTextColor(sel ? ACCENTS[i] : TFT_WHITE);
        fb.drawString(TITLES[i], r.x + r.w / 2, r.y + 88);

        fb.setTextColor(TFT_LIGHTGREY);
        fb.drawString(SUBS[i], r.x + r.w / 2, r.y + 104);

        char badge[20];
        if (i == 0) {
            snprintf(badge, sizeof(badge), "HI:%lu", static_cast<unsigned long>(sd.action_best_score));
        } else if (i == 1) {
            snprintf(badge, sizeof(badge), "HI:%lu", static_cast<unsigned long>(best_rhythm));
        } else {
            snprintf(badge, sizeof(badge), "ES3C28P");
        }
        fb.setTextColor(TFT_CYAN);
        fb.drawString(badge, r.x + r.w / 2, r.y + 124);
    }

    // Footer Buttons
    char buf[20];
    fb.setTextDatum(MC_DATUM);
    fb.setTextSize(1);

    snprintf(buf, sizeof(buf), "VOL: %u%%", static_cast<unsigned>(sd.volume_pct));
    fb.fillRoundRect(RECT_VOL.x, RECT_VOL.y, RECT_VOL.w, RECT_VOL.h, 6, 0x2945);
    fb.drawRoundRect(RECT_VOL.x, RECT_VOL.y, RECT_VOL.w, RECT_VOL.h, 6, TFT_LIGHTGREY);
    fb.setTextColor(TFT_WHITE);
    fb.drawString(buf, RECT_VOL.x + RECT_VOL.w / 2, RECT_VOL.y + RECT_VOL.h / 2);

    snprintf(buf, sizeof(buf), "BL: %u%%", static_cast<unsigned>(sd.brightness_pct));
    fb.fillRoundRect(RECT_BRI.x, RECT_BRI.y, RECT_BRI.w, RECT_BRI.h, 6, 0x2945);
    fb.drawRoundRect(RECT_BRI.x, RECT_BRI.y, RECT_BRI.w, RECT_BRI.h, 6, TFT_LIGHTGREY);
    fb.drawString(buf, RECT_BRI.x + RECT_BRI.w / 2, RECT_BRI.y + RECT_BRI.h / 2);

    fb.fillRoundRect(RECT_GO.x, RECT_GO.y, RECT_GO.w, RECT_GO.h, 6, 0x2625);
    fb.drawRoundRect(RECT_GO.x, RECT_GO.y, RECT_GO.w, RECT_GO.h, 6, TFT_YELLOW);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("PLAY! (A)", RECT_GO.x + RECT_GO.w / 2, RECT_GO.y + RECT_GO.h / 2);
}
