#!/usr/bin/env bash
# Flash the badge, then prove it worked without a human in the room:
# capture serial across resets and photograph the screen with the webcam.
#
# The CH343 bridge on this board wedges when the supply dips (a laptop USB port
# dropping into a low-power state will do it), so uploads are retried rather
# than assumed. usage: flash-and-verify.sh [attempts] [outdir]
set -u

ATTEMPTS="${1:-20}"
OUT="${2:-outputs/bench}"
PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_DIR" || exit 1
mkdir -p "$OUT"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/verify.log"; }

for i in $(seq 1 "$ATTEMPTS"); do
  log "upload attempt $i/$ATTEMPTS"
  if timeout 90 pio run -t upload >>"$OUT/upload.log" 2>&1; then
    log "upload OK on attempt $i"

    timeout 40 python3 scripts/test/serial-watch.py 25 >"$OUT/serial.log" 2>&1
    log "serial captured: $(wc -l <"$OUT/serial.log") lines"

    # Photograph across a few seconds: the boot self-test cards and the running
    # visual look different, so a burst tells you which state the board is in.
    for shot in 1 2 3 4 5; do
      ffmpeg -hide_banner -loglevel error -f v4l2 -video_size 1600x1200 \
        -i /dev/video0 -vframes 6 -f image2 -update 1 -y "$OUT/shot_$shot.jpg" 2>/dev/null
      sleep 2
    done
    log "photos written to $OUT"
    exit 0
  fi
  log "attempt $i failed; waiting for the bridge to come back"
  sleep 20
done

log "gave up after $ATTEMPTS attempts -- the USB bridge needs a physical replug"
exit 1
