#include "hal/input.hpp"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "hal/ble_hid.hpp"
#include "hal/display.hpp"

namespace {

constexpr int64_t DEBOUNCE_US = 15000; // 15 ms

struct DebouncedBootPin {
    bool    stable_state   = false;
    bool    last_reading   = false;
    int64_t last_change_us = 0;
};

DebouncedBootPin s_boot_pin;
InputMode        s_mode       = InputMode::BOTH;
uint16_t         s_prev_held  = 0;
TouchState       s_prev_touch = {};

} // namespace

void input_init() {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = (1ULL << PIN_BTN_BOOT);
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    ble_hid_init();
}

void input_set_mode(InputMode mode) {
    s_mode = mode;
}

InputMode input_get_mode() {
    return s_mode;
}

InputState input_poll() {
    const int64_t now_us = esp_timer_get_time();
    const bool reading = (gpio_get_level(static_cast<gpio_num_t>(PIN_BTN_BOOT)) == 0);
    if (reading != s_boot_pin.last_reading) {
        s_boot_pin.last_reading   = reading;
        s_boot_pin.last_change_us = now_us;
    } else if ((now_us - s_boot_pin.last_change_us) >= DEBOUNCE_US) {
        s_boot_pin.stable_state = reading;
    }

    uint16_t held = ble_hid_get_buttons();
    if (s_boot_pin.stable_state) {
        held |= BTN_PAUSE;
    }

    InputState out = {};
    out.held     = held;
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
