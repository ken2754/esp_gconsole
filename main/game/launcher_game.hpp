#pragma once

#include <stdint.h>

#include "game/game_app.hpp"

enum class AppId : int8_t {
    NONE = -1,
    ACTION = 0,
    RHYTHM = 1,
    HW_TEST = 2,
};

/**
 * @brief Console home screen launcher for selecting games and console settings.
 */
class LauncherGame final : public GameApp {
public:
    const char* name() const override { return "LAUNCHER"; }
    void init() override;
    void update(uint32_t dt_ms, const InputState& input) override;
    void draw(lgfx::LGFX_Sprite& fb) override;

    AppId take_launch_request();

private:
    int      _selected   = 0;
    uint32_t _anim_ms    = 0;
    AppId    _launch_req = AppId::NONE;
};
