#pragma once

#include "game/game_app.hpp"

/**
 * @brief Interactive hardware verification applet for ES3C28P:
 *        - Tests 4-GPIO 6-button parallel switches + BOOT button
 *        - Tests FT6336G capacitive touch + DON/KA rhythm pads
 *        - Tests ES8311 + FM8002E + 8ohm speaker output
 *        - Tests PWM backlight & PSRAM framebuffer FPS
 */
class HwTestGame final : public GameApp {
public:
    const char* name() const override { return "HW_TEST"; }
    void init() override;
    void update(uint32_t dt_ms, const InputState& input) override;
    void draw(lgfx::LGFX_Sprite& fb) override;

private:
    uint8_t  _brightness_idx = 1; // 30%, 70%, 100%
    uint32_t _fps            = 0;
    uint32_t _frame_acc      = 0;
    uint32_t _time_acc_ms    = 0;
    int16_t  _last_tx        = -1;
    int16_t  _last_ty        = -1;
    uint32_t _ripple_ms      = 0;
    uint32_t _don_flash_ms   = 0;
    uint32_t _ka_flash_ms    = 0;
    uint16_t _last_held      = 0;
};
