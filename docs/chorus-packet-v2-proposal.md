# ChorusPacket v2 — a proposal, not an implementation

**Status: recommendation for the coordinator. Nothing in `conductor/` implements this.**
`src/chorus_packet.h` is frozen at 24 bytes and the conductor firmware speaks v1
exactly as written.

## What v1 costs us

v1 is four normalised floats, a sequence number, a hop count and a shader id:

```c
struct ChorusPacket {          // 24 bytes
  char     magic[4];           // "CRNS"
  uint16_t seq;
  uint8_t  hop;
  uint8_t  shader;
  float    features[4];        // bass, mid, treble, energy
};
```

It carries **state**. It cannot carry **events**, and events are the whole point
of the redesign. The conductor detects an onset with about 25 ms of latency and
100 ms of refractory hysteresis, and then has no way to say so. `effects/effect.h`
asks for exactly the two things v1 cannot send:

```c
uint8_t beat;      // 1 only on the frame an onset fired
float   beat_env;  // 1.0 at onset decaying to 0.0
```

### The workaround in use today

The conductor expresses onsets by **shaping the envelopes it transmits**: an
onset drives `energy` to 1.0 in a single frame and releases it on a designed
decay curve, and drives whichever of bass/mid/treble owned the transient in
proportion to its share of the spectral flux. So a badge can reconstruct both
fields approximately:

```c
in.beat     = (features[FEAT_ENERGY] - prev_energy) > 0.25f;
in.beat_env = features[FEAT_ENERGY];   // already an attack-decay shape
```

This is good enough to ship and it is what the badge stream should implement
now. Its limits are real, though:

- A hard beat during an already-loud passage produces a smaller jump, so the
  rising-edge test misses some beats. Recall is maybe 80–90%, not 100%.
- A badge that drops a packet (or gets one out of order via a relay path) sees a
  jump that never happened, or misses one that did. `seq` lets it detect the gap
  but not repair it.
- The badge cannot tell an onset from a fast crescendo.
- Beat *phase* is unavailable, so nothing can anticipate. Every effect is
  reactive by construction.

### Considered and rejected: reusing `shader`

`shader` is a `uint8_t` that in practice needs about three bits, and the spare
bits could carry a beat flag. Rejected: it silently changes the meaning of a
field in a frozen contract, so a badge built against the old header would read
beats as shader changes and strobe through effects on every kick. If the
contract is going to be opened it should be opened honestly.

## Proposed v2 — 32 bytes

```c
struct __attribute__((packed)) ChorusPacketV2 {
  char     magic[4];        // "CRN2" -- a NEW magic, see note below
  uint16_t seq;
  uint8_t  hop;
  uint8_t  shader;
  float    features[4];     // bass, mid, treble, energy   (unchanged, 0..1)
  // --- new in v2 ---------------------------------------------------------
  uint8_t  triggers;        // bit0 onset, bit1 kick, bit2 hat/treble onset,
                            // bit3 downbeat, bit4 drop/breakdown, bits5-7 spare
  uint8_t  beat_env;        // 255 at onset, decaying; /255.0f -> EffectInput.beat_env
  uint8_t  bpm;             // estimated tempo, 0 = unknown (60..255 covers everything)
  uint8_t  beat_phase;      // 0..255 across the current beat, 255 = unknown
  uint32_t t_ms;            // conductor's monotonic clock at capture
};
static_assert(sizeof(ChorusPacketV2) == 32);
```

Why each field earns its bytes:

- **`triggers`** — the one genuinely load-bearing addition. Turns "infer the
  event from the envelope" into "the event was sent". Separate bits for kick and
  hat let an effect respond differently to low and high transients, which the
  single blended `energy` channel cannot express at all.
- **`beat_env`** — one byte of the decay envelope, so the badge does not have to
  reuse `energy` for two jobs. 8 bits is plenty for an animation ramp.
- **`bpm` + `beat_phase`** — these are what make effects **predictive** rather
  than reactive. With phase, a badge can start a swell 80 ms *before* the next
  beat and land exactly on it, which is the difference between a swarm that
  follows the music and one that plays along with it. This is the single biggest
  perceptual upgrade available and it costs two bytes.
- **`t_ms`** — the conductor's capture timestamp. Lets a badge measure its own
  end-to-end latency, lets relays be ordered properly across paths, and makes
  the whole system debuggable at camp with no laptop. Four bytes is generous;
  `uint16_t` of milliseconds-mod-65536 would also work if 32 bytes is somehow a
  hard ceiling.

### Migration notes

- **Change the magic to `"CRN2"`.** The magic exists to make version skew
  detectable; keeping `"CRNS"` at a new length turns a clean rejection into a
  garbage read. `chorusPacketValid()` already checks length, so a v1 badge
  ignores a v2 packet automatically, and vice versa. Field for field, everything
  in v1 keeps its offset and meaning, so a conductor can broadcast **both**
  packet formats during a transition — 30 Hz of 24 bytes plus 30 Hz of 32 bytes
  is still under 2 kB/s and ESP-NOW will not notice.
- 32 bytes is well inside the 250-byte ESP-NOW payload limit; there is no
  fragmentation risk and no meaningful airtime cost.
- `bpm` and `beat_phase` require a tempo tracker (autocorrelation of the onset
  train over a 4–8 s window). That is a conductor-side change of maybe 60 lines
  and it does not block the rest of v2 — ship them as 0/255 ("unknown") until
  the tracker exists.

## Recommendation

Adopt v2 at the next natural break in the schedule, before effects start
accumulating workarounds for the missing beat channel. The rising-edge hack
works, but every effect that ships against it is an effect that has to be
revisited later. If only one field can be added, add `triggers`.
