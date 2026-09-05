// Shared, allocate-once scaffolding for every effect in effects/.
//
// This is deliberately NOT part of the frozen effect.h contract -- effect.h
// defines what an effect *is*, this file is the toolbox the three shipped
// effects happen to share so the 240x240 polar geometry is only built once.
//
// Rules inherited from effect.h and enforced here:
//   - pure C99 + stdint (math.h is used, but only outside inner loops)
//   - no floats in any per-pixel loop; floats are allowed while filling the
//     small (<=768 entry) per-frame lookup tables, which is ~1% of the pixels
//   - everything big is allocated once, in init()
#pragma once

#include <stdint.h>
#include "effect.h"

#ifdef __cplusplus
extern "C" {
#endif

// Big LUTs go through EFFECT_ALLOC so the badge firmware can move them to
// PSRAM without editing (or platform-contaminating) any effect source. The
// polar LUT is read strictly sequentially per scanline, which is the one
// access pattern PSRAM is actually good at.
//
// To put it in PSRAM, add to the PlatformIO env:
//   build_flags =
//       -DEFFECT_ALLOC_HEADER='"esp_heap_caps.h"'
//       -D'EFFECT_ALLOC(n)=heap_caps_malloc((n), MALLOC_CAP_SPIRAM)'
// Left off by default: 115KB of internal SRAM does fit today, and nobody has
// measured the PSRAM read cost on real hardware yet.
#ifdef EFFECT_ALLOC_HEADER
#include EFFECT_ALLOC_HEADER
#endif
#ifndef EFFECT_ALLOC
#include <stdlib.h>
#define EFFECT_ALLOC(bytes) malloc(bytes)
#endif

// ---------------------------------------------------------------------------
// Shared geometry. effect_geom_init() is idempotent: all three effects call it
// from their own init() and only the first call allocates.
// ---------------------------------------------------------------------------

// effect_polar[y * EFFECT_W + x] == (angle << 8) | radius
//   radius: 0 at the centre, 255 at EFFECT_RADIUS (clamped past the rim)
//   angle : 0..255 for one full turn
// Packed into one uint16 on purpose: one sequential load per pixel yields both
// halves, which halves the PSRAM stream count in the inner loop.
extern const uint16_t *effect_polar;

// Inclusive x span of the visible disc for each scanline. Pixels outside are
// never seen but must still be written black (effect.h).
extern int16_t effect_row_x0[EFFECT_H];
extern int16_t effect_row_x1[EFFECT_H];

// sin(2*pi * i/256) scaled to +/-127.
extern int8_t effect_sin8[256];

void effect_geom_init(void);

// ---------------------------------------------------------------------------
// Small helpers. None are used per pixel except effect_clamp_u8.
// ---------------------------------------------------------------------------

static inline int effect_clamp_u8(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline float effect_clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Monotonic delta that survives the ~49 day wrap of EffectInput.time_ms.
// uint32 subtraction wraps correctly; the clamp absorbs the first frame and
// any stall (a paused debugger, a long blocking ESP-NOW call).
static inline uint32_t effect_dt_ms(uint32_t *last, uint32_t now) {
  uint32_t dt = now - *last;
  *last = now;
  if (dt > 100u) dt = 100u;
  return dt;
}

// ---------------------------------------------------------------------------
// Colour, cribbed from paper-cranes rather than reinvented.
//
// The hue wheel these effects used before was a six-region RGB ramp: cheap,
// and perceptually wrong in the way that makes a visual look generic. Full
// yellow is blinding next to full blue, "brightness" means multiplying RGB so
// saturated colours go muddy as they dim, and lifting toward white washes
// everything pastel. paper-cranes solved this years ago with Oklab, and its
// shaders are the reference for what these badges are supposed to look like.
//
// Palettes are built once per frame (<=256 entries), never per pixel, so the
// float and trig cost here is inside the budget effect.h sets.
// ---------------------------------------------------------------------------

// Oklab -> RGB, the exact matrix from paper-cranes' shader-wrapper.js. Like
// that one it returns LINEAR values with no gamma encode: paper-cranes writes
// them straight to the framebuffer, so matching its look means matching that
// too, not "correcting" it.
void effect_oklab_rgb(float L, float a, float b, int *r, int *g, int *bl);

// The `lush()` palette from redaphid/chromadepth-lattice/6.frag: a perceptual
// Oklch journey, high chroma so it reads as neon rather than pastel, bounded
// away from both white and black.
//   s   hue position; wraps, so it can be accumulated forever
//   lit 0..1 lightness
void effect_lush(float s, float lit, int *r, int *g, int *b);

// The ChromaDepth ramp from paper-cranes' scripts/fix-chromadepth-shader.md,
// which exists because getting this wrong is the normal outcome. Through the
// glasses red reads nearest and violet farthest, so t is depth: 0 near, 1 far.
// It MUST be Oklab -- an HSL or RGB hue wheel is the documented failure mode,
// and it is what the badge's chroma effect was doing.
void effect_chromadepth(float t, int *r, int *g, int *b);

// paper-cranes' GLOW LIFT: gamma up then gain, so mid-tones emit instead of
// sitting in the mud. High chroma is what keeps this neon rather than washed
// out, which is why it belongs with the palettes above and not on its own.
void effect_glow_lift(int *r, int *g, int *b);

// Per-device palette identity, paper-cranes' `seed2`. Its shaders seed the
// palette from a per-device random persisted in localStorage so every screen
// gets its own one-of-a-kind look; a badge has something better than random --
// its MAC. 0..1, default 0.5. Affects chroma (saturation) in effect_lush().
void effect_set_seed(float seed01);

#define EFFECT_TAU 6.28318530718f

// 0..255 sine, for palette and shape ramps.
static inline int effect_sinu(uint8_t phase) {
  return 128 + effect_sin8[phase];
}

#ifdef __cplusplus
}
#endif
