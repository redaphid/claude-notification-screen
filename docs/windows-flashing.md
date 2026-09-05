# Flashing badges from Windows (2026-09-04)

The bench in `NOTES.md` is Linux (`/dev/ttyACM0`). This is the Windows box that
flashes the giveaway badges in bulk. Everything here lives on branch `follower`;
`main` stays the Linux bench's branch.

## Toolchain

- PlatformIO 6.1.19 via `uv tool install platformio --python 3.12 --with esptool`.
  `pio.exe` lands in `%USERPROFILE%\.local\bin\`. uv does not expose esptool's
  exe, so esptool 5.4 is at `%APPDATA%\uv\tools\platformio\Scripts\esptool.exe`.
- PlatformIO's `tool-esptoolpy` post-install runs `python -m pip`, and a uv
  venv has no pip. Fix: `uv pip install --python %APPDATA%\uv\tools\platformio\Scripts\python.exe pip`.
- The registry mirror `usc1.contabostorage.com` resolves to `0.0.0.0` on this
  network (a DNS filter). `pio pkg install` loops forever on "Looking for
  another mirror". Workaround used: download the tarball with
  `curl --resolve usc1.contabostorage.com:443:209.126.15.85 ...`, then
  `pio pkg install -g -t file://<tarball>` and set `.piopm` in
  `~/.platformio/packages/tool-esptoolpy` to owner `platformio`, name
  `tool-esptoolpy`, requirements `~2.41100.0` so the platform accepts it.
  Adding a hosts entry for that host would avoid all of this (needs admin).
- CH343 driver was already present (wch.cn 2.1.2025.7). Badges enumerate as
  `USB-Enhanced-SERIAL CH343 (COMn)`, instance `USB\VID_1A86&PID_55D3\<usb serial>`.
  The USB serial (e.g. `5B91046672`) is stable per board and is what the
  scripts use as the board identity.

## Build

`pio run -e waveshare_esp32s3_lcd128` at commit 991ad59: 803,632 byte
firmware, RAM 18%, first build 79 s. `platformio.ini` hardcodes
`upload_port = /dev/ttyACM0`; on Windows always pass `--upload-port COMn`.

## Scripts (`tools/`)

| script | what it does |
|---|---|
| `flash-all.ps1` | build once, enumerate every attached badge, flash them all in parallel (one `pwsh` per board), aggregate into `flash-log.csv`. Needs PowerShell 7. |
| `flash-one.ps1 -Port COMn [-Claimant name]` | flash one board from the prebuilt images with the exact esptool call `pio run -t upload` uses, then reset and watch serial for `[badge] boot`, `panel up`, `role:` and steady `N fps` lines. Claims the board by USB serial (`flash-results\claims\<serial>.claim`, atomic create); exit 3 if someone else owns it. Writes `flash-results\<serial>.json`. |
| `flash.ps1` | interactive plug-one-at-a-time loop with the same checks, optional full 16MB stock dump (`-Backup`, `-BackupAll`). |

Verification thresholds: `OK` needs a `role:` line, at least two fps samples,
fps > 0 and ESP-NOW up. `OK_NO_RADIO` means it rendered but the radio did not
come up (power). The extra reset the verify step does is safe: the boot-loop
counter in NVS clears after 3 s of steady rendering.

## Team pattern

Several agents (or shells) can share one hub: each runs `flash-one.ps1`
against ports in turn with its own `-Claimant`; exit code 3 means "taken, try
the next port". The claim directory is wiped by `flash-all.ps1` at the start
of a run.

## First batch, 2026-09-04

| port | usb serial | MAC | result |
|---|---|---|---|
| COM4 | 5B91046616 | 3C:0F:02:6F:29:D0 | OK, RECEIVER, 31 fps (via `pio run -t upload`) |
| COM5 | 5B91046672 | 3C:0F:02:6F:2A:C8 | OK, RECEIVER, 31 fps (via `flash-one.ps1`) |
| COM6 | 5B91046175 | 3C:0F:02:6E:FD:7C | OK, RECEIVER, 31 fps (agent flasher-a via `flash-one.ps1`) |
| COM7 | 5B91046671 | 3C:0F:02:6F:2A:CC | OK, RECEIVER, 31 fps (agent flasher-b via `flash-one.ps1`) |

| COM8 | 5B5F000321 | 90:70:69:85:DC:F8 | OK, RECEIVER, 31 fps (via `flash-one.ps1`, flashed with 4f78753) |

The fifth board did not enumerate at all on its first two cables or hub
ports (no CH343, no problem device in Device Manager); it appeared as COM8
after the user re-seated it. Its MAC is from a different block
(`90:70:69:...`, like the Linux bench's badge) than the other four
(`3C:0F:02:...`), so the ten-board order spans two production batches. Two agents ran `flash-one.ps1`
concurrently against the same hub; the claim files kept them on separate
boards, and each flash took about 35 s including the 15 s serial verification.

Second pass, after merging main at 4f78753 (badges report neighbours and hop
distance): `flash-all.ps1` rebuilt in 22 s and reflashed all four in parallel
in 22 s, all OK.

## Cross-bench mesh test, 2026-09-04

The Linux bench's leader (`44:1B:F6:83:F3:5C`, shows as `83F35C`) was on the
air the whole time, so it served as the sole conductor and all four Windows
badges were receivers. `tools/watch-badges.ps1` watched all four at once.

| firmware | window | fps (min/avg/max) | rx per badge per s | notes |
|---|---|---|---|---|
| 991ad59 | 120 s | 31 / 31.0 / 31 | 15.4 | rx deltas 1831, 1831, 1816, 1833; no reboots |
| 4f78753 | 120 s | 30 / 30.8 / 31 | 17.5 | rx deltas 2078, 2078, 2076, 2079; no reboots |

Final neighbour tables on 4f78753 (`heard <mac>:<count>(h<hop>)`):

| badge | direct from leader (h0) | via Linux badge 85DC30 (h1) | via Windows badges | by hop |
|---|---|---|---|---|
| COM4 6F29D0 | 2292 | 78 | 6EFD7C:1 6F2ACC:2 (h1) | 2292/81/0/0/0 |
| COM5 6F2AC8 | 2215 | 106 | 6F2ACC:14 6F29D0:17 6EFD7C:22 (h1) | 2215/158/1/0/0 |
| COM6 6EFD7C | 2269 | 90 | 6F29D0:11 6F2ACC:3 (h1) | 2269/104/0/0/0 |
| COM7 6F2ACC | 2273 | 88 | 6F29D0:9 (h1), 6EFD7C:4 (h2) | 2273/98/3/0/0 |

The Linux side then found their conductor was transmitting at about 21.6 Hz,
not 30 (PACKET_INTERVAL_MS aliases against the 16.1 ms analysis hop), so the
per-badge rates above are roughly 71 percent and 81 percent delivery of the
real stream across two benches, not dedupe loss.

Conductor-restart test (240 s window from 20:45:28, four badges watched,
the Linux side re-flashed their leader mid-window, which is a 10 to 15 s
outage rather than a battery-swap-length one):

| badge | samples | fps min/avg/max | rx delta | rx per s | longest rx stall | resyncs | reboots |
|---|---|---|---|---|---|---|---|
| COM4 | 234 | 30 / 31.0 / 31 | 3545 | 14.9 | 12.2 s | 2 | 0 |
| COM5 | 234 | 30 / 31.0 / 31 | 3562 | 14.9 | 11.2 s | 2 | 0 |
| COM6 | 234 | 30 / 31.0 / 31 | 3562 | 14.9 | 11.2 s | 2 | 0 |
| COM7 | 234 | 30 / 31.0 / 31 | 3561 | 14.9 | 12.3 s | 2 | 0 |

Reception resumed by itself on every badge the moment the leader was back,
with no reflash and no reset on our side; the only gap is the leader's own
outage. The epoch-reset fix from main (accept a far-behind sequence as a new
epoch) is what makes that work. After the fifth badge joined, COM4's table
also listed it: `85DCF8:14(h1)`.

Reading: about 96 percent of what each badge counts is the leader heard
directly; the Linux badge is the main hop-1 relay into this room; the four
Windows badges relay a little to each other; COM7 saw a real hop-2 path
through COM6. No relay storm, dedupe holding. Shader byte from the leader is
0, so every HUD reads "plasma".

All ESP32-S3 (QFN56) rev v0.2, 2MB embedded PSRAM, 16MB quad flash. Every
badge reported `rx` packets within seconds of boot, so a conductor was already
on the air in range (the Linux bench).
