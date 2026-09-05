# ADR-001: ESP-NOW broadcast for the feature stream, not a leader-hosted WiFi AP

**Status:** decided, with a hybrid recommendation.
**Question:** should the leader run a SoftAP that every badge associates with,
instead of broadcasting over ESP-NOW?

## Answer

**No for the 30Hz feature stream. Yes, optionally, as a side-channel for a phone.**

The two are not mutually exclusive and the interesting design is a hybrid — but
the real-time path should stay ESP-NOW. The reasons below are ordered by how
badly each one bites at the camp, not by elegance.

## 1. The client limit is a hard wall at exactly our fleet size

An ESP32 SoftAP supports a small number of associated stations — the default is
4, and the practical ceiling is around 10. **There are ten badges in the order,
plus a conductor, plus possibly a phone.** So the AP design runs out of room at
precisely the scale this project already bought hardware for, and it fails by
refusing to associate a badge someone is holding, in a field, at night.

ESP-NOW broadcast has no association and therefore no client limit. Fifty badges
cost the conductor exactly what one badge costs: a single transmission.

## 2. Airtime scales with badge count, and the stream is continuous

At 30Hz the leader sends 30 packets a second forever. Over ESP-NOW broadcast
that is **30 transmissions per second total, regardless of fleet size**.

Over WiFi it is one of two bad options:
- **Unicast to each client:** 30 x N transmissions per second. At ten badges
  that is 300/sec plus ACKs, which is real airtime contention in a crowd that
  also contains everyone's phones on 2.4GHz.
- **Multicast/broadcast to associated clients:** sent at the lowest basic rate,
  unacknowledged, and buffered against DTIM for any client in power-save. This
  is the worst of both worlds — the unreliability of broadcast plus the latency
  of association.

## 3. Association is state, and state is what breaks in a field

Everything about the AP model adds state that must be established, maintained,
and re-established: association, DHCP lease, an IP stack, a socket per badge.
Every one of those is a thing that can be in a wrong condition at 2am in a pine
forest with nobody able to debug it.

We already learned this the expensive way. **A conductor restart currently
resyncs in about a second** (see the sequence-epoch fix). Under an AP model, a
conductor reboot means ten badges simultaneously losing their association and
re-associating, with DHCP, while the conductor's AP is still coming up. That is
a multi-second blackout at best, and a reassociation storm at worst — and
battery swaps *will* happen mid-event.

The current design's headline property is that **the rig comes up by being
switched on**, in any order, with no negotiation. That is worth protecting.

## 4. Reliability is the wrong thing to optimize here

The strongest argument for WiFi is that unicast gets ACKs and retries, where
ESP-NOW broadcast gets neither. But a dropped packet in this system is worth
almost nothing: another arrives in 33ms, and the badge is designed to decay
gracefully rather than stutter. We deliberately do not smooth on the badge, and
`presence` fades a badge out over ~600ms if the conductor genuinely goes away.

Paying association overhead, client limits and O(N) airtime to guarantee
delivery of a packet that is obsolete 33ms later is a bad trade.

## 5. What the measurements say so far

From the two-board bench:
- ESP-NOW broadcast at 30Hz: **zero send failures**, receive counter tracking
  the transmit counter, mesh relay working, dedupe working.
- Analysis-to-air is well inside budget: DSP 1.67ms against a 16ms hop, and
  kick-to-packet estimated ~27ms typical.

Nothing measured so far suggests the transport is the constraint. **Range and
body absorption are still unmeasured**, and that is the risk that could actually
change this decision — see below.

## Where an AP genuinely helps, and the hybrid worth building

A SoftAP earns its place for things that are **not** the real-time stream:

- **A phone as controller or conductor.** A browser cannot speak ESP-NOW. A
  captive-portal page served by the leader is a real path to shader selection,
  parameter tweaks, and firmware updates in the field, and it may be a better
  answer than the planned BLE GATT because it needs no app and no pairing.
- **Config and diagnostics.** "Which badges are alive, what is the mic hearing" —
  a status page beats a serial cable at a camp.

**The hybrid:** leader runs ESP-NOW for the stream and raises a SoftAP only when
someone asks for it (a button, or always-on if it proves harmless).

### The gotcha that must be designed around

**ESP-NOW and SoftAP share one radio and therefore one channel.** The stream is
hardcoded to channel 1 (`CHORUS_WIFI_CHANNEL`) precisely because nothing
negotiates at camp. If the leader's AP is moved to another channel — or if the
leader ever *joins* someone's network as a station, which forces its channel to
follow that network — **every badge goes deaf instantly**. Any AP work must pin
the AP to the same channel as the stream, and the leader must never become a
station on a foreign network while conducting.

## If range turns out to be the problem

Before reaching for a different transport, try these in order:

1. **`WIFI_PROTOCOL_LR`** — Espressif's long-range mode, roughly 2x range for
   ESP-to-ESP links. Costs nothing but ESP-only compatibility, which we already
   have. This is the first thing to test.
2. **Raise TX power** on the conductor, which has its own supply and does not
   share the badges' power budget.
3. **Lean on the mesh relay** already implemented: hop counting is in the packet
   and a dense crowd of badges is a *good* topology rather than a bad one. Note
   this is still untested with three or more boards — with two, every relay is a
   leaf.

## Revisit this if

- The fleet needs to exceed what broadcast can serve *and* individually
  addressed behaviour becomes a requirement (per-badge visuals, not one truth).
- Field testing shows packet loss high enough that badges visibly desynchronize,
  and LR mode plus relay do not fix it.
- The phone-as-conductor path becomes primary rather than a fallback, in which
  case the leader is mostly a bridge and its AP is doing real work.
