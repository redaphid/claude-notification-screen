# Event runbook (packed 2026-09-04)

What the bag does, how to drive it, and what to check when it does not.

## What is in the bag

Five badges (Waveshare ESP32-S3-LCD-1.28), all on `follower` commit 9691f07,
badge env `waveshare_esp32s3_lcd128`:

| badge | MAC tail | crest |
|---|---|---|
| A | 6F29D0 | kiku (chrysanthemum) |
| B | 6F2AC8 | tomoe (comma swirl) |
| C | 6EFD7C | kikyo (bellflower) |
| D | 6F2ACC | ume (plum) |
| E | 85DCF8 | hakkaku (eight-point star) |

The crest is chosen from the MAC in `src/main.cpp`, so a badge keeps its
crest across reflashes. The Linux bench's badge (85DC30) is mokko.

## What a badge does on its own

1. Boot: RED, GREEN, BLUE self-test cards, then a raw-sprite card. If you
   see those, panel and backlight are fine.
2. It then shows its crest in **chroma** (ChromaDepth: red near, violet far,
   glasses make it 3D), calm, no conductor needed.
3. When it hears a conductor it follows the packet's shader byte, so the
   leader decides what everyone shows. The features (bass, mid, treble,
   energy, beat) come from the conductor: the crest zooms with energy and
   bass, glows with energy, kicks and pushes toward the viewer on the beat.
4. If the conductor goes silent it fades to stillness over about 600 ms and,
   after 10 s of silence, goes back to the calm chroma crest.
5. A badge whose radio fails still renders. A badge that boot-loops three
   times skips the radio and renders locally.

## Driving the swarm

**The leader** (the 1.46 board with the microphone) broadcasts what it hears
at ~30 Hz. Its shader byte picks the effect for every badge.

- Open https://redaphid.github.io/claude-notification-screen/leader.html in
  Chrome or Edge (laptop or Android phone with a USB-C cable). It works
  offline once it has been opened once; Chrome offers Install.
- Plug the leader in, tap Connect, pick its port. The page sends `?` and
  shows a button per effect the firmware knows.
- Or any serial terminal at 115200 with DTR asserted, one command per line:

```
?              list effects and the current one
mon            named crests, coloured per crest
chroma         crests as ChromaDepth depth maps (index 4)
plasma  tunnel  iris
shader 4       by index
next  prev
cycle 20000    auto-advance every 20 s; cycle 0 holds
```

The leader's own panel shows the same effect it broadcasts. If the leader's
firmware predates the `chroma` effect it answers "unknown command"; merge
`follower` into `main` and reflash it (`pio run -e conductor -t upload` on
the Linux bench).

**No leader?** Hold BOOT on any badge while resetting it: that badge becomes
the conductor with a built-in 118 BPM mock DJ and takes the same serial
commands over its own USB port (CH343, 115200). Everyone else follows it.

## Effects (shader index)

0 plasma, 1 tunnel, 2 iris, 3 mon, 4 chroma. The registry is append-only;
new ones land at 5 and up. `docs/effects.md` says how to add one.

## When something is off

- **Black screen, no cards at boot:** power. Try another cable or bank; the
  radio bring-up is the biggest current spike.
- **Cards show, then black:** the effect buffer failed to allocate; serial
  shows `FATAL`. Power-cycle.
- **Badge shows crest but ignores the leader:** it is not hearing packets.
  Check the leader is transmitting (its console prints tx counts), that both
  are on WiFi channel 1 (a bench build may have `-DBADGE_WIFI_CHANNEL`), and
  range. The badge HUD prints `rx:` per second; a frozen count means no
  packets.
- **All badges snap between effects on their own:** two conductors are on
  the air. Only one leader, or only one badge with BOOT held.
- **Leader console ignores typed commands:** DTR is not asserted (native USB
  CDC needs it). The PWA asserts it; in Python set `dtr = True` before open.
- **Reflash a badge from Windows:** plug it in, run `.\tools\flash-all.ps1`
  from the repo; it builds, flashes every attached badge in parallel, and
  verifies fps over serial. `tools/flash.ps1` is the one-at-a-time loop.
- **Recovery gesture:** hold BOOT, tap RESET, release BOOT puts a board in
  download mode.

## Known limits

- The two benches' clocks differ by ~57 minutes; cross-bench logs are
  anchored on the conductor's sequence reset, not wall clocks.
- Delivery across two benches measured 71 to 81 percent of packets; every
  badge heard the leader directly and relays worked to hop 2. Range at a
  real venue is unmeasured.
- Battery life on the badges is unmeasured.
- The ESP Web Tools flasher on the same Pages site has no firmware staged
  yet; badge recipients cannot self-flash from the web until
  `scripts/build-web-release.sh` output is published to `gh-pages`.
