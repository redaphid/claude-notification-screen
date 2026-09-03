# Pending vault append

Destined for the vault note `paper-cranes-medallion/esp32-badge-swarm-architecture.md`
(the project's master context file). The markdown-vault MCP server returned 502
on every call across the whole session, so this is staged here to be appended
verbatim when it is back. Delete this file once it has landed.

---

## Bench session, 2026-09-02 evening — Stage 1 RUNNING ON HARDWARE

Verified on a real board with a webcam pointed at it. Full write-up in the
firmware repo at `docs/bench-log-2026-09-02.md`. PRs: firmware
redaphid/claude-notification-screen#1, hypnosound#18 and #19, paper-cranes#138.

**Stage 1 is done.** A ported plasma renders on the round panel at **32 fps**,
driven by feature envelopes, with ESP-NOW up, transmit confirmed
(`tx ok 33, fail 0`), and 1.97MB PSRAM free. The bring-up section was right on
every point: **GPIO40** backlight, **RST 12**, **`invert = true`**, LovyanGFX
with in-sketch config, 40MHz SPI.

**Correction to the fleet notes: the board previously suspected defective is
fine.** The bench unit reports USB serial `5B5F000273` — the one recorded as
"screen never lit under any firmware tried." It lights perfectly once the
backlight pin is right. Do not RMA it; treat that suspicion about any other unit
as a pin question until proven otherwise.

**Sprite byte order, proven by experiment:** LovyanGFX 16-bit sprites store
**byte-swapped RGB565**. Raw `uint16` words written into `getBuffer()` display
correctly only when packed swapped — which matters because effects write pixels
directly into the sprite for speed.

### Power: what is established and what is not

Bringing the radio up **before** the panel and its backlight made boots reliable
and repeatable across many flashes; with the old ordering it failed every boot.
That ordering is cheap and right regardless — badges run off power banks and
flat LiPos, where the headroom argument holds on its own.

**Important caveat, discovered at the end of the session: the laptop driving the
bench was itself losing power**, and had been dropping into a low-power state
before that. So the more dramatic claim — *"a badge cannot conduct on USB power
because sustained 30Hz transmit collapses the rail"* — is **not trustworthy**.
That symptom was observed on a failing host and may have been the laptop, not
the badge. The reduced-TX-power result (survives tens of seconds at 2dBm, dies
immediately at 11dBm) was measured against the same unstable host.

To settle it: power a badge from a wall charger or charged power bank, flash the
conductor build, and watch. **Do not design around "badges cannot conduct" yet.**
General lesson: a symptom appearing at the moment of a current spike is not proof
of where the current went — board, cable, host port and host are all in that
circuit.

Host-side, `cdc_acm` can wedge (`Errno 71`, then `Errno 110` on every esptool
reset) while the device still enumerates: no software recovery without root,
replug the cable.

### Field mitigations that stand on their own

- **A badge whose radio fails still renders** its own local heartbeat; radio init
  failure is non-fatal. A giveaway badge that looks dead is worse than one that
  is merely alone.
- **A boot-loop counter in flash, not RTC memory.** RTC survives a panic but not
  a power loss — exactly the failure it exists to escape, and measurably it
  never counted past 1. In flash it works: counted to 1, cleared after three
  seconds of steady rendering.
- A real null dereference found by panic backtrace: `esp_now_send()` was called
  on the conductor path without checking that init succeeded, so a conductor
  whose radio failed would panic instead of degrading.
- Broadcast cadence aliasing: a 33ms interval checked once per rendered frame at
  31fps is satisfied only every second frame, silently halving the packet rate
  to ~16Hz.

### Second frozen contract

`effects/effect.h`: `(features, time) -> 240x240 RGB565`, compiling unchanged for
the badge and a desktop harness, so effects are developed and *watched* on a
laptop with no hardware. Three effects ported, measured by instruction count on
the actual xtensa objects: **plasma ~5.8ms, iris ~8.8ms, tunnel ~9.4ms** per
frame at 240MHz against a 25ms budget. The shared polar LUT lives in PSRAM — a
115KB sprite plus a 115KB LUT plus the WiFi stack does not fit in the S3's 320KB
internal RAM.

### Design decision worth preserving: do not smooth twice

The badge does **not** smooth received features. The conductor already ships
designed attack-decay envelopes; filtering them again reintroduces exactly the
lag that shaping them was meant to remove. The only time-based term is
`presence`, fading a badge out over ~600ms when it stops hearing a conductor — a
designed release, not a filter.

### Conductor stream (Stage 2, compile-verified, never on hardware)

- **I2S mic pins for the 1.46**, from Waveshare's own `MIC_MSM.h` cross-checked
  against two wiki tables: **BCLK 15, WS 2, DIN 39**, standard I2S (not PDM).
  Which stereo slot the mic drives is undocumented, so the firmware listens to
  both at boot and takes whichever moves.
- Waveshare's demo targets Arduino core 3.1.1 and `ESP_I2S`, which does not
  exist in 2.0.11 — pin numbers carry over, the API calls do not.
- 1024-point window (64ms), 256-sample hop, 16kHz: big window for bass
  resolution, short hop for onset timing. **Kick-to-packet ~27ms typical.**
- **Wind:** a 2-pole 40Hz high-pass was nowhere near enough — gusting produced
  **53 false onsets** and drove bass to 0.67, because venue-adaptive
  normalization z-scores a noise floor up to full scale in a quiet field.
  Sixth-order Butterworth helped but did not fix it. The fix was a silence gate
  keyed on spectral flux, which separates music from wind by ~100x. Wind now
  yields **0 onsets** — the Iceland-wind problem solved before Iceland.

### Onset detection landed in hypnosound, not paper-cranes

Prior art existed on two **unmerged** hypnosound branches
(`feat/onset-detection` → `fix/onset-validation-defects`) with ground-truth
validation (P=1.0, R=1.0 on 90/120/128 BPM grids); the new work builds on those
rather than duplicating them.

The **envelope generator** went into hypnosound alongside the detector, on the
argument that the envelope *is* the aesthetic: if paper-cranes owned it, the
badges would reimplement it and badges and projector would render different
curves for the same music. Band choices and uniform names stayed in paper-cranes
as consumer policy.

Measured rather than asserted: on a click track buried in noise, raw spectral
flux reverses direction on **424 of 598 frames** while the synthesized envelope
reverses **18** times — one peak per beat, each within two frames of its beat.
Tuning: `attackMs 12`; `decayMs` kick 220 / snare 150 / hat 90 / full 140;
`releaseRatio 0.7`; `refractoryMs` kept at the existing validated 120 rather than
changing a ground-truth-validated constant without evidence. In paper-cranes the
detector runs on the main thread against `fftData` the moment it leaves the
AnalyserNode — ahead of the worker hop and the EMA — and onset keys are merged at
read time so the smoothing loop can never touch them.

### Still open

Two badges have never been in the same room with the radio on — relay, dedupe and
hop counting are single-board tested only. The conductor has never run on the
1.46. The web flasher has never actually flashed a board. And the
conductor-on-USB question needs a known-good power supply to settle.
