---
name: new-effect
description: Add a new audio-reactive visual (effect) to the Chorus badges, preview it without hardware, flash it, and switch the swarm to it. Use when asked for a new animation, visual, shader, or effect on the badges.
---

# Adding a badge effect

Read `docs/effects.md` first; it is the full contract. This skill is the
order of operations.

1. **Scaffold and register in one step.**
   `python tools/new-effect.py <name> "<one-line look>"` writes
   `effects/<name>.c` from a working template and appends it to
   `effects/effects.h`, `effects/effects.c` (`effects_all[]`) and
   `harness/Makefile`. Never reorder `effects_all[]`: the packet's shader
   byte indexes it.

2. **Write the render.** Keep the file's shape: `init()` builds tables,
   `render()` is integer-only per pixel, palette rebuilt once per frame,
   pixels through `effect_rgb565()`, everything outside the disc black.
   Wire audio deliberately and say so in the header comment: continuous
   looks off `beat_env`, discrete changes off `beat`, never hue or scale off
   raw bass. Budget 25 ms per frame on an ESP32-S3.

3. **Prove it is portable C99.** `make -f harness/Makefile strict` (needs a
   C compiler; on this Windows box use WSL distro `survivor`). Fix warnings.

4. **Look at it before flashing.** `make gif EFFECT=<name>` (Linux, needs
   ffmpeg) or render PPM frames with `harness/build/preview --effect <name>
   --frames 150 --fps 30 --out <dir>` and assemble with Pillow. View the
   result; if it is ugly or static, iterate here, not on hardware.

5. **Build and flash.** `pio run` (default env is the badge). Linux:
   `pio run -t upload`. Windows: `.\tools\flash-all.ps1` flashes every badge
   on the hub in parallel and verifies fps over serial; expect 28 or better.

6. **Switch the swarm to it.** On the leader's serial console type the
   effect's name, or `shader <index>`; `?` lists them. A badge made conductor
   by holding BOOT at reset takes the same commands.

7. **Per-badge variants** (each badge different): expose a setter outside
   the frozen contract, as `mon_select()` does, and pick it in
   `src/main.cpp` from the MAC.

8. **Commit** the effect, the registry edits and, if any, the generator
   script plus generated data. Mention the new shader index in the commit so
   the conductor side updates `CONDUCTOR_SHADER_COUNT`.

Do not touch `effects/effect.h` or `src/chorus_packet.h`; they are the two
frozen contracts shared by every workstream.
