#include "hal/display.hpp"

#include <inttypes.h>

#include "board_pins.h"
#include "esp_log.h"

static const char* TAG = "display";

namespace {

class Panel_ILI9341_ES3C28P : public lgfx::Panel_ILI9341 {
protected:
    const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0xEF       , 3, 0x03,0x80,0x02,
            0xCF       , 3, 0x00,0xC1,0x30,
            0xED       , 4, 0x64,0x03,0x12,0x81,
            0xE8       , 3, 0x85,0x00,0x78,
            0xCB       , 5, 0x39,0x2C,0x00,0x34,0x02,
            0xF7       , 1, 0x20,
            0xEA       , 2, 0x00,0x00,
            CMD_PWCTR1,  1, 0x23,
            CMD_PWCTR2,  1, 0x10,
            CMD_VMCTR1,  2, 0x3e,0x28,
            CMD_VMCTR2,  1, 0x86,
            CMD_FRMCTR1, 2, 0x00,0x13,
            0xF2       , 1, 0x00,
            CMD_GAMMASET,1, 0x01,
            CMD_GMCTRP1,15, 0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00,
            CMD_GMCTRN1,15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F,
            CMD_DFUNCTR, 3, 0x08,0x82,0x27, // GS=0, SS=0 matching ES3C28P ILI9341V
            CMD_SLPOUT , 0+CMD_INIT_DELAY, 120,
            CMD_IDMOFF , 0,
            CMD_DISPON , 0+CMD_INIT_DELAY, 100,
            0xFF,0xFF,
        };
        return (listno == 0) ? list0 : nullptr;
    }
};

class ES3C28P_LGFX : public lgfx::LGFX_Device {
    Panel_ILI9341_ES3C28P _panel;
    lgfx::Bus_SPI         _bus;
    lgfx::Light_PWM       _light;
    lgfx::Touch_FT5x06    _touch;

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
            cfg.offset_rotation  = 4; // With setRotation(1) => MADCTL 0x28 (matches ES3C28P)
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
            cfg.offset_rotation = 4; // Maps FT6336G raw portrait coords to match panel MADCTL 0x28
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
i2c_master_bus_handle_t s_i2c_bus = nullptr;

} // namespace

bool display_init() {
    // Initialize shared I2C_NUM_0 with driver_ng (i2c_master) before LovyanGFX init
    // so LovyanGFX detects foreign_bus=true and shares the bus with ES8311 cleanly.
    if (!s_i2c_bus) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port                   = static_cast<i2c_port_t>(I2C_SHARED_PORT);
        bus_cfg.sda_io_num                 = static_cast<gpio_num_t>(PIN_I2C_SDA);
        bus_cfg.scl_io_num                 = static_cast<gpio_num_t>(PIN_I2C_SCL);
        bus_cfg.clk_source                 = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt          = 7;
        bus_cfg.flags.enable_internal_pullup = true;
        const esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return false;
        }
    }

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

    ESP_LOGI(TAG, "Display initialized (%" PRId32 "x%" PRId32 "), PSRAM FB ready", s_lcd.width(), s_lcd.height());
    return true;
}

i2c_master_bus_handle_t display_get_i2c_bus() {
    return s_i2c_bus;
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
