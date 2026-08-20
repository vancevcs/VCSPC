#!/usr/bin/env python3
"""Find the ped field that marks the swimming climb-out, by recording it happening.

The vault needs the game's own pull-up animation, and the game only ever plays it in one place:
swim at a quay whose edge is above your head and the player grabs it and hauls himself out. Some
field in the ped says that is happening. This records the whole struct several times a second
while you do it, then looks for fields that behave the way a state does.

    python Tools/vcsclimb.py --seconds 60        # PPSSPP running with --debugger=1337

Swim up to a ledge, tread water for a few seconds, pull yourself up, then stay still. The climb is
located automatically from the vertical movement - a pull-up raises the ped 1.5-2m in under a
second, which nothing else in the water does.

The ranking is the whole idea. A ped struct is full of values that change every frame (position,
matrix, timers, animation blend weights), and any of them will differ across the climb. What a
STATE looks like is different: it holds one value, changes at the boundary, and changes back. So
candidates are scored on how rarely they change overall, and the ones that flip exactly at the
boundary and revert after it come first.
"""

import argparse
import asyncio
import base64
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PLAYER_BASE = 0x08bc8170
PED_SPAN = 0x1000          # covers everything the address table knows, with room to spare
POS_OFF = 0x30

# A pull-up out of the water. Treading water bobs by a few centimetres; this does not.
CLIMB_RISE = 1.20
CLIMB_WINDOW = 2.0         # seconds the rise must happen within


async def record(port, seconds, hz):
    from vcsscan import connect, Debugger

    frames = []
    async with connect(port) as ws:
        dbg = Debugger(ws)
        ped = (await dbg.call("memory.read_u32", address=PLAYER_BASE, timeout=6.0))["value"]
        if not ped:
            sys.exit("no player ped - is the game actually running?")
        print("ped %08x, recording %ds at %dHz - go and climb out of the water"
              % (ped, seconds, hz), file=sys.stderr)

        t0 = time.time()
        while time.time() - t0 < seconds:
            resp = await dbg.call("memory.read", address=ped, size=PED_SPAN, timeout=10.0)
            frames.append((time.time() - t0, base64.b64decode(resp["base64"])))
            await asyncio.sleep(1.0 / hz)
    return ped, frames


def z_of(blob):
    return struct.unpack_from("<f", blob, POS_OFF + 8)[0]


def find_climb(frames):
    """The first window in which the ped rises CLIMB_RISE within CLIMB_WINDOW seconds."""
    for i, (t, blob) in enumerate(frames):
        z0 = z_of(blob)
        for j in range(i + 1, len(frames)):
            t2, blob2 = frames[j]
            if t2 - t > CLIMB_WINDOW:
                break
            if z_of(blob2) - z0 >= CLIMB_RISE:
                return i, j
    return None, None


def churn(frames, off, size):
    """How many times this field changed across the whole recording - the noise measure."""
    fmt = {1: "<B", 2: "<H", 4: "<I"}[size]
    last = None
    n = 0
    for _, blob in frames:
        v = struct.unpack_from(fmt, blob, off)[0]
        if last is not None and v != last:
            n += 1
        last = v
    return n


def read(blob, off, size):
    return struct.unpack_from({1: "<B", 2: "<H", 4: "<I"}[size], blob, off)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--hz", type=float, default=12.0)
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    ped, frames = asyncio.run(record(args.port, args.seconds, args.hz))
    if len(frames) < 8:
        sys.exit("too few samples")

    lo, hi = find_climb(frames)
    if lo is None:
        zs = [z_of(b) for _, b in frames]
        sys.exit("no climb found: Z went %.2f..%.2f, which is not a pull-up"
                 % (min(zs), max(zs)))

    before = frames[max(0, lo - 1)][1]
    during = frames[(lo + hi) // 2][1]
    after = frames[-1][1]
    print("climb at %.2fs..%.2fs (Z %.2f -> %.2f)"
          % (frames[lo][0], frames[hi][0], z_of(before), z_of(during)))
    print()

    # Skip the transform: the whole head of the struct is the matrix and the position, all of which
    # obviously change, and none of which is a state.
    SKIP = range(0x00, 0x40)

    rows = []
    for size in (1, 2, 4):
        for off in range(0, PED_SPAN - size, size):
            if off in SKIP:
                continue
            b, d, a = read(before, off, size), read(during, off, size), read(after, off, size)
            if b == d:
                continue                      # unchanged across the climb: not it
            score = churn(frames, off, size)
            reverted = (a == b)
            rows.append((score, not reverted, off, size, b, d, a))

    rows.sort(key=lambda r: (r[0], r[1]))
    print("%-8s %-5s %-12s %-12s %-12s %s" % ("offset", "type", "before", "during", "after", "changes"))
    for score, not_reverted, off, size, b, d, a in rows[:args.top]:
        print("+0x%-5x u%-4d %-12d %-12d %-12d %d%s"
              % (off, size * 8, b, d, a, score, "" if not_reverted else "   <- reverts"))
    print()
    print("ped is %08x; the reverting rows with the lowest change counts are the states." % ped)


if __name__ == "__main__":
    main()
