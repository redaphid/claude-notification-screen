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

A fifth board was plugged into the hub but never enumerated on Windows (no
CH343, no problem device in Device Manager): charge-only cable or a dead hub
port. Two agents ran `flash-one.ps1` concurrently against the same hub; the
claim files kept them on separate boards, and each flash took about 35 s
including the 15 s serial verification.

All ESP32-S3 (QFN56) rev v0.2, 2MB embedded PSRAM, 16MB quad flash. Every
badge reported `rx` packets within seconds of boot, so a conductor was already
on the air in range (the Linux bench).
