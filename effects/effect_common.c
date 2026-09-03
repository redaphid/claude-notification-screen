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
