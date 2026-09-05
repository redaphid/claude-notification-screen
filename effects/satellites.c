// satellites -- this badge's crest at the centre, the other beads of the bag
// orbiting it on a string.
//
// Paper Cranes' hero shader gave up on the infinite tiling because a tiling
// has no centre and so can never have a hero: it draws a fixed handful of
// discrete instances instead, one hero plus satellites on an orbit, each
// satellite with its own driver, spin and colour so they read as individuals.
// This is that picture on the badge, with one change that only makes sense
// here: the satellites are the OTHER crests of the swarm, each in its own
// bead colour, so a badge shows its crest surrounded by the crests of the
// badges around it, threaded on a kandi string.
//
// The trick that makes it cheap: the hero is the mon effect's full-disc
// bilinear SDF pass (one sample per pixel), and each satellite is a second
// pass over only its own bounding box, ~3k pixels, nearest-sampled because it
// is minified, composited with a per-channel max so its glow lightens
// whatever is under it without an alpha blend. The orbit string is one
// compare per pixel on the shared polar radius. Five satellites cost about a
// fifth of the hero.
//
// Audio wiring
//   mid      -> orbit speed and hero spin speed (monotonic accumulators: the
//               music sets the rate, never an angle, so nothing snaps back)
//   bass     -> with beat_env, the hero's breath (scale)
//   treble   -> rim width and brightness, hero and satellites alike
//   energy   -> glow reach, fill brightness, how lit the string is
//   beat     -> DISCRETE: the orbit lurches forward and settles, the hero
//               gets a smaller kick, and the "hot" satellite advances one
//               place, so the beat visibly walks around the ring
//   beat_env -> the designed attack-decay: the hot satellite washes toward
//               white and swells, the hero rim washes, the glow flares
//   each satellite's size also rides its own slow driver (bass, mid,
//   treble, energy, beat_env in turn), always mixed 50/50 with beat_env
//   so no bead is scaled off raw bass alone
#include "effect_common.h"
#include "effects.h"
#include "mon_data.h"

#include <math.h>
#include <string.h>

#define SAT_PI 3.14159265358979f
#define SAT_COUNT 5
#define SAT_ORBIT 89.0f       // orbit radius, px
#define SAT_GLOW_PX 9         // how far a satellite's glow reaches, px
#define SAT_PAL_N (128 + SAT_GLOW_PX * MON_SDF_SCALE)  // palette entries a satellite needs
#define SAT_OPAQUE (128 + 6)  // inside + 1.5 px of rim: written opaque, beyond: lightened

static uint16_t s_hero_pal[256];
static uint16_t s_sat_pal[SAT_COUNT][SAT_PAL_N];
static uint16_t s_ring[16];      // the string, by polar-radius byte offset
static uint32_t s_last_ms;
static float s_theta;            // hero spin, rad
static float s_orbit;            // satellite orbit angle, rad
static float s_spin;             // satellite own-spin phase, rad
static float s_kick;             // rad/s impulses from the last onset, decay
static float s_okick;
static int s_hot;                // which satellite the beat is on

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

// Fill / rim / glow as a function of distance, split into a brightness k
// (Q8) and a white wash (0..255) so one shape serves several hues.
static void shape_table(int n, uint8_t *k, uint8_t *wash, float energy, float treble, float env,
                        float reach) {
  const float fill = 0.26f + 0.40f * energy;
  const float rimw = 1.2f + 2.2f * treble;
  for (int i = 0; i < n; ++i) {
    const float d = (float)(i - 128) / (float)MON_SDF_SCALE;
    float kk, ww;
    if (d < 0.0f) {
      const float lit = expf(d / 5.0f);
      kk = fill + (0.80f - fill) * lit;
      ww = 0.55f * env * lit;
    } else {
      const float glow = expf(-d / reach) * (0.55f + 0.35f * env);
      const float rim = d < rimw ? (1.0f - d / rimw) : 0.0f;
      kk = 0.05f + 0.85f * glow + 0.30f * rim;
      ww = rim * (0.20f + 0.55f * env);
    }
    k[i] = (uint8_t)effect_clamp_u8((int)(kk * 255.0f));
    wash[i] = (uint8_t)effect_clamp_u8((int)(ww * 255.0f));
  }
}

static void tint(uint16_t *pal, int n, const uint8_t *k, const uint8_t *wash, uint8_t hue) {
  int br, bg, bb;
  hue_rgb(hue, 15, &br, &bg, &bb);
  for (int i = 0; i < n; ++i) {
    const int w = wash[i];
    pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(((br * k[i]) >> 8) + w),
                           (uint8_t)effect_clamp_u8(((bg * k[i]) >> 8) + w),
                           (uint8_t)effect_clamp_u8(((bb * k[i]) >> 8) + w));
  }
}

static inline int sdf_sample(const uint8_t *sdf, int32_t u, int32_t w) {
  const int32_t ui = u >> 12;
  const int32_t wi = w >> 12;
  if ((uint32_t)ui >= (uint32_t)(MON_N - 1) || (uint32_t)wi >= (uint32_t)(MON_N - 1)) {
    const int cu = ui < 0 ? 0 : (ui > MON_N - 1 ? MON_N - 1 : ui);
    const int cw = wi < 0 ? 0 : (wi > MON_N - 1 ? MON_N - 1 : wi);
    int eu = ui - cu, ew = wi - cw;
    if (eu < 0) eu = -eu;
    if (ew < 0) ew = -ew;
    const int s = sdf[cw * MON_N + cu] + (eu > ew ? eu : ew) * MON_SDF_SCALE;
    return s > 255 ? 255 : s;
  }
  const int fu = (u >> 4) & 255;
  const int fw = (w >> 4) & 255;
  const uint8_t *p = sdf + wi * MON_N + ui;
  const int a = p[0], b = p[1], c = p[MON_N], d = p[MON_N + 1];
  const int top = a + (((b - a) * fu) >> 8);
  const int bot = c + (((d - c) * fu) >> 8);
  return top + (((bot - top) * fw) >> 8);
}

// Nearest-neighbour sample for the satellites: at 17..27 px they minify the
// 128 px field 2x or more, so the half-texel error is well under a screen
// pixel and the three lerps would buy nothing. Rounded, not truncated.
static inline int sdf_nearest(const uint8_t *sdf, int32_t u, int32_t w) {
  const int32_t ui = (u + 2048) >> 12;
  const int32_t wi = (w + 2048) >> 12;
  if ((uint32_t)ui >= (uint32_t)MON_N || (uint32_t)wi >= (uint32_t)MON_N) {
    const int cu = ui < 0 ? 0 : (ui > MON_N - 1 ? MON_N - 1 : ui);
    const int cw = wi < 0 ? 0 : (wi > MON_N - 1 ? MON_N - 1 : wi);
    int eu = ui - cu, ew = wi - cw;
    if (eu < 0) eu = -eu;
    if (ew < 0) ew = -ew;
    const int s = sdf[cw * MON_N + cu] + (eu > ew ? eu : ew) * MON_SDF_SCALE;
    return s > 255 ? 255 : s;
  }
  return sdf[wi * MON_N + ui];
}

// Per-channel max of two byte-swapped RGB565 pixels: "lighten".
static inline uint16_t lighten(uint16_t a, uint16_t b) {
  a = (uint16_t)((a >> 8) | (a << 8));
  b = (uint16_t)((b >> 8) | (b << 8));
  const uint16_t ar = a & 0xF800u, br = b & 0xF800u;
  const uint16_t ag = a & 0x07E0u, bg = b & 0x07E0u;
  const uint16_t ab = a & 0x001Fu, bb = b & 0x001Fu;
  const uint16_t c = (uint16_t)((ar > br ? ar : br) | (ag > bg ? ag : bg) | (ab > bb ? ab : bb));
  return (uint16_t)((c >> 8) | (c << 8));
}

static void satellites_init(void) {
  effect_geom_init();
  s_last_ms = 0;
  s_theta = 0.0f;
  s_orbit = 0.0f;
  s_spin = 0.0f;
  s_kick = 0.0f;
  s_okick = 0.0f;
  s_hot = 0;
  memset(s_hero_pal, 0, sizeof(s_hero_pal));
  memset(s_sat_pal, 0, sizeof(s_sat_pal));
  memset(s_ring, 0, sizeof(s_ring));
}

static void satellites_render(uint16_t *out, const EffectInput *in) {
  if (effect_polar == NULL) {
    memset(out, 0, (size_t)EFFECT_PIXELS * sizeof(uint16_t));
    return;
  }
  const uint32_t dt = effect_dt_ms(&s_last_ms, in->time_ms);

  int v = mon_selected();
  if (v < 0) v = (int)((in->time_ms / 7000u) % (uint32_t)MON_COUNT);
  if (v >= MON_COUNT) v = v % MON_COUNT;

  const float energy = effect_clamp01(in->energy);
  const float bass = effect_clamp01(in->bass);
  const float mid = effect_clamp01(in->mid);
  const float treble = effect_clamp01(in->treble);
  const float env = effect_clamp01(in->beat_env);
  const float dts = (float)dt * 0.001f;

  // ---- motion ---------------------------------------------------------------
  if (in->beat) {
    s_okick += 1.6f;
    s_kick += 0.9f;
    s_hot = (s_hot + 1) % SAT_COUNT;
  }
  s_okick *= expf(-(float)dt / 300.0f);
  s_kick *= expf(-(float)dt / 260.0f);
  s_orbit += (0.22f + 0.80f * mid + s_okick) * dts;
  s_theta += (0.10f + 0.45f * mid + s_kick) * dts;
  s_spin += 0.9f * dts;
  if (s_orbit > 2.0f * SAT_PI) s_orbit -= 2.0f * SAT_PI;
  if (s_theta > 2.0f * SAT_PI) s_theta -= 2.0f * SAT_PI;
  if (s_spin > 2.0f * SAT_PI) s_spin -= 2.0f * SAT_PI;

  // ---- palettes -------------------------------------------------------------
  static uint8_t k[256], wash[256];
  shape_table(256, k, wash, energy, treble, env, 2.5f + 8.0f * energy + 3.0f * env);
  tint(s_hero_pal, 256, k, wash, mon_hue(v));
  // satellites: a tighter glow, and the hot one washes with the beat
  shape_table(SAT_PAL_N, k, wash, energy, treble, 0.25f * env, 2.0f + 4.0f * energy);
  int crest[SAT_COUNT];
  for (int i = 0; i < SAT_COUNT; ++i) {
    crest[i] = (v + 1 + 2 * i) % MON_COUNT;  // five distinct other beads
    if (i != s_hot) tint(s_sat_pal[i], SAT_PAL_N, k, wash, mon_hue(crest[i]));
  }
  {
    static uint8_t kh[SAT_PAL_N], wh[SAT_PAL_N];
    shape_table(SAT_PAL_N, kh, wh, energy, treble, env, 2.0f + 4.0f * energy + 6.0f * env);
    tint(s_sat_pal[s_hot], SAT_PAL_N, kh, wh, mon_hue(crest[s_hot]));
  }
  // the string: a thin ring at the orbit radius, in the hero's colour
  int rr, rg, rb;
  hue_rgb(mon_hue(v), 40, &rr, &rg, &rb);
  const int ring_r0 = (int)lrintf((SAT_ORBIT - 3.5f) * 255.0f / (float)EFFECT_RADIUS);
  for (int i = 0; i < 16; ++i) {
    const float d = ((float)i + 0.5f) * (float)EFFECT_RADIUS / 255.0f - 3.5f;  // px from the orbit
    const float kk = expf(-d * d / 1.6f) * (0.14f + 0.30f * energy + 0.20f * env);
    s_ring[i] = effect_rgb565((uint8_t)(rr * kk), (uint8_t)(rg * kk), (uint8_t)(rb * kk));
  }
  const int ring_w = (int)(7.0f * 255.0f / (float)EFFECT_RADIUS);  // 7 px band, in radius bytes

  // ---- hero pass: whole disc, with the string ------------------------------
  {
    const float screen_r = 54.0f + 6.0f * bass + 8.0f * env;
    const float kk = (float)mon_radius[v] / screen_r;
    const int32_t cs = (int32_t)lrintf(cosf(s_theta) * kk * 4096.0f);
    const int32_t sn = (int32_t)lrintf(sinf(s_theta) * kk * 4096.0f);
    const uint8_t *sdf = mon_sdf[v];
    const int32_t half = (MON_N / 2) << 12;
    const uint16_t *polar = effect_polar;
    for (int y = 0; y < EFFECT_H; ++y) {
      uint16_t *o = out + (size_t)y * EFFECT_W;
      const uint16_t *p = polar + (size_t)y * EFFECT_W;
      const int x0 = effect_row_x0[y];
      const int x1 = effect_row_x1[y];
      const int32_t dy = y - EFFECT_H / 2;
      int32_t u = (x0 - EFFECT_W / 2) * cs - dy * sn + half;
      int32_t w = (x0 - EFFECT_W / 2) * sn + dy * cs + half;
      for (int x = 0; x < x0; ++x) o[x] = 0;
      for (int x = x0; x <= x1; ++x) {
        uint16_t c = s_hero_pal[sdf_sample(sdf, u, w)];
        const int rb8 = (p[x] & 255) - ring_r0;
        if ((unsigned)rb8 < (unsigned)ring_w) c = lighten(c, s_ring[rb8]);
        o[x] = c;
        u += cs;
        w += sn;
      }
      for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
    }
  }

  // ---- satellite passes: each over its own box only ------------------------
  for (int i = 0; i < SAT_COUNT; ++i) {
    const float ang = s_orbit + (float)i * (2.0f * SAT_PI / (float)SAT_COUNT);
    const float cxf = 119.5f + SAT_ORBIT * cosf(ang);
    const float cyf = 119.5f + SAT_ORBIT * sinf(ang);
    float drive;
    switch (i) {
      case 0: drive = bass; break;
      case 1: drive = mid; break;
      case 2: drive = treble; break;
      case 3: drive = energy; break;
      default: drive = env; break;
    }
    float rs = 17.0f + 4.0f * (0.5f * drive + 0.5f * env);
    if (i == s_hot) rs += 6.0f * env;  // the bead the beat is on swells
    const float kk = (float)mon_radius[crest[i]] / rs;
    const float spin = s_spin * (0.6f + 0.3f * (float)i) * ((i & 1) ? -1.0f : 1.0f);
    const float csf = cosf(spin) * kk, snf = sinf(spin) * kk;
    const int32_t cs = (int32_t)lrintf(csf * 4096.0f);
    const int32_t sn = (int32_t)lrintf(snf * 4096.0f);
    const uint8_t *sdf = mon_sdf[crest[i]];
    const uint16_t *pal = s_sat_pal[i];
    const float ext = rs * 1.02f + (float)SAT_GLOW_PX + 1.0f;  // radius + glow, a hair of slack
    int ya = (int)floorf(cyf - ext), yb = (int)ceilf(cyf + ext);
    if (ya < 0) ya = 0;
    if (yb > EFFECT_H - 1) yb = EFFECT_H - 1;
    for (int y = ya; y <= yb; ++y) {
      // the disc of radius ext, not its box: the corners are a fifth of it
      const float dyr = (float)y - cyf;
      const float hw = sqrtf(ext * ext - dyr * dyr);
      int xa = (int)floorf(cxf - hw), xb = (int)ceilf(cxf + hw);
      if (xa < effect_row_x0[y]) xa = effect_row_x0[y];
      if (xb > effect_row_x1[y]) xb = effect_row_x1[y];
      if (xb < xa) continue;
      uint16_t *o = out + (size_t)y * EFFECT_W;
      const float dx = (float)xa - cxf, dyf = (float)y - cyf;
      int32_t u = (int32_t)lrintf((dx * csf - dyf * snf + 64.0f) * 4096.0f);
      int32_t w = (int32_t)lrintf((dx * snf + dyf * csf + 64.0f) * 4096.0f);
      for (int x = xa; x <= xb; ++x) {
        const int s = sdf_nearest(sdf, u, w);
        if (s < SAT_OPAQUE) o[x] = pal[s];
        else if (s < SAT_PAL_N) o[x] = lighten(o[x], pal[s]);
        u += cs;
        w += sn;
      }
    }
  }
}

const Effect effect_satellites = {"satellites", satellites_init, satellites_render};
