// The effect interface — the second frozen contract of this project.
//
// An effect is a pure function from (audio features, time) to a 240x240
// RGB565 framebuffer. The SAME source file compiles for the badge (ESP32-S3,
// Arduino) and for the desktop harness, so effects can be developed and
// watched on a laptop and then run unchanged on hardware.
//
// Rules for effect implementations:
//   - No floats in the per-pixel inner loop. Fixed point and lookup tables.
//   - No Arduino/LovyanGFX/ESP-IDF headers. Pure C99 + stdint only.
//   - Budget: under 25ms per frame on an ESP32-S3 at 240MHz (>= 20fps once
//     the ~12ms SPI blit is accounted for).
//   - Allocate LUTs once in init(); render() must not allocate.
#pragma once

#include <stdint.h>

#define EFFECT_W 240
#define EFFECT_H 240
#define EFFECT_PIXELS (EFFECT_W * EFFECT_H)

// Everything an effect is allowed to react to. Features are already smoothed
// and normalised 0..1 by the caller; an effect must not do its own smoothing.
typedef struct {
  float bass;
  float mid;
  float treble;
  float energy;
  uint32_t time_ms;   // monotonic; wraps after ~49 days, effects must tolerate it
  uint8_t beat;       // 1 only on the frame an onset fired
  float beat_env;     // 1.0 at onset decaying to 0.0, the designed attack-decay
} EffectInput;

typedef struct {
  const char *name;
  void (*init)(void);                                  // build LUTs; may allocate
  void (*render)(uint16_t *out, const EffectInput *in);// write EFFECT_PIXELS words
} Effect;

// Badge framebuffers are LovyanGFX 16-bit sprites, which store byte-swapped
// RGB565. Every effect MUST produce pixels through this helper rather than
// packing 565 by hand, so the byte order lives in exactly one place.
static inline uint16_t effect_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((c >> 8) | (c << 8));
}

static inline void effect_unpack565(uint16_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint16_t c = (uint16_t)((v >> 8) | (v << 8));
  *r = (uint8_t)((c >> 8) & 0xF8);
  *g = (uint8_t)((c >> 3) & 0xFC);
  *b = (uint8_t)((c << 3) & 0xF8);
}

// The screen is round: pixels outside this radius are never seen. Effects may
// skip them, but must leave them black rather than undefined.
#define EFFECT_RADIUS 120
