# Chorus — a swarm of audio-reactive badges

Wearable round-LCD badges that render audio-reactive visuals **in sync with each
other**, over no infrastructure at all: no wifi, no router, no phone, no
pairing. Switch them on and they agree.

One badge — the **conductor** — has a microphone. It listens to the room, runs
lightweight audio analysis, and broadcasts a 24-byte feature packet at 30Hz over
ESP-NOW. Every other badge is **deaf**: it receives the packet, relays it onward
so a dense crowd becomes a good radio topology rather than a bad one, and renders
the visuals locally on its own screen.

Synchrony is the product. One listener, one truth, everyone breathing together.

The aesthetic lineage is [Paper Cranes](https://visuals.beadfamous.com) — the
same audio-reactive shader language, reduced to what a 240×240 round LCD driven
by an ESP32-S3 can do.

## Why one microphone

Cost is the small reason. The real one: twenty independent listeners produce
twenty slightly-different analyses, and *close* to synchronized reads as broken.
Redundancy comes from role, not hardware — any badge *could* conduct, a phone
could conduct, the conductor is just the one born with ears.

## Why features and not frames

BLE cannot carry video, and wifi frame-streaming murders batteries and caps the
swarm at hotspot limits. A raw 240×240 frame is 115KB; the feature packet is 24
bytes. So the badges each render locally from a shared description of the music,
which is also why a dropped packet degrades into a slow exhale instead of a
stutter.

## The two frozen contracts

These headers are the API between workstreams. Changing either one means
coordinating every stream at once.

**`src/chorus_packet.h`** — the 24 bytes every badge speaks: magic `"CRNS"`,
`uint16 seq` (dedupe, wrap-safe), `uint8 hop` (relay, max 3), `uint8 shader`
("everyone switch to 3"), `float features[4]` = bass/mid/treble/energy, 0..1.

**`effects/effect.h`** — `(features, time) -> 240×240 RGB565`. Pure C99, no
floats in the per-pixel loop, LUTs allocated once, budget ~25ms/frame against a
~12ms SPI blit. The same source compiles for the badge **and** for the desktop
harness, so visuals are developed and watched on a laptop with no hardware
attached.

## The idea that matters most: detect events, synthesize responses

The visuals used to lag the audio by about a second. The lag was never transport
— it was **smoothing used as a bandage for shudder**. Raw features jitter, jittery
features make visuals strobe, and the only knob anyone had was "smooth harder".

The root cause is that one signal was serving as both a *measurement* (which
wants fidelity) and an *animation driver* (which wants continuity) — opposing
requirements. The fix, borrowed from drum machines and lighting rigs: an onset
detector fires discrete triggers, and the renderer draws a *designed*
attack-decay envelope that cannot shudder because nothing noisy remains in its
path.

Which is why **the badge does not smooth what it receives.** The conductor ships
envelopes that are already the animation; filtering them again would reintroduce
exactly the lag that shaping them removed. The one time-based term on the badge
is `presence`, which fades a badge out over ~600ms when it stops hearing a
conductor — a designed release, not a filter. A badge that walks out of range
exhales rather than freezing on its last packet.

## Hardware

**Badges:** Waveshare ESP32-S3-LCD-1.28 (SKU 26541, non-touch). ESP32-S3 dual
LX7 @240MHz, 2MB PSRAM, 16MB flash, 240×240 round GC9A01 IPS over SPI, QMI8658
IMU, USB-C via a CH343 bridge.

**Conductor:** Waveshare ESP32-S3-Touch-LCD-1.46 — same core, 8MB PSRAM,
412×412 round IPS, onboard microphone and speaker.

### Pin map (official Waveshare wiki, verified on the bench)

| Function | GPIO |
|---|---|
| LCD CLK / MOSI / DC / CS / RST | 10 / 11 / 8 / 9 / 12 |
| **LCD backlight** | **40** |
| I2C SDA / SCL (QMI8658 IMU) | 6 / 7 |
| IMU INT1 / INT2 | 47 / 48 |
| Battery ADC | 1 (V = 3.3/4096 × 3 × raw) |
| BOOT button | 0 |

Three details cost real time, so they are worth stating plainly:

- **Backlight is GPIO40 on this SKU, not 2.** GPIO2 is the *touch* variant. A
  wrong backlight pin looks exactly like a dead panel: the driver initializes
  fine, the screen stays black.
- **Reset is GPIO12, not 14.** The 14 in circulation is wrong for this board.
- **`invert = true` is required** or the IPS panel renders colour negatives —
  a "working but wrong" state that tempts you into rewriting correct code.

We use **LovyanGFX rather than TFT_eSPI** on purpose: TFT_eSPI is configured by
editing a header *inside the library folder*, which is invisible in the repo and
easy to lose. The whole panel config lives in `src/display.h`.

## Layout

```
src/          badge firmware (role chosen at boot; ESP-NOW receive + relay + render)
effects/      visuals, pure C99, shared byte-for-byte with the harness
harness/      desktop preview: renders to GIF, no hardware, no SDL
conductor/    Stage 2: mic -> FFT -> Welford normalization -> broadcast
web/          ESP Web Tools flasher page for badge recipients
scripts/test/ bench tools (see below)
docs/         bench logs and proposals
```

## Build and flash

```sh
pio run                      # badge firmware (the default env)
pio run -t upload            # flash the badge
pio run -e conductor         # conductor for the 1.46 (compile-verified only)
pio run -e conductor_fake    # same chain on synthesized audio, any ESP32-S3
make gif EFFECT=plasma       # render an effect to a GIF, no hardware needed
```

Bench build flags, for when nobody is in the room to hold the BOOT button:
`-DBADGE_FORCE_CONDUCTOR`, `-DBADGE_SKIP_RADIO`, `-DBADGE_TX_POWER=...`.

### Roles

Hold **BOOT** while the board resets and that badge becomes the conductor.
Release it and the badge is a receiver. One binary for every board in the bag.

## Bench tools

Hardware debugging here is mostly about *seeing* what a board is doing when
nobody is holding it.

- **`scripts/test/serial-watch.py`** — reads the serial port *across resets and
  USB re-enumerations*. When a board browns out it takes the USB bridge with it,
  and plain `cat /dev/ttyACM0` dies with the port, making a reset loop look like
  silence. This makes it legible.
- **`scripts/test/flash-and-verify.sh`** — flashes, captures serial, and
  photographs the screen with a webcam, retrying while the USB bridge comes and
  goes. Unattended proof that a change actually reached the panel.
- **The boot self-test stays in the shipping firmware** — RED/GREEN/BLUE cards
  and a raw-sprite card. It is the only way to separate a dead backlight from a
  dead panel from a wrong colour order without instruments, and it is readable
  through a webcam from across a room.

## Field behaviour worth knowing

Most of these badges get **given away**, so the failure modes that matter are
the ones a stranger would experience.

- **A badge whose radio fails still renders.** Radio init failure is not fatal;
  the badge runs its own local heartbeat. A giveaway badge that looks dead is
  worse than one that is merely alone.
- **A badge caught in a boot loop escapes it.** A boot-attempt counter in flash
  (not RTC memory, which a power loss wipes) makes the badge skip the radio
  after three failed boots and render locally.
- **The radio comes up before the backlight.** Bringing up wifi is the largest
  current spike the board makes; doing it before the panel draws anything is
  free headroom on a tired power bank.
- **Recovery gesture:** hold BOOT, tap RESET, release BOOT puts the board in
  download mode. If the host's `cdc_acm` wedges — esptool reporting `Errno 71`
  or `Errno 110` while the device still enumerates — replug the cable; there is
  no software fix without root.

## Status

**The swarm works.** Two boards, end to end: the leader's microphone hears the
room, analyzes it, broadcasts over ESP-NOW at 30Hz, and the badge renders it at
31 fps. Verified on hardware, watched through a webcam.

Verified: panel, backlight, colour order, sprite byte order, effect render path,
frame budget, radio bring-up, PSRAM, ESP-NOW transmit *and* receive, mesh relay,
sequence dedupe, conductor restart recovery, and the I2S microphone chain on the
1.46 — mic pins BCLK 15 / WS 2 / DIN 39 were right on the first try, and the
analysis costs 1.67ms average against a 16ms hop.

Not yet verified: more than two boards at once (relay hop counting has never had
a third node to hop *through*), range and body absorption at any real distance,
battery operation, and an actual flash from the web page.

### The bug that only two boards could find

The badge's receive counter froze at exactly 2434 and its screen went dark while
the leader transmitted happily and reported no send failures.

Sequence numbers restart at zero when the conductor reboots — a battery swap, a
reset, a crash, a reflash. A wrap-safe "is this newer?" dedupe then rejects
*every* packet until the counter climbs back past where it left off: over a
minute of dead swarm for a conductor that had been running two minutes, and
closer to half an hour in the worst case. At a rave, somebody swaps the
conductor's battery and every badge in the field goes dark with no indication
why.

It was diagnosed by prediction rather than inspection — if the theory was right,
reception should resume on its own the moment the counter passed 2434. It did.
The fix also accepts a packet when nothing has been heard recently, or when its
sequence is far enough behind to be a new epoch rather than a reordered
straggler; the narrow reorder window still suppresses relay storms. Verified by
restarting the conductor mid-stream: reception continued through it, the only
gap being the conductor's own boot time.

See `docs/bench-log-2026-09-02.md` for the earlier bench findings, including a
correction about which power problems were the badge and which were the host.
