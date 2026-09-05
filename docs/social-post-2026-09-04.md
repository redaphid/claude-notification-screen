# Social post draft, 2026-09-04

Photo/video idea: the four badges in a loose circle on a dark table, plasma
breathing in sync, phone shooting from above at a slight angle so the round
screens read as glowing coins. If one badge is held with BOOT down while it
resets it becomes the conductor (mock DJ at 118 BPM) and the other three lock
to it; that is the shot. Ten seconds of video beats any still.

## Short version (X / Threads / Bluesky)

Four little round screens, no wifi, no phone, no pairing. One listens to the
room, the rest hear it over ESP-NOW and breathe together. 24 bytes at 30 Hz is
all it takes to make a crowd of badges move as one. Flashed the batch tonight
from a Windows box, one agent per badge, each claiming its own port. 31 fps,
every one. #ESP32 #Chorus #wearables

## Longer version (Instagram / LinkedIn / Mastodon)

Meet Chorus: a swarm of audio-reactive badges that agree with each other
without any infrastructure.

- Each badge is an ESP32-S3 with a 1.28" round IPS display (240x240).
- One badge has the microphone. It listens, runs a lightweight FFT and onset
  detector, and broadcasts a 24-byte feature packet 30 times a second over
  ESP-NOW. No router, no app, no Bluetooth pairing dance.
- Every other badge hears that packet, relays it onward so a dense crowd
  becomes a better radio network instead of a worse one, and renders the
  visuals locally. Plasma tonight; iris and tunnel are in the bag too.
- Synchrony is the whole product. Twenty listeners would each hear the room
  slightly differently, and "almost in sync" reads as broken. One listener,
  one truth, everyone breathing together.
- A badge that walks out of range doesn't freeze; it exhales over ~600 ms.
  A badge whose radio fails still renders on its own heartbeat.

Tonight's batch: four badges flashed and verified in a few minutes from a
Windows PC, with parallel agents each claiming their own board over a USB hub
so nothing gets flashed twice. Every one came up at 31 fps and started
hearing packets within seconds of boot.

Visual lineage: Paper Cranes (visuals.beadfamous.com), reduced to what a
240x240 round LCD driven by a microcontroller can do.

#ESP32 #ESPNOW #generativeart #wearabletech #PlatformIO #Chorus
