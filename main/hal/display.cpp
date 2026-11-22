#include "hal/display.hpp"

#include "board_pins.h"
#include "esp_log.h"

static const char* TAG = "display";

namespace {

class ES3C28P_LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
    lgfx::Touch_FT5x06  _touch;

public:
    ES3C28P_LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = PIN_LCD_MISO;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        {
            auto cfg = _panel.config();
            cfg.pin_cs           = PIN_LCD_CS;
            cfg.pin_rst          = PIN_LCD_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = LCD_PANEL_WIDTH;
            cfg.panel_height     = LCD_PANEL_HEIGHT;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = LCD_INVERT_COLORS;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }

        {
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 0;
            _light.config(cfg);
            _panel.setLight(&_light);
        }

        {
            auto cfg = _touch.config();
            cfg.x_min           = 0;
            cfg.x_max           = LCD_PANEL_WIDTH - 1;
            cfg.y_min           = 0;
            cfg.y_max           = LCD_PANEL_HEIGHT - 1;
            cfg.pin_int         = PIN_TP_INT;
            cfg.pin_rst         = PIN_TP_RST;
            cfg.bus_shared      = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port        = I2C_SHARED_PORT;
            cfg.i2c_addr        = I2C_ADDR_FT6336;
            cfg.pin_sda         = PIN_I2C_SDA;
            cfg.pin_scl         = PIN_I2C_SCL;
            cfg.freq            = I2C_SHARED_FREQ_HZ;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }

        setPanel(&_panel);
    }
};

ES3C28P_LGFX s_lcd;
lgfx::LGFX_Sprite s_fb(&s_lcd);

} // namespace

bool display_init() {
    if (!s_lcd.init()) {
        ESP_LOGE(TAG, "LGFX init failed");
        return false;
    }

    s_lcd.setRotation(LCD_DEFAULT_ROTATION);
    s_lcd.setColorDepth(16);
    display_set_brightness(70);
    s_lcd.fillScreen(TFT_BLACK);

    s_fb.setPsram(true);
    s_fb.setColorDepth(16);
    if (!s_fb.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT)) {
        ESP_LOGE(TAG, "Failed to allocate %dx%d framebuffer in PSRAM", SCREEN_WIDTH, SCREEN_HEIGHT);
        return false;
    }

    ESP_LOGI(TAG, "Display initialized (%dx%d), PSRAM FB ready", s_lcd.width(), s_lcd.height());
    return true;
}

lgfx::LGFX_Sprite& display_framebuffer() {
    return s_fb;
}

void display_present() {
    s_lcd.startWrite();
    s_fb.pushSprite(&s_lcd, 0, 0);
    s_lcd.endWrite();
}

void display_set_brightness(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    const uint8_t pwm = static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255) / 100);
    s_lcd.setBrightness(pwm);
}

bool display_get_touch(int16_t* x, int16_t* y) {
    uint16_t tx = 0;
    uint16_t ty = 0;
    if (s_lcd.getTouch(&tx, &ty)) {
        if (x) *x = static_cast<int16_t>(tx);
        if (y) *y = static_cast<int16_t>(ty);
        return true;
    }
    return false;
}
