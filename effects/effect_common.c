#include "effect_common.h"

#include <math.h>
#include <stddef.h>

const uint16_t *effect_polar = NULL;
int16_t effect_row_x0[EFFECT_H];
int16_t effect_row_x1[EFFECT_H];
int8_t effect_sin8[256];

static uint16_t *g_polar_storage = NULL;

void effect_geom_init(void) {
  if (g_polar_storage != NULL) return;  // idempotent: three effects, one build

  for (int i = 0; i < 256; ++i) {
    effect_sin8[i] = (int8_t)lrintf(127.0f * sinf((float)i * (2.0f * 3.14159265358979f / 256.0f)));
  }

  // 240 * 240 * 2 == 115200 bytes. This is the only large allocation the
  // effects make, and it is shared by all of them.
  g_polar_storage = (uint16_t *)EFFECT_ALLOC((size_t)EFFECT_PIXELS * sizeof(uint16_t));
  if (g_polar_storage == NULL) return;  // render() degrades to black, never crashes
  effect_polar = g_polar_storage;

  // Geometric centre of a 240px panel sits at 120.0, pixel centres at x+0.5.
  const float cx = (float)EFFECT_W * 0.5f - 0.5f;
  const float cy = (float)EFFECT_H * 0.5f - 0.5f;
  const float rscale = 255.0f / (float)EFFECT_RADIUS;
  const float ascale = 256.0f / (2.0f * 3.14159265358979f);

  for (int y = 0; y < EFFECT_H; ++y) {
    const float dy = (float)y - cy;
    int x0 = EFFECT_W;
    int x1 = -1;
    for (int x = 0; x < EFFECT_W; ++x) {
      const float dx = (float)x - cx;
      const float rr = sqrtf(dx * dx + dy * dy);

      int ri = (int)lrintf(rr * rscale);
      if (ri < 0) ri = 0;
      if (ri > 255) ri = 255;

      float av = atan2f(dy, dx) * ascale;
      int ai = ((int)lrintf(av)) & 255;

      g_polar_storage[y * EFFECT_W + x] = (uint16_t)((ai << 8) | ri);

      if (rr <= (float)EFFECT_RADIUS) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
      }
    }
    if (x1 < x0) { x0 = 0; x1 = -1; }  // empty scanline: whole row is off-disc
    effect_row_x0[y] = (int16_t)x0;
    effect_row_x1[y] = (int16_t)x1;
  }
}

// --- colour ---------------------------------------------------------------
// See the long note in effect_common.h for why this is Oklab and where each
// constant comes from. Nothing here runs per pixel.

static float s_seed2 = 0.5f;
static float s_seed_hue = 0.0f;
static float s_seed_struct = 0.5f;

void effect_set_seeds(float hue, float sat, float structure) {
  s_seed_hue = effect_clamp01(hue);
  s_seed2 = effect_clamp01(sat);
  s_seed_struct = effect_clamp01(structure);
}

float effect_seed_hue(void) { return s_seed_hue; }
float effect_seed_structure(void) { return s_seed_struct; }

// Linear -> sRGB.
//
// paper-cranes' oklab2rgb returns LINEAR values and writes them straight to a
// WebGL framebuffer, which the display then reads as though they were already
// sRGB. Every colour therefore lands darker and richer than Oklab nominally
// asked for. That is not a bug to fix here: it IS the paper-cranes look, the
// one these badges are meant to share, and "correcting" it produced a pastel
// wash -- measured, mean saturation across the five effects fell from 0.87-1.00
// to 0.32-0.69. So effect_lush() writes linear, exactly as the shaders do.
//
// The ChromaDepth ramp is the one place that cannot afford it, which is why
// this exists and why only effect_chromadepth() calls it. See the note there.
#define SRGB_LUT_N 257
static float s_srgb_lut[SRGB_LUT_N];
static int s_srgb_built = 0;

static float srgb_encode(float c) {
  if (!s_srgb_built) {
    for (int i = 0; i < SRGB_LUT_N; ++i) {
      const float x = (float)i / (float)(SRGB_LUT_N - 1);
      s_srgb_lut[i] = x <= 0.0031308f ? x * 12.92f : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
    }
    s_srgb_built = 1;
  }
  c = effect_clamp01(c);
  const float f = c * (float)(SRGB_LUT_N - 1);
  const int i = (int)f;
  if (i >= SRGB_LUT_N - 1) return s_srgb_lut[SRGB_LUT_N - 1];
  // Interpolated, so the curve stays smooth where it is steepest (near black)
  // rather than banding into 256 visible steps.
  return s_srgb_lut[i] + (s_srgb_lut[i + 1] - s_srgb_lut[i]) * (f - (float)i);
}

void effect_oklab_rgb(float L, float a, float b, int *r, int *g, int *bl) {
  float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
  float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
  float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
  l_ = l_ * l_ * l_;
  m_ = m_ * m_ * m_;
  s_ = s_ * s_ * s_;
  const float R = 4.0767416621f * l_ - 3.3077115913f * m_ + 0.2309699292f * s_;
  const float G = -1.2684380046f * l_ + 2.6097574011f * m_ - 0.3413193965f * s_;
  const float B = -0.0041960863f * l_ - 0.7034186147f * m_ + 1.7076147010f * s_;
  *r = effect_clamp_u8((int)(effect_clamp01(R) * 255.0f + 0.5f));
  *g = effect_clamp_u8((int)(effect_clamp01(G) * 255.0f + 0.5f));
  *bl = effect_clamp_u8((int)(effect_clamp01(B) * 255.0f + 0.5f));
}

void effect_lush(float s, float lit, int *r, int *g, int *b) {
  const float f = s - floorf(s);  // fract(), and s is accumulated so it must wrap
  const float h = f * EFFECT_TAU;
  float L = 0.50f + 0.36f * effect_clamp01(lit);
  if (L < 0.12f) L = 0.12f;
  if (L > 0.88f) L = 0.88f;
  // High chroma is the whole point: it is what makes this read as neon from
  // across a field rather than as a pastel smear.
  const float C = (0.125f + s_seed2 * 0.05f) + 0.05f * sinf(s * EFFECT_TAU * 0.5f + 1.3f);
  effect_oklab_rgb(L, C * cosf(h), C * sinf(h), r, g, b);
}

void effect_lush_shaded(float s, float lit, int *r, int *g, int *b) {
  const float l = effect_clamp01(lit);
  effect_lush(s, 0.22f + 0.62f * l, r, g, b);
  // smoothstep, so it is exactly 0 at 0 and the low end is crushed rather than
  // merely dim -- contrast on a 240px disc in a dark field is the whole point.
  const float shade = l * l * (3.0f - 2.0f * l);
  *r = (int)((float)*r * shade);
  *g = (int)((float)*g * shade);
  *b = (int)((float)*b * shade);
}

void effect_chromadepth(float t, int *r, int *g, int *b) {
  t = effect_clamp01(t);
  // paper-cranes' shape, with its two endpoints moved. Its ramp starts at
  // Oklab hue 0, which is not red -- it is pink-magenta (238,96,148 once the
  // transfer function is applied), and a pink near-plane is a weak depth cue.
  // sRGB red sits at Oklch hue 29 degrees, so the ramp starts there and spans
  // 261 degrees to a blue-violet rather than wrapping on round to magenta,
  // which would read as NEAR again at the far end.
  //
  // Chroma is 0.235 rather than the document's stated 0.18 floor. 0.18 is a
  // minimum, and at these lightnesses it left both ends washed out; every step
  // is now at least 0.85 saturated, which is what the same document is asking
  // for when it says the colours must be vivid and the bands distinct.
  //
  // And unlike everything else here, this one is gamma-encoded. Written linear
  // the way the shaders do it, this ramp is wildly uneven in brightness: red
  // lands at 0.84 and the green and cyan bands at 0.27-0.35. On a phone a glow
  // lift hides that. Through ChromaDepth glasses on a 240px disc it means the
  // middle depth planes are not there at all -- a red crest floating on almost
  // nothing, which is exactly how this effect failed. Depth here is carried by
  // hue, so every plane has to be equally visible; that is a functional
  // requirement, not a matter of taste, so the transfer is applied.
  const float hue = 0.081f + t * 0.725f;
  const float chromaBoost = 1.0f + 0.15f * sinf(t * EFFECT_TAU);
  const float L = 0.64f - t * 0.20f;
  const float C = 0.235f * chromaBoost;
  const float h = hue * EFFECT_TAU;
  const float a = C * cosf(h), bb = C * sinf(h);
  float l_ = L + 0.3963377774f * a + 0.2158037573f * bb;
  float m_ = L - 0.1055613458f * a - 0.0638541728f * bb;
  float s_ = L - 0.0894841775f * a - 1.2914855480f * bb;
  l_ = l_ * l_ * l_;
  m_ = m_ * m_ * m_;
  s_ = s_ * s_ * s_;
  *r = effect_clamp_u8(
      (int)(srgb_encode(4.0767416621f * l_ - 3.3077115913f * m_ + 0.2309699292f * s_) * 255.0f + 0.5f));
  *g = effect_clamp_u8(
      (int)(srgb_encode(-1.2684380046f * l_ + 2.6097574011f * m_ - 0.3413193965f * s_) * 255.0f + 0.5f));
  *b = effect_clamp_u8(
      (int)(srgb_encode(-0.0041960863f * l_ - 0.7034186147f * m_ + 1.7076147010f * s_) * 255.0f + 0.5f));
}

void effect_glow_lift(int *r, int *g, int *b) {
  // pow(col, 0.80) * 1.15, exactly as chromadepth-lattice/6.frag ends.
  //
  // Through a table, because the input is an integer 0..255 and there are only
  // 256 possible answers. Calling powf three times per palette entry was 768 of
  // them per frame and cost the badge about two frames a second, for a function
  // that fits in 256 bytes and is exact.
  static uint8_t lut[256];
  static int built = 0;
  if (!built) {
    for (int i = 0; i < 256; ++i) {
      const float v = powf((float)i / 255.0f, 0.80f) * 1.15f;
      lut[i] = (uint8_t)effect_clamp_u8((int)(effect_clamp01(v) * 255.0f + 0.5f));
    }
    built = 1;
  }
  *r = lut[effect_clamp_u8(*r)];
  *g = lut[effect_clamp_u8(*g)];
  *b = lut[effect_clamp_u8(*b)];
}
