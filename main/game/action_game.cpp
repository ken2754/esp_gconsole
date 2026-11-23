#include "game/action_game.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "hal/audio.hpp"
#include "hal/save_data.hpp"

namespace {

// ============================================================================
// Chiptune BGM Tables (sample-accurate via hal/audio)
// ============================================================================

// Cheerful Overworld Loop (125 BPM -> 1 eighth note = 240 ms, 16 bars = 7680 ms)
constexpr BgmNote BGM_OVERWORLD[] = {
    // Melody (Square) + Bass (Triangle)
    {0,    659, 0, 140, Waveform::SQUARE,   22}, {0,    165, 0, 210, Waveform::TRIANGLE, 26},
    {240,  659, 0, 140, Waveform::SQUARE,   22},
    {480,  0,   0, 10,  Waveform::SQUARE,   0},  {480,  196, 0, 210, Waveform::TRIANGLE, 26},
    {600,  659, 0, 140, Waveform::SQUARE,   22},
    {840,  523, 0, 140, Waveform::SQUARE,   22}, {960,  262, 0, 210, Waveform::TRIANGLE, 26},
    {1080, 659, 0, 180, Waveform::SQUARE,   22},
    {1320, 784, 0, 280, Waveform::SQUARE,   24}, {1440, 196, 0, 360, Waveform::TRIANGLE, 26},
    {1800, 392, 0, 260, Waveform::SQUARE,   20},

    {2160, 523, 0, 200, Waveform::SQUARE,   22}, {2160, 131, 0, 220, Waveform::TRIANGLE, 26},
    {2520, 392, 0, 200, Waveform::SQUARE,   20}, {2640, 196, 0, 220, Waveform::TRIANGLE, 26},
    {2880, 330, 0, 220, Waveform::SQUARE,   20}, {3120, 165, 0, 220, Waveform::TRIANGLE, 26},
    {3240, 440, 0, 160, Waveform::SQUARE,   22},
    {3480, 494, 0, 160, Waveform::SQUARE,   22}, {3600, 247, 0, 200, Waveform::TRIANGLE, 26},
    {3720, 466, 0, 120, Waveform::SQUARE,   20},
    {3840, 440, 0, 200, Waveform::SQUARE,   22}, {3840, 220, 0, 220, Waveform::TRIANGLE, 26},

    {4080, 392, 0, 150, Waveform::SQUARE,   22}, {4080, 196, 0, 200, Waveform::TRIANGLE, 26},
    {4320, 659, 0, 150, Waveform::SQUARE,   22},
    {4560, 784, 0, 150, Waveform::SQUARE,   22}, {4560, 262, 0, 200, Waveform::TRIANGLE, 26},
    {4800, 880, 0, 220, Waveform::SQUARE,   24},
    {5040, 698, 0, 140, Waveform::SQUARE,   22}, {5040, 175, 0, 220, Waveform::TRIANGLE, 26},
    {5280, 784, 0, 140, Waveform::SQUARE,   22},
    {5520, 659, 0, 180, Waveform::SQUARE,   22}, {5520, 196, 0, 220, Waveform::TRIANGLE, 26},
    {5760, 523, 0, 150, Waveform::SQUARE,   22},
    {6000, 587, 0, 150, Waveform::SQUARE,   22}, {6000, 196, 0, 220, Waveform::TRIANGLE, 26},
    {6240, 494, 0, 220, Waveform::SQUARE,   22},
    {6480, 523, 0, 180, Waveform::SQUARE,   22}, {6480, 262, 0, 220, Waveform::TRIANGLE, 26},
    {6720, 392, 0, 160, Waveform::SQUARE,   18}, {6960, 196, 0, 200, Waveform::TRIANGLE, 24},
    {7200, 523, 0, 180, Waveform::SQUARE,   20}, {7440, 196, 0, 200, Waveform::TRIANGLE, 24},
};

// Starlight Castle Loop (Upbeat energetic minor groove, 3840 ms)
constexpr BgmNote BGM_CASTLE[] = {
    {0,    440, 0, 150, Waveform::SQUARE,   22}, {0,    110, 0, 180, Waveform::TRIANGLE, 28},
    {240,  523, 0, 150, Waveform::SQUARE,   22},
    {480,  659, 0, 150, Waveform::SQUARE,   22}, {480,  165, 0, 180, Waveform::TRIANGLE, 28},
    {720,  587, 0, 150, Waveform::SQUARE,   22},
    {960,  523, 0, 150, Waveform::SQUARE,   22}, {960,  131, 0, 180, Waveform::TRIANGLE, 28},
    {1200, 494, 0, 150, Waveform::SQUARE,   22},
    {1440, 440, 0, 200, Waveform::SQUARE,   24}, {1440, 147, 0, 200, Waveform::TRIANGLE, 28},
    {1680, 392, 0, 180, Waveform::SQUARE,   20},

    {1920, 440, 0, 150, Waveform::SQUARE,   22}, {1920, 110, 0, 180, Waveform::TRIANGLE, 28},
    {2160, 523, 0, 150, Waveform::SQUARE,   22},
    {2400, 659, 0, 150, Waveform::SQUARE,   22}, {2400, 165, 0, 180, Waveform::TRIANGLE, 28},
    {2640, 784, 0, 180, Waveform::SQUARE,   24},
    {2880, 880, 0, 200, Waveform::SQUARE,   24}, {2880, 220, 0, 200, Waveform::TRIANGLE, 28},
    {3120, 784, 0, 160, Waveform::SQUARE,   22},
    {3360, 659, 0, 200, Waveform::SQUARE,   22}, {3360, 165, 0, 200, Waveform::TRIANGLE, 28},
    {3600, 587, 0, 180, Waveform::SQUARE,   20},
};

// Stage Clear Fanfare (non-looping)
constexpr BgmNote BGM_CLEAR_FANFARE[] = {
    {0,   523, 0, 110, Waveform::SQUARE, 28}, {0,   262, 0, 110, Waveform::TRIANGLE, 26},
    {120, 659, 0, 110, Waveform::SQUARE, 28},
    {240, 784, 0, 110, Waveform::SQUARE, 28},
    {360, 1046, 0, 240, Waveform::SQUARE, 32}, {360, 523, 0, 240, Waveform::TRIANGLE, 28},
    {620, 784, 0, 120, Waveform::SQUARE, 28},
    {760, 1046, 0, 520, Waveform::SQUARE, 34}, {760, 262, 0, 520, Waveform::TRIANGLE, 30},
};

// ============================================================================
// Handcrafted Stage Maps (15 rows tall)
// Legend:
//   . = air, # = ground, B = brick, ? = coin block, M = mushroom block,
//   S = star block, = = one-way platform, []/() = pipe, ^ = spike,
//   o = coin, G = Kuri, K = Kame, J = Flappy, P = start, C = checkpoint, F = goal
// ============================================================================

constexpr const char* STAGE_1_ROWS[15] = {
    "................................................................................................",
    "................................................................................................",
    "................................................................................................",
    "................................................................................................",
    "....................ooo............................................ooo....................F.....",
    "..................B?M?B.........................oooo.............=o===o=..................F.....",
    "..............................................========....................................F.....",
    ".............o....................ooo.......................B?B.....................oo....F.....",
    "...........B?B...................B?B?B..............oo.............................BMB....F.....",
    "...................................................====...................B?B.............F.....",
    "........................[]...............[]......................[].......................F.....",
    "........................()......oo.......()......................()......ooo......B.......F.....",
    ".P....o.....G....G......()..G...==...K...()..G.....C......G..K...()..G...===..G..BB..K....F.....",
    "###################..#################################..####################..##################",
    "###################..#################################..####################..##################",
};

constexpr const char* STAGE_2_ROWS[15] = {
    "........................................................................................................",
    "........................................................................................................",
    "........................................................................................................",
    "......................................oooo..............................................................",
    "......................ooo...........========..............................ooo.....................F.....",
    "....................==o==o==.......................B?S?B................==o==o==..................F.....",
    "..................................................................................................F.....",
    "............B?M?B..................J.............................................B?B..............F.....",
    "...........................................ooo................ooo.................................F.....",
    ".......ooo.................===............==o==..............==o==.........J..............BB......F.....",
    "......=====.......................................[]................[]...................BBB......F.....",
    "....................oo...........[]...............().......J........()...........oo.....BBBB......F.....",
    ".P....G......K......==....G......()..G......K.....()...C......G..K..()...G...K...==....BBBBB......F.....",
    "################..####..###..#########################..###############..########..#####################",
    "################..####..###..#########################..###############..########..#####################",
};

constexpr const char* STAGE_3_ROWS[15] = {
    "................................................................................................................",
    "................................................................................................................",
    "................................................................................................................",
    "................................................................................................................",
    "..........................oooo......................................oooo..................................F.....",
    "........................B?M?B?B...................................==o==o==................................F.....",
    ".............................................B?S?B......................................B?B...............F.....",
    ".............ooo..........................................J...............................................F.....",
    "............==o==.............J................................................ooo................BB......F.....",
    "......................BB...............ooo................===.................==o==..............BBB......F.....",
    ".......[]............BBB..............==o==.......................[]....................J.......BBBB......F.....",
    ".......()......oo...BBBB.......................oo.................().......oo..................BBBBB......F.....",
    ".P..G..()..K...==..BBBBB....G.....K.....G......==....C.....G...K..()..G....==...K....G........BBBBBB......F.....",
    "##########^^^^################..####..#####^^^^###############..##################^^^^##########################",
    "################################################################################################################",
};

struct StageDef {
    const char*        title;
    const char* const* rows;
    uint16_t           sky_top;
    uint16_t           sky_bot;
    uint16_t           ground_top;
    uint16_t           ground_fill;
    bool               is_castle;
};

constexpr StageDef STAGES[] = {
    {"1-1 SUNNY MEADOW",     STAGE_1_ROWS, 0x3C9F, 0x7E3F, 0x2E45, 0xA322, false},
    {"1-2 SKY BRIDGES",      STAGE_2_ROWS, 0xF9E6, 0xFDE9, 0x4565, 0x8262, false},
    {"1-3 STARLIGHT CASTLE", STAGE_3_ROWS, 0x1064, 0x2088, 0x6B4D, 0x39C7, true},
};

constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

constexpr float GRAVITY_PX       = 980.0f;
constexpr float JUMP_VEL         = -332.0f;
constexpr float DASH_JUMP_VEL    = -352.0f;
constexpr float WALK_SPEED       = 112.0f;
constexpr float DASH_SPEED       = 166.0f;
constexpr float ACCEL_GROUND     = 920.0f;
constexpr float ACCEL_AIR        = 620.0f;
constexpr float FRICTION_GROUND  = 860.0f;

struct Rect {
    int16_t x, y, w, h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr Rect RECT_PAUSE_BTN  = {278, 2, 40, 18};
constexpr Rect RECT_TITLE_GO   = {76, 154, 168, 36};
constexpr Rect RECT_TITLE_EXIT = {96, 198, 128, 26};

bool aabb_overlap(float ax, float ay, float aw, float ah,
                  float bx, float by, float bw, float bh) {
    return ax < (bx + bw) && (ax + aw) > bx && ay < (by + bh) && (ay + ah) > by;
}

uint16_t rainbow_color(uint32_t ms) {
    static constexpr uint16_t PAL[] = {
        TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_CYAN, TFT_MAGENTA
    };
    return PAL[(ms / 60) % 6];
}

} // namespace

void ActionGame::init() {
    _wants_exit     = false;
    _stage_idx      = 0;
    _lives          = 3;
    _score          = 0;
    _coins          = 0;
    _anim_ms        = 0;
    _pause_cursor   = 0;
    _is_super       = false;
    _state          = State::TITLE;
    input_set_mode(InputMode::LR_AB);
    audio_bgm_stop();
    load_stage(0, false);
}

void ActionGame::on_exit() {
    audio_bgm_stop();
}

void ActionGame::start_stage_bgm() {
    if (STAGES[_stage_idx].is_castle) {
        audio_bgm_play(BGM_CASTLE, sizeof(BGM_CASTLE) / sizeof(BGM_CASTLE[0]), 3840, true);
    } else {
        audio_bgm_play(BGM_OVERWORLD, sizeof(BGM_OVERWORLD) / sizeof(BGM_OVERWORLD[0]), 7680, true);
    }
}

void ActionGame::load_stage(uint8_t stage_idx, bool use_checkpoint) {
    _stage_idx = stage_idx % STAGE_COUNT;
    const StageDef& def = STAGES[_stage_idx];

    memset(_tiles, 0, sizeof(_tiles));
    memset(_coin_grid, 0, sizeof(_coin_grid));
    for (auto& e : _enemies) e = Enemy{};
    for (auto& it : _items) it = Item{};
    for (auto& p : _particles) p = Particle{};
    for (auto& pop : _popups) pop = Popup{};
    _bumped = BumpedTile{};

    _map_w          = static_cast<int>(strlen(def.rows[0]));
    if (_map_w > MAX_MAP_W) _map_w = MAX_MAP_W;
    _spawn_x        = 24.0f;
    _spawn_y        = 176.0f;
    _has_checkpoint = false;
    _goal_x         = (_map_w - 6) * TILE_SIZE;
    if (!use_checkpoint) {
        _checkpoint_hit = false;
        _stage_time_ms  = 0;
    }

    int enemy_cnt = 0;
    for (int r = 0; r < MAP_H; ++r) {
        const char* row = def.rows[r];
        const int row_len = static_cast<int>(strlen(row));
        for (int c = 0; c < _map_w && c < row_len; ++c) {
            const char ch = row[c];
            switch (ch) {
                case '#': _tiles[r][c] = TILE_GROUND;     break;
                case 'B': _tiles[r][c] = TILE_BRICK;      break;
                case '?': _tiles[r][c] = TILE_Q_COIN;     break;
                case 'M': _tiles[r][c] = TILE_Q_MUSHROOM; break;
                case 'S': _tiles[r][c] = TILE_Q_STAR;     break;
                case '=': _tiles[r][c] = TILE_ONEWAY;     break;
                case '[': _tiles[r][c] = TILE_PIPE_TL;    break;
                case ']': _tiles[r][c] = TILE_PIPE_TR;    break;
                case '(': _tiles[r][c] = TILE_PIPE_BL;    break;
                case ')': _tiles[r][c] = TILE_PIPE_BR;    break;
                case '^': _tiles[r][c] = TILE_SPIKE;      break;
                case 'o': _coin_grid[r][c] = true;        break;
                case 'P':
                    _spawn_x = c * TILE_SIZE + 2.0f;
                    _spawn_y = (r + 1) * TILE_SIZE - player_h();
                    break;
                case 'C':
                    _has_checkpoint = true;
                    _checkpoint_x = c * TILE_SIZE;
                    _checkpoint_y = (r + 1) * TILE_SIZE - player_h();
                    break;
                case 'F':
                    _goal_x = c * TILE_SIZE;
                    break;
                case 'G':
                case 'K':
                case 'J':
                    if (enemy_cnt < MAX_ENEMIES) {
                        Enemy& e = _enemies[enemy_cnt++];
                        e.active = true;
                        e.x      = c * TILE_SIZE + 1.0f;
                        e.y      = r * TILE_SIZE + 2.0f;
                        e.base_y = e.y;
                        e.vx     = (ch == 'K') ? -42.0f : -34.0f;
                        if (ch == 'G') e.kind = EnemyKind::KURI;
                        else if (ch == 'K') e.kind = EnemyKind::KAME;
                        else e.kind = EnemyKind::FLAPPY;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (use_checkpoint && _checkpoint_hit && _has_checkpoint) {
        _px = _checkpoint_x;
        _py = _checkpoint_y;
    } else {
        _px = _spawn_x;
        _py = _spawn_y;
    }
    _pvx          = 0.0f;
    _pvy          = 0.0f;
    _on_ground    = false;
    _facing_right = true;
    _coyote_ms    = 0;
    _jump_buf_ms  = 0;
    _invuln_ms    = 0;
    _star_ms      = 0;
    _flag_y       = 4.5f * TILE_SIZE;
    _cam_x        = _px - SCREEN_WIDTH * 0.35f;
    if (_cam_x < 0.0f) _cam_x = 0.0f;
    const float max_cam = static_cast<float>(_map_w * TILE_SIZE - SCREEN_WIDTH);
    if (_cam_x > max_cam) _cam_x = max_cam;
}

bool ActionGame::is_solid(int tx, int ty) const {
    if (tx < 0 || tx >= _map_w) return true;
    if (ty < 0) return false;
    if (ty >= MAP_H) return false;
    const TileType t = _tiles[ty][tx];
    return t == TILE_GROUND || t == TILE_BRICK || t == TILE_Q_COIN ||
           t == TILE_Q_MUSHROOM || t == TILE_Q_STAR || t == TILE_USED ||
           t == TILE_PIPE_TL || t == TILE_PIPE_TR || t == TILE_PIPE_BL || t == TILE_PIPE_BR;
}

bool ActionGame::is_oneway(int tx, int ty) const {
    if (tx < 0 || tx >= _map_w || ty < 0 || ty >= MAP_H) return false;
    return _tiles[ty][tx] == TILE_ONEWAY;
}

void ActionGame::add_score(uint32_t pts, float world_x, float world_y, uint16_t color, const char* label) {
    _score += pts;
    save_data_update_action_best(_score);
    for (auto& pop : _popups) {
        if (!pop.active) {
            pop.active  = true;
            pop.x       = world_x;
            pop.y       = world_y;
            pop.color   = color;
            pop.life_ms = 650;
            if (label) {
                snprintf(pop.text, sizeof(pop.text), "%s", label);
            } else {
                snprintf(pop.text, sizeof(pop.text), "+%lu", static_cast<unsigned long>(pts));
            }
            break;
        }
    }
}

void ActionGame::spawn_particles(float x, float y, uint16_t color, int count, float speed) {
    int spawned = 0;
    for (auto& p : _particles) {
        if (p.active) continue;
        const float ang = (6.28318f * static_cast<float>(spawned)) / static_cast<float>(count) + 0.35f;
        p.active  = true;
        p.x       = x;
        p.y       = y;
        p.vx      = cosf(ang) * speed;
        p.vy      = sinf(ang) * speed - 90.0f;
        p.color   = color;
        p.size    = 4;
        p.life_ms = 420;
        if (++spawned >= count) break;
    }
}

void ActionGame::spawn_item(ItemKind kind, float x, float y) {
    for (auto& it : _items) {
        if (!it.active) {
            it.active    = true;
            it.kind      = kind;
            it.x         = x + 2.0f;
            it.y         = y - 4.0f;
            it.vx        = 58.0f;
            it.vy        = 0.0f;
            it.rise_left = 12.0f;
            break;
        }
    }
}

void ActionGame::hit_block_from_below(int tx, int ty) {
    if (tx < 0 || tx >= _map_w || ty < 0 || ty >= MAP_H) return;
    TileType& t = _tiles[ty][tx];

    _bumped.tx       = tx;
    _bumped.ty       = ty;
    _bumped.timer_ms = 140;

    const float wx = tx * TILE_SIZE + 8.0f;
    const float wy = ty * TILE_SIZE;

    // Bump any enemy standing right above this block
    for (auto& e : _enemies) {
        if (!e.active || e.squashed) continue;
        if (aabb_overlap(e.x, e.y, 14.0f, 14.0f, tx * TILE_SIZE, (ty - 1) * TILE_SIZE, 16.0f, 16.0f)) {
            e.active = false;
            spawn_particles(e.x + 7.0f, e.y + 7.0f, TFT_ORANGE, 6, 80.0f);
            add_score(200, e.x, e.y - 6.0f, TFT_YELLOW);
            audio_play_sweep(350.0f, 150.0f, 90, Waveform::SQUARE, 0.28f);
        }
    }

    if (t == TILE_Q_COIN) {
        t = TILE_USED;
        ++_coins;
        if (_coins % 50 == 0) {
            ++_lives;
            add_score(500, wx, wy - 8.0f, TFT_GREEN, "1UP!");
            audio_play_tone(1046.5f, 120, Waveform::SQUARE, 0.35f);
        } else {
            add_score(100, wx, wy - 8.0f, TFT_YELLOW);
        }
        spawn_particles(wx, wy - 4.0f, TFT_YELLOW, 5, 70.0f);
        audio_play_tone(987.77f, 55, Waveform::SQUARE, 0.25f);
        audio_play_tone(1318.5f, 110, Waveform::SQUARE, 0.25f);
    } else if (t == TILE_Q_MUSHROOM) {
        t = TILE_USED;
        spawn_item(ItemKind::MUSHROOM, tx * TILE_SIZE, wy);
        audio_play_sweep(440.0f, 880.0f, 140, Waveform::TRIANGLE, 0.30f);
    } else if (t == TILE_Q_STAR) {
        t = TILE_USED;
        spawn_item(ItemKind::STAR, tx * TILE_SIZE, wy);
        audio_play_sweep(523.0f, 1046.0f, 160, Waveform::SQUARE, 0.30f);
    } else if (t == TILE_BRICK) {
        if (_is_super) {
            t = TILE_AIR;
            spawn_particles(wx, wy + 8.0f, 0xC362, 6, 110.0f);
            add_score(50, wx, wy, 0xFD20);
            audio_play_tone(180.0f, 80, Waveform::NOISE, 0.35f);
        } else {
            audio_play_tone(150.0f, 50, Waveform::TRIANGLE, 0.28f);
        }
    } else if (t == TILE_USED) {
        audio_play_tone(130.0f, 40, Waveform::TRIANGLE, 0.22f);
    }
}

void ActionGame::hurt_player() {
    if (_invuln_ms > 0 || _star_ms > 0) return;

    if (_is_super) {
        _is_super  = false;
        _py       += 6.0f;
        _invuln_ms = 1800;
        _pvy       = -180.0f;
        audio_play_sweep(580.0f, 180.0f, 220, Waveform::SQUARE, 0.32f);
        spawn_particles(_px + 6.0f, _py + 7.0f, TFT_WHITE, 6, 80.0f);
        return;
    }

    if (_lives > 0) --_lives;
    audio_bgm_stop();
    audio_play_sweep(440.0f, 110.0f, 380, Waveform::SQUARE, 0.35f);
    _state = State::HURT_WAIT;
    _state_timer_ms = 1100;
}

void ActionGame::update(uint32_t dt_ms, const InputState& input) {
    _anim_ms += dt_ms;

    if (_state == State::TITLE) {
        input_set_mode(InputMode::LR_AB);
        if (input.pressed & (BTN_A | BTN_X | BTN_PAUSE)) {
            _stage_idx = 0;
            _lives     = 3;
            _score     = 0;
            _coins     = 0;
            _is_super  = false;
            load_stage(0, false);
            _state = State::PLAYING;
            start_stage_bgm();
            audio_play_tone(783.99f, 90, Waveform::SQUARE, 0.28f);
        } else if (input.pressed & (BTN_B | BTN_Y)) {
            _wants_exit = true;
        } else if (input.touch.just_pressed) {
            if (RECT_TITLE_GO.contains(input.touch.x, input.touch.y)) {
                _stage_idx = 0;
                _lives     = 3;
                _score     = 0;
                _coins     = 0;
                _is_super  = false;
                load_stage(0, false);
                _state = State::PLAYING;
                start_stage_bgm();
                audio_play_tone(783.99f, 90, Waveform::SQUARE, 0.28f);
            } else if (RECT_TITLE_EXIT.contains(input.touch.x, input.touch.y)) {
                _wants_exit = true;
            }
        }
        return;
    }

    if (_state == State::PAUSED) {
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
            audio_play_tone(880.0f, 60, Waveform::SQUARE, 0.25f);
            if (_pause_cursor == 0) {
                _state = State::PLAYING;
                input_set_mode(InputMode::LR_AB);
                audio_bgm_set_paused(false);
            } else if (_pause_cursor == 1) {
                input_set_mode(InputMode::LR_AB);
                load_stage(_stage_idx, false);
                _state = State::PLAYING;
                start_stage_bgm();
            } else {
                audio_bgm_stop();
                _wants_exit = true;
            }
        }
        return;
    }

    if (_state == State::HURT_WAIT) {
        if (_state_timer_ms > dt_ms) {
            _state_timer_ms -= dt_ms;
        } else {
            if (_lives == 0) {
                _state = State::GAME_OVER;
                save_data_update_action_best(_score);
            } else {
                load_stage(_stage_idx, true);
                _state = State::PLAYING;
                start_stage_bgm();
            }
        }
        return;
    }

    if (_state == State::STAGE_CLEAR) {
        if (_flag_y < 11.5f * TILE_SIZE) {
            _flag_y += 110.0f * (dt_ms * 0.001f);
            if (_flag_y > 11.5f * TILE_SIZE) _flag_y = 11.5f * TILE_SIZE;
        }
        if (_state_timer_ms > dt_ms) {
            _state_timer_ms -= dt_ms;
        } else {
            if (_stage_idx + 1 < STAGE_COUNT) {
                load_stage(_stage_idx + 1, false);
                _state = State::PLAYING;
                start_stage_bgm();
            } else {
                save_data_update_action_best(_score);
                _state = State::VICTORY;
            }
        }
        return;
    }

    if (_state == State::GAME_OVER || _state == State::VICTORY) {
        if (input.pressed & (BTN_A | BTN_X | BTN_PAUSE) || input.touch.just_pressed) {
            if (input.touch.just_pressed && input.touch.x > 170 && input.touch.y > 170) {
                _wants_exit = true;
            } else {
                _stage_idx = 0;
                _lives     = 3;
                _score     = 0;
                _coins     = 0;
                _is_super  = false;
                load_stage(0, false);
                _state = State::PLAYING;
                start_stage_bgm();
            }
        } else if (input.pressed & (BTN_B | BTN_Y)) {
            _wants_exit = true;
        }
        return;
    }

    // PLAYING state
    if ((input.pressed & (BTN_PAUSE | BTN_SELECT)) ||
        (input.touch.just_pressed && RECT_PAUSE_BTN.contains(input.touch.x, input.touch.y))) {
        _state        = State::PAUSED;
        _pause_cursor = 0;
        input_set_mode(InputMode::LR_AB);
        audio_bgm_set_paused(true);
        audio_play_tone(523.25f, 50, Waveform::SQUARE, 0.20f);
        return;
    }

    update_playing(dt_ms, input);
}

void ActionGame::update_playing(uint32_t dt_ms, const InputState& input) {
    _stage_time_ms += dt_ms;
    const float dt = static_cast<float>(dt_ms) * 0.001f;

    if (_invuln_ms > dt_ms) _invuln_ms -= dt_ms; else _invuln_ms = 0;
    if (_star_ms > dt_ms)   _star_ms -= dt_ms;   else _star_ms = 0;
    if (_bumped.timer_ms > dt_ms) _bumped.timer_ms -= dt_ms;
    else { _bumped.timer_ms = 0; _bumped.tx = -1; }

    // Input handling (8BitDo Micro: D-Pad L/R, A/X = Jump, B/Y = Dash)
    const bool left_held  = (input.held & BTN_LEFT) != 0;
    const bool right_held = (input.held & BTN_RIGHT) != 0;
    const bool dash_held  = (input.held & (BTN_B | BTN_Y)) != 0 || (_star_ms > 0);
    const bool jump_held  = (input.held & (BTN_A | BTN_X)) != 0;
    const bool jump_press = (input.pressed & (BTN_A | BTN_X)) != 0;

    if (jump_press) {
        _jump_buf_ms = 110;
    } else if (_jump_buf_ms > dt_ms) {
        _jump_buf_ms -= dt_ms;
    } else {
        _jump_buf_ms = 0;
    }

    if (_on_ground) {
        _coyote_ms = 90;
    } else if (_coyote_ms > dt_ms) {
        _coyote_ms -= dt_ms;
    } else {
        _coyote_ms = 0;
    }

    const float max_spd = dash_held ? DASH_SPEED : WALK_SPEED;
    const float accel   = _on_ground ? ACCEL_GROUND : ACCEL_AIR;

    if (left_held && !right_held) {
        _pvx -= accel * dt;
        if (_pvx < -max_spd) _pvx = -max_spd;
        _facing_right = false;
    } else if (right_held && !left_held) {
        _pvx += accel * dt;
        if (_pvx > max_spd) _pvx = max_spd;
        _facing_right = true;
    } else {
        const float decel = (_on_ground ? FRICTION_GROUND : (FRICTION_GROUND * 0.35f)) * dt;
        if (_pvx > decel) _pvx -= decel;
        else if (_pvx < -decel) _pvx += decel;
        else _pvx = 0.0f;
    }

    // Jump trigger (coyote + buffer)
    if (_jump_buf_ms > 0 && _coyote_ms > 0) {
        _pvy         = (fabsf(_pvx) > 125.0f) ? DASH_JUMP_VEL : JUMP_VEL;
        _on_ground   = false;
        _coyote_ms   = 0;
        _jump_buf_ms = 0;
        audio_play_sweep(260.0f, 560.0f, 110, Waveform::SQUARE, 0.25f);
    }

    // Variable jump height cut
    if (!jump_held && _pvy < -120.0f) {
        _pvy *= 0.58f;
    }

    // Gravity
    _pvy += GRAVITY_PX * dt;
    if (_pvy > 480.0f) _pvy = 480.0f;

    const float pw = player_w();
    const float ph = player_h();

    // Horizontal move & tile collision
    _px += _pvx * dt;
    if (_px < 0.0f) { _px = 0.0f; _pvx = 0.0f; }
    const float max_px = _map_w * TILE_SIZE - pw;
    if (_px > max_px) { _px = max_px; _pvx = 0.0f; }

    {
        const int ty0 = static_cast<int>(floorf((_py + 1.0f) / TILE_SIZE));
        const int ty1 = static_cast<int>(floorf((_py + ph - 1.0f) / TILE_SIZE));
        if (_pvx > 0.0f) {
            const int tx = static_cast<int>(floorf((_px + pw) / TILE_SIZE));
            for (int ty = ty0; ty <= ty1; ++ty) {
                if (is_solid(tx, ty)) {
                    _px  = tx * TILE_SIZE - pw - 0.01f;
                    _pvx = 0.0f;
                    break;
                }
            }
        } else if (_pvx < 0.0f) {
            const int tx = static_cast<int>(floorf(_px / TILE_SIZE));
            for (int ty = ty0; ty <= ty1; ++ty) {
                if (is_solid(tx, ty)) {
                    _px  = (tx + 1) * TILE_SIZE + 0.01f;
                    _pvx = 0.0f;
                    break;
                }
            }
        }
    }

    // Vertical move & tile collision
    const float prev_bottom = _py + ph;
    _py += _pvy * dt;
    _on_ground = false;

    {
        const int tx0 = static_cast<int>(floorf((_px + 1.0f) / TILE_SIZE));
        const int tx1 = static_cast<int>(floorf((_px + pw - 1.0f) / TILE_SIZE));
        if (_pvy >= 0.0f) {
            const int ty = static_cast<int>(floorf((_py + ph) / TILE_SIZE));
            for (int tx = tx0; tx <= tx1; ++tx) {
                const bool solid = is_solid(tx, ty);
                const bool oneway = is_oneway(tx, ty) && (prev_bottom <= ty * TILE_SIZE + 3.0f);
                if (solid || oneway) {
                    _py        = ty * TILE_SIZE - ph;
                    _pvy       = 0.0f;
                    _on_ground = true;
                    break;
                }
            }
        } else {
            const int ty = static_cast<int>(floorf(_py / TILE_SIZE));
            int hit_tx = -1;
            float best_dist = 999.0f;
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (is_solid(tx, ty)) {
                    const float dist = fabsf((_px + pw * 0.5f) - (tx * TILE_SIZE + 8.0f));
                    if (dist < best_dist) {
                        best_dist = dist;
                        hit_tx    = tx;
                    }
                }
            }
            if (hit_tx >= 0) {
                _py  = (ty + 1) * TILE_SIZE + 0.01f;
                _pvy = 0.0f;
                hit_block_from_below(hit_tx, ty);
            }
        }
    }

    // Pit fall check
    if (_py > MAP_H * TILE_SIZE + 12.0f) {
        _is_super  = false;
        _invuln_ms = 0;
        _star_ms   = 0;
        hurt_player();
        return;
    }

    // Coin & Spike checks
    {
        const int tx0 = static_cast<int>(floorf((_px + 1.0f) / TILE_SIZE));
        const int tx1 = static_cast<int>(floorf((_px + pw - 1.0f) / TILE_SIZE));
        const int ty0 = static_cast<int>(floorf((_py + 1.0f) / TILE_SIZE));
        const int ty1 = static_cast<int>(floorf((_py + ph - 1.0f) / TILE_SIZE));
        for (int ty = ty0; ty <= ty1; ++ty) {
            if (ty < 0 || ty >= MAP_H) continue;
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (tx < 0 || tx >= _map_w) continue;
                if (_coin_grid[ty][tx]) {
                    _coin_grid[ty][tx] = false;
                    ++_coins;
                    if (_coins % 50 == 0) {
                        ++_lives;
                        add_score(500, tx * TILE_SIZE + 8.0f, ty * TILE_SIZE, TFT_GREEN, "1UP!");
                        audio_play_tone(1046.5f, 120, Waveform::SQUARE, 0.35f);
                    } else {
                        add_score(100, tx * TILE_SIZE + 8.0f, ty * TILE_SIZE, TFT_YELLOW);
                    }
                    spawn_particles(tx * TILE_SIZE + 8.0f, ty * TILE_SIZE + 8.0f, TFT_YELLOW, 4, 65.0f);
                    audio_play_tone(987.77f, 45, Waveform::SQUARE, 0.22f);
                    audio_play_tone(1318.5f, 95, Waveform::SQUARE, 0.22f);
                }
                if (_tiles[ty][tx] == TILE_SPIKE && _star_ms == 0) {
                    hurt_player();
                    return;
                }
            }
        }
    }

    // Checkpoint check
    if (_has_checkpoint && !_checkpoint_hit && (_px + pw) >= _checkpoint_x) {
        _checkpoint_hit = true;
        add_score(300, _checkpoint_x, _checkpoint_y - 12.0f, TFT_CYAN, "CHECK!");
        spawn_particles(_checkpoint_x + 8.0f, _checkpoint_y, TFT_CYAN, 8, 95.0f);
        audio_play_sweep(523.0f, 1046.0f, 180, Waveform::TRIANGLE, 0.30f);
    }

    // Goal Flagpole check
    if ((_px + pw) >= _goal_x) {
        _state          = State::STAGE_CLEAR;
        _state_timer_ms = 2200;
        const uint32_t height_bonus = (_py < 120.0f) ? 1000 : 500;
        add_score(height_bonus, _goal_x, _py, TFT_YELLOW, height_bonus >= 1000 ? "+1000" : "+500");
        spawn_particles(_goal_x + 4.0f, _py, TFT_YELLOW, 12, 120.0f);
        audio_bgm_play(BGM_CLEAR_FANFARE, sizeof(BGM_CLEAR_FANFARE) / sizeof(BGM_CLEAR_FANFARE[0]), 1600, false);
        return;
    }

    // Update entities
    update_items(dt_ms);
    update_enemies(dt_ms);
    update_particles(dt_ms);

    // Camera follow with slight forward look-ahead
    const float target_cam = _px - SCREEN_WIDTH * 0.38f + (_facing_right ? 18.0f : -8.0f);
    _cam_x += (target_cam - _cam_x) * fminf(1.0f, dt * 8.0f);
    if (_cam_x < 0.0f) _cam_x = 0.0f;
    const float max_cam = static_cast<float>(_map_w * TILE_SIZE - SCREEN_WIDTH);
    if (_cam_x > max_cam) _cam_x = max_cam;
}

void ActionGame::update_items(uint32_t dt_ms) {
    const float dt = static_cast<float>(dt_ms) * 0.001f;
    const float pw = player_w();
    const float ph = player_h();

    for (auto& it : _items) {
        if (!it.active) continue;
        if (it.rise_left > 0.0f) {
            const float step = 28.0f * dt;
            it.y -= step;
            it.rise_left -= step;
            continue;
        }

        it.vy += GRAVITY_PX * 0.85f * dt;
        if (it.vy > 360.0f) it.vy = 360.0f;

        it.x += it.vx * dt;
        const int tx_side = static_cast<int>(floorf((it.vx > 0 ? it.x + 12.0f : it.x) / TILE_SIZE));
        const int ty_mid  = static_cast<int>(floorf((it.y + 6.0f) / TILE_SIZE));
        if (is_solid(tx_side, ty_mid)) {
            it.vx = -it.vx;
            it.x += it.vx * dt;
        }

        it.y += it.vy * dt;
        const int tx0 = static_cast<int>(floorf((it.x + 1.0f) / TILE_SIZE));
        const int tx1 = static_cast<int>(floorf((it.x + 11.0f) / TILE_SIZE));
        const int ty_bot = static_cast<int>(floorf((it.y + 12.0f) / TILE_SIZE));
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (is_solid(tx, ty_bot) || is_oneway(tx, ty_bot)) {
                it.y = ty_bot * TILE_SIZE - 12.0f;
                it.vy = (it.kind == ItemKind::STAR) ? -230.0f : 0.0f;
                break;
            }
        }

        if (it.y > MAP_H * TILE_SIZE + 20.0f) {
            it.active = false;
            continue;
        }

        if (aabb_overlap(_px, _py, pw, ph, it.x, it.y, 12.0f, 12.0f)) {
            it.active = false;
            if (it.kind == ItemKind::MUSHROOM) {
                if (!_is_super) {
                    _is_super = true;
                    _py -= 6.0f;
                    add_score(1000, it.x, it.y, TFT_GREEN, "SUPER!");
                } else {
                    ++_lives;
                    add_score(1000, it.x, it.y, TFT_GREEN, "1UP!");
                }
                spawn_particles(it.x + 6.0f, it.y + 6.0f, TFT_GREEN, 8, 95.0f);
                audio_play_sweep(392.0f, 1046.0f, 220, Waveform::SQUARE, 0.32f);
            } else {
                _star_ms = 8000;
                add_score(1000, it.x, it.y, TFT_YELLOW, "STAR!");
                spawn_particles(it.x + 6.0f, it.y + 6.0f, TFT_YELLOW, 10, 110.0f);
                audio_play_sweep(523.0f, 1318.0f, 260, Waveform::SQUARE, 0.35f);
            }
        }
    }
}

void ActionGame::update_enemies(uint32_t dt_ms) {
    const float dt = static_cast<float>(dt_ms) * 0.001f;
    const float pw = player_w();
    const float ph = player_h();

    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy& e = _enemies[i];
        if (!e.active) continue;

        if (!e.awake) {
            if (e.x < _cam_x + SCREEN_WIDTH + 48.0f && e.x > _cam_x - 64.0f) {
                e.awake = true;
            } else {
                continue;
            }
        }

        if (e.kick_grace_ms > dt_ms) e.kick_grace_ms -= dt_ms;
        else e.kick_grace_ms = 0;

        if (e.squashed) {
            if (e.timer_ms > dt_ms) e.timer_ms -= dt_ms;
            else e.active = false;
            continue;
        }

        // Movement per enemy kind
        if (e.kind == EnemyKind::FLAPPY) {
            e.timer_ms += dt_ms;
            e.x += e.vx * dt;
            e.y = e.base_y + sinf(e.timer_ms * 0.0045f) * 14.0f;
            const int tx_side = static_cast<int>(floorf((e.vx > 0 ? e.x + 14.0f : e.x) / TILE_SIZE));
            const int ty_mid  = static_cast<int>(floorf((e.y + 7.0f) / TILE_SIZE));
            if (is_solid(tx_side, ty_mid) || e.x < 8.0f || e.x > (_map_w - 2) * TILE_SIZE) {
                e.vx = -e.vx;
            }
        } else {
            e.vy += GRAVITY_PX * 0.85f * dt;
            if (e.vy > 360.0f) e.vy = 360.0f;

            const float move_vx = (e.in_shell && !e.shell_moving) ? 0.0f : e.vx;
            e.x += move_vx * dt;
            const int tx_side = static_cast<int>(floorf((move_vx > 0 ? e.x + 14.0f : e.x) / TILE_SIZE));
            const int ty_mid  = static_cast<int>(floorf((e.y + 7.0f) / TILE_SIZE));
            if (move_vx != 0.0f && (is_solid(tx_side, ty_mid) || e.x < 4.0f)) {
                e.vx = -e.vx;
                e.x += e.vx * dt;
                if (e.shell_moving) {
                    audio_play_tone(220.0f, 35, Waveform::TRIANGLE, 0.20f);
                }
            }

            e.y += e.vy * dt;
            const int tx0 = static_cast<int>(floorf((e.x + 1.0f) / TILE_SIZE));
            const int tx1 = static_cast<int>(floorf((e.x + 13.0f) / TILE_SIZE));
            const int ty_bot = static_cast<int>(floorf((e.y + 14.0f) / TILE_SIZE));
            bool e_ground = false;
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (is_solid(tx, ty_bot) || is_oneway(tx, ty_bot)) {
                    e.y = ty_bot * TILE_SIZE - 14.0f;
                    e.vy = 0.0f;
                    e_ground = true;
                    break;
                }
            }
            // Keep non-shell walkers from walking straight off ledges (kid-friendly)
            if (e_ground && !e.in_shell && e.kind == EnemyKind::KAME) {
                const int ahead_tx = static_cast<int>(floorf((e.vx > 0 ? e.x + 15.0f : e.x - 1.0f) / TILE_SIZE));
                if (!is_solid(ahead_tx, ty_bot) && !is_oneway(ahead_tx, ty_bot)) {
                    e.vx = -e.vx;
                }
            }
        }

        if (e.y > MAP_H * TILE_SIZE + 24.0f) {
            e.active = false;
            continue;
        }

        // Moving shell hits other enemies
        if (e.in_shell && e.shell_moving) {
            for (int j = 0; j < MAX_ENEMIES; ++j) {
                if (i == j) continue;
                Enemy& o = _enemies[j];
                if (!o.active || o.squashed) continue;
                if (aabb_overlap(e.x, e.y, 14.0f, 14.0f, o.x, o.y, 14.0f, 14.0f)) {
                    o.active = false;
                    spawn_particles(o.x + 7.0f, o.y + 7.0f, TFT_ORANGE, 6, 95.0f);
                    add_score(300, o.x, o.y - 6.0f, TFT_YELLOW);
                    audio_play_sweep(420.0f, 200.0f, 80, Waveform::SQUARE, 0.25f);
                }
            }
        }

        // Player-Enemy collision
        if (aabb_overlap(_px, _py, pw, ph, e.x + 1.0f, e.y + 1.0f, 12.0f, 13.0f)) {
            if (_star_ms > 0) {
                e.active = false;
                spawn_particles(e.x + 7.0f, e.y + 7.0f, TFT_YELLOW, 8, 105.0f);
                add_score(300, e.x, e.y - 6.0f, TFT_YELLOW);
                audio_play_sweep(480.0f, 760.0f, 90, Waveform::SQUARE, 0.28f);
                continue;
            }

            const bool stomped = (_pvy > 0.0f) && ((_py + ph - _pvy * dt) <= e.y + 9.0f);
            if (stomped) {
                _pvy = -255.0f;
                if (e.kind == EnemyKind::KAME && !e.in_shell) {
                    e.in_shell     = true;
                    e.shell_moving = false;
                    add_score(200, e.x, e.y - 6.0f, TFT_CYAN);
                } else if (e.kind == EnemyKind::KAME && e.in_shell && !e.shell_moving) {
                    e.shell_moving  = true;
                    e.vx            = (_px + pw * 0.5f < e.x + 7.0f) ? 210.0f : -210.0f;
                    e.kick_grace_ms = 180;
                    add_score(200, e.x, e.y - 6.0f, TFT_CYAN);
                } else if (e.kind == EnemyKind::KAME && e.in_shell && e.shell_moving) {
                    e.shell_moving = false;
                } else {
                    e.squashed = true;
                    e.timer_ms = 320;
                    add_score(200, e.x, e.y - 6.0f, TFT_YELLOW);
                }
                audio_play_sweep(340.0f, 120.0f, 95, Waveform::SQUARE, 0.30f);
            } else {
                // Side touch on stationary shell kicks it safely
                if (e.kind == EnemyKind::KAME && e.in_shell && !e.shell_moving) {
                    e.shell_moving  = true;
                    e.vx            = (_px + pw * 0.5f < e.x + 7.0f) ? 210.0f : -210.0f;
                    e.kick_grace_ms = 200;
                    audio_play_tone(320.0f, 55, Waveform::SQUARE, 0.25f);
                } else if (e.kick_grace_ms == 0) {
                    hurt_player();
                    return;
                }
            }
        }
    }
}

void ActionGame::update_particles(uint32_t dt_ms) {
    const float dt = static_cast<float>(dt_ms) * 0.001f;
    for (auto& p : _particles) {
        if (!p.active) continue;
        if (p.life_ms <= dt_ms) { p.active = false; continue; }
        p.life_ms -= dt_ms;
        p.vy += 450.0f * dt;
        p.x  += p.vx * dt;
        p.y  += p.vy * dt;
    }
    for (auto& pop : _popups) {
        if (!pop.active) continue;
        if (pop.life_ms <= dt_ms) { pop.active = false; continue; }
        pop.life_ms -= dt_ms;
        pop.y -= 28.0f * dt;
    }
}

void ActionGame::draw(lgfx::LGFX_Sprite& fb) {
    draw_background(fb);
    draw_tiles(fb);
    draw_entities(fb);
    if (_state != State::HURT_WAIT || ((_anim_ms / 80) % 2 == 0)) {
        draw_player(fb);
    }
    draw_hud(fb);
    draw_overlay(fb);
}

void ActionGame::draw_background(lgfx::LGFX_Sprite& fb) const {
    const StageDef& def = STAGES[_stage_idx];
    // Two-band sky gradient + horizon tint
    fb.fillRect(0, 0, SCREEN_WIDTH, 110, def.sky_top);
    fb.fillRect(0, 110, SCREEN_WIDTH, SCREEN_HEIGHT - 110, def.sky_bot);

    const int cam_i = static_cast<int>(_cam_x);
    if (def.is_castle) {
        // Twinkling stars + castle pillars
        for (int i = 0; i < 16; ++i) {
            const int sx = ((i * 73) - (cam_i / 4)) % SCREEN_WIDTH;
            const int wx = (sx < 0) ? (sx + SCREEN_WIDTH) : sx;
            const int sy = 26 + ((i * 37) % 110);
            const uint16_t col = ((_anim_ms / 250 + i) % 3 == 0) ? TFT_YELLOW : TFT_WHITE;
            fb.fillRect(wx, sy, 2, 2, col);
        }
        for (int x = -(cam_i / 2) % 80; x < SCREEN_WIDTH; x += 80) {
            fb.fillRect(x + 12, 56, 26, 160, 0x2949);
            fb.fillRect(x + 16, 72, 6, 14, 0x1064);
            fb.fillRect(x + 28, 72, 6, 14, 0x1064);
        }
    } else {
        // Parallax clouds
        for (int i = 0; i < 5; ++i) {
            int cx = (i * 90 - (cam_i / 3)) % (SCREEN_WIDTH + 60);
            if (cx < -40) cx += (SCREEN_WIDTH + 60);
            const int cy = 34 + (i % 2) * 24;
            fb.fillCircle(cx, cy, 10, TFT_WHITE);
            fb.fillCircle(cx + 12, cy - 3, 12, TFT_WHITE);
            fb.fillCircle(cx + 24, cy, 10, TFT_WHITE);
            fb.fillRect(cx, cy, 24, 10, TFT_WHITE);
        }
        // Parallax hills
        const uint16_t hill_col = (_stage_idx == 0) ? 0x2626 : 0xB3A4;
        for (int x = -(cam_i / 2) % 120; x < SCREEN_WIDTH + 60; x += 120) {
            fb.fillCircle(x + 30, 212, 34, hill_col);
            fb.fillCircle(x + 84, 218, 26, hill_col);
        }
    }
}

void ActionGame::draw_tiles(lgfx::LGFX_Sprite& fb) const {
    const StageDef& def = STAGES[_stage_idx];
    const int cam_i = static_cast<int>(_cam_x);
    const int c0 = cam_i / TILE_SIZE;
    const int c1 = (cam_i + SCREEN_WIDTH) / TILE_SIZE + 1;

    for (int ty = 0; ty < MAP_H; ++ty) {
        for (int tx = c0; tx <= c1; ++tx) {
            if (tx < 0 || tx >= _map_w) continue;
            const int sx = tx * TILE_SIZE - cam_i;
            int sy = ty * TILE_SIZE;
            if (_bumped.tx == tx && _bumped.ty == ty && _bumped.timer_ms > 0) {
                sy -= static_cast<int>(sinf((_bumped.timer_ms / 140.0f) * 3.14159f) * 5.0f);
            }

            // Floating Coin
            if (_coin_grid[ty][tx]) {
                const int phase = (_anim_ms / 120 + tx) % 4;
                const int rx = (phase == 1 || phase == 3) ? 3 : (phase == 2 ? 2 : 5);
                fb.fillEllipse(sx + 8, sy + 8, rx, 6, TFT_YELLOW);
                fb.drawEllipse(sx + 8, sy + 8, rx, 6, 0xFC00);
                if (rx >= 4) fb.drawFastVLine(sx + 8, sy + 5, 6, 0xFC00);
            }

            const TileType t = _tiles[ty][tx];
            switch (t) {
                case TILE_GROUND: {
                    const bool top_exposed = (ty > 0 && _tiles[ty - 1][tx] != TILE_GROUND);
                    fb.fillRect(sx, sy, TILE_SIZE, TILE_SIZE, def.ground_fill);
                    if (top_exposed) {
                        fb.fillRect(sx, sy, TILE_SIZE, 5, def.ground_top);
                        fb.drawFastHLine(sx, sy + 5, TILE_SIZE, 0x18C3);
                    }
                    break;
                }
                case TILE_BRICK:
                    fb.fillRect(sx, sy, TILE_SIZE, TILE_SIZE, 0xB302);
                    fb.drawRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x6180);
                    fb.drawFastHLine(sx, sy + 8, TILE_SIZE, 0x6180);
                    fb.drawFastVLine(sx + 8, sy, 8, 0x6180);
                    fb.drawFastVLine(sx + 4, sy + 8, 8, 0x6180);
                    break;
                case TILE_Q_COIN:
                case TILE_Q_MUSHROOM:
                case TILE_Q_STAR: {
                    const uint16_t qcol = ((_anim_ms / 240) % 2 == 0) ? 0xFE60 : 0xFD20;
                    fb.fillRoundRect(sx, sy, TILE_SIZE, TILE_SIZE, 2, qcol);
                    fb.drawRoundRect(sx, sy, TILE_SIZE, TILE_SIZE, 2, 0x9300);
                    fb.setTextDatum(MC_DATUM);
                    fb.setTextColor(0x5180);
                    fb.setTextSize(1);
                    fb.drawString("?", sx + 8, sy + 8);
                    break;
                }
                case TILE_USED:
                    fb.fillRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x7306);
                    fb.drawRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x4202);
                    break;
                case TILE_ONEWAY:
                    fb.fillRoundRect(sx, sy, TILE_SIZE, 6, 2, 0xFFE0);
                    fb.drawFastHLine(sx + 1, sy, TILE_SIZE - 2, TFT_WHITE);
                    fb.fillRect(sx + 2, sy + 6, 12, 2, 0x9492);
                    break;
                case TILE_PIPE_TL:
                    fb.fillRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x1EC4);
                    fb.fillRect(sx + 2, sy + 2, 4, TILE_SIZE - 4, 0x5FE9);
                    fb.drawRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x0B81);
                    break;
                case TILE_PIPE_TR:
                    fb.fillRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x1543);
                    fb.drawRect(sx, sy, TILE_SIZE, TILE_SIZE, 0x0B81);
                    break;
                case TILE_PIPE_BL:
                    fb.fillRect(sx + 2, sy, TILE_SIZE - 2, TILE_SIZE, 0x1EC4);
                    fb.fillRect(sx + 4, sy, 3, TILE_SIZE, 0x5FE9);
                    fb.drawFastVLine(sx + 2, sy, TILE_SIZE, 0x0B81);
                    break;
                case TILE_PIPE_BR:
                    fb.fillRect(sx, sy, TILE_SIZE - 2, TILE_SIZE, 0x1543);
                    fb.drawFastVLine(sx + TILE_SIZE - 3, sy, TILE_SIZE, 0x0B81);
                    break;
                case TILE_SPIKE:
                    fb.fillTriangle(sx + 1, sy + 15, sx + 4, sy + 5, sx + 7, sy + 15, TFT_LIGHTGREY);
                    fb.fillTriangle(sx + 8, sy + 15, sx + 11, sy + 5, sx + 14, sy + 15, TFT_LIGHTGREY);
                    break;
                default:
                    break;
            }
        }
    }

    // Checkpoint flag
    if (_has_checkpoint) {
        const int cx = static_cast<int>(_checkpoint_x) - cam_i;
        const int cy = static_cast<int>(_checkpoint_y);
        if (cx >= -20 && cx < SCREEN_WIDTH + 20) {
            fb.fillRect(cx + 6, cy - 16, 2, 30, TFT_WHITE);
            const uint16_t fcol = _checkpoint_hit ? TFT_YELLOW : TFT_DARKGREY;
            fb.fillTriangle(cx + 8, cy - 16, cx + 20, cy - 11, cx + 8, cy - 6, fcol);
        }
    }

    // Goal Flagpole + Castle
    const int gx = static_cast<int>(_goal_x) - cam_i;
    if (gx >= -80 && gx < SCREEN_WIDTH + 40) {
        // Castle behind goal
        const int cas_x = gx + 28;
        fb.fillRect(cas_x, 152, 48, 56, 0x8410);
        fb.fillRect(cas_x + 12, 136, 24, 16, 0x8410);
        fb.fillRect(cas_x + 18, 180, 12, 28, TFT_BLACK);
        for (int b = 0; b < 4; ++b) {
            fb.fillRect(cas_x + b * 12 + 2, 146, 6, 6, 0x8410);
        }
        // Pole & ball
        fb.fillRect(gx + 6, 68, 3, 140, TFT_LIGHTGREY);
        fb.fillCircle(gx + 7, 66, 5, TFT_YELLOW);
        // Flag
        const int fy = static_cast<int>(_flag_y);
        fb.fillTriangle(gx + 6, fy, gx - 12, fy + 7, gx + 6, fy + 14, TFT_RED);
    }
}

void ActionGame::draw_entities(lgfx::LGFX_Sprite& fb) const {
    const int cam_i = static_cast<int>(_cam_x);

    // Items
    for (const auto& it : _items) {
        if (!it.active) continue;
        const int sx = static_cast<int>(it.x) - cam_i;
        const int sy = static_cast<int>(it.y);
        if (sx < -16 || sx > SCREEN_WIDTH + 16) continue;
        if (it.kind == ItemKind::MUSHROOM) {
            fb.fillCircle(sx + 6, sy + 5, 6, TFT_RED);
            fb.fillCircle(sx + 4, sy + 4, 2, TFT_WHITE);
            fb.fillCircle(sx + 8, sy + 4, 2, TFT_WHITE);
            fb.fillRect(sx + 3, sy + 6, 6, 6, 0xFDE0);
        } else {
            const uint16_t col = rainbow_color(_anim_ms);
            fb.fillCircle(sx + 6, sy + 6, 6, col);
            fb.fillCircle(sx + 6, sy + 6, 2, TFT_WHITE);
        }
    }

    // Enemies
    for (const auto& e : _enemies) {
        if (!e.active) continue;
        const int sx = static_cast<int>(e.x) - cam_i;
        const int sy = static_cast<int>(e.y);
        if (sx < -20 || sx > SCREEN_WIDTH + 20) continue;

        if (e.squashed) {
            fb.fillRoundRect(sx, sy + 9, 14, 5, 2, 0x9300);
            continue;
        }

        if (e.kind == EnemyKind::KURI) {
            fb.fillCircle(sx + 7, sy + 6, 6, 0xB300);
            fb.fillRect(sx + 3, sy + 8, 8, 6, 0xFDE0);
            fb.fillRect(sx + 4, sy + 4, 2, 3, TFT_WHITE);
            fb.fillRect(sx + 8, sy + 4, 2, 3, TFT_WHITE);
            const int step = (_anim_ms / 140) % 2;
            fb.fillRect(sx + 2 + step, sy + 12, 4, 2, 0x4100);
            fb.fillRect(sx + 8 - step, sy + 12, 4, 2, 0x4100);
        } else if (e.kind == EnemyKind::KAME) {
            if (e.in_shell) {
                fb.fillRoundRect(sx, sy + 3, 14, 11, 4, 0x1E83);
                fb.drawRoundRect(sx, sy + 3, 14, 11, 4, TFT_WHITE);
            } else {
                fb.fillRoundRect(sx + 1, sy + 3, 12, 9, 4, 0x1E83);
                const int head_x = (e.vx >= 0) ? (sx + 9) : sx;
                fb.fillCircle(head_x + 2, sy + 4, 3, TFT_YELLOW);
                fb.fillRect(sx + 2, sy + 11, 4, 3, TFT_ORANGE);
                fb.fillRect(sx + 8, sy + 11, 4, 3, TFT_ORANGE);
            }
        } else {
            // FLAPPY
            const int wing_y = ((_anim_ms / 100) % 2 == 0) ? (sy + 2) : (sy + 6);
            fb.fillCircle(sx + 7, sy + 7, 6, 0xF81F);
            fb.fillTriangle(sx, wing_y, sx + 5, sy + 6, sx + 1, sy + 9, TFT_WHITE);
            fb.fillTriangle(sx + 14, wing_y, sx + 9, sy + 6, sx + 13, sy + 9, TFT_WHITE);
            fb.fillRect(sx + 4, sy + 5, 2, 2, TFT_BLACK);
            fb.fillRect(sx + 8, sy + 5, 2, 2, TFT_BLACK);
        }
    }

    // Particles
    for (const auto& p : _particles) {
        if (!p.active) continue;
        const int sx = static_cast<int>(p.x) - cam_i;
        const int sy = static_cast<int>(p.y);
        fb.fillRect(sx, sy, p.size, p.size, p.color);
    }

    // Popups
    fb.setTextDatum(MC_DATUM);
    fb.setTextSize(1);
    for (const auto& pop : _popups) {
        if (!pop.active) continue;
        const int sx = static_cast<int>(pop.x) - cam_i;
        const int sy = static_cast<int>(pop.y);
        fb.setTextColor(pop.color);
        fb.drawString(pop.text, sx, sy);
    }
}

void ActionGame::draw_player(lgfx::LGFX_Sprite& fb) const {
    if (_invuln_ms > 0 && ((_anim_ms / 60) % 2 == 0)) {
        return;
    }

    const int sx = static_cast<int>(_px - _cam_x);
    const int sy = static_cast<int>(_py);
    const int ph = static_cast<int>(player_h());

    const uint16_t cap_col  = (_star_ms > 0) ? rainbow_color(_anim_ms) : TFT_RED;
    const uint16_t suit_col = (_star_ms > 0) ? rainbow_color(_anim_ms + 120) : (_is_super ? 0x04FF : TFT_BLUE);

    // Hat + Head
    fb.fillRoundRect(sx, sy, 12, 5, 2, cap_col);
    fb.fillRect(sx + 1, sy + 4, 10, 5, 0xFDE0);
    // Eye
    const int eye_x = _facing_right ? (sx + 8) : (sx + 2);
    fb.fillRect(eye_x, sy + 5, 2, 2, TFT_BLACK);

    // Body / Overalls
    const int body_h = ph - 11;
    fb.fillRect(sx + 1, sy + 9, 10, body_h, suit_col);
    if (_is_super) {
        fb.fillRect(sx + 3, sy + 10, 2, 2, TFT_YELLOW);
        fb.fillRect(sx + 7, sy + 10, 2, 2, TFT_YELLOW);
    }

    // Feet animation
    const int stride = (_on_ground && fabsf(_pvx) > 15.0f) ? (((_anim_ms / 90) % 2 == 0) ? 1 : -1) : 0;
    fb.fillRect(sx + 1 + stride, sy + ph - 3, 4, 3, 0x6180);
    fb.fillRect(sx + 7 - stride, sy + ph - 3, 4, 3, 0x6180);
}

void ActionGame::draw_hud(lgfx::LGFX_Sprite& fb) const {
    fb.fillRect(0, 0, SCREEN_WIDTH, 22, 0x1082);
    fb.drawFastHLine(0, 22, SCREEN_WIDTH, 0x4228);

    fb.setTextSize(1);
    fb.setTextDatum(ML_DATUM);
    char buf[48];
    snprintf(buf, sizeof(buf), "SCR:%05lu  COIN:%02lu",
             static_cast<unsigned long>(_score),
             static_cast<unsigned long>(_coins));
    fb.setTextColor(TFT_WHITE);
    fb.drawString(buf, 6, 11);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_YELLOW);
    snprintf(buf, sizeof(buf), "S%u  HP:%u", static_cast<unsigned>(_stage_idx + 1), static_cast<unsigned>(_lives));
    fb.drawString(buf, 194, 11);

    // Pause touch button
    fb.fillRoundRect(RECT_PAUSE_BTN.x, RECT_PAUSE_BTN.y, RECT_PAUSE_BTN.w, RECT_PAUSE_BTN.h, 4, 0x2965);
    fb.drawRoundRect(RECT_PAUSE_BTN.x, RECT_PAUSE_BTN.y, RECT_PAUSE_BTN.w, RECT_PAUSE_BTN.h, 4, TFT_LIGHTGREY);
    fb.setTextColor(TFT_WHITE);
    fb.drawString("||", RECT_PAUSE_BTN.x + RECT_PAUSE_BTN.w / 2, RECT_PAUSE_BTN.y + RECT_PAUSE_BTN.h / 2);
}

void ActionGame::draw_overlay(lgfx::LGFX_Sprite& fb) const {
    if (_state == State::PLAYING || _state == State::HURT_WAIT) return;

    if (_state == State::TITLE) {
        fb.fillRoundRect(24, 34, 272, 198, 10, 0x18E4);
        fb.drawRoundRect(24, 34, 272, 198, 10, TFT_YELLOW);

        fb.setTextDatum(MC_DATUM);
        fb.setTextSize(2);
        fb.setTextColor(TFT_YELLOW);
        fb.drawString("HOPPY STAR!", 160, 62);

        fb.setTextSize(1);
        fb.setTextColor(TFT_CYAN);
        char hi[32];
        snprintf(hi, sizeof(hi), "BEST SCORE: %lu", static_cast<unsigned long>(save_data_get().action_best_score));
        fb.drawString(hi, 160, 88);

        fb.setTextColor(TFT_WHITE);
        fb.drawString("LEFT/RIGHT: Move   A: Jump", 160, 110);
        fb.drawString("B: Dash   BOOT/||: Pause", 160, 126);

        fb.fillRoundRect(RECT_TITLE_GO.x, RECT_TITLE_GO.y, RECT_TITLE_GO.w, RECT_TITLE_GO.h, 8, 0x2625);
        fb.drawRoundRect(RECT_TITLE_GO.x, RECT_TITLE_GO.y, RECT_TITLE_GO.w, RECT_TITLE_GO.h, 8, TFT_WHITE);
        fb.setTextColor(TFT_WHITE);
        fb.drawString("START (A / TOUCH)", 160, 172);

        fb.fillRoundRect(RECT_TITLE_EXIT.x, RECT_TITLE_EXIT.y, RECT_TITLE_EXIT.w, RECT_TITLE_EXIT.h, 6, 0x528A);
        fb.drawRoundRect(RECT_TITLE_EXIT.x, RECT_TITLE_EXIT.y, RECT_TITLE_EXIT.w, RECT_TITLE_EXIT.h, 6, TFT_LIGHTGREY);
        fb.drawString("MENU (B)", 160, 211);
        return;
    }

    if (_state == State::PAUSED) {
        fb.fillRoundRect(56, 48, 208, 154, 10, 0x1082);
        fb.drawRoundRect(56, 48, 208, 154, 10, TFT_WHITE);
        fb.setTextDatum(MC_DATUM);
        fb.setTextSize(2);
        fb.setTextColor(TFT_YELLOW);
        fb.drawString("PAUSED", 160, 68);

        fb.setTextSize(1);
        const char* opts[3] = {"RESUME", "RESTART STAGE", "QUIT TO MENU"};
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
        return;
    }

    if (_state == State::STAGE_CLEAR) {
        fb.fillRoundRect(48, 72, 224, 68, 8, 0x18E4);
        fb.drawRoundRect(48, 72, 224, 68, 8, TFT_YELLOW);
        fb.setTextDatum(MC_DATUM);
        fb.setTextSize(2);
        fb.setTextColor(TFT_YELLOW);
        fb.drawString("STAGE CLEAR!", 160, 96);
        fb.setTextSize(1);
        fb.setTextColor(TFT_WHITE);
        fb.drawString(STAGES[_stage_idx].title, 160, 122);
        return;
    }

    // GAME_OVER or VICTORY
    const bool win = (_state == State::VICTORY);
    fb.fillRoundRect(36, 52, 248, 146, 10, 0x18E4);
    fb.drawRoundRect(36, 52, 248, 146, 10, win ? TFT_YELLOW : TFT_RED);

    fb.setTextDatum(MC_DATUM);
    fb.setTextSize(2);
    fb.setTextColor(win ? TFT_YELLOW : TFT_ORANGE);
    fb.drawString(win ? "ALL CLEAR!" : "GAME OVER", 160, 82);

    fb.setTextSize(1);
    fb.setTextColor(TFT_WHITE);
    char msg[48];
    snprintf(msg, sizeof(msg), "SCORE: %lu   BEST: %lu",
             static_cast<unsigned long>(_score),
             static_cast<unsigned long>(save_data_get().action_best_score));
    fb.drawString(msg, 160, 114);

    fb.fillRoundRect(52, 148, 102, 30, 6, 0x2625);
    fb.drawRoundRect(52, 148, 102, 30, 6, TFT_WHITE);
    fb.drawString("RETRY (A)", 103, 163);

    fb.fillRoundRect(166, 148, 102, 30, 6, 0x528A);
    fb.drawRoundRect(166, 148, 102, 30, 6, TFT_LIGHTGREY);
    fb.drawString("MENU (B)", 217, 163);
}
