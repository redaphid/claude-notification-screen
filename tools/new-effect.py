#!/usr/bin/env python3
"""Scaffold a new badge effect and register it everywhere it has to be listed.

    python tools/new-effect.py <name> ["one-line description"]

Creates effects/<name>.c from a working template (concentric rings that ride
outward on the beat), then appends the effect to:
  - effects/effects.h   (extern declaration)
  - effects/effects.c   (effects_all[], so it gets the next shader index)
  - harness/Makefile    (SRC and the strict-C99 check)

Append-only on purpose: the ChorusPacket shader byte indexes effects_all[], so
reordering would change what every badge in the swarm shows. After this, see
docs/effects.md for the develop/preview/flash loop.
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]

if len(sys.argv) < 2 or not re.fullmatch(r"[a-z][a-z0-9_]{1,23}", sys.argv[1]):
    raise SystemExit("usage: new-effect.py <name> [description]   (name: lowercase, letters/digits/_)")
name = sys.argv[1]
desc = sys.argv[2] if len(sys.argv) > 2 else "describe the look in one line"

src = root / "effects" / f"{name}.c"
if src.exists():
    raise SystemExit(f"{src} already exists")

TEMPLATE = '''// {name} -- {desc}
//
// Template effect: concentric rings that ride outward, recolouring on the
// downbeat. Replace the pixel loop and the audio wiring; keep the shape of the
// file. The rules that make an effect fit the badge (effect.h):
//   - no floats in the per-pixel loop: fixed point and lookup tables only
//   - allocate in init(), never in render()
//   - under ~25 ms per frame on the ESP32-S3 (this template is ~6 ms)
//   - write every pixel; the ones outside the disc must be black
//
// Audio wiring
//   bass     -> number of rings
//   mid      -> how fast the rings travel
//   treble   -> (unused here; a good home for fine detail)
//   energy   -> palette gain
//   beat     -> DISCRETE: the palette hue jumps, so the field recolours on the
//               downbeat instead of drifting
//   beat_env -> the designed attack-decay: a white wash that decays. Drive the
//               *look* off this, never off raw bass, or the visual shudders.
#include "effect_common.h"

#include <string.h>

static uint16_t s_pal[256];
static uint32_t s_last_ms;
static uint16_t s_phase;  // Q8.8 ring phase, wraps for free
static uint8_t s_hue;     // beat-fired palette anchor

static void {name}_init(void) {{
  effect_geom_init();  // shared polar LUT, allocated once for every effect
  s_last_ms = 0;
  s_phase = 0;
  s_hue = 0;
  memset(s_pal, 0, sizeof(s_pal));
}}

static void {name}_render(uint16_t *out, const EffectInput *in) {{
  if (effect_polar == NULL) {{
    memset(out, 0, (size_t)EFFECT_PIXELS * sizeof(uint16_t));
    return;
  }}
  const uint32_t dt = effect_dt_ms(&s_last_ms, in->time_ms);
  const float energy = effect_clamp01(in->energy);
  const float bass = effect_clamp01(in->bass);
  const float mid = effect_clamp01(in->mid);
  const float env = effect_clamp01(in->beat_env);

  // ---- motion (floats are fine here: once per frame) ----------------------
  s_phase += (uint16_t)((float)dt * (3.0f + 12.0f * mid));
  if (in->beat) s_hue = (uint8_t)(s_hue + 43u);

  // ---- palette: 256 entries, rebuilt per frame ---------------------------
  const int gain = effect_clamp_u8((int)(90.0f + 165.0f * (0.35f + 0.65f * energy)));
  const int wash = (int)(90.0f * env * env);
  for (int i = 0; i < 256; ++i) {{
    const uint8_t p = (uint8_t)(i + s_hue);
    const int r = ((effect_sinu(p) * gain) >> 8) + wash;
    const int g = ((effect_sinu((uint8_t)(p + 85)) * gain) >> 8) + wash;
    const int b = ((effect_sinu((uint8_t)(p + 170)) * gain) >> 8) + wash;
    s_pal[i] = effect_rgb565((uint8_t)effect_clamp_u8(r), (uint8_t)effect_clamp_u8(g),
                             (uint8_t)effect_clamp_u8(b));
  }}

  // ---- pixels: integer only ----------------------------------------------
  const uint8_t ph = (uint8_t)(s_phase >> 8);
  const int rings = 2 + (int)(4.0f * bass);
  const uint16_t *polar = effect_polar;
  for (int y = 0; y < EFFECT_H; ++y) {{
    uint16_t *o = out + (size_t)y * EFFECT_W;
    const uint16_t *p = polar + (size_t)y * EFFECT_W;
    const int x0 = effect_row_x0[y];
    const int x1 = effect_row_x1[y];
    for (int x = 0; x < x0; ++x) o[x] = 0;
    for (int x = x0; x <= x1; ++x) {{
      const int radius = p[x] & 0xFF;  // 0 centre .. 255 rim
      const int angle = p[x] >> 8;     // 0..255 for one turn
      o[x] = s_pal[(uint8_t)(radius * rings - ph + (angle >> 2))];
    }}
    for (int x = x1 + 1; x < EFFECT_W; ++x) o[x] = 0;
  }}
}}

const Effect effect_{name} = {{"{name}", {name}_init, {name}_render}};
'''

src.write_text(TEMPLATE.format(name=name, desc=desc), newline="\n")
print(f"wrote {src.relative_to(root)}")


def edit(path, old, new, what):
    p = root / path
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"{path}: could not find anchor for {what}; add it by hand")
    p.write_text(text.replace(old, new, 1), newline="\n")
    print(f"updated {path} ({what})")


# effects.h: declare after the last extern Effect line.
h = (root / "effects" / "effects.h").read_text()
externs = re.findall(r"^extern const Effect effect_\w+;$", h, flags=re.M)
edit("effects/effects.h", externs[-1], externs[-1] + f"\nextern const Effect effect_{name};", "extern")

# effects.c: append to effects_all[] (the next shader index).
c = (root / "effects" / "effects.c").read_text()
m = re.search(r"effects_all\[\] = \{\n(.*?)\n\};", c, flags=re.S)
if not m:
    raise SystemExit("effects/effects.c: effects_all[] not found")
last = m.group(1).rstrip().splitlines()[-1]
edit("effects/effects.c", last, last + f"\n    &effect_{name},", "effects_all[]")
index = m.group(1).count("&effect_")

# harness/Makefile: SRC list and the strict check.
edit("harness/Makefile", "       $(HARNESS)/mock_dj.c $(HARNESS)/preview.c",
     f"       $(EFFECTS)/{name}.c \\\n       $(HARNESS)/mock_dj.c $(HARNESS)/preview.c", "SRC")
mk = (root / "harness" / "Makefile").read_text()
strict_line = re.search(r"^\s+\$\(EFFECTS\)/plasma\.c \$\(EFFECTS\)/tunnel\.c \$\(EFFECTS\)/iris\.c.*$", mk, flags=re.M)
if strict_line:
    edit("harness/Makefile", strict_line.group(0), strict_line.group(0) + f" $(EFFECTS)/{name}.c", "strict")

print(f"""
{name} is shader index {index}. Next:
  make -f harness/Makefile strict          # still portable C99?
  make gif EFFECT={name}                    # outputs/{name}.gif (needs cc + ffmpeg; see docs/effects.md)
  pio run && tools/flash-all.ps1            # badge build + flash (Windows) or pio run -t upload (Linux)
Then tell the conductor side the shader count grew (conductor_config.h CONDUCTOR_SHADER_COUNT).""")
