#pragma once

#include <stdint.h>

#include "game/game_app.hpp"

/**
 * @brief Mario-style side-scrolling 2D platformer ("HOPPY STAR").
 *        - 3 handcrafted stages (Sunny Meadow, Sky Bridges, Starlight Castle)
 *        - Variable-height jump, coyote time, jump buffer, B-button dash
 *        - ? blocks, breakable bricks, power-up mushroom, invincibility star
 *        - 3 enemy types (Kuri walker, Kame kickable shell, Flappy hopper)
 *        - Checkpoint flags, goal flagpole, particles, chiptune BGM & SFX
 */
class ActionGame final : public GameApp {
public:
    const char* name() const override { return "HOPPY STAR"; }
    void init() override;
    void update(uint32_t dt_ms, const InputState& input) override;
    void draw(lgfx::LGFX_Sprite& fb) override;
    void on_exit() override;
    bool wants_exit() const override { return _wants_exit; }

private:
    enum class State : uint8_t {
        TITLE = 0,
        PLAYING,
        PAUSED,
        STAGE_CLEAR,
        HURT_WAIT,
        GAME_OVER,
        VICTORY,
    };

    static constexpr int MAP_H = 15;
    static constexpr int MAX_MAP_W = 128;
    static constexpr int TILE_SIZE = 16;
    static constexpr int MAX_ENEMIES = 32;
    static constexpr int MAX_ITEMS = 12;
    static constexpr int MAX_PARTICLES = 40;
    static constexpr int MAX_POPUPS = 16;

    enum TileType : uint8_t {
        TILE_AIR = 0,
        TILE_GROUND,
        TILE_BRICK,
        TILE_Q_COIN,
        TILE_Q_MUSHROOM,
        TILE_Q_STAR,
        TILE_USED,
        TILE_ONEWAY,
        TILE_PIPE_TL,
        TILE_PIPE_TR,
        TILE_PIPE_BL,
        TILE_PIPE_BR,
        TILE_SPIKE,
    };

    enum class EnemyKind : uint8_t {
        KURI = 0,
        KAME,
        FLAPPY,
    };

    struct Enemy {
        bool      active       = false;
        bool      awake        = false;
        EnemyKind kind         = EnemyKind::KURI;
        bool      in_shell     = false;
        bool      shell_moving = false;
        bool      squashed     = false;
        float     x            = 0.0f;
        float     y            = 0.0f;
        float     vx           = 0.0f;
        float     vy           = 0.0f;
        float     base_y       = 0.0f;
        uint32_t  timer_ms     = 0;
        uint32_t  kick_grace_ms = 0;
    };

    enum class ItemKind : uint8_t {
        MUSHROOM = 0,
        STAR,
    };

    struct Item {
        bool     active = false;
        ItemKind kind   = ItemKind::MUSHROOM;
        float    x      = 0.0f;
        float    y      = 0.0f;
        float    vx     = 0.0f;
        float    vy     = 0.0f;
        float    rise_left = 0.0f;
    };

    struct Particle {
        bool     active  = false;
        float    x       = 0.0f;
        float    y       = 0.0f;
        float    vx      = 0.0f;
        float    vy      = 0.0f;
        uint16_t color   = 0xFFFF;
        uint8_t  size    = 4;
        uint32_t life_ms = 0;
    };

    struct Popup {
        bool     active  = false;
        float    x       = 0.0f;
        float    y       = 0.0f;
        char     text[8] = {};
        uint16_t color   = 0xFFFF;
        uint32_t life_ms = 0;
    };

    struct BumpedTile {
        int      tx      = -1;
        int      ty      = -1;
        uint32_t timer_ms = 0;
    };

    State    _state            = State::TITLE;
    bool     _wants_exit       = false;
    uint8_t  _stage_idx        = 0;
    uint8_t  _lives            = 3;
    uint32_t _score            = 0;
    uint32_t _coins            = 0;
    uint32_t _anim_ms          = 0;
    uint32_t _state_timer_ms   = 0;
    uint32_t _stage_time_ms    = 0;
    uint8_t  _pause_cursor     = 0;

    // Level data
    int      _map_w            = 0;
    TileType _tiles[MAP_H][MAX_MAP_W] = {};
    bool     _coin_grid[MAP_H][MAX_MAP_W] = {};
    float    _spawn_x          = 32.0f;
    float    _spawn_y          = 176.0f;
    float    _checkpoint_x     = 0.0f;
    float    _checkpoint_y     = 0.0f;
    bool     _has_checkpoint   = false;
    bool     _checkpoint_hit   = false;
    float    _goal_x           = 1400.0f;

    // Player physics
    float    _px               = 32.0f;
    float    _py               = 176.0f;
    float    _pvx              = 0.0f;
    float    _pvy              = 0.0f;
    bool     _on_ground        = false;
    bool     _facing_right     = true;
    bool     _is_super         = false;
    uint32_t _coyote_ms        = 0;
    uint32_t _jump_buf_ms      = 0;
    uint32_t _invuln_ms        = 0;
    uint32_t _star_ms          = 0;
    float    _cam_x            = 0.0f;
    float    _flag_y           = 80.0f;

    Enemy      _enemies[MAX_ENEMIES];
    Item       _items[MAX_ITEMS];
    Particle   _particles[MAX_PARTICLES];
    Popup      _popups[MAX_POPUPS];
    BumpedTile _bumped;

    void load_stage(uint8_t stage_idx, bool use_checkpoint);
    void start_stage_bgm();
    void update_playing(uint32_t dt_ms, const InputState& input);
    void update_enemies(uint32_t dt_ms);
    void update_items(uint32_t dt_ms);
    void update_particles(uint32_t dt_ms);
    void hit_block_from_below(int tx, int ty);
    void hurt_player();
    void add_score(uint32_t pts, float world_x, float world_y, uint16_t color, const char* label = nullptr);
    void spawn_particles(float x, float y, uint16_t color, int count, float speed);
    void spawn_item(ItemKind kind, float x, float y);
    bool is_solid(int tx, int ty) const;
    bool is_oneway(int tx, int ty) const;
    float player_w() const { return 12.0f; }
    float player_h() const { return _is_super ? 20.0f : 14.0f; }

    void draw_background(lgfx::LGFX_Sprite& fb) const;
    void draw_tiles(lgfx::LGFX_Sprite& fb) const;
    void draw_entities(lgfx::LGFX_Sprite& fb) const;
    void draw_player(lgfx::LGFX_Sprite& fb) const;
    void draw_hud(lgfx::LGFX_Sprite& fb) const;
    void draw_overlay(lgfx::LGFX_Sprite& fb) const;
};
