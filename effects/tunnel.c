// tunnel -- the radial/angular LUT tunnel.
//
// Standard demoscene construction, adapted to the shared packed polar LUT:
//   depth = 8192 / (radius + 8) + z   (large near the centre => rings bunch up
//                                      at the vanishing point, like perspective)
//   angle = polar angle + rotation
// Those two index a 64x64 wall texture built once in init(). Distance fog is a
// single multiply against a 256-entry visibility table indexed by radius, and
// the result lands in a 512-entry palette whose upper half ramps to white, so
// beat highlights bloom instead of clipping.
//
// Inner loop per pixel: 5 table loads, 1 multiply, ~4 ALU ops, 1 store.
//
// Audio wiring
//   energy   -> forward speed down the tunnel
//   mid      -> rotation rate
//   treble   -> texture contrast (the walls get crunchier)
//   bass     -> fog depth, so the throat opens up on bass
//   beat     -> DISCRETE: a lurch forward (z jumps) plus a ring spawned at the
//               camera that flies outward, and a hue step. Four ring slots,
//               each with its own designed decay, so a run of fast beats
//               stacks visible rings instead of just brightening.
//   beat_env -> global brightness envelope on the fog table.
#include "effect_common.h"
#include "knobs.h"

#include <string.h>

#define TUNNEL_RINGS 4

// 64 steps along the tunnel x 256 steps around it. The angular axis is a full
// 256 on purpose: at the rim one sector spans 754/N pixels, so N=64 shows as
// visible wedges on a 240px disc. 16KB, internal SRAM, hot in cache.
static uint8_t s_tex[64 * 256];
static uint16_t s_invr[256];  // 8192 / (radius + 14), the perspective term
static uint8_t s_vis[256];    // per-frame fog * brightness
static uint8_t s_ringlut[256];
static uint16_t s_pal[512];

static uint32_t s_last_ms;
static uint32_t s_z;    // Q24.8 depth; 64 whole units == one texture repeat
static uint16_t s_rot;  // Q8.8 angle
static uint8_t s_hue;

static uint8_t s_ring_depth[TUNNEL_RINGS];
static float s_ring_life[TUNNEL_RINGS];
static int s_ring_next;

static void tunnel_init(void) {
  effect_geom_init();

  // 64x64 wall: two sine bands crossed with a coarse brick, so both the
  // rotation and the forward motion have something to bite on.
  for (int v = 0; v < 64; ++v) {
    // One mortar ring per texture repeat: without them there is no sense of
    // travel, only rotation. The perspective LUT packs ~8.5 repeats into the
    // 120px radius, so one ring per repeat is already 8 rings on screen --
    // any denser and the inner ones alias into streaks.
    // Feathered three texels each side, again because the perspective stretch
    // near the mouth turns any hard edge into a staircase.
    const int m = (v < 32) ? v : 64 - v;
    const int ring = (m == 0) ? 255 : (m == 1 ? 165 : (m == 2 ? 80 : (m == 3 ? 30 : 0)));
    for (int u = 0; u < 256; ++u) {
      // 8 lengthwise flutes around the bore, crossed with a slow swell along
      // the tunnel. All sine, no hard edges, for the same reason.
      const int base = (effect_sinu((uint8_t)(u * 8)) * 2 + effect_sinu((uint8_t)(v * 4)) * 3) / 5;
      int t = 55 + (base * 160) / 255;
      if (ring) t = t + ((255 - t) * ring) / 255;
      s_tex[(v << 8) | u] = (uint8_t)effect_clamp_u8(t);
    }
  }

  // 1/r perspective. The constant is chosen from the *outer* half of the disc,
  // not the inner: with K too small the ring spacing past r=128 exceeds the
  // whole radius and only one ring is ever on screen. K=49152, c=12 puts
  // d(r=112)=396 and d(r=255)=184, so ~3.3 wall repeats live between the fog
  // horizon and the rim -- three or four rings marching outward at all times.
  // It still aliases hard inside ~30px, which is why the fog below takes the
  // core to black rather than trying to keep it legible. Same trade every
  // tunnel demo has made since the 90s.
  for (int r = 0; r < 256; ++r) s_invr[r] = (uint16_t)(49152 / (r + 12));

  s_last_ms = 0;
  s_z = 0;
  s_rot = 0;
  s_hue = 20;
  s_ring_next = 0;
  for (int i = 0; i < TUNNEL_RINGS; ++i) {
    s_ring_depth[i] = 0;
    s_ring_life[i] = 0.0f;
  }
  memset(s_pal, 0, sizeof(s_pal));
  memset(s_vis, 0, sizeof(s_vis));
  memset(s_ringlut, 0, sizeof(s_ringlut));
}

static void tunnel_render(uint16_t *out, const EffectInput *in) {
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

  // A ring crosses the visible ~212 units of depth in 1.4s idling, 240ms flat
  // out. Faster than that and the rings strobe against the 30fps frame rate.
  // Shared knob meanings, see knobs.h.
  const float react = knob(0) * 2.0f;
  const float speedk = knob(2) * 2.0f;
  const float glowk = 0.4f + 1.2f * knob(4);

  s_z += (uint32_t)((float)dt * (0.15f * speedk + 0.75f * energy * react) * 256.0f);
  s_rot += (uint16_t)((float)dt * (6.0f * speedk + 60.0f * mid * react));

  if (in->beat) {
    s_z += 40u * 256u;  // the lurch: two thirds of a wall band, instantly
    s_hue = (uint8_t)(s_hue + (uint8_t)(29.0f * react));
    s_ring_depth[s_ring_next] = (uint8_t)(s_z >> 8);
    s_ring_life[s_ring_next] = 1.0f;
    s_ring_next = (s_ring_next + 1) & (TUNNEL_RINGS - 1);
  }

  // ---- rings: designed decay, fired by the event, not tracking a signal ---
  memset(s_ringlut, 0, sizeof(s_ringlut));
  const float decay = 1.0f - (float)dt / 260.0f;
  for (int i = 0; i < TUNNEL_RINGS; ++i) {
    if (s_ring_life[i] <= 0.01f) { s_ring_life[i] = 0.0f; continue; }
    s_ring_life[i] *= (decay > 0.0f ? decay : 0.0f);
    const int amp = (int)(230.0f * s_ring_life[i]);
    const int c = s_ring_depth[i];
    for (int w = -3; w <= 3; ++w) {
      const int falloff = amp - (w < 0 ? -w : w) * (amp / 4);
      if (falloff <= 0) continue;
      const uint8_t idx = (uint8_t)(c + w);
      if (falloff > s_ringlut[idx]) s_ringlut[idx] = (uint8_t)falloff;
    }
  }

  // ---- fog: dark at the vanishing point, bright at the mouth --------------
  // `horizon` is where the fog starts to lift, in radius units. Bass pulls it
  // toward the centre, which reads as the throat of the tunnel opening up.
  // The core goes fully black: that is the vanishing point, and it is also
  // where the 1/r LUT aliases, so the fog is doing double duty.
  const float horizon = 0.36f - 0.18f * bass;  // bass pulls the throat open
  const float bright = (0.80f + (0.18f * energy + 0.20f * treble + 0.32f * env) * react) * glowk;
  for (int r = 0; r < 256; ++r) {
    float f = effect_clamp01(((float)r / 255.0f - horizon) / (1.0f - horizon));
    f = f * f * (3.0f - 2.0f * f);  // smoothstep: soft horizon, no fog banding
    s_vis[r] = (uint8_t)effect_clamp_u8((int)(f * bright * 255.0f));
  }

  // ---- palette ------------------------------------------------------------
  // paper-cranes' Oklch `lush()` instead of three offset sines. The hue also
  // travels a little with depth, which is 6.frag's "depth-coherent" idea in
  // its simplest form: the far end of the tunnel is not merely a darker
  // version of the mouth, it is a different colour.
  const float hueBase = (float)s_hue / 255.0f + knob(3);
  for (int i = 0; i < 256; ++i) {
    const float u = (float)i / 256.0f;  // already the brightness ramp
    int r, g, b;
    effect_lush(hueBase + u * 0.38f, u * bright, &r, &g, &b);
    // The throat is the vanishing point and has to reach true black, so the
    // palette is faded rather than left at lush()'s lightness floor.
    if (u < 0.30f) {
      const float k = u / 0.30f;
      r = (int)(r * k);
      g = (int)(g * k);
      b = (int)(b * k);
    }
    effect_glow_lift(&r, &g, &b);
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(r), (uint8_t)effect_clamp_u8(g),
                             (uint8_t)effect_clamp_u8(b));
  }
  // Bloom half: ring highlights used to ramp toward white, which is both the
  // pastel failure and, for anyone in ChromaDepth glasses, a smear.
  for (int i = 256; i < 512; ++i) {
    const float k = (float)(i - 256) / 255.0f;
    int r, g, b;
    effect_lush(hueBase + 0.38f + 0.18f, 1.0f, &r, &g, &b);
    uint8_t br, bgc, bb;
    effect_unpack565(s_pal[255], &br, &bgc, &bb);
    r = br + (int)(r * k * 0.85f);
    g = bgc + (int)(g * k * 0.85f);
    b = bb + (int)(b * k * 0.85f);
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(r), (uint8_t)effect_clamp_u8(g),
                             (uint8_t)effect_clamp_u8(b));
  }

  // ---- pixels -------------------------------------------------------------
  const uint8_t z = (uint8_t)(s_z >> 8);
  const uint8_t rot = (uint8_t)(s_rot >> 8);
  const uint16_t *polar = effect_polar;

  for (int y = 0; y < EFFECT_H; ++y) {
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const uint16_t *p = polar + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];

    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x0; x <= x1; ++x) {
      const uint16_t ra = p[x];
      const uint8_t r = (uint8_t)ra;
      const uint8_t a = (uint8_t)(ra >> 8);
      const uint8_t d = (uint8_t)(s_invr[r] + z);
      const uint8_t u = (uint8_t)(a + rot);
      const int t = s_tex[((d & 63) << 8) | u];
      o[x] = s_pal[((t * s_vis[r]) >> 8) + s_ringlut[d]];
    }
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
  }
}

const Effect effect_tunnel = {"tunnel", tunnel_init, tunnel_render};
