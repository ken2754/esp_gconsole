#pragma once

#include <stdint.h>

enum class InputMode : uint8_t {
    LR_AB,      // LEFT, RIGHT, A (pin 14), B (pin 21)
    DPAD_4WAY,  // LEFT, RIGHT, UP (pin 14), DOWN (pin 21)
    BOTH,       // Sets both A/UP and B/DOWN simultaneously (for diagnostics)
};

enum ButtonMask : uint16_t {
    BTN_NONE  = 0,
    BTN_LEFT  = 1 << 0,
    BTN_RIGHT = 1 << 1,
    BTN_UP    = 1 << 2,
    BTN_DOWN  = 1 << 3,
    BTN_A     = 1 << 4,
    BTN_B     = 1 << 5,
    BTN_PAUSE = 1 << 6,
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
 * @brief Configure tactile switch GPIOs with internal pull-ups.
 */
void input_init();

/**
 * @brief Select how parallel-wired UP/A and DOWN/B pins map to logical buttons.
 */
void input_set_mode(InputMode mode);

InputMode input_get_mode();

/**
 * @brief Poll physical buttons (with debounce) and touch state once per frame.
 */
InputState input_poll();
