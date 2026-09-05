#include "conductor_display.h"

#include <Arduino.h>
#include <Wire.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "Panel_SPD2010.hpp"
#include "../effects/effect.h"
#include "../effects/effects.h"

namespace {

class LeaderDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_SPD2010 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

public:
  LeaderDisplay() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = LCD_SPI_MODE;
      cfg.freq_write = LCD_SPI_HZ;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = LCD_PIN_SCK;
      cfg.pin_mosi = LCD_PIN_D0;
      cfg.pin_miso = LCD_PIN_D1;
      cfg.pin_dc = LCD_PIN_D2;  // QSPI: data lines 1..3 ride in these fields
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = LCD_PIN_CS;
      cfg.pin_rst = -1;  // reset lives on the I2C expander, handled separately
      cfg.pin_busy = -1;
      cfg.panel_width = LCD_W;
      cfg.panel_height = LCD_H;
      cfg.memory_width = LCD_W;
      cfg.memory_height = LCD_H;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = LCD_PIN_BACKLIGHT;
      cfg.invert = false;
      cfg.freq = 20000;
      cfg.pwm_channel = 1;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LeaderDisplay display;
LGFX_Sprite canvas(&display);
bool displayReady = false;

bool expanderWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(EXPANDER_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool expanderRead(uint8_t reg, uint8_t *value) {
  Wire.beginTransmission(EXPANDER_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)EXPANDER_I2C_ADDR, 1) != 1) return false;
  *value = Wire.read();
  return true;
}

// Low, wait, high -- the sequence Waveshare's SPD2010_Reset() performs.
bool releasePanelFromReset() {
  const uint8_t mask = (uint8_t)(1 << (EXPANDER_LCD_RESET_PIN - 1));
  uint8_t out = 0;
  if (!expanderRead(EXPANDER_OUTPUT_REG, &out)) return false;
  if (!expanderWrite(EXPANDER_CONFIG_REG, 0x00)) return false;  // all outputs
  if (!expanderWrite(EXPANDER_OUTPUT_REG, (uint8_t)(out & ~mask))) return false;
  delay(50);
  if (!expanderWrite(EXPANDER_OUTPUT_REG, (uint8_t)(out | mask))) return false;
  delay(50);
  return true;
}

}  // namespace

bool conductorDisplayInit() {
  Wire.begin(EXPANDER_I2C_SDA, EXPANDER_I2C_SCL, 400000);
  delay(20);

  if (!releasePanelFromReset()) {
    Serial.println("[leader] TCA9554 expander did not ACK -- is this really the 1.46?");
    return false;
  }
  Serial.println("[leader] panel released from reset via expander");

  pinMode(LCD_PIN_TE, OUTPUT);

  if (!display.init()) {
    Serial.println("[leader] SPD2010 init FAILED");
    return false;
  }
  display.setBrightness(200);
  display.fillScreen(0);

  // The effects render at 240x240; the panel is 412x412. Draw into a sprite at
  // the effect's native size and scale it up on the way out, rather than
  // teaching the effects about a second resolution.
  canvas.setColorDepth(16);
  canvas.setPsram(true);  // 8MB on this board; the badge has no such luxury
  if (!canvas.createSprite(EFFECT_W, EFFECT_H)) {
    Serial.println("[leader] could not allocate 240x240 sprite");
    return false;
  }
  canvas.fillSprite(0);

  for (int i = 0; i < effects_count; i++) {
    if (effects_all[i]->init) effects_all[i]->init();
  }

  displayReady = true;
  Serial.printf("[leader] display up: %dx%d SPD2010 QSPI, %d effects\n", LCD_W, LCD_H,
                effects_count);
  return true;
}

void conductorDisplayDraw(const float features[4], uint8_t beat, float beatEnv,
                          uint32_t txCount, uint32_t analysisFps) {
  if (!displayReady) return;

  EffectInput in;
  in.bass = features[0];
  in.mid = features[1];
  in.treble = features[2];
  in.energy = features[3];
  in.time_ms = millis();
  in.beat = beat;
  in.beat_env = beatEnv;

  effects_by_index(0)->render((uint16_t *)canvas.getBuffer(), &in);

  // The conductor's screen is a monitor, not just a visual: it shows what this
  // board is hearing, so a silent mic or a wedged radio is visible from across
  // a room without a serial cable.
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextSize(1);
  canvas.setTextColor(canvas.color888(255, 255, 255));
  char line[32];
  snprintf(line, sizeof(line), "LEADER %lufps", (unsigned long)analysisFps);
  canvas.drawString(line, 6, 6);
  snprintf(line, sizeof(line), "tx %lu", (unsigned long)txCount);
  canvas.drawString(line, 6, 18);

  static const char *labels[4] = {"B", "M", "T", "E"};
  for (int i = 0; i < 4; i++) {
    const int h = (int)(features[i] * 60.0f);
    const int x = 8 + i * 14;
    canvas.fillRect(x, EFFECT_H - 12 - h, 10, h, canvas.color888(255, 255, 255));
    canvas.drawString(labels[i], x + 1, EFFECT_H - 10);
  }
  if (beat) {
    canvas.drawCircle(EFFECT_W / 2, EFFECT_H / 2, 110, canvas.color888(255, 255, 255));
  }

  // 240 -> 412 is 1.716x; pushRotateZoom centres it on the round panel.
  canvas.pushRotateZoom(&display, LCD_W / 2, LCD_H / 2, 0.0f, LCD_W / (float)EFFECT_W,
                        LCD_H / (float)EFFECT_H);
}
