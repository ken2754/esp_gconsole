#pragma once

#include <stddef.h>
#include <stdint.h>

#include "game/game_app.hpp"
#include "hal/audio.hpp"

/**
 * @brief Taiko & Rhythm Heaven inspired rhythm game ("TAIKO BEAT!").
 *        - Uses I2S sample-accurate BGM sequencer clock (zero A/V drift)
 *        - 3 original chiptune songs x 2 difficulties (EASY / NORMAL)
 *        - Supports both Touch drum pads (left=DON, right=KA) and physical buttons (L/A=DON, R/B=KA)
 *        - Beat-synced dancing mascots, fever gauge, combo popups, S/A/B/C grading
 */
class RhythmGame final : public GameApp {
public:
    const char* name() const override { return "TAIKO BEAT!"; }
    void init() override;
    void update(uint32_t dt_ms, const InputState& input) override;
    void draw(lgfx::LGFX_Sprite& fb) override;
    void on_exit() override;
    bool wants_exit() const override { return _wants_exit; }

private:
    enum class State : uint8_t {
        SONG_SELECT = 0,
        PLAYING,
        PAUSED,
        RESULT,
    };

    enum class NoteKind : uint8_t {
        DON = 0,
        KA,
        BIG_DON,
        BIG_KA,
    };

    enum class JudgeKind : uint8_t {
        NONE = 0,
        PERFECT,
        GOOD,
        MISS,
    };

    struct ChartNote {
        uint32_t time_ms = 0;
        NoteKind kind    = NoteKind::DON;
        bool     hit     = false;
        bool     missed  = false;
    };

    static constexpr int SONG_COUNT      = 3;
    static constexpr int MAX_BGM_NOTES   = 512;
    static constexpr int MAX_CHART_NOTES = 256;

    State    _state          = State::SONG_SELECT;
    bool     _wants_exit     = false;
    uint8_t  _song_idx       = 0;
    uint8_t  _difficulty     = 0; // 0 = EASY, 1 = NORMAL
    uint8_t  _pause_cursor   = 0;
    uint32_t _anim_ms        = 0;

    // Song playback & chart
    BgmNote   _bgm_notes[MAX_BGM_NOTES] = {};
    size_t    _bgm_count                = 0;
    ChartNote _chart[MAX_CHART_NOTES]   = {};
    size_t    _chart_count              = 0;
    size_t    _next_note_idx            = 0;
    uint32_t  _song_len_ms              = 0;
    uint32_t  _beat_ms                  = 500;
    int32_t   _song_time_ms             = 0;
    uint32_t  _end_wait_ms              = 0;

    // Scoring & feedback
    uint32_t  _score          = 0;
    uint32_t  _combo          = 0;
    uint32_t  _max_combo      = 0;
    uint16_t  _perfect_cnt    = 0;
    uint16_t  _good_cnt       = 0;
    uint16_t  _miss_cnt       = 0;
    int16_t   _soul_pct       = 30; // 0..100, >=70 clear
    bool      _new_record     = false;

    JudgeKind _last_judge     = JudgeKind::NONE;
    uint32_t  _judge_timer_ms = 0;
    uint32_t  _don_flash_ms   = 0;
    uint32_t  _ka_flash_ms    = 0;
    uint32_t  _mascot_cheer_ms = 0;

    void start_song(uint8_t song_idx, uint8_t difficulty);
    void build_song_data(uint8_t song_idx, uint8_t difficulty);
    void handle_hit(bool is_don);
    void trigger_judge(JudgeKind j, bool is_big);

    void update_song_select(const InputState& input);
    void update_playing(uint32_t dt_ms, const InputState& input);
    void update_paused(const InputState& input);
    void update_result(const InputState& input);

    void draw_song_select(lgfx::LGFX_Sprite& fb) const;
    void draw_playing(lgfx::LGFX_Sprite& fb) const;
    void draw_paused(lgfx::LGFX_Sprite& fb) const;
    void draw_result(lgfx::LGFX_Sprite& fb) const;
};
