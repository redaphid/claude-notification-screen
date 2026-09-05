// iris -- a Paper Cranes flavoured concentric aperture.
//
// Three separable layers multiplied and added, all reduced to 256-entry tables
// rebuilt once per frame:
//
//   radial  s_rlut[r] = (twist << 8) | brightness
//             - a dilating pupil (dark core) whose radius breathes on bass
//             - concentric rings drifting outward
//             - a rim vignette so the round bezel reads as intentional
//             - shockwave rings, spawned by beat, expanding on their own clock
//           the high byte is a per-radius angular offset, which is what turns
//           the straight petals into a spiral. One load gives both.
//
//   angular s_alut[a] = petals; blade count is a discrete, beat-fired state
//
//   sparkle s_spark[noise] = a treble-gated threshold over a static 64x64
//           hash field that pans a little each frame. Below the threshold it
//           contributes nothing, so sparkle *appears* rather than fades in.
//
// Inner loop per pixel: 6 table loads, 1 multiply, ~5 ALU ops, 1 store.
//
// Why this shape: the whole design is "designed envelopes fired by detected
// events". The pupil follows bass because bass is genuinely slow and smooth;
// everything sharp -- the bloom, the shockwave, the blade count, the hue --
// is driven off beat/beat_env, which have a deliberate attack and decay.
// Driving the sharp things off raw treble instead makes the iris shudder.
#include "effect_common.h"
#include "knobs.h"

#include <string.h>

#define IRIS_SHOCKS 3

static uint8_t s_noise[64 * 64];
static uint16_t s_rlut[256];
static uint8_t s_alut[256];
static uint8_t s_spark[256];
static uint16_t s_pal[512];

static uint32_t s_last_ms;
static uint16_t s_ringphase;
static uint16_t s_swirl;
static uint8_t s_hue;
static uint8_t s_blades;
static uint8_t s_beatcount;
static uint8_t s_noise_x, s_noise_y;

static float s_shock_r[IRIS_SHOCKS];
static float s_shock_life[IRIS_SHOCKS];
static int s_shock_next;

static void iris_init(void) {
  effect_geom_init();

  // Static hash field. Deterministic so the badge and the harness sparkle
  // identically, which matters when comparing a GIF against a photo.
  uint32_t h = 0x9E3779B9u;
  for (int i = 0; i < 64 * 64; ++i) {
    h ^= h << 13;
    h ^= h >> 17;
    h ^= h << 5;
    s_noise[i] = (uint8_t)(h >> 24);
  }

  s_last_ms = 0;
  s_ringphase = 0;
  s_swirl = 0;
  s_hue = 150;
  s_blades = 6;
  s_beatcount = 0;
  s_noise_x = 0;
  s_noise_y = 0;
  s_shock_next = 0;
  for (int i = 0; i < IRIS_SHOCKS; ++i) {
    s_shock_r[i] = 0.0f;
    s_shock_life[i] = 0.0f;
  }
  memset(s_pal, 0, sizeof(s_pal));
  memset(s_rlut, 0, sizeof(s_rlut));
  memset(s_alut, 0, sizeof(s_alut));
  memset(s_spark, 0, sizeof(s_spark));
}

static void iris_render(uint16_t *out, const EffectInput *in) {
  if (effect_polar == NULL) {
    memset(out, 0, (size_t)EFFECT_PIXELS * sizeof(uint16_t));
    return;
  }

  const uint32_t dt = effect_dt_ms(&s_last_ms, in->time_ms);

  const float energy = effect_clamp01(in->energy);
  const float bass = effect_clamp01(in->bass);
  const float mid = effect_clamp01(in->mid);
  const float treble = effect_clamp01(in->treble);
  const float env = effect_clamp01(in->beat_env);

  // Shared knob meanings, see knobs.h.
  const float react = knob(0) * 2.0f;
  const float speedk = knob(2) * 2.0f;

  s_ringphase += (uint16_t)((float)dt * (3.0f * speedk + 22.0f * energy * react));
  s_swirl += (uint16_t)((float)dt * (2.0f * speedk + 26.0f * mid * react));
  s_noise_x = (uint8_t)(s_noise_x + (dt > 20u ? 1u : 0u));
  s_noise_y = (uint8_t)(s_noise_y + (dt > 40u ? 1u : 0u));

  if (in->beat) {
    s_hue = (uint8_t)(s_hue + (uint8_t)(23.0f * react));
    s_beatcount++;
    if ((s_beatcount & 3) == 0) {          // discrete state, every 4th beat
      static const uint8_t blade_cycle[4] = {5, 6, 8, 3};
      s_blades = blade_cycle[(s_beatcount >> 2) & 3];
    }
    s_shock_r[s_shock_next] = 0.0f;
    s_shock_life[s_shock_next] = 1.0f;
    s_shock_next = (s_shock_next + 1) % IRIS_SHOCKS;
  }

  // ---- radial table -------------------------------------------------------
  // The pupil dilates on bass (slow, smooth signal -> slow, smooth motion) and
  // blooms open on beat_env (designed attack-decay -> a snap, then a settle).
  const int pupil = effect_clamp_u8((int)(46.0f + 78.0f * bass + 64.0f * env));
  const int ringamp = (int)(60.0f + 55.0f * mid);
  const uint8_t rph = (uint8_t)(s_ringphase >> 8);
  const int twistk = (int)(8.0f + 46.0f * energy);

  for (int r = 0; r < 256; ++r) {
    // pupil edge: hard-ish inside, feathered over 26 units
    int aperture;
    if (r <= pupil - 13) {
      aperture = 18;                                  // not pure black: a dark eye
    } else if (r >= pupil + 13) {
      aperture = 255;
    } else {
      aperture = 18 + (r - (pupil - 13)) * (255 - 18) / 26;
    }

    int rings = 150 + ((effect_sin8[(uint8_t)(((r * 900) >> 8) - rph)] * ringamp) / 127);

    // rim vignette: last 22 units of radius fall to black
    int vign = 255;
    if (r > 233) vign = (255 - r) * 255 / 22;

    int v = (aperture * rings) >> 8;
    v = (v * vign) >> 8;
    s_rlut[r] = (uint16_t)(((uint8_t)((r * twistk) >> 6) << 8) | (uint8_t)effect_clamp_u8(v));
  }

  // ---- shockwaves: their own clock, fired by beat, added on top -----------
  for (int i = 0; i < IRIS_SHOCKS; ++i) {
    if (s_shock_life[i] <= 0.02f) { s_shock_life[i] = 0.0f; continue; }
    s_shock_r[i] += (float)dt * 0.62f;
    s_shock_life[i] *= (1.0f - (float)dt / 420.0f);
    const int c = (int)s_shock_r[i];
    if (c > 262) { s_shock_life[i] = 0.0f; continue; }
    const int amp = (int)(210.0f * s_shock_life[i]);
    for (int w = -6; w <= 6; ++w) {
      const int r = c + w;
      if (r < 0 || r > 255) continue;
      const int falloff = amp - (w < 0 ? -w : w) * (amp / 7);
      if (falloff <= 0) continue;
      const int base = s_rlut[r] & 255;
      const int lit = effect_clamp_u8(base + falloff);
      s_rlut[r] = (uint16_t)((s_rlut[r] & 0xFF00u) | (uint16_t)lit);
    }
  }

  // ---- angular table ------------------------------------------------------
  const uint8_t sw = (uint8_t)(s_swirl >> 8);
  const int petalamp = (int)(62.0f + 88.0f * treble);
  for (int a = 0; a < 256; ++a) {
    const int p = 168 + ((effect_sin8[(uint8_t)(a * s_blades + sw)] * petalamp) / 127);
    s_alut[a] = (uint8_t)effect_clamp_u8(p);
  }

  // ---- sparkle threshold --------------------------------------------------
  // Sparkle is a garnish, not a texture: at most ~7% of pixels light up even
  // flat out. The threshold shape means treble makes sparkles *appear* rather
  // than fading a haze in and out, which is the whole point.
  const int thresh = effect_clamp_u8((int)(252.0f - 12.0f * treble - 9.0f * env));
  for (int n = 0; n < 256; ++n) {
    s_spark[n] = (n <= thresh) ? 0 : (uint8_t)effect_clamp_u8((n - thresh) * 255 / (256 - thresh));
  }

  // ---- palette ------------------------------------------------------------
  const int gain = effect_clamp_u8((int)(150.0f + 105.0f * (0.35f + 0.40f * energy + 0.45f * env)));
  for (int i = 0; i < 256; ++i) {
    const uint8_t p = (uint8_t)((i >> 2) + s_hue + (uint8_t)(knob(3) * 255.0f));
    int r = (effect_sinu(p) * i) >> 8;
    int g = (effect_sinu((uint8_t)(p + 96)) * i) >> 8;
    int b = (effect_sinu((uint8_t)(p + 176)) * i) >> 8;
    s_pal[i] = effect_rgb565((uint8_t)((r * gain) >> 8), (uint8_t)((g * gain) >> 8),
                             (uint8_t)((b * gain) >> 8));
  }
  for (int i = 256; i < 512; ++i) {  // sparkle bloom half
    const int k = i - 256;
    uint8_t r, g, b;
    effect_unpack565(s_pal[255], &r, &g, &b);
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(r + k), (uint8_t)effect_clamp_u8(g + k),
                             (uint8_t)effect_clamp_u8(b + k));
  }

  // ---- pixels -------------------------------------------------------------
  const uint16_t *polar = effect_polar;
  for (int y = 0; y < EFFECT_H; ++y) {
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const uint16_t *p = polar + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];
    const int nrow = (((y + s_noise_y) & 63) << 6);

    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x0; x <= x1; ++x) {
      const uint16_t ra = p[x];
      const uint16_t rl = s_rlut[(uint8_t)ra];
      const uint8_t a = (uint8_t)((ra >> 8) + (uint8_t)(rl >> 8));
      const int v = ((rl & 255) * s_alut[a]) >> 8;
      o[x] = s_pal[v + s_spark[s_noise[nrow | ((x + s_noise_x) & 63)]]];
    }
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
  }
}

const Effect effect_iris = {"iris", iris_init, iris_render};
