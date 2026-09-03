#!/usr/bin/env bash
#
# build-web-release.sh — build the badge firmware and stage it for the web flasher.
#
# BUILD ONLY. This script never talks to a serial port and never uploads.
# It runs `pio run`, copies the four flash images into web/firmware/<version>/,
# and writes the ESP Web Tools manifests.
#
# Usage:
#   scripts/build-web-release.sh                 # version from git describe
#   scripts/build-web-release.sh v0.3.0          # explicit version
#   VERSION=v0.3.0 scripts/build-web-release.sh
#
# Idempotent: re-running with the same version overwrites that version's
# directory in place and rewrites the manifests. Nothing else is touched.

set -euo pipefail

# ---------------------------------------------------------------------------
# Locate the repo (this script lives in <repo>/scripts/)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

PIO_ENV="${PIO_ENV:-waveshare_esp32s3_lcd128}"
BUILD_DIR=".pio/build/${PIO_ENV}"
WEB_DIR="web"
FW_ROOT="${WEB_DIR}/firmware"

# Product metadata shown in the ESP Web Tools dialog.
PRODUCT_NAME="${PRODUCT_NAME:-Chorus Badge}"
CHIP_FAMILY="ESP32-S3"

# ---------------------------------------------------------------------------
# ESP32-S3 flash layout.
#
# These are NOT the classic-ESP32 offsets. On ESP32-S3 (and C3/C6/H2) the
# second-stage bootloader lives at 0x0; only ESP32 and ESP32-S2 put it at
# 0x1000. Verified against PlatformIO's own upload command for this project:
#
#   $ pio run -t envdump | grep -A6 FLASH_EXTRA_IMAGES
#   'FLASH_EXTRA_IMAGES': [ ('0x0000', .../bootloader.bin),
#                           ('0x8000', .../partitions.bin),
#                           ('0xe000', .../boot_app0.bin)]
#
# and the app offset from platforms/espressif32/builder/main.py:
#   bound = int(board.get("upload.offset_address", "0x10000"), 16)
# (this board declares no upload.offset_address, so the app lands at 0x10000,
#  which also matches app0 in the partition CSV).
# ---------------------------------------------------------------------------
OFF_BOOTLOADER=0      # 0x0000
OFF_PARTITIONS=32768  # 0x8000
OFF_BOOT_APP0=57344   # 0xe000
OFF_APP=65536         # 0x10000

log()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m warn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------
VERSION="${1:-${VERSION:-}}"
if [[ -z "${VERSION}" ]]; then
  if git -C "${REPO_ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
    VERSION="$(git -C "${REPO_ROOT}" describe --tags --always --dirty 2>/dev/null || true)"
  fi
fi
[[ -n "${VERSION}" ]] || VERSION="$(date -u +%Y%m%d-%H%M%S)"
# Keep it filesystem- and URL-safe.
VERSION="$(printf '%s' "${VERSION}" | tr -c 'A-Za-z0-9._-' '-')"
log "version: ${VERSION}"

# ---------------------------------------------------------------------------
# Build (never upload)
# ---------------------------------------------------------------------------
command -v pio >/dev/null 2>&1 || die "pio (PlatformIO Core) not found on PATH"

log "building env ${PIO_ENV} (build only, no upload)"
pio run -e "${PIO_ENV}"

# ---------------------------------------------------------------------------
# Locate boot_app0.bin — it ships with the Arduino framework package, not the
# build dir. Ask PlatformIO where the package is rather than hardcoding a path.
# ---------------------------------------------------------------------------
ARDUINO_PKG="$(pio system info --json-output 2>/dev/null \
  | python3 -c 'import json,sys; print(json.load(sys.stdin).get("core_dir",{}).get("value",""))' 2>/dev/null || true)"
[[ -n "${ARDUINO_PKG}" ]] || ARDUINO_PKG="${HOME}/.platformio"
BOOT_APP0="${ARDUINO_PKG}/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
if [[ ! -f "${BOOT_APP0}" ]]; then
  BOOT_APP0="$(find "${ARDUINO_PKG}/packages" -name boot_app0.bin -path '*partitions*' 2>/dev/null | head -1 || true)"
fi
[[ -f "${BOOT_APP0}" ]] || die "could not find boot_app0.bin under ${ARDUINO_PKG}/packages"

for f in "${BUILD_DIR}/bootloader.bin" "${BUILD_DIR}/partitions.bin" "${BUILD_DIR}/firmware.bin"; do
  [[ -s "$f" ]] || die "missing or empty build artifact: $f"
done

# Sanity: the bootloader and app must be real ESP images (magic byte 0xE9).
check_magic() {
  local f="$1"
  local magic
  magic="$(head -c1 "$f" | od -An -tx1 | tr -d ' \n')"
  [[ "${magic}" == "e9" ]] || die "$f does not start with ESP image magic 0xE9 (got 0x${magic})"
}
check_magic "${BUILD_DIR}/bootloader.bin"
check_magic "${BUILD_DIR}/firmware.bin"

# ---------------------------------------------------------------------------
# Stage artifacts
# ---------------------------------------------------------------------------
OUT="${FW_ROOT}/${VERSION}"
log "staging into ${OUT}/"
rm -rf "${OUT}"
mkdir -p "${OUT}"
install -m 0644 "${BUILD_DIR}/bootloader.bin" "${OUT}/bootloader.bin"
install -m 0644 "${BUILD_DIR}/partitions.bin" "${OUT}/partitions.bin"
install -m 0644 "${BOOT_APP0}"                "${OUT}/boot_app0.bin"
install -m 0644 "${BUILD_DIR}/firmware.bin"   "${OUT}/firmware.bin"

# ---------------------------------------------------------------------------
# Optional convenience artifact: a single merged image for command-line
# recovery (`esptool.py write_flash 0x0 merged.bin`). Best effort — the web
# flasher does not use it.
# ---------------------------------------------------------------------------
ESPTOOL="$(find "${ARDUINO_PKG}/packages/tool-esptoolpy" -maxdepth 1 -name esptool.py 2>/dev/null | head -1 || true)"
if [[ -n "${ESPTOOL}" ]]; then
  if python3 "${ESPTOOL}" --chip esp32s3 merge_bin \
        --output "${OUT}/merged.bin" \
        --flash_mode keep --flash_freq keep --flash_size keep \
        "${OFF_BOOTLOADER}" "${OUT}/bootloader.bin" \
        "${OFF_PARTITIONS}" "${OUT}/partitions.bin" \
        "${OFF_BOOT_APP0}"  "${OUT}/boot_app0.bin" \
        "${OFF_APP}"        "${OUT}/firmware.bin" >/dev/null 2>&1; then
    log "wrote ${OUT}/merged.bin (CLI recovery only)"
  else
    warn "esptool merge_bin failed; skipping merged.bin"
    rm -f "${OUT}/merged.bin"
  fi
else
  warn "esptool.py not found; skipping merged.bin"
fi

# ---------------------------------------------------------------------------
# Manifests
#
# Two are written:
#   web/firmware/<version>/manifest.json  — self-contained, paths are bare
#                                           filenames next to it. Permalink.
#   web/manifest.json                     — the "latest" pointer the landing
#                                           page loads. Paths are relative to
#                                           web/.
#
# No `serialType` is set. This hardware has been seen with both a native-USB
# variant and a CH343 USB-UART bridge; leaving serialType off makes this build
# the fallback for every connection type instead of filtering it out.
# ---------------------------------------------------------------------------
write_manifest() {
  local dest="$1" prefix="$2"
  python3 - "$dest" "$prefix" "$PRODUCT_NAME" "$VERSION" "$CHIP_FAMILY" \
      "$OFF_BOOTLOADER" "$OFF_PARTITIONS" "$OFF_BOOT_APP0" "$OFF_APP" <<'PY'
import json, sys
dest, prefix, name, version, chip, *offs = sys.argv[1:]
b, p, ba, app = (int(x) for x in offs)
manifest = {
    "name": name,
    "version": version,
    "new_install_prompt_erase": True,
    "builds": [
        {
            "chipFamily": chip,
            "parts": [
                {"path": prefix + "bootloader.bin", "offset": b},
                {"path": prefix + "partitions.bin", "offset": p},
                {"path": prefix + "boot_app0.bin",  "offset": ba},
                {"path": prefix + "firmware.bin",   "offset": app},
            ],
        }
    ],
}
with open(dest, "w") as fh:
    json.dump(manifest, fh, indent=2)
    fh.write("\n")
PY
}

write_manifest "${OUT}/manifest.json" ""
write_manifest "${WEB_DIR}/manifest.json" "firmware/${VERSION}/"

# ---------------------------------------------------------------------------
# Release index — every staged version, newest-mtime first.
# ---------------------------------------------------------------------------
python3 - "${FW_ROOT}" "${VERSION}" <<'PY'
import json, os, sys
root, latest = sys.argv[1], sys.argv[2]
vers = []
for name in os.listdir(root):
    d = os.path.join(root, name)
    if os.path.isdir(d) and os.path.isfile(os.path.join(d, "manifest.json")):
        vers.append({
            "version": name,
            "manifest": f"firmware/{name}/manifest.json",
            "built": int(os.path.getmtime(d)),
        })
vers.sort(key=lambda v: v["built"], reverse=True)
with open(os.path.join(root, "releases.json"), "w") as fh:
    json.dump({"latest": latest, "releases": vers}, fh, indent=2)
    fh.write("\n")
PY

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
log "release ${VERSION} staged:"
printf '  %-16s 0x%04x  %s\n' bootloader.bin "${OFF_BOOTLOADER}" "$(stat -c%s "${OUT}/bootloader.bin") bytes"
printf '  %-16s 0x%04x  %s\n' partitions.bin "${OFF_PARTITIONS}" "$(stat -c%s "${OUT}/partitions.bin") bytes"
printf '  %-16s 0x%04x  %s\n' boot_app0.bin  "${OFF_BOOT_APP0}"  "$(stat -c%s "${OUT}/boot_app0.bin") bytes"
printf '  %-16s 0x%04x  %s\n' firmware.bin   "${OFF_APP}"        "$(stat -c%s "${OUT}/firmware.bin") bytes"
echo
log "serve locally:  python3 -m http.server 8000 --directory ${WEB_DIR}"
log "then open:      http://localhost:8000/"
