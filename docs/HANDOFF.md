# Handoff — zod2 (the bench with the boards), 2026-09-05

This machine is shutting down. It is the one with the hardware physically
attached, so anything below marked "verified" was watched on real boards through
a webcam, and anything not marked that way was not.

## Hardware state as I left it

| board | port | firmware | state |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.46 ("leader") | /dev/ttyACM0, native USB | `-e conductor` from `main` | boots on **chroma**, mic live, transmitting ~32 pkt/s on channel 1 |
| Waveshare ESP32-S3-LCD-1.28 ("badge") | /dev/ttyACM1, CH343 | `-e waveshare_esp32s3_lcd128` from `main` | receiver, 28 fps, `fx chroma` |

**When this laptop powers off, the swarm loses its conductor.** Both boards are
USB-powered from it. The Windows bench's five badges have been following this
leader, so they will stop receiving, fade out over ~600ms (`presence`), and
revert to their own default effect after `DEFAULT_REVERT_MS`. That is designed
behaviour, not a fault — but if someone is watching those badges go blank, this
is why.

## What is verified on hardware

- Panel, backlight (GPIO40), colour order, sprite byte order, PSRAM.
- Badge render loop at 28-32 fps; effects plasma / tunnel / iris / mon / chroma.
- Leader mic → 4-band FFT → Welford → onset → ESP-NOW at ~32 pkt/s; DSP ~2ms
  against a 16ms hop; analysis 62 hops/sec with the panel drawing on core 0.
- Leader's 412x412 SPD2010 QSPI panel, full-screen, ~24 fps.
- **Mesh relay with a node to hop through** — the Windows bench measured a hop-2
  path (COM7 → COM6 → this leader), clean neighbour tables, no relay storm.
- **Conductor restart recovery** — `resyncs` incremented on four independent
  badges and reception continued across a restart.
- **Phone-as-conductor over BLE** — 750 frames at 30/sec all accepted, badge
  reporting `PHONE-LED` and broadcasting at ~32 pkt/s.
- BLE costs no measurable ESP-NOW reception **at desk range** (28.9 vs 28.9 vs
  28.8 pkt/s). See the caveat in ADR-002; this was never tested at distance.

## What is NOT verified

- The phone web page (`web/phone/`) has never talked to a real badge. Everything
  from `requestDevice()` onward is untested: scan filter, 30Hz writes, status
  notifications, reconnect. Its byte encoding **is** tested against the real
  struct.
- iOS cannot run it at all — Safari has no Web Bluetooth.
- The web flasher has never actually flashed a board.
- Range: everything here was two boards a few feet apart. The only real range
  data came from the Windows bench (71-81% delivery between rooms).
- Three or more boards *on this bench* — the mesh result came from theirs.

## Open items, most important first

1. **Version skew shows the wrong effect silently.** A badge whose firmware has
   fewer effects than the conductor wraps the shader byte
   (`effects_by_index()` is deliberately modulo, so a corrupt byte cannot pick
   garbage) and renders something wrong with no error. Observed: a 4-effect badge
   rendered `plasma` while the leader broadcast `chroma` (4 % 4 = 0). Worth
   making unmistakable — a solid colour for an out-of-range byte beats quietly
   showing the wrong thing.
2. **gh-pages firmware publish is unresolved and needs a human decision.**
   `web/firmware/` is gitignored, so a plain `git subtree split --prefix=web`
   publishes the page *without* binaries; they must be force-added onto the
   gh-pages commit. The Windows bench's leader PWA is already live on that
   branch, so do not force-push blindly — people may have it installed.
3. **The vault note was never updated.** The markdown-vault MCP returned 502 for
   the entire session. The exact text is staged in
   `docs/vault-append-2026-09-02.md`; paste it into
   `paper-cranes-medallion/esp32-badge-swarm-architecture.md` and delete the file.
4. **BLE at range** — repeat the reception measurement far enough away to matter
   before anyone considers making BLE the default.
5. **A v2 packet** would solve two documented problems: no source identity (two
   phones on two badges split the swarm) and no version field (see item 1).
   Proposal in `docs/chorus-packet-v2-proposal.md`. The current 24-byte
   `ChorusPacket` is frozen; changing it needs agreement in `CONVERSATION.md`.

## Working with the hardware, the things that cost time

- **The leader's console needs DTR asserted.** On the 1.46's native USB CDC,
  writing with DTR low produces no response and looks exactly like a broken
  parser. In Python: `s.dtr = True` before `open()`.
- **Do not use esptool to "reset" the 1.46.** `--after hard_reset` leaves it in
  download mode (`boot:0x0 DOWNLOAD`), off the air until re-flashed. Re-flash to
  recover; that is the reliable restart.
- **`scripts/test/serial-watch.py`** reads across resets and USB
  re-enumerations. Plain `cat /dev/ttyACM0` dies with the port and makes a boot
  loop look like silence.
- **`scripts/test/flash-and-verify.sh`** flashes, captures serial and
  photographs the screen unattended.
- **`scripts/test/phone-sim.py`** speaks the BLE contract from a laptop, so the
  firmware can be tested without a phone.
- **The boot self-test stays in shipping firmware on purpose** — it is the only
  way to separate a dead backlight from a dead panel from a wrong colour order
  without instruments, and it reads through a webcam.
- Bench-only flags: `-DBADGE_WIFI_CHANNEL=n` (isolate a bench),
  `-DCONDUCTOR_SILENT` (leader analyses but transmits nothing),
  `-DBADGE_FORCE_CONDUCTOR`, `-DBADGE_SKIP_RADIO`, `-DBADGE_TX_POWER`.

## Cross-machine coordination

`CONVERSATION.md` on `main` is the channel; it documents its own protocol.
**Append only, and on a conflict keep both entries in timestamp order** — never
drop a message to resolve one.

**Our clocks disagree by about 57 minutes.** Anchor on events, not timestamps.
The relay agent that was polling `main` from this machine dies with it, so from
now on the Windows bench's entries will go unanswered until someone here reads
them.

## The judgement call worth repeating

The single most useful thing that happened was the other machine measuring my
conductor from across a room. My own counters looked healthy — 62 fps analysis,
DSP inside budget, tx incrementing, zero send failures — while the conductor was
actually broadcasting at 21.6 Hz instead of 30, because a millisecond cadence
checked once per analysis hop silently rounded up to the next whole hop. Four
badges elsewhere reporting ~15 packets/sec is what exposed it.

**A local counter that reports what you expect is not evidence. An independent
receiver is.**
