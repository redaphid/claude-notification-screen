"""Show the leader's GATT table the way nRF Connect or LightBlue would.

The knob design lives or dies on whether a generic BLE scanner presents it as
something a person can use, so this prints exactly what such an app reads: each
characteristic, its properties, its 0x2901 user description, and its current
value. If a line here is unlabelled or unreadable, it is unlabelled in the app
too.
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SVC = "c8a0f200-0451-4000-b000-63726e730001"
CUD = "00002901-0000-1000-8000-00805f9b34fb"


async def main():
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SVC in (ad.service_uuids or []) or d.name == "Chorus Leader", timeout=15.0)
    if not dev:
        print("no leader advertising")
        return 1
    async with BleakClient(dev) as c:
        for svc in c.services:
            if svc.uuid.lower() != SVC:
                continue
            print(f"service {svc.uuid}")
            for ch in svc.characteristics:
                props = ",".join(ch.properties)
                label = ""
                for d in ch.descriptors:
                    if d.uuid.lower() == CUD:
                        try:
                            label = (await c.read_gatt_descriptor(d.handle)).decode(errors="replace")
                        except Exception:
                            label = "?"
                val = ""
                if "read" in ch.properties:
                    try:
                        raw = await c.read_gatt_char(ch.uuid)
                        val = str(raw[0]) if len(raw) == 1 else raw.hex()[:40]
                    except Exception as e:
                        val = f"<{e}>"
                short = ch.uuid.split("-")[0]
                print(f"  {short}  {label or '(no description)':<22} {val:<12} [{props}]")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
