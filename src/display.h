// Panel config for the Waveshare ESP32-S3-LCD-1.28 (SKU 26541, non-touch).
// Pins are from the official wiki, not guessed:
//   https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28
// LovyanGFX rather than TFT_eSPI on purpose: the whole config lives in this
// file instead of inside a library folder nobody remembers to edit.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static constexpr int PIN_LCD_SCLK = 10;
static constexpr int PIN_LCD_MOSI = 11;
static constexpr int PIN_LCD_DC = 8;
static constexpr int PIN_LCD_CS = 9;
static constexpr int PIN_LCD_RST = 12;
static constexpr int PIN_LCD_BL = 40;  // NOT 2 -- that is the touch SKU
static constexpr int PIN_BATT_ADC = 1;
static constexpr int PIN_BOOT_BUTTON = 0;

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 240;

class RoundBadgeDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

public:
  RoundBadgeDisplay() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      // 40MHz is the speed this board has been proven at. The wiki allows 80,
      // which doubles the blit ceiling -- try it only with eyes on the screen.
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = SCREEN_W;
      cfg.panel_height = SCREEN_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;  // without this the IPS panel shows colour negatives
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
