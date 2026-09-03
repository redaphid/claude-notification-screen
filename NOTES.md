# Status (stopped mid-debug, 2026-09-02)

## Goal
BLE REST-ish interface so Claude can post notifications to an ESP32 round
display: set text + background color, text marquees.

## The bigger plan this feeds (recovered from mindmeld + Vikunja task 319)
The notification screen is a stepping stone. The real target is a
**music-reactive display rig for the camp rave, SEPT 4-6 2026**, pine forest
near Coconino — no wifi, no router, no phone infrastructure.

Architecture as originally described:
- **One sender unit** has a microphone (INMP441 I2S was the recommended part)
  and does the audio analysis / FFT.
- **All other units are receivers**, listening over **ESP-NOW** (connectionless,
  no access point, one-to-many broadcast, single-digit-ms latency). This is the
  only thing that works with zero infrastructure, and the rig comes up just by
  being switched on.
- Aesthetic lineage is Paper Cranes (`visuals.beadfamous.com`) — the same
  audio-reactive shader language (iris/lattice/plasma, wavelet + spectral
  features), reduced to what a 240x240 round LCD driven by an ESP32-S3 can do.
  Nothing has been ported yet; that is still an open design question.

Scope note carried forward from the task: **one sender + one receiver blinking
to sound is a complete, showable thing.** Everything past that is polish.

## Hardware — IDENTIFIED (2026-09-02, from the Waveshare order email)
Order **#260713-003325-E0**, placed 2026-07-13, paid $190.53 via PayPal,
shipped 2026-08-14 via FedEx:

  **10x — SKU 26541 — "ESP32-S3 Development Board, 32-bit LX7 Dual-core
  Processor, Integrates GC9A01 Display Driver Chip, With 1.28inch IPS Round
  LCD"**

That is the genuine Waveshare **ESP32-S3-LCD-1.28 (NON-TOUCH)**. Aaron's
correction was right, and the "rebrand/clone" suspicion below was wrong — these
are real Waveshare boards, bought direct. Ten of them, which is exactly the
quantity the ESP-NOW sender/receiver plan wants.

**Also in hand:** a separate **waveshare ESP32-S3 1.46inch** board bought via
Amazon, delivered 2026-09-02/03. Different board (larger round panel, touch),
not the one this firmware targets. Not yet unboxed/evaluated.

ESP32-S3 (2MB PSRAM, 16MB flash), round 1.28" 240x240 GC9A01A IPS, USB-C via a
WCH CH343 UART bridge.

- **Unit 1** — port `/dev/cu.usbmodem5B5F0002731`. Screen never lit under
  any firmware tried (raw GPIO, LEDC PWM, exact official register-level
  GC9A01 init sequence). Currently unclear if defective or just never got
  the right pin. Original stock firmware was overwritten early on and was
  NOT backed up first (mistake — should have `esptool read_flash` before
  ever writing). With 10 boards in the order, do not burn more time on this
  one — grab a fresh board.
- **Unit 2** — port `/dev/cu.usbmodem5B910466501`. Confirmed **working**:
  stock firmware shows a lit blue screen (booted, visibly running UI/LVGL).
  Full stock flash dumped to `backups/stock_firmware_16MB.bin` (16MB, intact)
  before any writes — use this to restore or to keep reverse-engineering.
  My firmware flashed on top of it does NOT light the backlight — confirmed
  via clean A/B test (stock lit → my fw dark, same unit, same cable).

## Backlight pin: RESOLVED — GPIO40
From the official Waveshare wiki for this exact board
(https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28), fetched 2026-09-02:

> "the development board uses **GPIO40** to control backlight brightness"

Full official pin map (applies to both the touch and non-touch versions):

| GPIO | Function      |
|------|---------------|
| 0    | BOOT0         |
| 1    | Battery ADC (200K/100K divider; V = 3.3/(1<<12) * 3 * raw) |
| 5    | TP_INT (touch version only) |
| 6    | I2C SDA (QMI8658 IMU + touch controller) |
| 7    | I2C SCL       |
| 8    | LCD_DC        |
| 9    | LCD_CS        |
| 10   | LCD_CLK       |
| 11   | LCD_MOSI      |
| 12   | LCD_RST       |
| **40** | **LCD_BL**  |
| 47   | IMU INT1      |
| 48   | IMU INT2      |

So `TFT_BL 2` (from the spencershepard DEV_Config.h) was simply **wrong** for
this board, which is why every attempt failed. `TFT_RST` is **12**, not 14.
SPI can run up to 80 MHz.

**Correction to the earlier correction:** the non-touch version still HAS the
**QMI8658 6-axis IMU** on I2C (GPIO6/7, INT on 47/48) — only the touch
controller is absent. An IMU is available for gesture/orientation reactivity if
wanted. Do NOT assume a touch controller (CST816S) is present.

The 3-pin blink sweep currently in `src/main.cpp` includes GPIO40 — it was
almost certainly correct and just never got visually confirmed before we
stopped. GPIO40 blinks ONCE per cycle.

### Superseded notes on the pin question (kept for context)
Sources disagreed before the board was identified:
- One community repo (spencershepard/Waveshare-ESP32-S3-Touch-LCD-1.28,
  DEV_Config.h): `LCD_BL_PIN = 2`, active-high, plain digitalWrite. WRONG for
  this board.
- A TFT_eSPI GitHub discussion ("Incorrect pins for the ESP32-S3-LCD-1.28...",
  https://github.com/Bodmer/TFT_eSPI/discussions/3283) gave conflicting
  answers within the same thread: one config said `TFT_BL 2`, an
  "alternative" further down said `TFT_BL 40`. The alternative was right.
  It also says `#define USE_HSPI_PORT` is required to avoid a StoreProhibited
  crash on ESP32-S3 with TFT_eSPI.
- Other pins (SCLK=10, MOSI=11, CS=9, DC=8) were consistent across every
  source and are confirmed correct by the official wiki.

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
  reconstruct it. The last version used `TFT_BL 2`, which is wrong for this
  board; rebuild it with **`TFT_BL 40`** and `TFT_RST 12` per the official
  pin map above.
- `backups/stock_firmware_16MB.bin` — full untouched dump of unit 2's
  factory image. Treat as read-only ground truth / restore point.
- Deleted: earlier diagnostic files (DEV_Config.*, LCD_1in28.*, Debug.h)
  that were staged to build Waveshare's own register-level driver as a
  test — that test ran cleanly (no crash, full GC9A01 init sequence sent,
  red fill written) on **unit 1** and still showed nothing, which is part
  of why unit 1 is suspected defective independent of the pin question.

## Reverse-engineering attempt (ABANDONED — no longer needed)
The official wiki answered the pin question (GPIO40), so this line of attack is
moot. Kept only because the ELF-wrapper technique is reusable.

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
