#!/usr/bin/env python3
"""Pretend to be the phone: drive a badge as conductor over BLE.

The real client is a web page using Web Bluetooth, which cannot run on this
machine headlessly. This speaks the identical GATT contract from src/phone_link.h
so the firmware side can be tested end to end without a phone in the room --
and so a failure can be localised to the firmware or the page rather than
"BLE doesn't work".

usage: phone-sim.py [seconds] [bpm]
"""
import asyncio
import math
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "c8a0f100-0451-4000-b000-63726e730001"
FEATURES = "c8a0f101-0451-4000-b000-63726e730001"
STATUS = "c8a0f102-0451-4000-b000-63726e730001"

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
BPM = float(sys.argv[2]) if len(sys.argv) > 2 else 118.0
SEND_HZ = 30.0

ROLES = {0: "IDLE", 1: "PHONE-LED", 2: "RECEIVER (a real conductor is on air)"}


def frame(t, seq):
    """Same shape the page must produce: a kick envelope plus moving bands."""
    beat_period = 60.0 / BPM
    since = math.fmod(t, beat_period)
    kick = math.exp(-since / 0.09)
    beat = 1 if since < (1.0 / SEND_HZ) else 0

    bass = kick
    mid = 0.35 + 0.35 * math.sin(t * 1.3)
    treble = 0.25 + 0.25 * math.sin(t * 2.7) + 0.4 * math.exp(-math.fmod(t, beat_period / 2) / 0.04)
    energy = 0.35 + 0.3 * kick + 0.2 * math.sin(t * 0.7)

    def q(v):
        return max(0, min(255, int(v * 255)))

    return struct.pack("<BBBBBBH", q(bass), q(mid), q(treble), q(energy), beat, 0, seq & 0xFFFF)


def on_status(_, data):
    if len(data) != 8:
        return
    role, heard, rx, tx, frames = struct.unpack("<BBHHH", data)
    print(f"  [status] role={ROLES.get(role, role)} espnow_heard={heard} "
          f"rx={rx} tx={tx} frames_accepted={frames}", flush=True)


async def main():
    print("scanning for a badge advertising the Chorus service...", flush=True)
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [u.lower() for u in (ad.service_uuids or [])]
        or (d.name or "").startswith("Chorus-"),
        timeout=15.0,
    )
    if dev is None:
        print("no badge found -- is it advertising?")
        return 1
    print(f"found {dev.name} [{dev.address}], connecting...", flush=True)

    async with BleakClient(dev) as client:
        print(f"connected: {client.is_connected}", flush=True)
        try:
            await client.start_notify(STATUS, on_status)
        except Exception as e:
            print(f"  (status notify unavailable: {e})")

        seq = 0
        sent = 0
        t0 = time.time()
        period = 1.0 / SEND_HZ
        next_send = t0
        while time.time() - t0 < DURATION:
            now = time.time()
            if now >= next_send:
                next_send += period
                # Write without response: at 30Hz an ack per frame costs more
                # than dropping one, and a feature frame is stale in 33ms.
                await client.write_gatt_char(FEATURES, frame(now - t0, seq), response=False)
                seq += 1
                sent += 1
            await asyncio.sleep(0.002)

        elapsed = time.time() - t0
        print(f"sent {sent} frames in {elapsed:.1f}s ({sent/elapsed:.1f}/sec)", flush=True)
    return 0


sys.exit(asyncio.run(main()))
