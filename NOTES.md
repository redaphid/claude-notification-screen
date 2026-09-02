# Status (stopped mid-debug, 2026-09-02)

## Goal
BLE REST-ish interface so Claude can post notifications to an ESP32 round
display: set text + background color, text marquees.

## Hardware
Two physical units, both ESP32-S3 (2MB PSRAM, 16MB flash), round ~1.28"
display, USB-C via a WCH CH343 UART bridge. Believed to be a Waveshare
ESP32-S3-LCD-1.28-style board, but pin numbers found online for this
board are inconsistent across sources (see below) — likely a rebrand/clone
sold under a different name, not verified as genuine Waveshare.

**Correction (per Aaron, 2026-09-02):** probably the **non-touch**
ESP32-S3-LCD-1.28, not the Touch variant. Earlier assumption that it has
CST816S touch + QMI8658 IMU (based on the spencershepard DEV_Config.h,
which includes Touch_INT_PIN/Touch_RST_PIN/BAT_ADC_PIN) may not apply to
this unit — that reference repo might just be for a different/superset
board. Don't assume touch or IMU hardware is present; verify before writing
any code that depends on it.

- **Unit 1** — port `/dev/cu.usbmodem5B5F0002731`. Screen never lit under
  any firmware tried (raw GPIO, LEDC PWM, exact official register-level
  GC9A01 init sequence). Currently unclear if defective or just never got
  the right pin. Original stock firmware was overwritten early on and was
  NOT backed up first (mistake — should have `esptool read_flash` before
  ever writing).
- **Unit 2** — port `/dev/cu.usbmodem5B910466501`. Confirmed **working**:
  stock firmware shows a lit blue screen (booted, visibly running UI/LVGL).
  Full stock flash dumped to `backups/stock_firmware_16MB.bin` (16MB, intact)
  before any writes — use this to restore or to keep reverse-engineering.
  My firmware flashed on top of it does NOT light the backlight — confirmed
  via clean A/B test (stock lit → my fw dark, same unit, same cable).

## Backlight pin: still unresolved
Tried on **unit 2** (known-good hardware), isolated tests, no display code:
- GPIO2 static HIGH — no light
- GPIO48, 47, 38, 21, 1 — untested/inconclusive (early sweep, never watched)
- GPIO40, GPIO41 — sweep flashed but never visually confirmed before we
  stopped (file `src/main.cpp` currently holds this 3-pin blink sweep:
  candidates `{40, 2, 41}`, pin 40 blinks once, pin 2 twice, pin 41 three
  times, repeating)

Sources disagree:
- One community repo (spencershepard/Waveshare-ESP32-S3-Touch-LCD-1.28,
  DEV_Config.h): `LCD_BL_PIN = 2`, active-high, plain digitalWrite.
- A TFT_eSPI GitHub discussion ("Incorrect pins for the ESP32-S3-LCD-1.28...",
  https://github.com/Bodmer/TFT_eSPI/discussions/3283) gives conflicting
  answers within the same thread: one config says `TFT_BL 2`, an
  "alternative" further down says `TFT_BL 40`. Also flags `TFT_RST` as 12
  vs 14 depending on source, and says `#define USE_HSPI_PORT` is required
  to avoid a StoreProhibited crash on ESP32-S3 with TFT_eSPI.
- Other pins (SCLK=10, MOSI=11, CS=9, DC=8) are consistent across every
  source checked and are probably right.

Confirmed via strings extraction from the stock firmware dump: it's built
with **Arduino core + TFT_eSPI + LVGL** (not the raw LCD_1in28.c driver, not
Adafruit_GC9A01A). String evidence:
`/C:\Users\ysx06\...\Arduino\libraries\TFT_eSPI\Processors/TFT_eSPI_ESP32_S3.c`,
`"Hello Ardino and LVGL!"`, `"I am LVGL_Arduino"`. No plaintext pin numbers
survive in strings (they're compile-time constants baked into machine code,
not stored as strings).

## Firmware/code state
- `platformio.ini`: env `waveshare_esp32s3_touch_lcd128`, board
  `esp32-s3-devkitc-1`, 16MB flash, qio_qspi PSRAM, currently points
  `upload_port`/`monitor_port` at unit 2's port (`5B910466501`).
- `src/main.cpp` currently holds the **3-pin blink sweep test**, not the
  real firmware. The real BLE marquee firmware (GC9A01/Adafruit,
  NimBLE, JSON `{text, bg, color, speedMs}` over a Nordic-UART-UUID
  characteristic) was overwritten for this diagnostic — need to
  reconstruct it (last known-good version used `TFT_BL 2`, which is now
  proven wrong on unit 2; swap in whichever pin the sweep confirms, or
  finish the disassembly approach below).
- `backups/stock_firmware_16MB.bin` — full untouched dump of unit 2's
  factory image. Treat as read-only ground truth / restore point.
- Deleted: earlier diagnostic files (DEV_Config.*, LCD_1in28.*, Debug.h)
  that were staged to build Waveshare's own register-level driver as a
  test — that test ran cleanly (no crash, full GC9A01 init sequence sent,
  red fill written) on **unit 1** and still showed nothing, which is part
  of why unit 1 is suspected defective independent of the pin question.

## Reverse-engineering attempt (in progress, unfinished)
Was disassembling `backups/stock_firmware_16MB.bin` to read the real
backlight GPIO out of the compiled machine code, since string search can't
find it (baked-in constant, not text).

Steps done:
1. Parsed partition table at 0x8000 → `app0` partition at offset 0x10000,
   size 0x300000.
2. Extracted app0, parsed its ESP32 image header via
   `esptool.py --chip esp32s3 image_info`: 5 segments; the code segment is
   Segment 4, IROM, load addr `0x42000020`, file offset `0x20018` within
   app0.bin, length `0x4fd30` (326960 bytes).
3. `xtensa-esp32s3-elf-objdump -D -b binary -m xtensa` **segfaults** on raw
   `-b binary` input with this toolchain build (crosstool-NG
   esp-2021r2-patch5) — reproduced even on a 4-byte dummy file. Don't reuse
   `-b binary` mode with this objdump.
4. Workaround: wrote a minimal hand-built ELF32 wrapper (single PT_LOAD
   `.text` section at the correct load address, machine type EM_XTENSA
   =0x5E) around the raw code segment bytes, saved as
   `scratchpad/irom.elf`. `objdump -D irom.elf` **works** — confirmed
   correctly disassembling real Xtensa instructions at the right addresses
   (verified output starting at `42000020`).

Not yet done (next steps if resuming):
- Search the `objdump -D irom.elf` output for GPIO pin setup: look for
  `movi`/`movi.n` immediates matching backlight candidates (2, 40, 41, 12,
  14) that appear shortly before a `call`/`callx` in the same function as
  other movi's for 8, 9, 10, 11 (the CS/DC/SCLK/MOSI pins, which are
  trusted) — that function is almost certainly the display pin/SPI init,
  and whatever constant sits next to 8/9/10/11 for the backlight-looking
  call is very likely the real `TFT_BL` value.
- Alternative/complementary: grep the DROM/.rodata segment (Segment 1,
  load `0x3c050020`, file offset `0x18`, length `0x174e8`) for the GC9A01
  init command byte sequence (`0xEF 0xEB 0x14 ...` — same table format
  seen in Waveshare's public LCD_1in28.c) to confirm driver identity and
  possibly find nearby structured config data.
- Once a pin is confirmed working (via disasm or by finally watching a
  physical test on unit 2), rebuild `src/main.cpp` from the last full BLE
  marquee version (Adafruit_GC9A01A + NimBLE), fix `TFT_BL`, and also add
  `USE_HSPI_PORT`-equivalent care if switching to TFT_eSPI, or just confirm
  the Adafruit library path doesn't need it (current firmware uses
  `SPIClass tftSPI(HSPI)` and Adafruit_GC9A01A, not TFT_eSPI — the
  StoreProhibited crash warning was TFT_eSPI-specific, may not apply).
- Re-flash unit 2 (do NOT touch unit 1 further without deciding if it's
  being treated as defective/returned).
- Once display works, still need to re-verify the full BLE JSON marquee
  behavior end-to-end (never got that far — was blocked on backlight the
  entire session).

## Mistakes / lessons for next time
- Should have dumped unit 1's stock flash with `esptool read_flash` before
  the very first write. Always back up flash before first write on unread
  hardware.
- `xtensa-esp32s3-elf-objdump -b binary` segfaults on this toolchain build;
  wrap raw segments in a minimal ELF instead.
- Camera-based visual checks are slow and error-prone here (glare, wrong
  face photographed twice, ambiguous "lit vs unlit" under office lighting).
  Prefer serial log evidence + known-good/known-bad A/B swaps over repeated
  photos when possible.
