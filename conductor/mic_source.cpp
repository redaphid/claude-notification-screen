#include "mic_source.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#ifndef CONDUCTOR_FAKE_MIC

// Arduino core 2.0.11 ships the legacy driver/i2s.h API. (Core 3.x's ESP_I2S /
// I2SClass, which Waveshare's own demo uses, does not exist here -- checked
// against the installed headers, not remembered.)
#include <driver/i2s.h>

#define MIC_I2S_PORT I2S_NUM_0

bool MicSource::begin() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  // Stereo even though there is one mic: which slot an MSM-series mic drives
  // depends on an L/R strap pin the wiki does not document, so capture both and
  // work it out at runtime.
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  // 256-frame DMA buffers, matched to the hop, so a read returns exactly one
  // analysis hop and never sits on a half-full buffer.
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = HOP_SIZE;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[mic] i2s_driver_install failed: %d\n", (int)err);
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = MIC_PIN_MCLK;
  pins.bck_io_num = MIC_PIN_BCLK;
  pins.ws_io_num = MIC_PIN_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_PIN_DIN;

  err = i2s_set_pin(MIC_I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[mic] i2s_set_pin failed: %d (BCLK=%d WS=%d DIN=%d)\n", (int)err,
                  MIC_PIN_BCLK, MIC_PIN_WS, MIC_PIN_DIN);
    return false;
  }

  i2s_zero_dma_buffer(MIC_I2S_PORT);
  Serial.printf("[mic] I2S up: BCLK=%d WS=%d DIN=%d @ %d Hz\n", MIC_PIN_BCLK, MIC_PIN_WS,
                MIC_PIN_DIN, AUDIO_SAMPLE_RATE);

#if MIC_CHANNEL_SELECT == 2
  resolveChannel();
#else
  _channel = MIC_CHANNEL_SELECT;
  Serial.printf("[mic] channel forced to %s\n", _channel ? "RIGHT" : "LEFT");
#endif
  return true;
}

void MicSource::resolveChannel() {
  // A mic strapped to one slot leaves the other at a dead constant. Listen
  // briefly and take whichever slot actually moves. Costs under a second at
  // boot and removes an entire class of "the mic is broken" bring-up bug.
  double sumL = 0.0, sumR = 0.0;
  uint32_t deadline = millis() + MIC_AUTODETECT_MS;
  uint32_t frames = 0;
  while ((int32_t)(millis() - deadline) < 0) {
    size_t got = 0;
    if (i2s_read(MIC_I2S_PORT, _raw, sizeof(_raw), &got, pdMS_TO_TICKS(100)) != ESP_OK) continue;
    size_t n = got / (2 * sizeof(int16_t));
    // Mean-remove crudely by differencing: a constant slot contributes nothing.
    for (size_t i = 1; i < n; i++) {
      sumL += fabs((double)_raw[2 * i] - (double)_raw[2 * i - 2]);
      sumR += fabs((double)_raw[2 * i + 1] - (double)_raw[2 * i - 1]);
    }
    frames += n;
  }
  _channel = (sumR > sumL) ? 1 : 0;
  Serial.printf("[mic] channel auto-detect over %u frames: L=%.1f R=%.1f -> %s\n",
                (unsigned)frames, sumL, sumR, _channel ? "RIGHT" : "LEFT");
  if (sumL + sumR < 1.0) {
    Serial.println("[mic] WARNING: both slots are flat. Check the PIN BLOCK in "
                   "conductor_config.h before blaming the analysis.");
  }
}

bool MicSource::readHop(float *out) {
  size_t got = 0;
  esp_err_t err = i2s_read(MIC_I2S_PORT, _raw, sizeof(_raw), &got, pdMS_TO_TICKS(200));
  if (err != ESP_OK) return false;
  size_t n = got / (2 * sizeof(int16_t));
  size_t i = 0;
  for (; i < n && i < HOP_SIZE; i++) out[i] = (float)_raw[2 * i + _channel];
  for (; i < HOP_SIZE; i++) out[i] = 0.0f;  // short read: pad rather than stall
  return n > 0;
}

const char *MicSource::sourceName() const { return "I2S mic (ESP32-S3-Touch-LCD-1.46)"; }

#else  // ------------------------------- CONDUCTOR_FAKE_MIC -------------------

// A 128 BPM loop with a kick on the beat, a hat on the offbeat, a sustained bass
// note and a mid pad -- plus a slow 30 s level drift, which is there on purpose:
// it is what proves the venue-adaptive normalisation actually adapts instead of
// just scaling. Watch the normalised features stay in range while rawDb slides.
static const double kBpm = 128.0;

float MicSource::noise() {
  _rngState = _rngState * 1664525u + 1013904223u;
  return ((float)(int32_t)(_rngState >> 8) / 8388608.0f) - 1.0f;
}

bool MicSource::begin() {
  _t = 0.0;
  _nextDueMs = millis();
  _channel = 0;
  Serial.println("[mic] SYNTHESISED input (-DCONDUCTOR_FAKE_MIC). No hardware is being read.");
  return true;
}

bool MicSource::readHop(float *out) {
  // Pace generation to real time so the 30 Hz packet cadence, the refractory
  // period and the normalisation time constants all mean what they say.
  const uint32_t hopMs = (uint32_t)(1000.0 * (double)HOP_SIZE / (double)AUDIO_SAMPLE_RATE);
  while ((int32_t)(millis() - _nextDueMs) < 0) delay(1);
  _nextDueMs += hopMs;

  const double dt = 1.0 / (double)AUDIO_SAMPLE_RATE;
  const double beat = 60.0 / kBpm;

  for (int i = 0; i < HOP_SIZE; i++) {
    double t = _t + (double)i * dt;
    double phase = fmod(t, beat);
    double halfPhase = fmod(t, beat * 0.5);

    // Kick: 55 Hz with a fast decay. Pitch-drops slightly, like a real one.
    double kEnv = exp(-phase / 0.06);
    double kFreq = 55.0 + 45.0 * exp(-phase / 0.02);
    double kick = kEnv * sin(2.0 * M_PI * kFreq * phase);

    // Hat on the offbeat: short noise burst.
    double hEnv = (phase > beat * 0.4) ? exp(-(halfPhase) / 0.025) : 0.0;
    double hat = hEnv * (double)noise() * 0.5;

    // Sustained low end and a mid pad, so bass/mid/treble are all populated.
    double bassNote = 0.25 * sin(2.0 * M_PI * 82.0 * t);
    double pad = 0.18 * sin(2.0 * M_PI * 440.0 * t) * (0.6 + 0.4 * sin(2.0 * M_PI * 0.25 * t));

    // Venue drift: the room gets loud and quiet over 30 s.
    double venue = 0.12 + 0.88 * (0.5 + 0.5 * sin(2.0 * M_PI * t / 30.0));

    // A little sub-40 Hz rumble, so the high-pass filter has something to earn
    // its keep against. If the bass band tracks this, the filter is broken.
    double rumble = 0.6 * sin(2.0 * M_PI * 11.0 * t) + 0.3 * sin(2.0 * M_PI * 23.0 * t);

    double s = venue * (0.9 * kick + 0.5 * hat + bassNote + pad) + rumble;
    out[i] = (float)(s * 9000.0);
  }
  _t += (double)HOP_SIZE * dt;
  return true;
}

const char *MicSource::sourceName() const { return "SYNTHESISED 128 BPM loop"; }

#endif
