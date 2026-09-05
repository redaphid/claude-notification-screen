// nfold -- the crest in a kaleidoscope whose fold count is the crest's own.
//
// Paper Cranes' nfold shader folds a lattice cell into a TAU/N wedge whose
// mirror axis is a true mirror axis of every mon, and measured that a matched
// fold on a *centred* motif is the identity: it can only do nothing. Here the
// crest sits OFF the axis, inside the wedge, so the fold does what a real
// kaleidoscope does: 2N copies of the crest (N direct, N mirrored) meet at the
// mirror lines, merge into new symmetric shapes and separate again as the crest
// slides across its wedge. N is the crest's own rotational order (kiku 12,
// hakkaku 8, kikko 6, ume and kikyo 5 ...), so the rosette a badge draws has
// the symmetry of the bead it wears, and a single tomoe comma folded three
// ways is the mitsu-domoe.
//
// The trick that makes it cheap: inside each of the 2N wedges the whole map
// from screen pixel to crest sample is one affine transform (rotate or reflect
// into the fundamental wedge, then the crest's own rotate+scale), and a wedge
// change along a scanline is found by one incremental dot product against the
// next mirror line. Every copy is smaller than the field, so the sample is
// nearest-neighbour. Per pixel: 3 adds, 1 compare, 2 shifts, 1 range check,
// 1 byte load, 1 palette load, 1 store. The 2N affine maps and the two
// palettes are rebuilt once per frame; the small whole crest at the apex is
// a second pass over its own box only.
//
// Audio wiring
//   mid      -> RATE of the crest's slide across its wedge and of the slow turn
//               of the whole rosette. Both are monotonic accumulators: music
//               sets the speed, never the angle, so nothing ever snaps back.
//   bass     -> together with beat_env, how far the crest sits from the apex
//               (the rosette expands outward on the kick and settles)
//   treble   -> width and brightness of the rim band
//   energy   -> glow reach and fill brightness
//   beat     -> DISCRETE: an angular kick to the slide (the rosette lurches,
//               then settles) and the mirrored copies step 43/256 around the
//               hue wheel, so the alternate copies recolour on the downbeat
//               while the direct copies keep the badge's own colour
//   beat_env -> the designed attack-decay: rim washes toward white, the outer
//               glow flares, the crest itself swells a little, then it decays
#include "effect_common.h"
#include "effects.h"
#include "mon_data.h"

#include <math.h>
#include <string.h>

#define NF_PI 3.14159265358979f
#define NF_MAXN 12
#define NF_MAXSEC (2 * NF_MAXN)

// Fold count per crest, mon_names order: the measured rotational order of the
// baked silhouettes (paper-cranes scripts/mon-fold.py). The crests that are
// drawn as a single element on purpose -- tomoe's one comma, suhama's mounds,
// ogi's fan -- take the count of their traditional composition, which is what
// the kaleidoscope reassembles.
static const uint8_t NF_FOLD[MON_COUNT] = {
    12,  // kiku
    3,   // tomoe
    5,   // kikyo
    5,   // ume
    8,   // hakkaku
    4,   // mokko
    6,   // kikko
    3,   // suhama
    3,   // matsukawa
    3,   // katabami
    5,   // ogi
};

// Per-wedge affine map, Q12: crest u = a*X2 + b*Y2 + tu, w = c*X2 + d*Y2 + tw,
// where (X2, Y2) = (2x - 239, 2y - 239) is twice the offset from the panel
// centre (so no pixel sits exactly on a mirror line's apex). nx, ny is the
// unit normal of mirror line m, the lower boundary of wedge m.
typedef struct {
  int32_t a, b, c, d, tu, tw;
  int32_t nx, ny;
} NfWedge;

static NfWedge s_wedge[NF_MAXSEC];
static int s_nsec;
static uint16_t s_pal[2][256];
static uint32_t s_last_ms;
static float s_rot;    // whole-rosette turn, rad, monotonic
static float s_slide;  // crest angle around the apex, rad, monotonic
static float s_spin;   // crest's own spin, rad, monotonic
static float s_kick;   // rad/s impulse from the last onset, decays
static uint8_t s_alt_hue;

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

// Same fill / rim / glow shape as mon. The two copy parities share every
// entry outside the crest (rim, glow, ground), so the mirror lines are
// invisible in the field and show only where a crest straddles one; inside,
// the mirrored copies take the alternate hue.
static void build_palettes(uint8_t hue, uint8_t alt_hue, float energy, float treble, float env) {
  int br, bg, bb, ar, ag, ab;
  hue_rgb(hue, 15, &br, &bg, &bb);
  hue_rgb(alt_hue, 15, &ar, &ag, &ab);
  const float fill = 0.26f + 0.40f * energy;
  const float reach = 2.5f + 8.0f * energy + 3.0f * env;
  const float rimw = 1.5f + 2.2f * treble;  // never under a screen pixel: nearest-sampled
  for (int i = 0; i < 256; ++i) {
    const float d = (float)(i - 128) / (float)MON_SDF_SCALE;
    float k, wash;
    if (d < 0.0f) {
      const float lit = expf(d / 5.0f);
      k = fill + (0.80f - fill) * lit;
      wash = 0.55f * env * lit;
    } else {
      const float glow = expf(-d / reach) * (0.55f + 0.35f * env);
      const float rim = d < rimw ? (1.0f - d / rimw) : 0.0f;
      k = 0.05f + 0.85f * glow + 0.30f * rim;
      wash = rim * (0.20f + 0.55f * env);
    }
    const int w = (int)(255.0f * wash);
    s_pal[0][i] = effect_rgb565((uint8_t)effect_clamp_u8((int)(br * k) + w),
                                (uint8_t)effect_clamp_u8((int)(bg * k) + w),
                                (uint8_t)effect_clamp_u8((int)(bb * k) + w));
    s_pal[1][i] = d < 0.0f ? effect_rgb565((uint8_t)effect_clamp_u8((int)(ar * k * 0.9f) + w),
                                           (uint8_t)effect_clamp_u8((int)(ag * k * 0.9f) + w),
                                           (uint8_t)effect_clamp_u8((int)(ab * k * 0.9f) + w))
                           : s_pal[0][i];
  }
}

// Per-channel max of two byte-swapped RGB565 pixels, for the apex crest's glow.
static inline uint16_t lighten(uint16_t a, uint16_t b) {
  a = (uint16_t)((a >> 8) | (a << 8));
  b = (uint16_t)((b >> 8) | (b << 8));
  const uint16_t ar = a & 0xF800u, br = b & 0xF800u;
  const uint16_t ag = a & 0x07E0u, bg = b & 0x07E0u;
  const uint16_t ab = a & 0x001Fu, bb = b & 0x001Fu;
  const uint16_t c = (uint16_t)((ar > br ? ar : br) | (ag > bg ? ag : bg) | (ab > bb ? ab : bb));
  return (uint16_t)((c >> 8) | (c << 8));
}

// Nearest-neighbour sample of the field at Q12 motif coordinates. Every crest
// this effect draws is smaller on screen than the 128 px field (minified 1.4x
// or more), so the half-texel error of a nearest sample is under half a screen
// pixel and bilinear filtering would buy nothing. Rounded, not truncated, so
// the crest does not shift by half a texel.
static inline int sdf_sample(const uint8_t *sdf, int32_t u, int32_t w) {
  const int32_t ui = (u + 2048) >> 12;
  const int32_t wi = (w + 2048) >> 12;
  if ((uint32_t)ui >= (uint32_t)MON_N || (uint32_t)wi >= (uint32_t)MON_N) {
    // Past the field: nearest border sample plus the distance to the border,
    // so the glow keeps fading instead of snapping along a square.
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

// 2x2 float helpers, used only while building the per-frame wedge maps.
static void mat_mul(float *o, const float *p, const float *q) {
  float r[4];
  r[0] = p[0] * q[0] + p[1] * q[2];
  r[1] = p[0] * q[1] + p[1] * q[3];
  r[2] = p[2] * q[0] + p[3] * q[2];
  r[3] = p[2] * q[1] + p[3] * q[3];
  o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; o[3] = r[3];
}

static void nfold_init(void) {
  effect_geom_init();
  s_last_ms = 0;
  s_rot = 0.0f;
  s_slide = 0.3f;
  s_spin = 0.0f;
  s_kick = 0.0f;
  s_alt_hue = 24;
  s_nsec = 6;
  memset(s_pal, 0, sizeof(s_pal));
  memset(s_wedge, 0, sizeof(s_wedge));
}

static void nfold_render(uint16_t *out, const EffectInput *in) {
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

  // ---- motion: three monotonic angles, rates from the music ---------------
  if (in->beat) {
    s_kick += 2.2f;
    s_alt_hue = (uint8_t)(s_alt_hue + 43u);
  }
  s_kick *= expf(-(float)dt / 280.0f);
  s_rot += (0.06f + 0.22f * mid) * dts;
  s_slide += (0.30f + 1.30f * mid + s_kick) * dts;
  s_spin += 0.35f * dts;
  if (s_rot > 2.0f * NF_PI) s_rot -= 2.0f * NF_PI;
  if (s_spin > 2.0f * NF_PI) s_spin -= 2.0f * NF_PI;

  // ---- geometry ------------------------------------------------------------
  const int n = NF_FOLD[v];
  const int nsec = 2 * n;
  const float wedge = NF_PI / (float)n;  // the fundamental half-wedge
  // Crest angle folded into one full period of the rotation group; in the
  // returning half the crest is seen through the mirror, so its sample space
  // is reflected too (a tomoe keeps its handedness per copy that way).
  float ang = fmodf(s_slide, 2.0f * wedge);
  if (ang < 0.0f) ang += 2.0f * wedge;
  const int through_mirror = ang >= wedge;
  const float orbit = 60.0f + 7.0f * bass + 9.0f * env;            // apex to crest centre, px
  float rs = orbit * sinf(wedge) * 1.25f;                            // crest radius, px
  if (rs < 24.0f) rs = 24.0f;
  if (rs > 40.0f) rs = 40.0f;
  rs *= 1.0f + 0.06f * env;
  const float k = (float)mon_radius[v] / rs;                         // motif px per screen px
  const float cx = orbit * cosf(ang), cy = orbit * sinf(ang);        // crest centre, fundamental frame

  // crest transform: q = K Rot(-spin) (G L - c) + 64, for L in the fundamental frame
  const float cs = cosf(s_spin), sn = sinf(s_spin);
  const float crest[4] = {k * cs, k * sn, -k * sn, k * cs};
  const float tu = 64.0f - (crest[0] * cx + crest[1] * cy);
  const float tw = 64.0f - (crest[2] * cx + crest[3] * cy);
  const float c2 = cosf(2.0f * wedge), s2 = sinf(2.0f * wedge);
  const float refl[4] = {c2, s2, s2, -c2};  // reflection across the wedge's upper mirror line
  float cg[4];
  if (through_mirror) mat_mul(cg, crest, refl);
  else { cg[0] = crest[0]; cg[1] = crest[1]; cg[2] = crest[2]; cg[3] = crest[3]; }

  for (int m = 0; m < nsec; ++m) {
    const float phi = s_rot + (float)m * wedge;
    // into the fundamental frame: even wedges rotate, odd ones rotate then reflect
    float mm[4];
    if ((m & 1) == 0) {
      const float pc = cosf(phi), ps = sinf(phi);
      mm[0] = pc; mm[1] = ps; mm[2] = -ps; mm[3] = pc;   // Rot(-phi)
    } else {
      const float phi0 = phi - wedge;
      const float pc = cosf(phi0), ps = sinf(phi0);
      const float r0[4] = {pc, ps, -ps, pc};
      mat_mul(mm, refl, r0);                                 // Refl_wedge * Rot(-phi0)
    }
    float am[4];
    mat_mul(am, cg, mm);
    NfWedge *W = &s_wedge[m];
    // (X2, Y2) is twice the pixel offset, so halve the linear part.
    W->a = (int32_t)lrintf(am[0] * 2048.0f);
    W->b = (int32_t)lrintf(am[1] * 2048.0f);
    W->c = (int32_t)lrintf(am[2] * 2048.0f);
    W->d = (int32_t)lrintf(am[3] * 2048.0f);
    W->tu = (int32_t)lrintf(tu * 4096.0f);
    W->tw = (int32_t)lrintf(tw * 4096.0f);
    W->nx = (int32_t)lrintf(-sinf(phi) * 4096.0f);
    W->ny = (int32_t)lrintf(cosf(phi) * 4096.0f);
  }
  s_nsec = nsec;

  // ---- palettes: the badge's hue for direct copies, a stepping hue for the
  //      mirrored ones so the 2N-fold structure reads even on a plain motif --
  const uint8_t hue = mon_hue(v);
  build_palettes(hue, (uint8_t)(hue + s_alt_hue), energy, treble, env);

  // ---- pixels ---------------------------------------------------------------
  const uint8_t *sdf = mon_sdf[v];
  for (int y = 0; y < EFFECT_H; ++y) {
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];
    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
    if (x1 < x0) continue;

    const int32_t Y2 = 2 * y - 239;
    int32_t X2 = 2 * x0 - 239;

    // Which wedge holds the first pixel: the one whose lower mirror line is
    // below the point and whose upper one is above. Brute force, once per row.
    int m = 0;
    for (int t = 0; t < nsec; ++t) {
      const NfWedge *A = &s_wedge[t];
      const NfWedge *B = &s_wedge[(t + 1) % nsec];
      if (A->nx * X2 + A->ny * Y2 >= 0 && B->nx * X2 + B->ny * Y2 < 0) { m = t; break; }
    }
    // Along a row the polar angle is monotonic: increasing above the centre
    // line, decreasing below it. Watch the one mirror line that can be
    // crossed next, as an incrementally updated dot product. Both directions
    // are folded into one test, `dot >= 0`: going up the watched line is the
    // wedge's upper one and `dot` is its plain dot product; going down it is
    // the lower one, negated and biased by 1 so "strictly below" reads >= 0.
    const int up = Y2 < 0;
    const int step = up ? 1 : nsec - 1;
    const int32_t sgn = up ? 1 : -1;
    const int32_t bias = up ? 0 : -1;
    const NfWedge *W = &s_wedge[m];
    int32_t u = W->a * X2 + W->b * Y2 + W->tu;
    int32_t w = W->c * X2 + W->d * Y2 + W->tw;
    const NfWedge *L = &s_wedge[up ? (m + 1) % nsec : m];
    int32_t dot = sgn * (L->nx * X2 + L->ny * Y2) + bias;
    int32_t dstep = sgn * 2 * L->nx;
    const uint16_t *pal = s_pal[m & 1];

    for (int x = x0; x <= x1; ++x) {
      if (dot >= 0) {
        // crossed a mirror line: next wedge, new affine map, new watch line
        int guard = nsec;
        do {
          m = (m + step) % nsec;
          W = &s_wedge[m];
          L = &s_wedge[up ? (m + 1) % nsec : m];
          dot = sgn * (L->nx * X2 + L->ny * Y2) + bias;
        } while (dot >= 0 && --guard > 0);
        dstep = sgn * 2 * L->nx;
        u = W->a * X2 + W->b * Y2 + W->tu;
        w = W->c * X2 + W->d * Y2 + W->tw;
        pal = s_pal[m & 1];
      }
      o[x] = pal[sdf_sample(sdf, u, w)];
      u += 2 * W->a;
      w += 2 * W->c;
      dot += dstep;
      X2 += 2;
    }
  }

  // ---- the apex: the crest itself, whole and small, where every mirror
  //      line meets, so the badge's own crest stays nameable inside the
  //      rosette it makes. Drawn over its own box only, glow lightened in. --
  {
    const float rc = 21.0f + 3.0f * env;
    const float kc = (float)mon_radius[v] / rc;
    const float csf = cosf(-0.6f * s_spin) * kc, snf = sinf(-0.6f * s_spin) * kc;  // counter-turn
    const int32_t cs = (int32_t)lrintf(csf * 4096.0f);
    const int32_t sn = (int32_t)lrintf(snf * 4096.0f);
    const uint16_t *pal = s_pal[0];
    const float ext = rc + 7.0f;
    const int ya = (int)floorf(119.5f - ext), yb = (int)ceilf(119.5f + ext);
    const int xa = (int)floorf(119.5f - ext), xb = (int)ceilf(119.5f + ext);
    for (int y = ya; y <= yb; ++y) {
      uint16_t *o = out + (size_t)y * EFFECT_W;
      const float dyf = (float)y - 119.5f;
      const float dx = (float)xa - 119.5f;
      int32_t u = (int32_t)lrintf((dx * csf - dyf * snf + 64.0f) * 4096.0f);
      int32_t w = (int32_t)lrintf((dx * snf + dyf * csf + 64.0f) * 4096.0f);
      for (int x = xa; x <= xb; ++x) {
        const int s = sdf_sample(sdf, u, w);
        if (s < 128 + 6) o[x] = pal[s];
        else if (s < 128 + 6 * MON_SDF_SCALE) o[x] = lighten(o[x], pal[s]);
        u += cs;
        w += sn;
      }
    }
  }
}

const Effect effect_nfold = {"nfold", nfold_init, nfold_render};
