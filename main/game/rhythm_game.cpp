#include "game/rhythm_game.hpp"

#include <math.h>
#include <stdio.h>

#include "board_pins.h"
#include "hal/save_data.hpp"

namespace {

constexpr int16_t TARGET_X       = 64;
constexpr int16_t LANE_Y         = 134;
constexpr float   SCROLL_PX_MS   = 0.165f; // ~1.55s lookahead across 256px

constexpr uint32_t WIN_PERFECT_MS = 48;
constexpr uint32_t WIN_GOOD_MS    = 98;
constexpr uint32_t WIN_MISS_MS    = 140;

// MIDI-style frequency helper for clean musical phrasing
constexpr uint16_t N_REST = 0;
constexpr uint16_t N_C3 = 131, N_D3 = 147, N_E3 = 165, N_F3 = 175, N_G3 = 196, N_A3 = 220, N_B3 = 247;
constexpr uint16_t N_C4 = 262, N_D4 = 294, N_E4 = 330, N_F4 = 349, N_G4 = 392, N_A4 = 440, N_B4 = 494;
constexpr uint16_t N_C5 = 523, N_D5 = 587, N_E5 = 659, N_F5 = 698, N_G5 = 784, N_A5 = 880, N_B5 = 988, N_C6 = 1047;

struct SongMeta {
    const char* title;
    const char* subtitle;
    uint16_t    bpm;
    uint16_t    accent_color;
    uint8_t     bars; // Each bar = 4 beats, bar 0 is 4-beat count-in
};

constexpr SongMeta SONGS[3] = {
    {"SUNNY MARCH",    "Cheerful Parade",  108, TFT_ORANGE,  10},
    {"SPACE GROOVE",   "Rhythm Hop!",      126, TFT_CYAN,    12},
    {"MATSURI BEAT",   "Festival Taiko!",  140, TFT_MAGENTA, 12},
};

struct Rect {
    int16_t x, y, w, h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr Rect RECT_PAUSE_BTN = {278, 2, 40, 18};
constexpr Rect RECT_PAD_DON   = {8, 168, 148, 66};
constexpr Rect RECT_PAD_KA    = {164, 168, 148, 66};

void play_don_sfx() {
    audio_play_sweep(175.0f, 64.0f, 95, Waveform::TRIANGLE, 0.55f);
}

void play_ka_sfx() {
    audio_play_tone(880.0f, 45, Waveform::NOISE, 0.30f);
    audio_play_tone(1240.0f, 55, Waveform::SQUARE, 0.24f);
}

} // namespace

void RhythmGame::init() {
    _wants_exit = false;
    _state      = State::SONG_SELECT;
    _anim_ms    = 0;
    input_set_mode(InputMode::LR_AB);
    audio_bgm_stop();
}

void RhythmGame::on_exit() {
    audio_bgm_stop();
}

void RhythmGame::build_song_data(uint8_t song_idx, uint8_t difficulty) {
    _bgm_count   = 0;
    _chart_count = 0;

    const SongMeta& meta = SONGS[song_idx];
    const uint32_t beat_ms  = 60000U / meta.bpm;
    const uint32_t eighth_ms = beat_ms / 2U;
    _beat_ms = beat_ms;

    auto push_bgm = [&](uint32_t t_ms, uint16_t f, uint16_t end_f, uint16_t dur, Waveform w, uint8_t g) {
        if (_bgm_count < MAX_BGM_NOTES) {
            _bgm_notes[_bgm_count++] = BgmNote{t_ms, f, end_f, dur, w, g};
        }
    };
    auto push_note = [&](uint32_t t_ms, NoteKind k) {
        if (_chart_count < MAX_CHART_NOTES) {
            _chart[_chart_count++] = ChartNote{t_ms, k, false, false};
        }
    };

    // Bar 0: 4-beat count-in click so player locks onto BPM immediately
    for (int b = 0; b < 4; ++b) {
        const uint32_t t = static_cast<uint32_t>(b) * beat_ms;
        push_bgm(t, (b == 0) ? 1047 : 784, 0, 45, Waveform::SQUARE, 28);
    }

    // Melody & bass patterns per song (8 eighth-notes per bar)
    static constexpr uint16_t MEL_0[4][8] = {
        {N_C5, 0, N_E5, 0, N_G5, 0, N_E5, 0},
        {N_F5, 0, N_E5, 0, N_D5, 0, N_G4, 0},
        {N_C5, N_D5, N_E5, 0, N_G5, 0, N_A5, 0},
        {N_G5, 0, N_E5, 0, N_C5, 0, 0, 0},
    };
    static constexpr uint16_t BASS_0[4][4] = {
        {N_C3, N_G3, N_C3, N_G3},
        {N_F3, N_C3, N_G3, N_D3},
        {N_C3, N_G3, N_E3, N_A3},
        {N_G3, N_D3, N_C3, N_G3},
    };

    static constexpr uint16_t MEL_1[4][8] = {
        {N_A4, 0, N_C5, N_E5, 0, N_E5, N_D5, 0},
        {N_F5, 0, N_E5, 0, N_C5, N_D5, N_E5, 0},
        {N_G4, 0, N_B4, N_D5, 0, N_G5, N_E5, 0},
        {N_A5, 0, N_G5, N_E5, N_C5, 0, N_A4, 0},
    };
    static constexpr uint16_t BASS_1[4][4] = {
        {N_A3, N_E3, N_A3, N_E3},
        {N_F3, N_C3, N_G3, N_D3},
        {N_G3, N_D3, N_E3, N_B3},
        {N_A3, N_E3, N_A3, 0},
    };

    static constexpr uint16_t MEL_2[4][8] = {
        {N_D5, N_D5, N_E5, N_G5, N_A5, 0, N_G5, 0},
        {N_E5, 0, N_D5, N_B4, N_A4, 0, N_D5, 0},
        {N_G5, N_A5, N_B5, 0, N_A5, N_G5, N_E5, 0},
        {N_D5, 0, N_E5, N_G5, N_D5, 0, 0, 0},
    };
    static constexpr uint16_t BASS_2[4][4] = {
        {N_D3, N_A3, N_D3, N_A3},
        {N_G3, N_D3, N_A3, N_E3},
        {N_G3, N_D3, N_B3, N_A3},
        {N_D3, N_A3, N_D3, 0},
    };

    // Rhythm pattern strings per 8 eighth-notes:
    // '.' = rest, 'd' = DON, 'k' = KA, 'D' = BIG DON, 'K' = BIG KA
    static constexpr const char* PAT_EASY[3][4] = {
        {"d.d.k.d.", "d.k.d.k.", "d.d.k.k.", "D.k.D..."},
        {"d.k.d...", "d.k.k.d.", "k.d.k.d.", "D.k.D..."},
        {"d.d.k.d.", "k.d.k.d.", "d.k.d.k.", "D.K.D..."},
    };
    static constexpr const char* PAT_NORM[3][4] = {
        {"ddk.d.k.", "d.kkd.k.", "dkdkd.k.", "D.kkD.d."},
        {"d.kdk.d.", "ddk.kkd.", "kdk.dkd.", "D.dkD.k."},
        {"ddk.ddk.", "kdk.ddk.", "dkdkddk.", "D.KKD.d."},
    };

    const int play_bars = meta.bars - 1;
    for (int bar = 0; bar < play_bars; ++bar) {
        const uint32_t bar_t = static_cast<uint32_t>(bar + 1) * 4U * beat_ms;
        const int p_idx = bar % 4;

        // Beat percussion + bass
        for (int b = 0; b < 4; ++b) {
            const uint32_t bt = bar_t + static_cast<uint32_t>(b) * beat_ms;
            const uint16_t bass_f = (song_idx == 0) ? BASS_0[p_idx][b]
                                  : (song_idx == 1) ? BASS_1[p_idx][b]
                                                    : BASS_2[p_idx][b];
            if (bass_f > 0) {
                push_bgm(bt, bass_f, 0, static_cast<uint16_t>(beat_ms * 8 / 10), Waveform::TRIANGLE, 26);
            }
            if (b == 0 || b == 2) {
                push_bgm(bt, 120, 55, 65, Waveform::SINE, 20); // Soft kick pulse
            } else {
                push_bgm(bt, 900, 0, 32, Waveform::NOISE, 12); // Light snare/hat
            }
        }

        // Melody + Chart notes on eighth-note grid
        const char* pat = (difficulty == 0) ? PAT_EASY[song_idx][p_idx] : PAT_NORM[song_idx][p_idx];
        for (int e = 0; e < 8; ++e) {
            const uint32_t et = bar_t + static_cast<uint32_t>(e) * eighth_ms;
            const uint16_t mel_f = (song_idx == 0) ? MEL_0[p_idx][e]
                                 : (song_idx == 1) ? MEL_1[p_idx][e]
                                                   : MEL_2[p_idx][e];
            if (mel_f > 0) {
                push_bgm(et, mel_f, 0, static_cast<uint16_t>(eighth_ms + 40), Waveform::SQUARE, 22);
            }

            // Leave final bar's last 2 beats free so ending feels clean
            if (bar == play_bars - 1 && e >= 5) continue;

            const char ch = pat[e];
            if (ch == 'd')      push_note(et, NoteKind::DON);
            else if (ch == 'k') push_note(et, NoteKind::KA);
            else if (ch == 'D') push_note(et, NoteKind::BIG_DON);
            else if (ch == 'K') push_note(et, NoteKind::BIG_KA);
        }
    }

    _song_len_ms = static_cast<uint32_t>(meta.bars) * 4U * beat_ms + 800U;
}

void RhythmGame::start_song(uint8_t song_idx, uint8_t difficulty) {
    audio_bgm_stop();
    _song_idx       = song_idx % SONG_COUNT;
    _difficulty     = difficulty;
    _score          = 0;
    _combo          = 0;
    _max_combo      = 0;
    _perfect_cnt    = 0;
    _good_cnt       = 0;
    _miss_cnt       = 0;
    _soul_pct       = 30;
    _new_record     = false;
    _next_note_idx  = 0;
    _song_time_ms   = 0;
    _end_wait_ms    = 0;
    _last_judge     = JudgeKind::NONE;
    _judge_timer_ms = 0;
    _don_flash_ms   = 0;
    _ka_flash_ms    = 0;
    _mascot_cheer_ms = 0;

    build_song_data(_song_idx, _difficulty);
    input_set_mode(InputMode::LR_AB);
    _state = State::PLAYING;
    audio_bgm_play(_bgm_notes, _bgm_count, _song_len_ms, false);
}

void RhythmGame::trigger_judge(JudgeKind j, bool is_big) {
    _last_judge     = j;
    _judge_timer_ms = 420;
    const uint32_t mul = is_big ? 2U : 1U;

    if (j == JudgeKind::PERFECT) {
        ++_perfect_cnt;
        ++_combo;
        if (_combo > _max_combo) _max_combo = _combo;
        const uint32_t combo_bonus = (_combo / 10U) * 50U;
        _score += (300U + combo_bonus) * mul;
        _soul_pct += is_big ? 6 : 4;
        _mascot_cheer_ms = 240;
    } else if (j == JudgeKind::GOOD) {
        ++_good_cnt;
        ++_combo;
        if (_combo > _max_combo) _max_combo = _combo;
        _score += 150U * mul;
        _soul_pct += is_big ? 3 : 2;
        _mascot_cheer_ms = 140;
    } else {
        ++_miss_cnt;
        _combo = 0;
        _soul_pct -= 4;
    }

    if (_soul_pct > 100) _soul_pct = 100;
    if (_soul_pct < 0)   _soul_pct = 0;
}

void RhythmGame::handle_hit(bool is_don) {
    if (is_don) {
        _don_flash_ms = 120;
        play_don_sfx();
    } else {
        _ka_flash_ms = 120;
        play_ka_sfx();
    }

    // Find earliest unhit/unmissed note within miss window
    while (_next_note_idx < _chart_count &&
           (_chart[_next_note_idx].hit || _chart[_next_note_idx].missed)) {
        ++_next_note_idx;
    }
    if (_next_note_idx >= _chart_count) return;

    ChartNote& n = _chart[_next_note_idx];
    const int32_t diff = static_cast<int32_t>(n.time_ms) - _song_time_ms;
    const uint32_t abs_diff = static_cast<uint32_t>(diff < 0 ? -diff : diff);
    if (abs_diff > WIN_MISS_MS) {
        return; // Stray tap between notes: play drum sound without unfair miss
    }

    const bool note_is_don = (n.kind == NoteKind::DON || n.kind == NoteKind::BIG_DON);
    const bool is_big      = (n.kind == NoteKind::BIG_DON || n.kind == NoteKind::BIG_KA);

    if (note_is_don != is_don) {
        n.missed = true;
        ++_next_note_idx;
        trigger_judge(JudgeKind::MISS, is_big);
        return;
    }

    n.hit = true;
    ++_next_note_idx;
    if (abs_diff <= WIN_PERFECT_MS) {
        trigger_judge(JudgeKind::PERFECT, is_big);
    } else if (abs_diff <= WIN_GOOD_MS) {
        trigger_judge(JudgeKind::GOOD, is_big);
    } else {
        trigger_judge(JudgeKind::MISS, is_big);
    }
}

void RhythmGame::update(uint32_t dt_ms, const InputState& input) {
    _anim_ms += dt_ms;
    if (_judge_timer_ms > dt_ms) _judge_timer_ms -= dt_ms; else _judge_timer_ms = 0;
    if (_don_flash_ms > dt_ms)   _don_flash_ms -= dt_ms;   else _don_flash_ms = 0;
    if (_ka_flash_ms > dt_ms)    _ka_flash_ms -= dt_ms;    else _ka_flash_ms = 0;
    if (_mascot_cheer_ms > dt_ms) _mascot_cheer_ms -= dt_ms; else _mascot_cheer_ms = 0;

    switch (_state) {
        case State::SONG_SELECT: update_song_select(input); break;
        case State::PLAYING:     update_playing(dt_ms, input); break;
        case State::PAUSED:      update_paused(input); break;
        case State::RESULT:      update_result(input); break;
    }
}

void RhythmGame::update_song_select(const InputState& input) {
    input_set_mode(InputMode::LR_AB);

    if (input.pressed & (BTN_LEFT | BTN_UP)) {
        _song_idx = (_song_idx + SONG_COUNT - 1) % SONG_COUNT;
        audio_play_tone(660.0f, 45, Waveform::SQUARE, 0.22f);
    } else if (input.pressed & (BTN_RIGHT | BTN_DOWN)) {
        _song_idx = (_song_idx + 1) % SONG_COUNT;
        audio_play_tone(660.0f, 45, Waveform::SQUARE, 0.22f);
    } else if (input.pressed & (BTN_B | BTN_Y)) {
        _difficulty ^= 1;
        audio_play_tone(784.0f, 45, Waveform::TRIANGLE, 0.24f);
    } else if (input.pressed & BTN_L) {
        save_data_set_rhythm_offset(save_data_get().rhythm_offset_ms - 4);
        audio_play_tone(523.0f, 35, Waveform::SINE, 0.20f);
    } else if (input.pressed & BTN_R) {
        save_data_set_rhythm_offset(save_data_get().rhythm_offset_ms + 4);
        audio_play_tone(587.0f, 35, Waveform::SINE, 0.20f);
    } else if (input.pressed & (BTN_A | BTN_X)) {
        play_don_sfx();
        start_song(_song_idx, _difficulty);
        return;
    } else if (input.pressed & (BTN_PAUSE | BTN_SELECT)) {
        _wants_exit = true;
        return;
    }

    if (input.touch.just_pressed) {
        const int16_t tx = input.touch.x;
        const int16_t ty = input.touch.y;
        // Header MENU button
        if (tx >= 256 && ty < 26) {
            audio_play_tone(660.0f, 50, Waveform::SQUARE, 0.22f);
            _wants_exit = true;
            return;
        }
        // Song cards
        for (int i = 0; i < SONG_COUNT; ++i) {
            const int16_t cy = 36 + i * 48;
            if (tx >= 14 && tx < 306 && ty >= cy && ty < cy + 42) {
                if (_song_idx == i) {
                    play_don_sfx();
                    start_song(_song_idx, _difficulty);
                } else {
                    _song_idx = i;
                    audio_play_tone(660.0f, 45, Waveform::SQUARE, 0.22f);
                }
                return;
            }
        }
        // Footer buttons: Difficulty | Offset -/+ | PLAY
        if (ty >= 190) {
            if (tx < 110) {
                _difficulty ^= 1;
                audio_play_tone(784.0f, 45, Waveform::TRIANGLE, 0.24f);
            } else if (tx < 160) {
                save_data_set_rhythm_offset(save_data_get().rhythm_offset_ms - 4);
                audio_play_tone(523.0f, 35, Waveform::SINE, 0.20f);
            } else if (tx < 210) {
                save_data_set_rhythm_offset(save_data_get().rhythm_offset_ms + 4);
                audio_play_tone(587.0f, 35, Waveform::SINE, 0.20f);
            } else {
                play_don_sfx();
                start_song(_song_idx, _difficulty);
            }
        }
    }
}

void RhythmGame::update_playing(uint32_t dt_ms, const InputState& input) {
    if ((input.pressed & (BTN_PAUSE | BTN_SELECT)) ||
        (input.touch.just_pressed && RECT_PAUSE_BTN.contains(input.touch.x, input.touch.y))) {
        _state        = State::PAUSED;
        _pause_cursor = 0;
        input_set_mode(InputMode::LR_AB);
        audio_bgm_set_paused(true);
        audio_play_tone(523.25f, 45, Waveform::SQUARE, 0.20f);
        return;
    }

    // Master sync from I2S DMA clock minus calibrated DMA offset
    const int32_t raw_ms = static_cast<int32_t>(audio_bgm_get_time_ms());
    _song_time_ms = raw_ms - static_cast<int32_t>(save_data_get().rhythm_offset_ms);

    // 8BitDo Micro + Touch drum pads (Left-side/A/L1 = DON, Right-side/B/R1 = KA)
    if (input.pressed & (BTN_LEFT | BTN_DOWN | BTN_A | BTN_Y | BTN_L)) {
        handle_hit(true);
    }
    if (input.pressed & (BTN_RIGHT | BTN_UP | BTN_B | BTN_X | BTN_R)) {
        handle_hit(false);
    }
    if (input.touch.just_pressed && input.touch.y >= 158) {
        if (input.touch.x < 160) {
            handle_hit(true);
        } else {
            handle_hit(false);
        }
    }

    // Mark passed notes as MISS
    while (_next_note_idx < _chart_count) {
        ChartNote& n = _chart[_next_note_idx];
        if (n.hit || n.missed) {
            ++_next_note_idx;
            continue;
        }
        if (_song_time_ms > static_cast<int32_t>(n.time_ms + WIN_GOOD_MS)) {
            n.missed = true;
            ++_next_note_idx;
            const bool is_big = (n.kind == NoteKind::BIG_DON || n.kind == NoteKind::BIG_KA);
            trigger_judge(JudgeKind::MISS, is_big);
        } else {
            break;
        }
    }

    // Check song completion
    if (!audio_bgm_is_playing()) {
        _end_wait_ms += dt_ms;
        if (_end_wait_ms >= 650) {
            _new_record = (_score > save_data_get().rhythm_best[_song_idx]);
            save_data_update_rhythm_best(_song_idx, _score);
            _state = State::RESULT;
            input_set_mode(InputMode::LR_AB);
            if (_soul_pct >= 70) {
                audio_play_tone(523.25f, 100, Waveform::SQUARE, 0.25f);
                audio_play_tone(659.25f, 100, Waveform::SQUARE, 0.25f);
                audio_play_tone(1046.5f, 240, Waveform::SQUARE, 0.30f);
            } else {
                audio_play_sweep(392.0f, 220.0f, 260, Waveform::TRIANGLE, 0.25f);
            }
        }
    }
}

void RhythmGame::update_paused(const InputState& input) {
    if (input.pressed & (BTN_PAUSE | BTN_B)) {
        _state = State::PLAYING;
        input_set_mode(InputMode::LR_AB);
        audio_bgm_set_paused(false);
        return;
    }

    if (input.pressed & (BTN_LEFT | BTN_UP)) {
        _pause_cursor = (_pause_cursor + 2) % 3;
        audio_play_tone(660.0f, 40, Waveform::SQUARE, 0.20f);
    } else if (input.pressed & (BTN_RIGHT | BTN_DOWN)) {
        _pause_cursor = (_pause_cursor + 1) % 3;
        audio_play_tone(660.0f, 40, Waveform::SQUARE, 0.20f);
    }

    bool confirm = (input.pressed & (BTN_A | BTN_X)) != 0;
    if (input.touch.just_pressed) {
        if (input.touch.y >= 88 && input.touch.y < 120) {
            _pause_cursor = 0; confirm = true;
        } else if (input.touch.y >= 122 && input.touch.y < 154) {
            _pause_cursor = 1; confirm = true;
        } else if (input.touch.y >= 156 && input.touch.y < 188) {
            _pause_cursor = 2; confirm = true;
        }
    }

    if (confirm) {
        audio_play_tone(880.0f, 55, Waveform::SQUARE, 0.24f);
        if (_pause_cursor == 0) {
            _state = State::PLAYING;
            input_set_mode(InputMode::LR_AB);
            audio_bgm_set_paused(false);
        } else if (_pause_cursor == 1) {
            start_song(_song_idx, _difficulty);
        } else {
            audio_bgm_stop();
            _state = State::SONG_SELECT;
        }
    }
}

void RhythmGame::update_result(const InputState& input) {
    if (input.pressed & (BTN_A | BTN_PAUSE)) {
        start_song(_song_idx, _difficulty);
        return;
    }
    if (input.pressed & BTN_B) {
        _state = State::SONG_SELECT;
        return;
    }
    if (input.touch.just_pressed) {
        if (input.touch.x < 160 && input.touch.y >= 175) {
            start_song(_song_idx, _difficulty);
        } else if (input.touch.x >= 160 && input.touch.y >= 175) {
            _state = State::SONG_SELECT;
        }
    }
}

void RhythmGame::draw(lgfx::LGFX_Sprite& fb) {
    switch (_state) {
        case State::SONG_SELECT: draw_song_select(fb); break;
        case State::PLAYING:     draw_playing(fb); break;
        case State::PAUSED:      draw_playing(fb); draw_paused(fb); break;
        case State::RESULT:      draw_result(fb); break;
    }
}

void RhythmGame::draw_song_select(lgfx::LGFX_Sprite& fb) const {
    fb.fillScreen(0x1064);

    // Header
    fb.fillRect(0, 0, SCREEN_WIDTH, 28, 0x2088);
    fb.setTextSize(1);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("TAIKO BEAT! - SELECT SONG", 10, 14);

    fb.fillRoundRect(262, 4, 50, 20, 4, 0x528A);
    fb.drawRoundRect(262, 4, 50, 20, 4, TFT_LIGHTGREY);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_WHITE);
    fb.drawString("MENU", 287, 14);

    // 3 Song cards
    for (int i = 0; i < SONG_COUNT; ++i) {
        const SongMeta& m = SONGS[i];
        const int16_t y = 36 + i * 48;
        const bool sel = (_song_idx == i);
        fb.fillRoundRect(14, y, 292, 42, 8, sel ? 0x294B : 0x18A6);
        fb.drawRoundRect(14, y, 292, 42, 8, sel ? m.accent_color : 0x4228);

        // Mini drum badge
        fb.fillCircle(36, y + 21, 13, m.accent_color);
        fb.fillCircle(36, y + 21, 8, TFT_WHITE);

        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(sel ? TFT_YELLOW : TFT_WHITE);
        fb.drawString(m.title, 58, y + 14);
        fb.setTextColor(TFT_LIGHTGREY);
        char sub[48];
        snprintf(sub, sizeof(sub), "%s  BPM %u", m.subtitle, static_cast<unsigned>(m.bpm));
        fb.drawString(sub, 58, y + 29);

        fb.setTextDatum(MR_DATUM);
        fb.setTextColor(TFT_CYAN);
        char hi[24];
        snprintf(hi, sizeof(hi), "HI:%lu", static_cast<unsigned long>(save_data_get().rhythm_best[i]));
        fb.drawString(hi, 296, y + 21);
    }

    // Footer controls
    fb.setTextDatum(MC_DATUM);
    // Difficulty
    fb.fillRoundRect(14, 192, 92, 36, 6, _difficulty == 0 ? 0x1D44 : 0x8842);
    fb.drawRoundRect(14, 192, 92, 36, 6, TFT_WHITE);
    fb.setTextColor(TFT_WHITE);
    fb.drawString(_difficulty == 0 ? "MODE: EASY" : "MODE: NORM", 60, 210);

    // Offset adjust
    fb.fillRoundRect(114, 192, 42, 36, 6, 0x2945);
    fb.drawRoundRect(114, 192, 42, 36, 6, TFT_LIGHTGREY);
    fb.drawString("OFS-", 135, 210);

    fb.fillRoundRect(160, 192, 46, 36, 6, 0x2945);
    fb.drawRoundRect(160, 192, 46, 36, 6, TFT_LIGHTGREY);
    char ofs_buf[16];
    snprintf(ofs_buf, sizeof(ofs_buf), "%+d+", save_data_get().rhythm_offset_ms);
    fb.drawString(ofs_buf, 183, 210);

    // PLAY button
    fb.fillRoundRect(214, 192, 92, 36, 6, 0xE186);
    fb.drawRoundRect(214, 192, 92, 36, 6, TFT_YELLOW);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("TAP TO PLAY", 260, 210);
}

void RhythmGame::draw_playing(lgfx::LGFX_Sprite& fb) const {
    const SongMeta& meta = SONGS[_song_idx];
    const float beat_phase = (_beat_ms > 0 && _song_time_ms >= 0)
                           ? fmodf(static_cast<float>(_song_time_ms), static_cast<float>(_beat_ms)) / static_cast<float>(_beat_ms)
                           : 0.0f;
    const int beat_bounce = static_cast<int>(sinf(beat_phase * 3.14159f) * 6.0f);

    // Top Stage background
    fb.fillRect(0, 22, SCREEN_WIDTH, 86, (_combo >= 10) ? 0x2847 : 0x1865);
    fb.fillRect(0, 88, SCREEN_WIDTH, 20, 0x3146);

    // Beat-pulsing stage spotlights
    const uint16_t spot_col = (beat_phase < 0.25f) ? meta.accent_color : 0x3188;
    fb.fillTriangle(50, 22, 18, 92, 82, 92, 0x2106);
    fb.fillTriangle(270, 22, 238, 92, 302, 92, 0x2106);
    fb.fillCircle(50, 26, 4, spot_col);
    fb.fillCircle(270, 26, 4, spot_col);

    // Dancing Mascots (Taiko-kun & Star-chan)
    const int cheer_y = (_mascot_cheer_ms > 0) ? -6 : 0;
    const int m1_y = 66 - beat_bounce + cheer_y;
    fb.fillCircle(120, m1_y, 14, TFT_RED);
    fb.fillCircle(120, m1_y, 10, 0xFDE0);
    fb.fillRect(115, m1_y - 3, 3, 3, TFT_BLACK);
    fb.fillRect(122, m1_y - 3, 3, 3, TFT_BLACK);
    fb.fillRect(117, m1_y + 3, 6, 2, TFT_BLACK);

    const int m2_y = 66 - ((beat_bounce + 3) % 6) + cheer_y;
    fb.fillCircle(200, m2_y, 13, TFT_CYAN);
    fb.fillCircle(200, m2_y, 9, TFT_WHITE);
    fb.fillRect(196, m2_y - 3, 2, 3, TFT_BLACK);
    fb.fillRect(202, m2_y - 3, 2, 3, TFT_BLACK);
    fb.fillRect(197, m2_y + 3, 6, 2, TFT_RED);

    // Combo banner on stage
    fb.setTextDatum(MC_DATUM);
    if (_combo >= 2) {
        fb.setTextSize(2);
        fb.setTextColor(_combo >= 10 ? TFT_YELLOW : TFT_WHITE);
        char cbuf[16];
        snprintf(cbuf, sizeof(cbuf), "%lu", static_cast<unsigned long>(_combo));
        fb.drawString(cbuf, 160, 56);
        fb.setTextSize(1);
        fb.setTextColor(_combo >= 10 ? TFT_ORANGE : TFT_CYAN);
        fb.drawString("COMBO!", 160, 76);
    } else {
        fb.setTextSize(1);
        fb.setTextColor(TFT_LIGHTGREY);
        fb.drawString(meta.title, 160, 40);
    }

    // HUD Header + Soul Gauge
    fb.fillRect(0, 0, SCREEN_WIDTH, 22, 0x1042);
    fb.setTextSize(1);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(TFT_WHITE);
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "SCR:%06lu", static_cast<unsigned long>(_score));
    fb.drawString(sbuf, 6, 11);

    // Soul gauge bar
    fb.fillRect(108, 6, 146, 10, 0x2945);
    const int fill_w = (_soul_pct * 144) / 100;
    const uint16_t gauge_col = (_soul_pct >= 70) ? TFT_YELLOW : TFT_ORANGE;
    if (fill_w > 0) fb.fillRect(109, 7, fill_w, 8, gauge_col);
    fb.drawFastVLine(108 + (146 * 70) / 100, 5, 12, TFT_WHITE);
    fb.drawRect(108, 6, 146, 10, TFT_LIGHTGREY);

    // Pause button
    fb.fillRoundRect(RECT_PAUSE_BTN.x, RECT_PAUSE_BTN.y, RECT_PAUSE_BTN.w, RECT_PAUSE_BTN.h, 4, 0x2965);
    fb.drawRoundRect(RECT_PAUSE_BTN.x, RECT_PAUSE_BTN.y, RECT_PAUSE_BTN.w, RECT_PAUSE_BTN.h, 4, TFT_LIGHTGREY);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_WHITE);
    fb.drawString("||", RECT_PAUSE_BTN.x + RECT_PAUSE_BTN.w / 2, RECT_PAUSE_BTN.y + RECT_PAUSE_BTN.h / 2);

    // Note Lane (y: 108..160)
    fb.fillRect(0, 108, SCREEN_WIDTH, 52, 0x2104);
    fb.drawFastHLine(0, 108, SCREEN_WIDTH, 0x6B4D);
    fb.drawFastHLine(0, 160, SCREEN_WIDTH, 0x6B4D);

    // Scrolling beat measure lines
    if (_beat_ms > 0) {
        const uint32_t first_beat = (_song_time_ms > 350) ? static_cast<uint32_t>(_song_time_ms - 350) / _beat_ms : 0;
        for (uint32_t b = first_beat; b < first_beat + 12; ++b) {
            const int32_t bt = static_cast<int32_t>(b * _beat_ms);
            const int16_t bx = TARGET_X + static_cast<int16_t>(static_cast<float>(bt - _song_time_ms) * SCROLL_PX_MS);
            if (bx >= 0 && bx < SCREEN_WIDTH) {
                fb.drawFastVLine(bx, 109, 50, (b % 4 == 0) ? 0x7BEF : 0x39E7);
            }
        }
    }

    // Target hit circle
    const int ring_r = 15 + ((_don_flash_ms > 0 || _ka_flash_ms > 0) ? 2 : 0);
    fb.drawCircle(TARGET_X, LANE_Y, ring_r, TFT_WHITE);
    fb.drawCircle(TARGET_X, LANE_Y, 11, TFT_LIGHTGREY);

    // Draw approaching notes (reverse order so earliest note renders on top!)
    for (int i = static_cast<int>(_chart_count) - 1; i >= static_cast<int>(_next_note_idx); --i) {
        const ChartNote& n = _chart[i];
        if (n.hit || n.missed) continue;
        const int32_t dt = static_cast<int32_t>(n.time_ms) - _song_time_ms;
        const int16_t nx = TARGET_X + static_cast<int16_t>(static_cast<float>(dt) * SCROLL_PX_MS);
        if (nx < -20 || nx > SCREEN_WIDTH + 20) continue;

        const bool is_don = (n.kind == NoteKind::DON || n.kind == NoteKind::BIG_DON);
        const bool is_big = (n.kind == NoteKind::BIG_DON || n.kind == NoteKind::BIG_KA);
        const int r = is_big ? 15 : 11;
        const uint16_t fill = is_don ? TFT_RED : TFT_BLUE;
        if (is_big) fb.drawCircle(nx, LANE_Y, r + 2, TFT_YELLOW);
        fb.fillCircle(nx, LANE_Y, r, TFT_WHITE);
        fb.fillCircle(nx, LANE_Y, r - 2, fill);
        fb.fillRect(nx - 3, LANE_Y - 2, 2, 3, TFT_WHITE);
        fb.fillRect(nx + 2, LANE_Y - 2, 2, 3, TFT_WHITE);
    }

    // Judge feedback popup
    if (_judge_timer_ms > 0 && _last_judge != JudgeKind::NONE) {
        const char* jtxt = (_last_judge == JudgeKind::PERFECT) ? "PERFECT!"
                         : (_last_judge == JudgeKind::GOOD)    ? "GOOD"
                                                               : "MISS";
        const uint16_t jcol = (_last_judge == JudgeKind::PERFECT) ? TFT_YELLOW
                            : (_last_judge == JudgeKind::GOOD)    ? TFT_CYAN
                                                                  : TFT_LIGHTGREY;
        fb.setTextDatum(MC_DATUM);
        fb.setTextSize(1);
        fb.setTextColor(jcol);
        fb.drawString(jtxt, TARGET_X, 98);
    }

    // Bottom Drum Pads (Touch & Physical mirror)
    const uint16_t don_fill = (_don_flash_ms > 0) ? TFT_RED : 0x8800;
    const uint16_t ka_fill  = (_ka_flash_ms > 0)  ? TFT_CYAN : 0x0212;
    fb.fillRoundRect(RECT_PAD_DON.x, RECT_PAD_DON.y, RECT_PAD_DON.w, RECT_PAD_DON.h, 10, don_fill);
    fb.drawRoundRect(RECT_PAD_DON.x, RECT_PAD_DON.y, RECT_PAD_DON.w, RECT_PAD_DON.h, 10, TFT_WHITE);
    fb.fillRoundRect(RECT_PAD_KA.x, RECT_PAD_KA.y, RECT_PAD_KA.w, RECT_PAD_KA.h, 10, ka_fill);
    fb.drawRoundRect(RECT_PAD_KA.x, RECT_PAD_KA.y, RECT_PAD_KA.w, RECT_PAD_KA.h, 10, TFT_WHITE);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_WHITE);
    fb.setTextSize(2);
    fb.drawString("DON", RECT_PAD_DON.x + RECT_PAD_DON.w / 2, RECT_PAD_DON.y + 24);
    fb.drawString("KA",  RECT_PAD_KA.x + RECT_PAD_KA.w / 2,  RECT_PAD_KA.y + 24);
    fb.setTextSize(1);
    fb.setTextColor(0xFDE0);
    fb.drawString("[L / A / L1 / TAP]", RECT_PAD_DON.x + RECT_PAD_DON.w / 2, RECT_PAD_DON.y + 49);
    fb.drawString("[R / B / R1 / TAP]", RECT_PAD_KA.x + RECT_PAD_KA.w / 2,  RECT_PAD_KA.y + 49);
}

void RhythmGame::draw_paused(lgfx::LGFX_Sprite& fb) const {
    fb.fillRoundRect(56, 48, 208, 154, 10, 0x1082);
    fb.drawRoundRect(56, 48, 208, 154, 10, TFT_WHITE);
    fb.setTextDatum(MC_DATUM);
    fb.setTextSize(2);
    fb.setTextColor(TFT_YELLOW);
    fb.drawString("PAUSED", 160, 68);

    fb.setTextSize(1);
    const char* opts[3] = {"RESUME", "RESTART SONG", "SONG SELECT"};
    for (int i = 0; i < 3; ++i) {
        const int y = 90 + i * 34;
        const bool sel = (_pause_cursor == i);
        fb.fillRoundRect(82, y, 156, 26, 6, sel ? 0x2589 : 0x2945);
        fb.drawRoundRect(82, y, 156, 26, 6, sel ? TFT_YELLOW : TFT_LIGHTGREY);
        fb.setTextColor(sel ? TFT_YELLOW : TFT_WHITE);
        fb.drawString(opts[i], 160, y + 13);
    }
    fb.setTextColor(TFT_LIGHTGREY);
    fb.drawString("L/R:Select  A:OK  B:Back", 160, 194);
}

void RhythmGame::draw_result(lgfx::LGFX_Sprite& fb) const {
    fb.fillScreen(0x1064);
    const bool cleared = (_soul_pct >= 70);

    fb.fillRoundRect(16, 14, 288, 212, 10, 0x18C6);
    fb.drawRoundRect(16, 14, 288, 212, 10, cleared ? TFT_YELLOW : TFT_ORANGE);

    fb.setTextDatum(MC_DATUM);
    fb.setTextSize(2);
    fb.setTextColor(cleared ? TFT_YELLOW : TFT_ORANGE);
    fb.drawString(cleared ? "STAGE CLEAR!" : "TRY AGAIN!", 160, 38);

    const uint32_t total = _perfect_cnt + _good_cnt + _miss_cnt;
    const uint32_t acc = (total > 0) ? (((_perfect_cnt * 100U) + (_good_cnt * 60U)) / total) : 0;
    const char* rank = (acc >= 90) ? "S" : (acc >= 78) ? "A" : (acc >= 62) ? "B" : "C";

    // Big Rank Badge
    fb.fillCircle(66, 106, 28, cleared ? 0xD400 : 0x4228);
    fb.drawCircle(66, 106, 28, TFT_WHITE);
    fb.setTextSize(3);
    fb.setTextColor(TFT_WHITE);
    fb.drawString(rank, 66, 106);

    // Stats table
    fb.setTextDatum(ML_DATUM);
    fb.setTextSize(1);
    char line[40];
    snprintf(line, sizeof(line), "SCORE: %lu %s",
             static_cast<unsigned long>(_score), _new_record ? "NEW!" : "");
    fb.setTextColor(_new_record ? TFT_YELLOW : TFT_WHITE);
    fb.drawString(line, 110, 74);

    snprintf(line, sizeof(line), "PERFECT: %u   GOOD: %u", _perfect_cnt, _good_cnt);
    fb.setTextColor(TFT_CYAN);
    fb.drawString(line, 110, 96);

    snprintf(line, sizeof(line), "MISS: %u   MAX COMBO: %lu",
             _miss_cnt, static_cast<unsigned long>(_max_combo));
    fb.setTextColor(TFT_LIGHTGREY);
    fb.drawString(line, 110, 114);

    snprintf(line, sizeof(line), "SOUL GAUGE: %d%%  (%lu%% ACC)",
             static_cast<int>(_soul_pct), static_cast<unsigned long>(acc));
    fb.setTextColor(cleared ? TFT_GREEN : TFT_ORANGE);
    fb.drawString(line, 110, 134);

    // Action buttons
    fb.setTextDatum(MC_DATUM);
    fb.fillRoundRect(32, 178, 118, 34, 6, 0x2625);
    fb.drawRoundRect(32, 178, 118, 34, 6, TFT_WHITE);
    fb.setTextColor(TFT_WHITE);
    fb.drawString("RETRY (A)", 91, 195);

    fb.fillRoundRect(170, 178, 118, 34, 6, 0x528A);
    fb.drawRoundRect(170, 178, 118, 34, 6, TFT_LIGHTGREY);
    fb.drawString("SONGS (B)", 229, 195);
}
