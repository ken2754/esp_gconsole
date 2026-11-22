#pragma once

#include <stdint.h>

#include "hal/display.hpp"
#include "hal/input.hpp"

/**
 * @brief Common interface for all console games / applets.
 */
class GameApp {
public:
    virtual ~GameApp() = default;

    virtual const char* name() const = 0;
    virtual void init() = 0;
    virtual void update(uint32_t dt_ms, const InputState& input) = 0;
    virtual void draw(lgfx::LGFX_Sprite& fb) = 0;
};
