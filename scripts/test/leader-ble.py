"""Drive the leader's BLE console from a laptop, the way web/control.html does.

This exists because the page's other end is a phone, and a phone is a bad place
to find out that a characteristic returns the wrong number of bytes. Everything
here speaks exactly the layout in conductor/leader_link.h, so a mismatch shows
up as a Python error on a bench rather than as a dead button in a field.

    python3 scripts/test/leader-ble.py            # connect, dump state + roster
    python3 scripts/test/leader-ble.py iris       # set what the swarm shows
    python3 scripts/test/leader-ble.py 85dcdc tunnel   # pin one badge
    python3 scripts/test/leader-ble.py 85dcdc free
    python3 scripts/test/leader-ble.py 85dcdc find
"""
import asyncio
import struct
import sys

from bleak import BleakClient, BleakScanner

SVC = "c8a0f200-0451-4000-b000-63726e730001"
C_CTL = "c8a0f201-0451-4000-b000-63726e730001"
C_ST = "c8a0f202-0451-4000-b000-63726e730001"
C_ROS = "c8a0f203-0451-4000-b000-63726e730001"
C_NAM = "c8a0f204-0451-4000-b000-63726e730001"

OP_SET_EFFECT, OP_NEXT, OP_PREV, OP_CYCLE = 1, 2, 3, 4
OP_BADGE_EFFECT, OP_BADGE_RELEASE, OP_BADGE_IDENTIFY, OP_BADGE_BRIGHTNESS, OP_ROLL_CALL = (
    16, 17, 18, 19, 20)

FLAGS = [(1, "pinned"), (2, "hearing"), (4, "conducting"), (8, "identifying")]


def frame(op, target=(0, 0, 0), a0=0, a1=0, a2=0):
    return bytes([op, target[0], target[1], target[2], a0, a1, a2, 0])


def parse_id(text):
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


async def main():
    args = sys.argv[1:]
    print("scanning for the leader...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SVC in (ad.service_uuids or []) or d.name == "Chorus Leader", timeout=15.0)
    if not dev:
        print("no leader advertising. Is it flashed with -e conductor_ble?")
        return 1
    print(f"found {dev.name} at {dev.address}")

    async with BleakClient(dev) as c:
        names = (await c.read_gatt_char(C_NAM)).decode().split("\n")
        print("effects:", ", ".join(f"{i}={n}" for i, n in enumerate(names)))

        if args:
            if len(args) == 1 and args[0] in names:
                await c.write_gatt_char(C_CTL, frame(OP_SET_EFFECT, a0=names.index(args[0])), True)
                print(f"swarm -> {args[0]}")
            elif len(args) == 2:
                who, what = parse_id(args[0]), args[1]
                if what == "free":
                    await c.write_gatt_char(C_CTL, frame(OP_BADGE_RELEASE, who), True)
                elif what == "find":
                    await c.write_gatt_char(C_CTL, frame(OP_BADGE_IDENTIFY, who, 6), True)
                else:
                    await c.write_gatt_char(
                        C_CTL, frame(OP_BADGE_EFFECT, who, names.index(what)), True)
                print(f"{args[0]} -> {what}")
            else:
                print("unrecognised arguments; see the docstring")
                return 2
            await asyncio.sleep(2.5)  # let a beacon come back before reading the roster

        st = await c.read_gatt_char(C_ST)
        shader, count, badges, hearing, cycle_s, tx, up, bass, energy = struct.unpack("<BBBBHHHBB", st)
        print(f"leader: {names[shader]} | {badges} badge(s) | {tx}/s on air | "
              f"{'hearing music' if hearing else 'quiet'} | up {up}s | energy {energy}")

        ros = await c.read_gatt_char(C_ROS)
        n = ros[0]
        print(f"roster: {n}")
        for i in range(n):
            bid, shd, flags, fps, crest, age = struct.unpack_from("<3sBBBBB", ros, 1 + i * 8)
            marks = " ".join(name for bit, name in FLAGS if flags & bit)
            print(f"  {bid.hex()}  {names[shd]:<8} {fps:>3} fps  {age}s ago  {marks}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
