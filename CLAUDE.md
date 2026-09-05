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

## The button on the back

Held at reset it makes a badge the conductor. While the badge is running: tap
for the next effect (and stay on it), hold 1.2s to follow the leader again. A
pin outranks `BADGE_LOCK_EFFECT` on purpose -- the bag builds pick a default,
they do not forbid a wearer from changing their mind.

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
- **Control the swarm from a phone:** flash the leader with
  `pio run -e conductor_ble -t upload`, then open
  https://redaphid.github.io/claude-notification-screen/control.html in Chrome
  on Android. Pick what everyone shows, see every badge that has beaconed, and
  pin one person's badge to one visual. `docs/control.md` has the whole story;
  `conductor/leader_link.h` is the contract. From a laptop instead:
  `python3 scripts/test/leader-ble.py`.
- **Knobs (live parameters):** eight of them, `effects/knobs.h`, the paper-cranes
  idea. `knobs` / `knob <1-8> <0-255>` / `reset` on the leader's console, sliders
  on the control page, or one byte per knob straight from nRF Connect -- each is
  its own BLE characteristic with a 0x2901 label for exactly that reason. Slot
  meanings are consistent across effects (1 reactivity, 2 scale, 3 speed, 4 hue,
  5 glow); values reload that effect's defaults on an effect change. The harness
  takes them too: `preview --effect chroma --knob 1=0`.
- **Crests:** `crests`, `crest <id|all> <name>`. Remembered in flash and
  outranks the MAC-derived default -- a crest is an identity, not a setting.
- **Address one badge:** badges answer to the last three bytes of their MAC
  (printed at boot, `[badge] id 85dcdc`). On the leader's console: `who`,
  `rollcall`, `pin <id|all> <effect>`, `free <id|all>`, `find <id|all> [secs]`,
  `dim <id|all> <0-255>`. These ride `src/chorus_command.h` -- a second wire
  contract alongside the frozen packet, so old badges ignore it rather than
  breaking.
- **Choose what the swarm shows:** on the leader's serial console type an
  effect name (`plasma`, `tunnel`, `iris`, `mon`, `chroma`), `shader <n>`,
  `next`, `prev`, `cycle <ms>`, or `?`. Every badge follows; each keeps its
  own crest in `mon` and `chroma`. Or use the offline-capable Web Serial
  page: https://redaphid.github.io/claude-notification-screen/leader.html
  (source `web/leader.html`; publish with
  `git subtree split --prefix=web -b gh-pages && git push -f origin gh-pages`).
- **Pin badges to one effect** regardless of the leader: envs `badge_mon`
  (3) and `badge_chroma` (4), e.g. `.\tools\flash-all.ps1 -Env badge_chroma`.
- **Watch badges:** `.\tools\watch-badges.ps1 -Seconds 60` summarises fps,
  rx rate, relays and stalls across every attached badge.

## Hardware facts that cost time

Ports are resolved by USB identity (`tools/pick-port.py`), not by ttyACM number:
the badge is a CH343 bridge (1A86:55D3), the leader is the S3's own USB
(303A:1001). Before that existed, the badge env hardcoded `/dev/ttyACM0` -- which
with the leader plugged in first IS the leader.

`WiFi.setSleep(true)` is a no-op that only logs when it dislikes the current
mode. Call `esp_wifi_set_ps()` directly and check the result; a skipped call
here shows up as an `abort()` in `coex_core_enable()` three lines later.

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
