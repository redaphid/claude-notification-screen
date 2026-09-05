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
#include "knobs.h"

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
int mon_variant_count(void) { return MON_COUNT; }
const char *mon_variant_name(int variant) {
  return (variant >= 0 && variant < MON_COUNT) ? mon_names[variant] : "auto";
}

// The crest in colour. paper-cranes' `lush()` from chromadepth-lattice/6.frag:
// a perceptual Oklch palette, high chroma so it reads as neon, bounded away
// from white and black.
//
// Two things changed with it, both taken from that shader rather than invented:
//
// The old palette multiplied one RGB hue by a brightness factor, which is how a
// saturated colour turns to mud as it dims -- in Oklch lightness moves without
// dragging chroma down with it, so a dim crest is still coloured.
//
// And the beat used to add white ("wash"), which is the documented way to make
// a visual pastel: 6.frag's note on its own bloom is "gentle so loud stays
// saturated, not pastel". The accent here is a hue-shifted BRIGHTER sample of
// the same palette -- `lush(s + 0.18, 1.0) * wave * 0.7` in the original -- so
// a hit changes the colour rather than bleaching it.
//
// The background is not black either. 6.frag is explicit about wanting "no
// black voids" so the whole screen emits light and reads from across a room;
// the surround here is a dim, hue-shifted member of the same family.
static void build_palette(uint8_t hue, float energy, float treble, float env) {
  // Per-device hue on top of the crest's own, so two badges beside each other
  // are visibly different objects rather than two copies of one.
  const float s0 = (float)hue / 255.0f + effect_seed_hue() * 0.35f;
  const float fill = 0.30f + 0.42f * energy;                // deep-inside lightness
  const float reach = 2.0f + 5.0f * energy + 3.5f * env;    // outer glow e-fold, motif px
  const float rimw = 1.2f + 2.2f * treble;                  // rim band width, motif px
  for (int i = 0; i < 256; ++i) {
    const float d = (float)(i - 128) / (float)MON_SDF_SCALE;
    float lit, accent;
    if (d < 0.0f) {
      const float edge = expf(d / 5.0f);                    // 1 at the edge, 0 deep inside
      lit = fill + (0.92f - fill) * edge;
      accent = 0.55f * env * edge;
    } else {
      const float glow = expf(-d / reach) * (0.50f + 0.50f * env);
      const float rim = d < rimw ? (1.0f - d / rimw) : 0.0f;
      // No floor. The surround is the dark the crest is a shape against, and a
      // 0.16 floor here is what made the whole disc glow evenly and read as
      // washed out.
      lit = 0.82f * glow + 0.30f * rim;
      accent = rim * (0.20f + 0.55f * env);
    }
    int r, g, b;
    // The crest is a lit SHAPE and the surround is the dark it is a shape
    // against, so the two halves of the field are coloured by different rules.
    // Shading the inside as well turned the crest into a filigree outline --
    // its interior went dark and only the boundary survived, which is a
    // contour map, not a family crest.
    if (d < 0.0f) {
      effect_lush(s0, lit, &r, &g, &b);
    } else {
      effect_lush_shaded(s0, lit, &r, &g, &b);
    }
    if (accent > 0.0f) {
      int ar, ag, ab;
      effect_lush(s0 + 0.18f, 1.0f, &ar, &ag, &ab);
      const float k = accent * 0.7f;
      r += (int)(ar * k);
      g += (int)(ag * k);
      b += (int)(ab * k);
    }
    effect_glow_lift(&r, &g, &b);
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(r), (uint8_t)effect_clamp_u8(g),
                             (uint8_t)effect_clamp_u8(b));
  }
}

// ChromaDepth palette. Those glasses are a prism: red lands nearest the eye,
// then orange, yellow, green, cyan, blue, and violet farthest. So colour IS
// depth, and the distance field becomes a height map: the crest is a raised
// dome, the rim a ridge, and the surround falls away into violet and finally
// into true black at the edge of the disc.
//
// This used to run a six-region RGB hue wheel and it never worked through the
// glasses. paper-cranes has a whole document about why -- scripts/
// fix-chromadepth-shader.md -- and the first thing it says is that the ramp
// MUST be Oklab, that an HSL or RGB wheel is the failure mode, and that chroma
// has to stay at or above 0.18 or the depth cue collapses into grey. That is
// exactly what was happening here.
//
// The other three things that document asks for, which the old ramp also got
// wrong:
//   - STRONG contrast between near and far, in DISTINCT bands rather than one
//     smooth gradient. A continuous ramp blurs the depth cue; the eye needs
//     edges to separate the layers onto different planes.
//   - Black that is truly (0,0,0), not dark grey. Grey has a wavelength and
//     the glasses will happily place it somewhere.
//   - No white anywhere: white is every wavelength at once and it smears.
static int s_chroma = 0;

// Depth bands. Quantising t before the ramp is what turns a gradient into
// layers; the small residual slope inside each band keeps the shape readable
// without letting the band drift into its neighbour.
#define CHROMA_BANDS 7

static void build_palette_chroma(float energy, float treble, float env) {
  const float depth = 0.4f + 1.6f * knob(4);
  const float hue0 = knob(3) * 0.35f;
  const float reach = (6.0f + 10.0f * energy + 4.0f * env) * depth;  // px the surround recedes
  const float rimw = 1.0f + 2.0f * treble;
  const float push = 0.22f * env;  // the whole map comes nearer on the beat
  for (int i = 0; i < 256; ++i) {
    const float d = (float)(i - 128) / (float)MON_SDF_SCALE;
    float t;      // 0 nearest .. 1 farthest
    float fade;   // 1 lit .. 0 gone to black, only used far out
    if (d < 0.0f) {
      // Inside the crest: a shallow dome, held near the red end.
      const float k = 1.0f - expf(d / 8.0f);  // 0 at the edge, 1 deep inside
      t = 0.26f - 0.26f * k;
      fade = 1.0f;
    } else if (d < rimw) {
      t = 0.34f;  // the ridge, a single flat plane so it reads as an edge
      fade = 1.0f;
    } else {
      float k = (d - rimw) / reach;
      if (k > 1.0f) k = 1.0f;
      t = 0.42f + 0.58f * k;
      // The surround is the FAR plane, so it stays lit and violet -- it is a
      // depth cue, not empty space. An earlier version faded it exponentially
      // to black and destroyed the green through blue bands with it, leaving a
      // red crest on almost nothing: the ramp was correct and invisible.
      // Only the last of it goes dark, and only once the depth ramp has been
      // fully travelled.
      fade = k > 0.86f ? (1.0f - (k - 0.86f) / 0.14f) : 1.0f;
      if (fade < 0.0f) fade = 0.0f;
    }
    t = t + hue0 - push;
    t = effect_clamp01(t);
    // Quantise into planes. Without this the ramp is continuous and the eye
    // gets one soft blur instead of a stack of surfaces.
    const float band = floorf(t * (float)CHROMA_BANDS) / (float)(CHROMA_BANDS - 1);
    const float within = t * (float)CHROMA_BANDS - floorf(t * (float)CHROMA_BANDS);
    const float tq = effect_clamp01(band + (within - 0.5f) * 0.10f);
    int r, g, b;
    effect_chromadepth(tq, &r, &g, &b);
    if (fade < 1.0f) {
      r = (int)(r * fade);
      g = (int)(g * fade);
      b = (int)(b * fade);
    }
    s_pal[i] = effect_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
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

  // ---- knobs -------------------------------------------------------------
  // knob 1 is how much the music is allowed to move the crest at all. At 0 the
  // silhouette is geometrically still and only its colour answers the room --
  // which is what the paper-cranes work on this artwork concluded it should
  // do, so that the crest stays nameable. At 1 it is the old swelling
  // behaviour. Nobody has to be right about this in the abstract; it is a knob.
  const float react = knob(0) * 2.0f;   // 0..2, so 128 is roughly as it shipped
  const float sizek = 0.6f + knob(1);   // 0.6..1.6 of the base radius
  const float spink = knob(2) * 2.0f;
  const float kickk = knob(5) * 2.0f;

  // ---- motion: slow turn, faster with mid, a discrete kick on the onset ----
  if (in->beat) s_kick += 2.4f * kickk;
  s_kick *= expf(-(float)dt / 260.0f);
  s_theta += (0.12f * spink * (0.7f + 0.6f * effect_seed_structure())
              + 0.60f * mid * spink * react * 0.5f + s_kick) * (float)dt * 0.001f;
  if (s_theta > 6.2831853f) s_theta -= 6.2831853f;

  // ---- size: the crest fills screen_r pixels ------------------------------
  // The base radius is the knob; everything the music adds to it is scaled by
  // reactivity, so turning that down does not also shrink the crest.
  // The crest used to fill 83% of the disc radius, which magnified a 128px
  // signed-distance field by 1.74x and looked exactly as soft as that sounds.
  // At 74px it fills 62%, the field is magnified 1.3x, and the silhouette edge
  // is carried by more source pixels than screen pixels for the first time.
  const float base_r = (s_chroma ? 82.0f : 74.0f) * sizek;
  const float swell = s_chroma
      ? (42.0f * energy + 18.0f * bass + 16.0f * env)
      : (12.0f * bass + 10.0f * env);
  // chroma's old base was 58px with the swell riding on top; keep that shape at
  // full reactivity by pulling the base down as the swell is allowed to grow.
  const float screen_r = base_r - (s_chroma ? 38.0f : 6.0f) * react * 0.5f + swell * react * 0.5f;
  const float k = (float)mon_radius[v] / screen_r;  // motif px per screen px
  const int32_t cs = (int32_t)lrintf(cosf(s_theta) * k * 4096.0f);
  const int32_t sn = (int32_t)lrintf(sinf(s_theta) * k * 4096.0f);

  if (s_chroma) build_palette_chroma(energy, treble, env);
  else build_palette((uint8_t)(MON_HUE[v] + (int)(knob(3) * 255.0f)), energy, treble, env);

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
