# ADR-002: A phone can conduct the swarm over BLE, at a measurable cost

**Status:** implemented and verified on hardware, on branch
`feat/phone-conductor-ble`. Not merged to `main`.
**Question:** can a phone connect to a badge over BLE and act as the microphone
when there is no leader?

## Answer: yes, and it works end to end — but it is not free, and it is not for every badge

Verified on real hardware, not designed on paper:

- A badge advertises as `Chorus-DC30` and is discoverable from a separate
  machine.
- A client connected and wrote **750 feature frames at 30.0/sec, all 750
  accepted** — no loss over BLE.
- The badge reported `role=PHONE-LED`, and **broadcast to the swarm at ~32
  packets/sec** over ESP-NOW while doing it.
- With a real conductor on the air, the same badge reported
  `role=RECEIVER (a real conductor is on air)` and left the phone on standby,
  which is the intended precedence.

## The cost, which is the part worth knowing

**BLE and ESP-NOW share one radio, and coexistence is not free.**

The firmware originally called `WiFi.setSleep(false)` so ESP-NOW reception would
never miss a packet to modem sleep. With that setting, bringing up BLE **aborts**:

```
abort() at coex_core_enable  <- coex_enable <- esp_bt_controller_enable
                             <- NimBLEDevice::init <- phoneLinkBegin
```

That is from a decoded backtrace on hardware. WiFi/BT coexistence can only work
if WiFi yields airtime, so a badge that can be conducted by a phone **must** let
WiFi sleep, and pays for that in ESP-NOW reception.

**Therefore the phone link is a separate build (`[env:badge_phone_link]`), not a
default.** A giveaway badge should not carry a BLE stack, or that reception cost,
for a feature it will never use. Static RAM: 62.6KB without, 68.5KB with.

## The design: a phone is a conductor, not a special case

The phone does not talk to the swarm. It talks to **one badge**, which turns the
8-byte BLE frames into ordinary `ChorusPacket`s. Every other badge relays and
renders exactly as it always does and never learns a phone was involved. One
listener, one truth — the same principle as one microphone.

Frames are 8 bytes, not the 24-byte `ChorusPacket`: features are quantised to a
byte each because nobody can see 1/255 steps of bass on a 240px disc. Writes are
**without response**, for the same reason ESP-NOW broadcast has no ACK — at 30Hz
an acknowledgement per frame costs more than dropping one that is stale in 33ms.

### Arbitration always favours the swarm

A real conductor on the air wins, every time. A phone takes over only after
`PHONE_LINK_YIELD_MS` (2s) of silence. This is not politeness — two sources
driving one swarm would tear it, and because their sequence numbers come from
different counters, **each would look to the other like a conductor that had just
restarted**, so they would trigger each other's resync logic endlessly rather
than blend.

The badge tells the phone which of these is happening, over a status
characteristic, so the page can say "you are conducting" or "a leader is present,
you are on standby". A phone that looks connected while being silently ignored is
the failure this prevents.

## Known limitations, stated rather than hidden

**iOS cannot do this from a web page.** Safari does not support Web Bluetooth.
Chrome on Android works; an iPhone needs a native app or a browser such as
Bluefy. For a rig handed to strangers, that is a real constraint, not a footnote.

**Two phones on two badges in a silent room will split the swarm.** Both become
conductors, and nothing arbitrates between them. The packet carries no source
identity to break the tie and it is frozen, so this is documented rather than
solved. If it matters, the v2 packet should carry a source id and a priority —
see `docs/chorus-packet-v2-proposal.md`.

**Reception cost: measured, and smaller than expected.** Same badge, same leader
transmitting at ~32 pkt/s, three builds back to back, 30 samples each:

| build | ESP-NOW reception |
|---|---|
| plain badge (no BLE) | **28.9 pkt/s** |
| BLE advertising, no phone connected | **28.9 pkt/s** |
| BLE + phone writing at 30Hz | **28.8 pkt/s** |

**There is no measurable cost at this range.** An earlier draft of this document
asserted the phone build "pays for it in ESP-NOW reception". That claim was
reasoning from the mechanism -- coexistence forces modem sleep, therefore
reception must suffer -- and the measurement does not support it. The mechanism
is real; the consequence was assumed. Corrected here rather than quietly
softened, because the difference between "we measured" and "it stands to reason"
is the whole point of having hardware on the desk.

**The caveat that remains:** this was measured with the badge a few feet from the
leader, where the link has margin to spare. Modem sleep costs airtime, and
airtime matters most when signal is marginal. **At range, or through a crowd,
the cost could be real and this measurement would not have caught it.** Anyone
considering BLE as a default should repeat it at distance before doing so.

## Bench tooling this produced

- `scripts/test/phone-sim.py` — speaks the identical GATT contract from a laptop,
  so the firmware can be tested without a phone in the room, and a failure can be
  localised to firmware or page rather than "BLE doesn't work".
- `-DBADGE_WIFI_CHANNEL=n` — moves a badge off channel 1 to isolate a bench when
  two benches in radio range would otherwise count each other's packets. Bench
  use only: at camp everything must be on channel 1, because nothing negotiates
  and a badge on the wrong channel is simply deaf with no error to say why.
- `-DCONDUCTOR_SILENT` — the conductor analyses and draws but transmits nothing,
  for handing the channel to another bench or testing the phone path.
