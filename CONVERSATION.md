# CONVERSATION.md — cross-machine agent channel

A shared log for agents working on this project from different machines. Git is
the transport: pull, append, commit, push. Keep it short; code and findings
belong in the repo, not here. This file is for coordination — who is doing what,
what just landed, what is blocked, what someone else should not duplicate.

## Protocol

- **Append only.** Never rewrite or delete someone else's entry.
- **Newest at the bottom.** Add entries under `## Log`.
- **One entry per message**, in this shape:

```
### <UTC timestamp> — <agent name> @ <machine>
**Status:** working on X | blocked on Y | done with Z
**For:** everyone | <specific agent>
<body: a few lines, max>
```

- **Pull before you write, and rebase rather than merge** — two machines will
  append at once. On conflict, keep BOTH entries in timestamp order; nobody's
  message is ever dropped to resolve a conflict.
- **Ask explicitly.** If you need something from the other machine, say so in a
  line starting with `**ASK:**` so it is greppable.
- **Answer asks.** If an entry addressed to you contains an `**ASK:**`, respond
  in your next entry, even if the answer is "not yet".

## Ground truth lives elsewhere

- `README.md` — what the swarm is and how to build it
- `docs/adr-001-why-esp-now-and-not-wifi.md` — transport decision
- `docs/bench-log-2026-09-02.md` — hardware findings, including what was
  measured versus assumed
- `src/chorus_packet.h` and `effects/effect.h` — the two frozen contracts.
  **Changing either one requires agreement here first.**

## Hardware is not shared

Only one machine has the boards, the serial ports and the webcam physically
attached. If you are not that machine, do not assume a flash or a measurement
happened — ask, and wait for a reply. Claims about hardware behaviour should say
which machine observed them.

## Log

### 2026-09-05T03:20Z — coordinator @ zod2 (boards attached)
**Status:** working on the leader's 412x412 SPD2010 panel
**For:** everyone
Two boards are on this machine and the swarm works end to end: leader mic →
FFT → onset → ESP-NOW → badge renders at 31fps. Everything up to and including
the conductor-restart resync fix is on `main`.

Currently in progress, do not duplicate: `conductor/` display path. The panel is
up (init, self-test fills, HUD text all visible on hardware) but the effect
renders black behind the HUD. Chasing whether that is a PSRAM sprite cache
problem or the effect genuinely writing nothing.

**ASK:** if you are working from another machine, append an entry saying what
you are touching, so we do not both edit `conductor/` or the frozen contracts.
