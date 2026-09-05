#include "conductor_display.h"

#include <Arduino.h>
#include <Wire.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "Panel_SPD2010.hpp"
#include "../effects/effect.h"
#include "../effects/effects.h"
#include "../effects/effect_common.h"

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
      // Quad SPI uses its own four pin fields. Setting all four is also what
      // makes Bus_SPI select quad mode at all -- with pin_mosi/pin_dc instead
      // it silently runs single-line and the panel never responds.
      cfg.pin_io0 = LCD_PIN_D0;
      cfg.pin_io1 = LCD_PIN_D1;
      cfg.pin_io2 = LCD_PIN_D2;
      cfg.pin_io3 = LCD_PIN_D3;
      cfg.pin_mosi = -1;
      cfg.pin_miso = -1;
      cfg.pin_dc = -1;
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
uint32_t lastDrawUs = 0;

// A frame costs ~30ms to transform and push -- measured, not estimated -- while
// the microphone needs servicing every 16ms. Drawing on the audio thread halved
// the analysis rate from 62 to 30 hops/sec, which means dropped audio.
//
// So the panel gets its own task on the other core. The audio path publishes a
// snapshot and never waits for pixels: the swarm matters more than the monitor.
struct FrameSnapshot {
  float features[4];
  uint8_t beat;
  float beatEnv;
  uint32_t txCount;
  uint32_t analysisFps;
};
portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
volatile FrameSnapshot pending = {};
volatile bool beatPending = false;
uint32_t drawFps = 0;

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

void displayTask(void *);  // defined below, runs on the other core

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

  // Same trick that settled the badge: unmistakable full-screen fills straight
  // through the panel API, before any sprite or effect is involved. If these
  // show, the QSPI transport and init sequence are good and any remaining
  // problem is in the sprite path. Readable through a webcam from across a room.
  struct Card { const char *name; uint8_t r, g, b; };
  static const Card cards[] = {
      {"RED", 255, 0, 0}, {"GREEN", 0, 255, 0}, {"BLUE", 0, 0, 255}, {"WHITE", 255, 255, 255}};
  for (const auto &c : cards) {
    display.fillScreen(display.color888(c.r, c.g, c.b));
    Serial.printf("[leader selftest] %s\n", c.name);
    delay(1200);
  }
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
  // The effects degrade to black rather than crashing when their LUT cannot be
  // allocated, which on a panel is indistinguishable from a dead display. Check
  // it once, out loud, so the next person does not debug the wrong layer.
  Serial.printf("[leader] free psram %u, free heap %u\n", (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getFreeHeap());
  if (effect_polar == NULL) {
    Serial.println("[leader] WARNING: effect LUT alloc FAILED -- effects will render BLACK");
  } else {
    Serial.println("[leader] effect LUT allocated");
  }

  displayReady = true;

  // Core 0: the Arduino loop (audio, analysis, radio) owns core 1.
  xTaskCreatePinnedToCore(displayTask, "leader-display", 6144, nullptr, 1, nullptr, 0);

  Serial.printf("[leader] display up: %dx%d SPD2010 QSPI, %d effects, drawing on core 0\n",
                LCD_W, LCD_H, effects_count);
  return true;
}

namespace {

void renderFrame(const FrameSnapshot &snap) {
  const float *features = snap.features;
  const uint8_t beat = snap.beat;
  const uint32_t txCount = snap.txCount;
  const uint32_t analysisFps = snap.analysisFps;

  EffectInput in;
  in.bass = features[0];
  in.mid = features[1];
  in.treble = features[2];
  in.energy = features[3];
  in.time_ms = millis();
  in.beat = beat;
  in.beat_env = snap.beatEnv;

  effects_by_index(0)->render((uint16_t *)canvas.getBuffer(), &in);

  // The conductor's screen is a monitor, not just a visual: it shows what this
  // board is hearing, so a silent mic or a wedged radio is visible from across
  // a room without a serial cable.
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.setTextSize(1);
  canvas.setTextColor(canvas.color888(255, 255, 255));
  char line[32];
  snprintf(line, sizeof(line), "LEADER a%lu d%lu", (unsigned long)analysisFps,
           (unsigned long)drawFps);
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

  // Scale the 240x240 effect up to fill the 412 circle. An earlier attempt at
  // this looked like a uniform wash, which was misread as the scaler being at
  // fault -- the real cause was the effect rendering black because its LUT
  // allocation had failed. With that fixed the scale-up is correct, and a
  // leader showing a small picture in a big black ring looks like a mistake.
  const uint32_t t0 = micros();
  canvas.pushRotateZoom(&display, LCD_W / 2, LCD_H / 2, 0.0f, LCD_W / (float)EFFECT_W,
                        LCD_H / (float)EFFECT_H);
  lastDrawUs = micros() - t0;
}

void displayTask(void *) {
  uint32_t frames = 0, lastFpsMs = millis();
  for (;;) {
    FrameSnapshot snap;
    portENTER_CRITICAL(&snapshotMux);
    snap = *(const FrameSnapshot *)&pending;
    // An onset lasts one analysis hop but the screen may not draw for another
    // 30ms, so a beat is latched until a frame has actually shown it.
    snap.beat = beatPending ? 1 : 0;
    beatPending = false;
    portEXIT_CRITICAL(&snapshotMux);

    renderFrame(snap);

    frames++;
    const uint32_t now = millis();
    if (now - lastFpsMs >= 1000) {
      drawFps = frames * 1000 / (now - lastFpsMs);
      frames = 0;
      lastFpsMs = now;
    }
    // Yield regardless: this task must never starve anything else on its core.
    vTaskDelay(1);
  }
}

}  // namespace

void conductorDisplayDraw(const float features[4], uint8_t beat, float beatEnv,
                          uint32_t txCount, uint32_t analysisFps) {
  if (!displayReady) return;
  portENTER_CRITICAL(&snapshotMux);
  for (int i = 0; i < 4; i++) pending.features[i] = features[i];
  pending.beatEnv = beatEnv;
  pending.txCount = txCount;
  pending.analysisFps = analysisFps;
  if (beat) beatPending = true;
  portEXIT_CRITICAL(&snapshotMux);
}

uint32_t conductorDisplayLastDrawUs() { return lastDrawUs; }
uint32_t conductorDisplayFps() { return drawFps; }
