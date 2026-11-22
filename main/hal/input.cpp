#include "hal/input.hpp"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "hal/display.hpp"

namespace {

constexpr int64_t DEBOUNCE_US = 15000; // 15 ms

enum RawPinIndex : int {
    RAW_LEFT = 0,
    RAW_RIGHT,
    RAW_A_UP,
    RAW_B_DOWN,
    RAW_BOOT,
    RAW_COUNT,
};

struct DebouncedPin {
    int     gpio;
    bool    stable_state;
    bool    last_reading;
    int64_t last_change_us;
};

DebouncedPin s_pins[RAW_COUNT] = {
    {PIN_BTN_LEFT,   false, false, 0},
    {PIN_BTN_RIGHT,  false, false, 0},
    {PIN_BTN_A_UP,   false, false, 0},
    {PIN_BTN_B_DOWN, false, false, 0},
    {PIN_BTN_BOOT,   false, false, 0},
};

InputMode  s_mode       = InputMode::BOTH;
uint16_t   s_prev_held  = 0;
TouchState s_prev_touch = {};

uint16_t map_raw_to_logical(const bool raw[RAW_COUNT], InputMode mode) {
    uint16_t mask = 0;
    if (raw[RAW_LEFT])  mask |= BTN_LEFT;
    if (raw[RAW_RIGHT]) mask |= BTN_RIGHT;
    if (raw[RAW_BOOT])  mask |= BTN_PAUSE;

    if (raw[RAW_A_UP]) {
        if (mode == InputMode::LR_AB || mode == InputMode::BOTH)     mask |= BTN_A;
        if (mode == InputMode::DPAD_4WAY || mode == InputMode::BOTH) mask |= BTN_UP;
    }
    if (raw[RAW_B_DOWN]) {
        if (mode == InputMode::LR_AB || mode == InputMode::BOTH)     mask |= BTN_B;
        if (mode == InputMode::DPAD_4WAY || mode == InputMode::BOTH) mask |= BTN_DOWN;
    }
    return mask;
}

} // namespace

void input_init() {
    uint64_t pin_mask = 0;
    for (const auto& p : s_pins) {
        pin_mask |= (1ULL << p.gpio);
    }

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = pin_mask;
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
}

void input_set_mode(InputMode mode) {
    s_mode = mode;
}

InputMode input_get_mode() {
    return s_mode;
}

InputState input_poll() {
    const int64_t now_us = esp_timer_get_time();
    bool raw[RAW_COUNT]  = {};

    for (int i = 0; i < RAW_COUNT; ++i) {
        const bool reading = (gpio_get_level(static_cast<gpio_num_t>(s_pins[i].gpio)) == 0);
        if (reading != s_pins[i].last_reading) {
            s_pins[i].last_reading   = reading;
            s_pins[i].last_change_us = now_us;
        } else if ((now_us - s_pins[i].last_change_us) >= DEBOUNCE_US) {
            s_pins[i].stable_state = reading;
        }
        raw[i] = s_pins[i].stable_state;
    }

    InputState out = {};
    out.held     = map_raw_to_logical(raw, s_mode);
    out.pressed  = static_cast<uint16_t>(out.held & ~s_prev_held);
    out.released = static_cast<uint16_t>(~out.held & s_prev_held);
    s_prev_held  = out.held;

    int16_t tx = 0;
    int16_t ty = 0;
    const bool down = display_get_touch(&tx, &ty);
    out.touch.down          = down;
    out.touch.just_pressed  = down && !s_prev_touch.down;
    out.touch.just_released = !down && s_prev_touch.down;
    if (down) {
        out.touch.x = tx;
        out.touch.y = ty;
    } else {
        out.touch.x = s_prev_touch.x;
        out.touch.y = s_prev_touch.y;
    }
    s_prev_touch = out.touch;

    return out;
}
