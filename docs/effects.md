# Adding a visual (effect) to the badges

An effect is one C file in `effects/`, a pure function from audio features and
time to a 240x240 RGB565 frame. The same file compiles unchanged for the badge
and for the desktop harness, so you can watch a new visual on a laptop before
any hardware is involved. This page is the whole loop, start to finish.

## 1. Scaffold

```sh
python tools/new-effect.py ripple "rings that ride out on the beat"
```

That writes `effects/ripple.c` from a working template and registers it in
`effects/effects.h`, `effects/effects.c` (`effects_all[]`, so it gets the next
shader index) and `harness/Makefile`. Append-only: the packet's shader byte is
an index into `effects_all[]`, so reordering changes what every badge shows.

## 2. The contract (`effects/effect.h`, frozen)

- `void init(void)` builds lookup tables and may allocate; `render()` never
  allocates.
- `void render(uint16_t *out, const EffectInput *in)` writes all 57,600 pixels.
  Pixels outside the round face must be black. `effect_row_x0[y]` and
  `effect_row_x1[y]` give the visible span per scanline.
- **No floats in the per-pixel loop.** Fixed point and lookup tables only.
  Floats are fine once per frame (palette, phases, per-frame tables).
- Budget: about 25 ms per frame on the ESP32-S3 at 240 MHz. plasma runs in
  ~8 ms, mon in ~12 ms; both hold 29 to 32 fps with the ~12 ms SPI blit.
- Produce pixels only through `effect_rgb565(r, g, b)`; it owns the byte order
  the LovyanGFX sprite expects.
- Do not smooth the input. The conductor already ships designed envelopes.
  Drive continuous looks off `beat_env` (attack-decay, 1.0 at the onset) and
  discrete changes off `beat` (1 on exactly one frame). Driving hue or scale
  off raw `bass` makes the visual shudder.

`EffectInput`: `bass mid treble energy` (0..1), `time_ms` (wraps; use
`effect_dt_ms`), `beat`, `beat_env`.

## 3. Shared scaffolding (`effects/effect_common.h`)

- `effect_geom_init()` builds, once, a polar lookup: `effect_polar[y*240+x]`
  packs `(angle << 8) | radius` (radius 0 centre .. 255 rim, angle 0..255 per
  turn). One sequential load per pixel gives both. Every effect calls it from
  `init()`.
- `effect_sin8[256]`, `effect_sinu(phase)`, `effect_clamp_u8`,
  `effect_clamp01`, `effect_dt_ms`.
- Big tables go through `EFFECT_ALLOC`; on the badge that is PSRAM.

The template effect is the smallest useful shape: a per-frame 256-entry
palette plus a per-pixel table lookup. plasma (separable sine tables), tunnel,
iris and mon (a signed-distance field sampled bilinearly) are the worked
examples; read the header comment of each for the trick that makes it cheap.

## 4. Watch it without hardware

The harness renders frames driven by a mock DJ at 118 BPM and needs a C
compiler plus ffmpeg. On Linux or WSL:

```sh
make -f harness/Makefile strict      # still strict C99 with no platform headers?
make gif EFFECT=ripple               # outputs/ripple.gif, 5 s, 30 fps
make bench                           # per-frame timing (x86 numbers, regression signal only)
```

On this Windows box there is no native compiler; the WSL distro `survivor` has
gcc and make but not ffmpeg, so render frames there
(`harness/build/preview --effect ripple --frames 150 --fps 30 --out /tmp/f`)
and assemble with Pillow, or install ffmpeg in WSL.

## 5. Put it on badges

```sh
pio run                              # badge firmware, default env
pio run -t upload                    # one badge (Linux; port from platformio.ini)
.\tools\flash-all.ps1                # Windows: every badge on the hub, in parallel
```

Every badge shows the effect the conductor's shader byte names. On the
leader's serial console: `shader 4`, `ripple`, `next`, `prev`, `cycle 20000`,
`?`. A badge that was made conductor by holding BOOT at reset takes the same
commands. The leader's own panel follows the same byte.

## 6. Per-badge variants

If an effect should look different on every badge (mon does: each badge wears
its own crest), give it a setter outside the frozen contract, as
`mon_select(int)` in `effects/effects.h`, and choose the variant in
`src/main.cpp` from the MAC (`monSelectForThisBadge`). The effect must still
render something sensible when nothing was selected; mon cycles.

## 7. Baked assets

Big shape data is generated, not hand-written. `tools/bake-mon.py` turns the
mon silhouettes into `effects/mon_data.c` (eleven 128x128 signed-distance
fields, 180 KB of flash). Follow the same pattern for any new asset: a script
in `tools/`, a generated `.c/.h` pair in `effects/`, and a comment at the top
saying which script made it. Never edit generated files by hand.

## Checklist before you call it done

- `make -f harness/Makefile strict` passes.
- A GIF or a photo exists and someone looked at it.
- fps on a real badge is 28 or better (the HUD prints it; so does serial).
- The conductor side knows the shader count grew (`CONDUCTOR_SHADER_COUNT`
  in `conductor/conductor_config.h`, or better, use `effects_count`).
- Documented in the effect's header comment: what it looks like, the trick
  that makes it cheap, and the audio wiring.
