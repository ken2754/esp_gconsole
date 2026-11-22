#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <stdint.h>

/**
 * @brief Initialize ES3C28P ILI9341V display, FT6336G touch, and PSRAM framebuffer.
 *        Also initializes I2C_NUM_0 for shared peripherals (Touch & ES8311).
 * @return true on success.
 */
bool display_init();

/**
 * @brief Returns the 320x240 RGB565 offscreen framebuffer allocated in PSRAM.
 */
lgfx::LGFX_Sprite& display_framebuffer();

/**
 * @brief Push the offscreen framebuffer to the LCD using SPI DMA.
 */
void display_present();

/**
 * @brief Set backlight brightness (0..100%).
 */
void display_set_brightness(uint8_t percent);

/**
 * @brief Read current touch state in rotated screen coordinates (320x240).
 */
bool display_get_touch(int16_t* x, int16_t* y);
