// Stage 1 bring-up: GC9A01 240x240 round LCD on Waveshare ESP32-S3-LCD-1.28 (SKU 26541).
// Pins from the official wiki: CLK 10, MOSI 11, DC 8, CS 9, RST 12, backlight 40.
// LovyanGFX (not TFT_eSPI) so the whole panel config lives in this file.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

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
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 10;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 8;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 9;
      cfg.pin_rst = 12;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;  // GC9A01 IPS shows color negatives without this
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 40;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

RoundBadgeDisplay display;

struct Step {
  const char *name;
  uint32_t color;
  uint32_t textColor;
};

const Step steps[] = {
    {"RED", 0xFF0000U, 0xFFFFFFU},
    {"GREEN", 0x00FF00U, 0x000000U},
    {"BLUE", 0x0000FFU, 0xFFFFFFU},
    {"WHITE", 0xFFFFFFU, 0x000000U},
};

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[badge] boot");

  display.init();
  Serial.println("[badge] panel init done");

  display.setBrightness(255);
  Serial.println("[badge] backlight GPIO40 at full");

  display.setTextDatum(middle_center);
  display.setTextSize(3);
}

void loop() {
  for (const auto &step : steps) {
    display.fillScreen(display.color888(step.color >> 16, (step.color >> 8) & 0xFF, step.color & 0xFF));
    display.setTextColor(display.color888(step.textColor >> 16, (step.textColor >> 8) & 0xFF, step.textColor & 0xFF));
    display.drawString(step.name, 120, 120);
    Serial.printf("[badge] showing %s\n", step.name);
    delay(2500);
  }
}
