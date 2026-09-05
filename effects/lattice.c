// lattice -- the crest as a lattice of itself, zooming forever inward.
//
// From the Paper Cranes lattice-bead work: the crest replaces the hexagon as
// the lattice's repeating unit, tiled by TRANSLATION so every cell holds one
// whole, unmirrored motif (the mirror fold could only ever show one quadrant),
// with the fold ratio locked to 2 so the levels nest without seams, and a
// perpetual self-similar zoom that wraps one octave onto the next: blossoms
// inside blossoms, forever. Two levels are drawn: the coarse crests, and the
// fine crests at half their pitch, which sit nested inside them and in the
// gaps between them. When the zoom has doubled, the fine level has become the
// coarse one and a new fine level has faded in, so the dive never ends and
// never jumps.
//
// The trick that makes it cheap: the crest is never magnified (a cell is at
// most 192 px, the field 128 px), so both levels are nearest-sampled -- per
// level per pixel, 2 adds, 2 masks, 2 tiny table loads that fold the cell
// coordinate into the field and give the distance past its border, 1 field
// load, 1 add. The two distances then index ONE 2D palette (128 coarse x 32
// fine entries), rebuilt per frame, that decides how a fine crest looks when
// it sits inside a coarse one (embossed into the fill) versus in the gap (a
// small lit crest on the dark ground). Nothing per pixel is a multiply.
//
// Audio wiring
//   energy   -> zoom RATE (monotonic: always inward, faster when loud, never
//               back), glow reach, fill brightness
//   mid      -> RATE of the slow turn of the whole lattice (monotonic)
//   bass     -> with beat_env, the crests' breath: every crest dilates inside
//               its cell (a shift of the distance palette, so the cells
//               breathe while the lattice itself holds still)
//   treble   -> width and brightness of the rim band
//   beat     -> DISCRETE: the fine crests step to the next hue of a triad, so
//               the small crests recolour on the downbeat while the big ones
//               keep the badge's own colour
//   beat_env -> the designed attack-decay: rim washes toward white, the glow
//               flares, the breath swells, then it decays
#include "effect_common.h"
#include "effects.h"
#include "mon_data.h"

#include <math.h>
#include <string.h>

#define LAT_PITCH 96.0f     // coarse cell pitch in screen px at zoom 1 (world units)
#define LAT_OCTAVE_S 13.0f  // seconds per zoom octave at rest
#define LAT_CB 7            // coarse palette bits
#define LAT_FB 5            // fine palette bits

static uint16_t s_pal2[1 << (LAT_CB + LAT_FB)];
static uint16_t s_wrap[256];  // (distance past the field's border << 8) | clamped field index
static uint32_t s_last_ms;
static float s_zphase;        // 0..1, log2 of the zoom, monotonic and wrapping
static float s_rot;           // rad, monotonic
static float s_pan_x, s_pan_y;  // world units
static float s_pan_dir;       // rad, the drift direction turns slowly
static uint8_t s_fine_hue;    // beat-stepped offset for the fine crests

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

static float smooth01(float x) {  // 0 below 0, 1 above 1, smooth between
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

static void lattice_init(void) {
  effect_geom_init();
  s_last_ms = 0;
  s_zphase = 0.0f;
  s_rot = 0.0f;
  s_pan_x = 0.0f;
  s_pan_y = 0.0f;
  s_pan_dir = 0.7f;
  s_fine_hue = 85;
  memset(s_pal2, 0, sizeof(s_pal2));
  // A cell is 256 units; the 128-unit field sits in the first half, the
  // second half is the gap to the next crest. Past the field the distance
  // keeps growing toward the middle of the gap and then shrinks toward the
  // neighbour, so the glow is continuous across cell walls.
  for (int i = 0; i < 256; ++i) {
    int c = i < 128 ? i : 127;
    int e = i < 128 ? 0 : (i - 127 < 256 - i ? i - 127 : 256 - i);
    if (e > 63) e = 63;
    s_wrap[i] = (uint16_t)((e << 8) | c);
  }
}

// The 2D palette. c indexes coarse distance (2 sdf units per entry), f fine
// distance (8 sdf units per entry).
static void build_palette(uint8_t hue, float energy, float treble, float env, float breath_px,
                          float spp_c, float spp_f, float wc, float wf) {
  int br, bg, bb, fr, fg, fb;
  hue_rgb(hue, 15, &br, &bg, &bb);
  hue_rgb((uint8_t)(hue + s_fine_hue), 15, &fr, &fg, &fb);

  const float fill = 0.26f + 0.40f * energy;
  const float reach = 3.0f + 7.0f * energy + 2.5f * env;  // px: the flare stays near the crests
  const float rimw = 1.0f + 1.8f * treble;                // px

  // coarse: base colour per entry, and how much "inside" it is. The field
  // saturates 32 motif px out, which at low zoom is only ~16 screen px, so
  // the glow at that distance is subtracted: the ground between crests is a
  // constant 6% and the beat flare lives around the crests, never on the
  // background (the "no breathing background" rule from the wall).
  static int base_r[1 << LAT_CB], base_g[1 << LAT_CB], base_b[1 << LAT_CB], ins_q[1 << LAT_CB];
  const float gain = 0.55f + 0.35f * env;
  const float d_far = (127.0f / (float)MON_SDF_SCALE) * spp_c - breath_px;
  const float glow_far = expf(-d_far / reach) * gain;
  for (int c = 0; c < (1 << LAT_CB); ++c) {
    const float d = ((float)(2 * c + 1 - 128) / (float)MON_SDF_SCALE) * spp_c - breath_px;
    float k, wash;
    if (d < 0.0f) {
      const float lit = expf(d / 4.0f);
      k = fill + (0.80f - fill) * lit;
      wash = 0.50f * env * lit;
    } else {
      float glow = expf(-d / reach) * gain - glow_far;
      if (glow < 0.0f) glow = 0.0f;
      const float rim = d < rimw ? (1.0f - d / rimw) : 0.0f;
      k = 0.06f + 0.80f * glow + 0.35f * rim;
      wash = rim * (0.20f + 0.55f * env);
    }
    // as the coarse level fades out its crest sinks into the ground
    k = 0.06f + (k - 0.06f) * wc;
    wash *= wc;
    const int w = (int)(255.0f * wash);
    base_r[c] = effect_clamp_u8((int)(br * k) + w);
    base_g[c] = effect_clamp_u8((int)(bg * k) + w);
    base_b[c] = effect_clamp_u8((int)(bb * k) + w);
    ins_q[c] = (int)(256.0f * wc * smooth01(0.5f - d));
  }

  // fine: an emboss multiplier for when it sits inside a coarse crest, and an
  // additive lit crest for when it sits in the gap
  static int mult_q[1 << LAT_FB], add_r[1 << LAT_FB], add_g[1 << LAT_FB], add_b[1 << LAT_FB];
  const float frim = 0.8f + 1.4f * treble;
  for (int f = 0; f < (1 << LAT_FB); ++f) {
    const float d = ((float)(8 * f + 4 - 128) / (float)MON_SDF_SCALE) * spp_f - breath_px * 0.7f;
    const float inside = smooth01(0.6f - d);
    const float rim = smooth01(1.0f - fabsf(d) / frim);
    const float glow = d > 0.0f ? expf(-d / (2.0f + 3.0f * env)) : 0.0f;
    const float m = 1.0f - 0.42f * wf * inside + 0.30f * wf * rim;
    mult_q[f] = (int)(256.0f * m);
    const float lit = wf * (0.55f * inside + 0.30f * glow);
    const float wwash = wf * rim * (0.35f + 0.45f * env);
    add_r[f] = (int)(fr * lit + 255.0f * wwash);
    add_g[f] = (int)(fg * lit + 255.0f * wwash);
    add_b[f] = (int)(fb * lit + 255.0f * wwash);
  }

  for (int c = 0; c < (1 << LAT_CB); ++c) {
    uint16_t *row = s_pal2 + (c << LAT_FB);
    const int r0 = base_r[c], g0 = base_g[c], b0 = base_b[c], iq = ins_q[c];
    for (int f = 0; f < (1 << LAT_FB); ++f) {
      const int m = mult_q[f];
      const int ir = (r0 * m) >> 8, ig = (g0 * m) >> 8, ib = (b0 * m) >> 8;   // embossed
      const int orr = r0 + add_r[f], og = g0 + add_g[f], ob = b0 + add_b[f];  // in the gap
      row[f] = effect_rgb565((uint8_t)effect_clamp_u8(orr + (((ir - orr) * iq) >> 8)),
                             (uint8_t)effect_clamp_u8(og + (((ig - og) * iq) >> 8)),
                             (uint8_t)effect_clamp_u8(ob + (((ib - ob) * iq) >> 8)));
    }
  }
}

static void lattice_render(uint16_t *out, const EffectInput *in) {
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

  // ---- motion: zoom, turn and drift are all one-way ------------------------
  if (in->beat) s_fine_hue = (uint8_t)(s_fine_hue + 85u);
  s_zphase += dts * (0.45f + 1.10f * energy) / LAT_OCTAVE_S;
  if (s_zphase >= 1.0f) {
    // one octave in: the fine level is now the coarse one. World coordinates
    // double so the same cells stay under the same pixels.
    s_zphase -= 1.0f;
    s_pan_x *= 2.0f;
    s_pan_y *= 2.0f;
  }
  s_rot += (0.025f + 0.10f * mid) * dts;
  if (s_rot > 6.2831853f) s_rot -= 6.2831853f;
  s_pan_dir += 0.06f * dts;
  s_pan_x += 2.4f * cosf(s_pan_dir) * dts;
  s_pan_y += 2.4f * sinf(s_pan_dir) * dts;
  s_pan_x = fmodf(s_pan_x, LAT_PITCH);
  s_pan_y = fmodf(s_pan_y, LAT_PITCH);
  if (s_pan_x < 0.0f) s_pan_x += LAT_PITCH;
  if (s_pan_y < 0.0f) s_pan_y += LAT_PITCH;

  const float zoom = exp2f(s_zphase);                 // 1 .. 2 screen px per world unit
  const float spp_c = LAT_PITCH * zoom / 256.0f;      // screen px per sdf unit, coarse
  const float spp_f = spp_c * 0.5f;
  const float wf = smooth01(s_zphase / 0.18f);        // fine level fades in early
  const float wc = 1.0f - smooth01((s_zphase - 0.82f) / 0.18f);  // coarse fades out late
  const float breath = 1.2f * bass + 2.2f * env;      // px of dilation

  build_palette(mon_hue(v), energy, treble, env, breath, spp_c, spp_f, wc, wf);

  // ---- per-level affine maps, Q8 cell units per screen px -----------------
  // cell = world * 256 * 2^L / PITCH, world = Rot(-rot) * p / zoom + pan
  int32_t la[2], lb[2], lc[2], ld[2], ltx[2], lty[2];
  const float cr = cosf(s_rot), sr = sinf(s_rot);
  for (int L = 0; L < 2; ++L) {
    const float s = 256.0f * (float)(1 << L) / (LAT_PITCH * zoom);
    const float sp = 256.0f * (float)(1 << L) / LAT_PITCH;
    la[L] = (int32_t)lrintf(s * cr * 256.0f);
    lb[L] = (int32_t)lrintf(s * sr * 256.0f);
    lc[L] = (int32_t)lrintf(-s * sr * 256.0f);
    ld[L] = (int32_t)lrintf(s * cr * 256.0f);
    ltx[L] = (int32_t)lrintf(sp * s_pan_x * 256.0f);
    lty[L] = (int32_t)lrintf(sp * s_pan_y * 256.0f);
  }

  // ---- pixels ---------------------------------------------------------------
  const uint8_t *sdf = mon_sdf[v];
  const uint16_t *wrap = s_wrap;
  const uint16_t *pal2 = s_pal2;
  const int32_t a0 = la[0], c0 = lc[0], a1 = la[1], c1 = lc[1];
  for (int y = 0; y < EFFECT_H; ++y) {
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];
    const int32_t X = x0 - EFFECT_W / 2;
    const int32_t Y = y - EFFECT_H / 2;
    int32_t u0 = a0 * X + lb[0] * Y + ltx[0];
    int32_t w0 = c0 * X + ld[0] * Y + lty[0];
    int32_t u1 = a1 * X + lb[1] * Y + ltx[1];
    int32_t w1 = c1 * X + ld[1] * Y + lty[1];

    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x0; x <= x1; ++x) {
      const int tu0 = wrap[(u0 >> 8) & 255], tw0 = wrap[(w0 >> 8) & 255];
      const int e0 = (tu0 > tw0 ? tu0 : tw0) >> 8;
      int s0 = sdf[((tw0 & 127) << 7) | (tu0 & 127)] + (e0 << 2);
      if (s0 > 255) s0 = 255;

      const int tu1 = wrap[(u1 >> 8) & 255], tw1 = wrap[(w1 >> 8) & 255];
      const int e1 = (tu1 > tw1 ? tu1 : tw1) >> 8;
      int s1 = sdf[((tw1 & 127) << 7) | (tu1 & 127)] + (e1 << 2);
      if (s1 > 255) s1 = 255;

      o[x] = pal2[((s0 >> (8 - LAT_CB)) << LAT_FB) | (s1 >> (8 - LAT_FB))];
      u0 += a0;
      w0 += c0;
      u1 += a1;
      w1 += c1;
    }
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
  }
}

const Effect effect_lattice = {"lattice", lattice_init, lattice_render};
