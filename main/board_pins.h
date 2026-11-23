#pragma once

#include <stdint.h>

// ============================================================================
// ES3C28P Hardware Pin Definitions (ESP32-S3R8, 2.8" IPS + FT6336G + ES8311)
// ============================================================================

// --- LCD (ILI9341V, 4-wire SPI on SPI2_HOST) ---
constexpr int PIN_LCD_CS   = 10;
constexpr int PIN_LCD_DC   = 46;
constexpr int PIN_LCD_SCK  = 12;
constexpr int PIN_LCD_MOSI = 11;
constexpr int PIN_LCD_MISO = 13;
constexpr int PIN_LCD_BL   = 45;
constexpr int PIN_LCD_RST  = -1; // Tied to CHIP_PU

constexpr int LCD_PANEL_WIDTH      = 240;
constexpr int LCD_PANEL_HEIGHT     = 320;
constexpr int LCD_DEFAULT_ROTATION = 1;    // 1 or 3 => 320x240 landscape
constexpr bool LCD_INVERT_COLORS   = true; // IPS ILI9341V panels typically require inversion

constexpr int SCREEN_WIDTH  = 320;
constexpr int SCREEN_HEIGHT = 240;

// --- Shared I2C Bus (FT6336G Capacitive Touch + ES8311 Audio Codec) ---
constexpr int I2C_SHARED_PORT = 0; // I2C_NUM_0
constexpr int PIN_I2C_SDA     = 16;
constexpr int PIN_I2C_SCL     = 15;
constexpr uint32_t I2C_SHARED_FREQ_HZ = 400000;

constexpr uint8_t I2C_ADDR_FT6336 = 0x38;
constexpr uint8_t I2C_ADDR_ES8311 = 0x18;

constexpr int PIN_TP_INT = 17;
constexpr int PIN_TP_RST = 18;

// --- Audio (ES8311 I2S + FM8002E Speaker Amp) ---
constexpr int PIN_I2S_MCLK = 4;
constexpr int PIN_I2S_BCLK = 5;
constexpr int PIN_I2S_DOUT = 8; // ESP32 DOUT -> ES8311 DSDIN (Speaker DAC)
constexpr int PIN_I2S_LRCK = 7;
constexpr int PIN_I2S_DIN  = 6; // ES8311 ASDOUT -> ESP32 DIN (Mic ADC)
constexpr int PIN_AMP_EN   = 1; // Active LOW enables FM8002E speaker amplifier

// --- User Tactile Switches (4-GPIO parallel 6-button + onboard BOOT) ---
// All buttons use internal pull-up and pull to GND when pressed (Active-Low).
constexpr int PIN_BTN_LEFT   = 2;
constexpr int PIN_BTN_RIGHT  = 3;
constexpr int PIN_BTN_A_UP   = 14; // Parallel: A and UP
constexpr int PIN_BTN_B_DOWN = 21; // Parallel: B and DOWN
constexpr int PIN_BTN_BOOT   = 0;  // Onboard BOOT button => Pause / Menu

// --- Onboard RGB LED ---
constexpr int PIN_RGB_LED = 42;
