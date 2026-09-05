// plasma -- the classic multi-sine field. The "one shader that works" for v1.
//
// Four sine terms sum into a single 0..255 palette index:
//   horizontal, vertical, diagonal, and radial (distance from centre).
// The trick that makes this cheap enough for an ESP32-S3 is that every term is
// separable: three of the four depend on x, y or x+y alone, so they collapse
// into 240/240/480-entry tables rebuilt once per frame. The radial term
// collapses into a 256-entry table indexed by the shared polar radius.
//
// Inner loop per pixel: 4 table loads, 3 adds, 1 shift, 1 palette load,
// 1 store. No multiplies, no branches, no floats.
//
// Audio wiring
//   bass    -> amplitude of the radial term (the field breathes outward)
//   mid     -> speed and amplitude of the x/y terms (the field flows faster)
//   treble  -> amplitude of the diagonal term (fine interference detail)
//   energy  -> global palette gain
//   beat    -> DISCRETE: the palette hue anchor jumps by 37/256 of a turn, so
//              the whole field recolours on the downbeat rather than drifting
//   beat_env-> the designed attack-decay: radial amplitude swells and the
//              palette washes toward white, then decays. This is the part that
//              reads as "on the beat" -- driving hue off raw bass instead
//              makes the field shudder, which is exactly what we are avoiding.
#include "effect_common.h"
#include "knobs.h"

#include <string.h>

static uint16_t s_pal[256];
static int16_t s_colx[EFFECT_W];
static int16_t s_rowy[EFFECT_H];
static int16_t s_diag[EFFECT_W + EFFECT_H];
static int16_t s_radl[256];

static uint32_t s_last_ms;
static uint16_t s_phx, s_phy, s_phd, s_phr;  // Q8.8 phases, wrap for free
static uint8_t s_hue;                        // beat-fired palette anchor

static void plasma_init(void) {
  effect_geom_init();
  s_last_ms = 0;
  s_phx = 0;
  s_phy = 12000;
  s_phd = 40000;
  s_phr = 25000;
  s_hue = 0;
  memset(s_pal, 0, sizeof(s_pal));
}

static void plasma_render(uint16_t *out, const EffectInput *in) {
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

  // ---- knobs --------------------------------------------------------------
  // Slot meanings are the shared ones (see knobs.h): 1 how far the music moves
  // it, 2 how big the pattern is, 3 how fast it runs with no music at all, 4
  // hue, 5 brightness.
  const float react = knob(0) * 2.0f;
  const float scalek = 0.35f + 1.6f * knob(1);
  const float speedk = knob(2) * 2.0f;
  const float glowk = 0.35f + 1.3f * knob(4);

  // ---- phase advance (Q8.8 angle units per millisecond) -------------------
  const float flow = speedk + (2.2f * mid + 1.4f * env) * react;
  s_phx += (uint16_t)((float)dt * 5.5f * flow);
  s_phy += (uint16_t)((float)dt * -4.1f * flow);
  s_phd += (uint16_t)((float)dt * 3.0f * (speedk + 2.0f * treble * react));
  s_phr += (uint16_t)((float)dt * (2.5f * speedk + 9.0f * energy * react));

  // ---- discrete beat event: recolour, do not drift ------------------------
  if (in->beat) s_hue = (uint8_t)(s_hue + (uint8_t)(37.0f * react));
  s_hue = (uint8_t)(s_hue + (uint8_t)((float)dt * 0.02f * (0.2f * speedk + mid * react)));

  // ---- amplitude budget ---------------------------------------------------
  // The four terms always sum to +/-508 so the >>2 below lands exactly in
  // 0..255 with no clamp in the inner loop; only the *split* between terms
  // moves with the audio.
  int wr = 42 + (int)((85.0f * bass + 110.0f * env) * react);
  int wx = 105 + (int)(45.0f * mid * react);
  int wy = 105 + (int)(45.0f * mid * react);
  int wd = 78 + (int)(80.0f * treble * react);
  const int wtot = wr + wx + wy + wd;
  const int ar = wr * 508 / wtot;
  const int ax = wx * 508 / wtot;
  const int ay = wy * 508 / wtot;
  const int ad = 508 - ar - ax - ay;

  // ---- per-frame tables (240 + 240 + 480 + 256 + 256 entries) -------------
  const uint8_t phx = (uint8_t)(s_phx >> 8);
  const uint8_t phy = (uint8_t)(s_phy >> 8);
  const uint8_t phd = (uint8_t)(s_phd >> 8);
  const uint8_t phr = (uint8_t)(s_phr >> 8);

  // Frequencies are mutually irrational-ish on purpose: 2.7 / 2.1 / 1.9 / 2.0
  // cycles across the disc. Sharing a common divisor makes the four terms lock
  // into a static moire instead of drifting past each other.
  // Scaled together so the four terms keep their ratio and stay mutually
  // irrational-ish; sharing a divisor is what locks them into a static moire.
  const int fx = (int)(727.0f * scalek), fy = (int)(563.0f * scalek);
  const int fd = (int)(259.0f * scalek), fr = (int)(497.0f * scalek);
  for (int x = 0; x < EFFECT_W; ++x)
    s_colx[x] = (int16_t)((effect_sin8[(uint8_t)(((x * fx) >> 8) + phx)] * ax) / 127);
  for (int y = 0; y < EFFECT_H; ++y)
    s_rowy[y] = (int16_t)((effect_sin8[(uint8_t)(((y * fy) >> 8) + phy)] * ay) / 127);
  for (int d = 0; d < EFFECT_W + EFFECT_H; ++d)
    s_diag[d] = (int16_t)((effect_sin8[(uint8_t)(((d * fd) >> 8) + phd)] * ad) / 127);
  for (int r = 0; r < 256; ++r)
    s_radl[r] = (int16_t)((effect_sin8[(uint8_t)(((r * fr) >> 8) + phr)] * ar) / 127);

  // ---- palette: integer cosine ramp, no sinf, ~1500 cycles ---------------
  const int gain = effect_clamp_u8(
      (int)((110.0f + 145.0f * (0.30f + (0.45f * energy + 0.45f * env) * react)) * glowk));
  // Squared, so this is a punch rather than a glow -- and capped short of a
  // full whiteout so the field structure stays readable through the flash.
  const int wash = (int)(82.0f * env * env * react);
  for (int i = 0; i < 256; ++i) {
    const uint8_t p = (uint8_t)(i + s_hue + (uint8_t)(knob(3) * 255.0f));
    int r = (effect_sinu(p) * gain) >> 8;
    int g = (effect_sinu((uint8_t)(p + 85)) * gain) >> 8;
    int b = (effect_sinu((uint8_t)(p + 170)) * gain) >> 8;
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(r + wash),
                             (uint8_t)effect_clamp_u8(g + wash),
                             (uint8_t)effect_clamp_u8(b + wash));
  }

  // ---- pixels -------------------------------------------------------------
  const uint16_t *polar = effect_polar;
  for (int y = 0; y < EFFECT_H; ++y) {
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const uint16_t *p = polar + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];
    const int ry = s_rowy[y];
    const int16_t *dg = s_diag + y;  // s_diag[x + y] becomes dg[x]

    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x0; x <= x1; ++x) {
      const int v = s_colx[x] + ry + dg[x] + s_radl[(uint8_t)p[x]];
      o[x] = s_pal[(v + 512) >> 2];
    }
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
  }
}

const Effect effect_plasma = {"plasma", plasma_init, plasma_render};
