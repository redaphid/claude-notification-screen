// mon -- one Japanese family crest (kamon) per badge, glowing to the music.
//
// The crest is a 128x128 signed-distance field baked by tools/bake-mon.py from
// the same silhouettes the 3D-printed beads and the Paper Cranes wall shader
// use, so print, wall and badge share one shape. Every pixel samples the field
// bilinearly (fixed point) and looks the result up in a 256-entry palette that
// is rebuilt once per frame, so fill, rim, outer glow and background are all a
// function of distance-to-edge and animating the look is a matter of rewriting
// 256 entries, not 57,600 pixels.
//
// Inner loop per pixel: 2 shifts, 1 range check, 4 byte loads, 3 lerps,
// 1 palette load, 1 store, 2 adds to step the rotated sample position.
// No multiplies by per-pixel values except the three 8-bit lerps, no floats.
//
// Audio wiring
//   bass     -> the crest breathes: scale swells with bass
//   mid      -> rotation speed
//   treble   -> width and brightness of the rim band
//   energy   -> glow reach and fill brightness
//   beat     -> DISCRETE: an angular kick; the crest lurches, then settles
//   beat_env -> the designed attack-decay: rim washes toward white and the
//               outer glow flares, then decays
//
// Which crest a badge wears is chosen by the caller through mon_select(); the
// badge firmware keys it off the MAC so every badge in the bag is a different
// bead. Left unselected, the effect cycles through all of them every 6 s,
// which is what the desktop harness shows.
#include "effect_common.h"
#include "effects.h"
#include "mon_data.h"

#include <math.h>
#include <string.h>

static int s_variant = -1;
static uint16_t s_pal[256];
static uint32_t s_last_ms;
static float s_theta;   // radians
static float s_kick;    // rad/s impulse from the last onset, decays

// One hue (0..255 around the wheel) per crest, in mon_names order, so the same
// bead is always the same colour wherever it is in the bag.
static const uint8_t MON_HUE[MON_COUNT] = {
    35,   // kiku      gold
    128,  // tomoe     cyan
    190,  // kikyo     violet
    222,  // ume       pink
    90,   // hakkaku   green
    18,   // mokko     orange
    115,  // kikko     teal
    160,  // suhama    blue
    28,   // matsukawa amber
    70,   // katabami  lime
    0,    // ogi       red
};

void mon_select(int variant) { s_variant = variant; }
int mon_selected(void) { return s_variant; }
uint8_t mon_hue(int variant) {
  return (variant >= 0 && variant < MON_COUNT) ? MON_HUE[variant] : MON_HUE[0];
}
int mon_variant_count(void) { return MON_COUNT; }
const char *mon_variant_name(int variant) {
  return (variant >= 0 && variant < MON_COUNT) ? mon_names[variant] : "auto";
}

// Hue wheel at full saturation, then softened `soften` percent toward white
// (the coloured palette uses 15 so primaries do not clip into a flat slab; the
// ChromaDepth palette uses 0 because the glasses need pure wavelengths).
static void hue_rgb(uint8_t h, int soften, int *r, int *g, int *b) {
  const int region = h / 43;
  int rem = (h - region * 43) * 6;
  if (rem > 255) rem = 255;
  const int q = 255 - rem, t = rem;
  switch (region) {
    case 0: *r = 255; *g = t;   *b = 0;   break;
    case 1: *r = q;   *g = 255; *b = 0;   break;
    case 2: *r = 0;   *g = 255; *b = t;   break;
    case 3: *r = 0;   *g = q;   *b = 255; break;
    case 4: *r = t;   *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;  *b = q;   break;
  }
  *r += (255 - *r) * soften / 100;
  *g += (255 - *g) * soften / 100;
  *b += (255 - *b) * soften / 100;
}

static void build_palette(uint8_t hue, float energy, float treble, float env) {
  int br, bg, bb;
  hue_rgb(hue, 15, &br, &bg, &bb);
  const float fill = 0.26f + 0.40f * energy;              // deep-inside brightness
  const float reach = 2.5f + 9.0f * energy + 6.0f * env;  // outer glow e-fold, motif px
  const float rimw = 1.2f + 2.2f * treble;                // rim band width, motif px
  for (int i = 0; i < 256; ++i) {
    const float d = (float)(i - 128) / (float)MON_SDF_SCALE;  // motif px, + outside
    float k, wash;
    if (d < 0.0f) {
      const float lit = expf(d / 5.0f);                       // 1 at the edge, 0 deep inside
      k = fill + (0.80f - fill) * lit;
      wash = 0.55f * env * lit;
    } else {
      const float glow = expf(-d / reach) * (0.50f + 0.50f * env);
      const float rim = d < rimw ? (1.0f - d / rimw) : 0.0f;
      k = 0.05f + 0.85f * glow + 0.30f * rim;
      wash = rim * (0.20f + 0.55f * env);
    }
    const int w = (int)(255.0f * wash);
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8((int)(br * k) + w),
                             (uint8_t)effect_clamp_u8((int)(bg * k) + w),
                             (uint8_t)effect_clamp_u8((int)(bb * k) + w));
  }
}

// ChromaDepth palette. Those glasses are a prism: red lands nearest the eye,
// then orange, yellow, green, cyan, blue, and violet farthest. So colour IS
// depth, and the distance field becomes a height map: the crest is a raised
// dome (red in the middle, orange toward its edge), the rim is a yellow-green
// ridge, and the surround falls away through cyan and blue into a dim violet
// background. The beat pushes the whole map toward red, so the crest jumps at
// the viewer on the downbeat and settles back. No white anywhere: white is
// every wavelength at once and the glasses smear it.
static int s_chroma = 0;

static void build_palette_chroma(float energy, float treble, float env) {
  const float reach = 6.0f + 10.0f * energy + 4.0f * env;  // px over which the surround recedes
  const float rimw = 1.0f + 2.0f * treble;
  const float push = 0.22f * env;                          // nearer on the beat
  for (int i = 0; i < 256; ++i) {
    const float d = (float)(i - 128) / (float)MON_SDF_SCALE;
    float z, v;  // z: 0 nearest .. 1 farthest; v: brightness
    if (d < 0.0f) {
      const float t = 1.0f - expf(d / 8.0f);  // 0 at the edge, 1 deep inside
      z = 0.30f - 0.30f * t;                  // orange at the edge, red in the middle
      v = 0.78f + 0.22f * t;
    } else if (d < rimw) {
      z = 0.38f;                              // yellow-green ridge
      v = 1.0f;
    } else {
      float t = (d - rimw) / reach;
      if (t > 1.0f) t = 1.0f;
      z = 0.45f + 0.55f * t;                  // green .. violet
      v = 0.85f * expf(-(d - rimw) / (reach * 0.9f)) + 0.07f;
    }
    z -= push;
    if (z < 0.0f) z = 0.0f;
    int r, g, b;
    hue_rgb((uint8_t)(z * 192.0f), 0, &r, &g, &b);  // 0 red .. 192 violet on the wheel
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8((int)(r * v)),
                             (uint8_t)effect_clamp_u8((int)(g * v)),
                             (uint8_t)effect_clamp_u8((int)(b * v)));
  }
}

static void mon_init(void) {
  effect_geom_init();
  s_last_ms = 0;
  s_theta = 0.0f;
  s_kick = 0.0f;
  memset(s_pal, 0, sizeof(s_pal));
}

static void mon_render(uint16_t *out, const EffectInput *in) {
  const uint32_t dt = effect_dt_ms(&s_last_ms, in->time_ms);

  int v = s_variant;
  if (v < 0) v = (int)((in->time_ms / 6000u) % (uint32_t)MON_COUNT);
  if (v >= MON_COUNT) v = v % MON_COUNT;

  const float energy = effect_clamp01(in->energy);
  const float bass = effect_clamp01(in->bass);
  const float mid = effect_clamp01(in->mid);
  const float treble = effect_clamp01(in->treble);
  const float env = effect_clamp01(in->beat_env);

  // ---- motion: slow turn, faster with mid, a discrete kick on the onset ----
  if (in->beat) s_kick += 2.4f;
  s_kick *= expf(-(float)dt / 260.0f);
  s_theta += (0.12f + 0.60f * mid + s_kick) * (float)dt * 0.001f;
  if (s_theta > 6.2831853f) s_theta -= 6.2831853f;

  // ---- size: the crest fills screen_r pixels, breathing with bass ---------
  const float screen_r = 86.0f + 12.0f * bass + 10.0f * env;
  const float k = (float)mon_radius[v] / screen_r;  // motif px per screen px
  const int32_t cs = (int32_t)lrintf(cosf(s_theta) * k * 4096.0f);
  const int32_t sn = (int32_t)lrintf(sinf(s_theta) * k * 4096.0f);

  if (s_chroma) build_palette_chroma(energy, treble, env);
  else build_palette(MON_HUE[v], energy, treble, env);

  // ---- pixels -------------------------------------------------------------
  const uint8_t *sdf = mon_sdf[v];
  const uint16_t far = s_pal[255];
  const int32_t half = (MON_N / 2) << 12;  // motif centre, Q12
  for (int y = 0; y < EFFECT_H; ++y) {
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];
    const int32_t dy = y - EFFECT_H / 2;
    int32_t u = (x0 - EFFECT_W / 2) * cs - dy * sn + half;  // Q12 motif px
    int32_t w = (x0 - EFFECT_W / 2) * sn + dy * cs + half;

    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x0; x <= x1; ++x) {
      const int32_t ui = u >> 12;
      const int32_t wi = w >> 12;
      if ((uint32_t)ui >= (uint32_t)(MON_N - 1) || (uint32_t)wi >= (uint32_t)(MON_N - 1)) {
        // Past the field's edge: take the nearest border sample and add the
        // distance to the border, so the surround keeps fading instead of
        // snapping to the far colour along a visible square.
        const int cu = ui < 0 ? 0 : (ui > MON_N - 1 ? MON_N - 1 : ui);
        const int cw = wi < 0 ? 0 : (wi > MON_N - 1 ? MON_N - 1 : wi);
        int eu = ui - cu, ew = wi - cw;
        if (eu < 0) eu = -eu;
        if (ew < 0) ew = -ew;
        int s = sdf[cw * MON_N + cu] + (eu > ew ? eu : ew) * MON_SDF_SCALE;
        o[x] = s > 255 ? far : s_pal[s];
      } else {
        const int fu = (u >> 4) & 255;
        const int fw = (w >> 4) & 255;
        const uint8_t *p = sdf + wi * MON_N + ui;
        const int a = p[0], b = p[1], c = p[MON_N], d = p[MON_N + 1];
        const int top = a + (((b - a) * fu) >> 8);
        const int bot = c + (((d - c) * fu) >> 8);
        const int s = top + (((bot - top) * fw) >> 8);
        o[x] = s_pal[s];
      }
      u += cs;
      w += sn;
    }
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
  }
}

static void mon_render_colour(uint16_t *out, const EffectInput *in) {
  s_chroma = 0;
  mon_render(out, in);
}

static void mon_render_chroma(uint16_t *out, const EffectInput *in) {
  s_chroma = 1;
  mon_render(out, in);
}

const Effect effect_mon = {"mon", mon_init, mon_render_colour};
// Same crest, same motion, ChromaDepth palette: put the glasses on.
const Effect effect_chroma = {"chroma", mon_init, mon_render_chroma};
