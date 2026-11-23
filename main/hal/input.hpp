#pragma once

#include <stdint.h>

enum class InputMode : uint8_t {
    LR_AB,
    DPAD_4WAY,
    BOTH,
};

enum ButtonMask : uint16_t {
    BTN_NONE   = 0,
    BTN_LEFT   = 1 << 0,
    BTN_RIGHT  = 1 << 1,
    BTN_UP     = 1 << 2,
    BTN_DOWN   = 1 << 3,
    BTN_A      = 1 << 4,
    BTN_B      = 1 << 5,
    BTN_PAUSE  = 1 << 6,
    BTN_X      = 1 << 7,
    BTN_Y      = 1 << 8,
    BTN_L      = 1 << 9,
    BTN_R      = 1 << 10,
    BTN_SELECT = 1 << 11,
};

struct TouchState {
    bool    down          = false;
    bool    just_pressed  = false;
    bool    just_released = false;
    int16_t x             = 0;
    int16_t y             = 0;
};

struct InputState {
    uint16_t   held     = 0;
    uint16_t   pressed  = 0;
    uint16_t   released = 0;
    TouchState touch;
};

/**
 * @brief Configure onboard BOOT button GPIO and initialize BLE HID Host input.
 */
void input_init();

/**
 * @brief Retained for interface compatibility; BLE HID provides independent D-pad and face keys.
 */
void input_set_mode(InputMode mode);

InputMode input_get_mode();

/**
 * @brief Poll BLE HID keyboard state, onboard BOOT button, and capacitive touch once per frame.
 */
InputState input_poll();
