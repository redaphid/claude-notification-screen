# Chorus: audio-reactive badge swarm

Read `README.md` for what this is and why. This file is the map for working
in the repo.

## Layout

- `src/` badge firmware (ESP32-S3, round 240x240 GC9A01). Receives
  `ChorusPacket` over ESP-NOW, relays it, renders locally. Role chosen at
  boot: hold BOOT while resetting to make a badge the conductor (mock DJ).
- `effects/` the visuals, pure C99, one file each, shared byte-for-byte with
  the desktop harness. Registry in `effects/effects.c`; the packet's shader
  byte indexes it. Generated data (`mon_data.c`) comes from `tools/`.
- `conductor/` the leader: Waveshare ESP32-S3-Touch-LCD-1.46 with the
  microphone. Analyses audio, broadcasts 24-byte features at ~30 Hz.
- `harness/` desktop preview: renders any effect to PPM/GIF with a mock DJ,
  no hardware, no SDL.
- `tools/` Windows batch flashing (`flash-all.ps1`, `flash-one.ps1`,
  `flash.ps1`, `watch-badges.ps1`), effect scaffolding (`new-effect.py`),
  asset baking (`bake-mon.py`).
- `web/` ESP Web Tools flasher page for badge recipients.
- `docs/` guides and bench logs. Start with `docs/effects.md`.
- `CONVERSATION.md` written conversation between the two benches' agents.

## The two frozen contracts

`src/chorus_packet.h` (the 24 bytes on the air) and `effects/effect.h` (what
an effect is). Changing either means coordinating every workstream at once.
Do not edit them casually.

## Common tasks

- **New visual:** `python tools/new-effect.py <name>` then follow
  `docs/effects.md` (or the `new-effect` skill in `.claude/skills/`).
- **Build:** `pio run` builds the badge. `pio run -e conductor` builds the
  leader (compile check on Windows; it runs on the Linux bench's 1.46).
- **Flash badges (Windows):** `.\tools\flash-all.ps1` flashes every badge on
  the hub in parallel and verifies fps over serial. Toolchain notes and quirks
  (uv-installed PlatformIO, a DNS-blocked registry mirror) are in
  `docs/windows-flashing.md`.
- **Flash a badge (Linux):** `pio run -t upload` (port in `platformio.ini`).
- **Choose what the swarm shows:** on the leader's serial console type an
  effect name, `shader <n>`, `next`, `prev`, `cycle <ms>`, or `?`. Every
  badge follows; each keeps its own crest in the `mon` effect.
- **Watch badges:** `.\tools\watch-badges.ps1 -Seconds 60` summarises fps,
  rx rate, relays and stalls across every attached badge.

## Hardware facts that cost time

Badge backlight is GPIO40 (not 2), reset GPIO12 (not 14), `invert = true`.
See the README pin map. Badges enumerate on Windows as CH343 COM ports; the
USB serial is the stable identity. Two benches within radio range hear each
other's conductors; anchor cross-bench timing on the sequence reset, not on
wall clocks (they differ by ~57 min).

## Branches and conventions

`main` is the Linux bench's branch. The Windows bench works on `follower`
and merges `main` in; both sides append to `CONVERSATION.md` on `main`.
Commit messages say why, not just what. Effects are append-only in the
registry. Keep `harness/Makefile` in step when adding effect sources.
