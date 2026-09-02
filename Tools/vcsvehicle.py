#!/usr/bin/env python3
"""Find out what the controls actually do in the vehicle you are sitting in.

The input layer has exactly one in-vehicle context and it assumes a car. VCS does not: a
helicopter reads L/R as yaw and the nub as pitch/roll, a boat has no handbrake, and the
forklift has forks on some button nobody has identified. Guessing which is which is the
mistake this whole project keeps re-learning, so this measures it instead.

The method is the button tester from the VCS debugger window, automated and quantified:
hold one PSP button, watch the vehicle's own transform move, release, report. A helicopter
that climbs 4 metres when Cross is held has answered the question, and it answered it in
numbers rather than in "it felt like it went up a bit".

Run PPSSPP with --debugger=1337, get into the vehicle you care about, then:

    python Tools/vcsvehicle.py info                # what am I in, and where
    python Tools/vcsvehicle.py probe               # hold each button, report what moved
    python Tools/vcsvehicle.py probe --stick       # same for the four nub directions
    python Tools/vcsvehicle.py record heli         # keep this vehicle's struct
    python Tools/vcsvehicle.py diff car heli       # which field is the vehicle TYPE
    python Tools/vcsvehicle.py watch 300           # follow along while someone drives

`diff` is the one that unblocks per-type bindings: record one of each class and the offsets
that differ consistently are the type discriminator, which is what the input layer needs to
tell a helicopter from a Sentinel.

Needs the `websockets` package. Nothing here writes to game memory - it only reads, and
injects buttons the player could have pressed anyway.
"""

import argparse
import asyncio
import base64
import json
import math
import os
import struct
import sys
import time

try:
    import websockets
except ImportError:
    sys.exit("websockets is required: pip install websockets")

SUBPROTOCOL = "debugger.ppsspp.org"

# From Core/VCS/VCSAddresses.h. 0 on foot, otherwise the occupied vehicle.
PLAYER_VEHICLE = 0x08BB4064
PLAYER_BASE = 0x08BC8170

# Entity layout, same as the player ped: four 16-byte-aligned vec3s, the last one being the
# world position. Documented for PlayerBase as "entity matrix at +0x00, world position at +0x30".
MATRIX_SIZE = 0x40
POS_OFFSET = 0x30

# How much of the vehicle struct `record` keeps. The ped's health lives at +0x4e4, so vehicle
# fields worth seeing are certainly past 0x200.
STRUCT_SIZE = 0x800

REC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".vcsvehicle")

# Model ids for this disc, read out of the game's own model-name table (stride 0x1c, entry 213
# is "maverick") and cross-checked against gtamods' vehicle list. Only the ids whose class is
# not "car" are listed - anything absent is a car as far as the controls are concerned.
#
# The classes are the game's own, from the type-name table at 0x08bafc6c:
#   car boat jetski train heli plane bike ferry bmx quad
MODEL_CLASS = {
    170: "quad",     # 6atv
    173: "plane",    # autogyro (Little Willie)
    178: "bmx", 179: "bmx",
    185: "boat",     # speeder2
    198: "boat",     # hovercr - hovercraft, unconfirmed which class
    205: "bike",     # sanchez
    212: "bike",     # pcj600
    188: "heli",     # huey
    189: "heli",     # hueyhosp - the Air Ambulance
    213: "heli",     # maverick
    214: "boat",     # reefer
    215: "boat",     # speeder
    219: "boat",     # predator
    222: "plane",    # biplane
    229: "bike",     # faggio
    230: "quad",     # quad
    231: "bike",     # angel
    232: "bike",     # freeway
    233: "jetski",   # jetski
    247: "boat",     # dinghy
    248: "boat",     # marquis
    249: "boat",     # rio
    250: "boat",     # tropic
    252: "bike",     # streetfi
    256: "bike",     # pheonix - unconfirmed
    257: "boat",     # squalo
    258: "boat",     # jetmax
    260: "heli",     # vcnmav
    261: "heli",     # polmav
    262: "heli",     # sparrow
    263: "heli",     # sesparow
    275: "heli",     # hunter
    277: "heli",     # coastg - Coastguard Maverick, not a boat
    278: "plane",    # skimmer
    279: "heli",     # chopper
    280: "plane",    # airtrain
}

MODEL_NAME = {
    170: "6atv", 171: "admiral", 172: "cheetah", 173: "autogyro", 174: "baggage",
    175: "banshee", 176: "peren", 177: "blistac", 178: "bmxboy", 179: "bmxgirl",
    180: "bobcat", 181: "bulldoze", 182: "burrito", 183: "cabbie", 184: "caddy",
    185: "speeder2", 186: "pimp", 187: "deluxo", 188: "huey", 189: "hueyhosp",
    190: "electrag", 191: "electrap", 192: "esperant", 193: "fbicar", 194: "firetruk",
    195: "glendale", 196: "greenwoo", 197: "hermes", 198: "hovercr", 199: "idaho",
    200: "landstal", 201: "manana", 202: "mop50", 203: "oceanic", 204: "vicechee",
    205: "sanchez", 206: "stallion", 207: "policem", 208: "bobo", 209: "patriot",
    210: "pony", 211: "sentinel", 212: "pcj600", 213: "maverick", 214: "reefer",
    215: "speeder", 216: "linerun", 217: "walton", 218: "barracks", 219: "predator",
    220: "flatbed", 221: "ammotruk", 222: "biplane", 223: "moonbeam", 224: "rumpo",
    225: "yola", 226: "taxi", 227: "ambulan", 228: "stretch", 229: "faggio",
    230: "quad", 231: "angel", 232: "freeway", 233: "jetski", 234: "enforce",
    235: "boxvil", 236: "benson", 237: "coach", 238: "mule", 239: "voodoo",
    240: "securica", 241: "trash", 242: "topfun", 243: "yankee", 244: "mrwhoo",
    245: "sandking", 246: "rhino", 247: "dinghy", 248: "marquis", 249: "rio",
    250: "tropic", 251: "forklift", 252: "streetfi", 253: "virgo", 254: "stinger",
    255: "bfinject", 256: "pheonix", 257: "squalo", 258: "jetmax", 259: "mesa",
    260: "vcnmav", 261: "polmav", 262: "sparrow", 263: "sesparow", 264: "scarab",
    265: "chollo", 266: "comet", 267: "cuban", 268: "fbiran", 269: "gangbur",
    270: "infernus", 271: "regina", 272: "sabre", 273: "sabretb", 274: "sentxs",
    275: "hunter", 276: "washin", 277: "coastg", 278: "skimmer", 279: "chopper",
    280: "airtrain",
}

# What `probe` holds. Triangle and Start are left out on purpose - one ejects the player from
# the vehicle mid-measurement and the other opens the pause menu, and both would silently
# invalidate every row after them. --all puts them back for when that is what you want.
SAFE_BUTTONS = ["cross", "square", "circle", "ltrigger", "rtrigger",
                "up", "down", "left", "right", "select"]
ALL_BUTTONS = SAFE_BUTTONS + ["triangle", "start"]

STICKS = [("stick up", 0.0, -1.0), ("stick down", 0.0, 1.0),
          ("stick left", -1.0, 0.0), ("stick right", 1.0, 0.0)]


class Debugger:
    def __init__(self, ws):
        self.ws = ws
        self.ticket = 0

    async def call(self, event, timeout=10.0, **params):
        self.ticket += 1
        ticket = self.ticket
        await self.ws.send(json.dumps(dict(event=event, ticket=ticket, **params)))
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise RuntimeError("%s: no response in %.0fs - is a game running?" % (event, timeout))
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), timeout=remaining))
            if msg.get("ticket") != ticket:
                continue
            if msg.get("event") == "error":
                raise RuntimeError("%s: %s" % (event, msg.get("message")))
            return msg

    async def read(self, address, size):
        resp = await self.call("memory.read", address=address, size=size)
        return base64.b64decode(resp["base64"])

    async def u32(self, address):
        return struct.unpack("<I", await self.read(address, 4))[0]

    async def buttons(self, **state):
        await self.call("input.buttons.send", buttons=state)

    async def analog(self, x, y):
        await self.call("input.analog.send", x=x, y=y)


def connect(port):
    url = "ws://127.0.0.1:%d/debugger" % port
    return websockets.connect(url, subprotocols=[SUBPROTOCOL], max_size=None,
                              ping_interval=None, ping_timeout=None,
                              open_timeout=10, close_timeout=5)


def vec3(raw, off):
    return struct.unpack_from("<3f", raw, off)


def transform(raw):
    """The four vec3s of an entity matrix: three basis rows then the world position."""
    return [vec3(raw, 0x00), vec3(raw, 0x10), vec3(raw, 0x20), vec3(raw, POS_OFFSET)]


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def length(v):
    return math.sqrt(sum(x * x for x in v))


def angle_between(a, b):
    """Degrees between two direction vectors, for reporting how far a basis row rotated."""
    la, lb = length(a), length(b)
    if la < 1e-6 or lb < 1e-6:
        return 0.0
    dot = sum(x * y for x, y in zip(a, b)) / (la * lb)
    return math.degrees(math.acos(max(-1.0, min(1.0, dot))))


async def current_vehicle(dbg):
    return await dbg.u32(PLAYER_VEHICLE)


def describe_model(model):
    name = MODEL_NAME.get(model, "?")
    klass = MODEL_CLASS.get(model, "car" if model in MODEL_NAME else "?")
    return name, klass


async def find_model_index(dbg, vehicle):
    """Every u16 in the struct's first 0x200 bytes that could be a vehicle model id.

    The model index offset is not known yet, so this reports candidates rather than claiming
    an answer. Run it on two different vehicles and the offset holding each one's real id is
    the field. Once that is pinned down it belongs in VCSAddresses.h, not here.
    """
    raw = await dbg.read(vehicle, 0x200)
    out = []
    for off in range(0, 0x200, 2):
        v = struct.unpack_from("<H", raw, off)[0]
        if v in MODEL_NAME:
            out.append((off, v, MODEL_NAME[v]))
    return out


async def do_info(dbg):
    vehicle = await current_vehicle(dbg)
    if not vehicle:
        pos = transform(await dbg.read(await dbg.u32(PLAYER_BASE), MATRIX_SIZE))[3] \
            if await dbg.u32(PLAYER_BASE) else None
        print("on foot" + ("  pos %.1f %.1f %.1f" % pos if pos else ""))
        return
    raw = await dbg.read(vehicle, MATRIX_SIZE)
    rows = transform(raw)
    print("vehicle at %08x" % vehicle)
    for i, label in enumerate(["row0", "row1", "row2", "pos "]):
        print("  %s  %9.3f %9.3f %9.3f" % (label, *rows[i]))
    cands = await find_model_index(dbg, vehicle)
    if cands:
        print("  model id candidates (offset, value, name):")
        for off, v, name in cands[:20]:
            print("    +0x%03x  %3d  %-9s %s" % (off, v, name, describe_model(v)[1]))
    else:
        print("  no plausible model id in the first 0x200 bytes")


async def do_record(dbg, label):
    vehicle = await current_vehicle(dbg)
    if not vehicle:
        sys.exit("not in a vehicle - get in one first")
    raw = await dbg.read(vehicle, STRUCT_SIZE)
    os.makedirs(REC_DIR, exist_ok=True)
    path = os.path.join(REC_DIR, label + ".bin")
    with open(path, "wb") as f:
        f.write(raw)
    meta = os.path.join(REC_DIR, label + ".json")
    with open(meta, "w") as f:
        json.dump({"address": vehicle, "size": STRUCT_SIZE}, f)
    print("recorded '%s': %d bytes from %08x" % (label, len(raw), vehicle))
    cands = await find_model_index(dbg, vehicle)
    for off, v, name in cands[:12]:
        print("    +0x%03x  %3d  %-9s %s" % (off, v, name, describe_model(v)[1]))


def do_diff(labels):
    blobs = {}
    for label in labels:
        path = os.path.join(REC_DIR, label + ".bin")
        if not os.path.exists(path):
            sys.exit("no recording named '%s' (looked in %s)" % (label, REC_DIR))
        blobs[label] = open(path, "rb").read()

    size = min(len(b) for b in blobs.values())
    print("offsets where the recordings differ (u8 / u16 / u32 views)")
    print("%-7s %s" % ("offset", "  ".join("%-12s" % l for l in labels)))
    shown = 0
    for off in range(0, size, 4):
        vals = [struct.unpack_from("<I", blobs[l], off)[0] for l in labels]
        if len(set(vals)) == 1:
            continue
        # Small integers are what a type enum or a model index looks like; pointers and floats
        # differ between any two objects and say nothing.
        if not all(v < 0x10000 for v in vals):
            continue
        print("+0x%04x %s" % (off, "  ".join("%-12d" % v for v in vals)))
        shown += 1
        if shown > 200:
            print("... truncated")
            break
    if not shown:
        print("(no small-integer field differs - try widening the filter)")


async def speed(dbg, vehicle, gap=0.2):
    """Rough speed in world units per second, from two position reads."""
    a = transform(await dbg.read(vehicle, MATRIX_SIZE))[3]
    await asyncio.sleep(gap)
    b = transform(await dbg.read(vehicle, MATRIX_SIZE))[3]
    return length(sub(b, a)) / gap


async def settle(dbg, vehicle, base, limit=6.0, still=0.6):
    """Wait for the vehicle to stop drifting before a measurement.

    Without this the first probe's momentum lands in the second probe's numbers, which is
    exactly how "Square moved the car forwards 12 units" gets recorded - it was the coast from
    the previous Cross, not braking. Returns the speed it gave up at.
    """
    deadline = time.time() + limit
    v = await speed(dbg, vehicle)
    while v > still and time.time() < deadline:
        v = await speed(dbg, vehicle)
    return v


async def measure(dbg, vehicle, hold, frames, base=None):
    """Hold something, watch the transform, release. Returns (before, after, residual speed)."""
    # Everything off first, then the base input (Cross for an aircraft, so the rotor is turning
    # and pitch/roll have something to act on), then let it stop drifting.
    await dbg.buttons(**{b: False for b in ALL_BUTTONS})
    await dbg.analog(0.0, 0.0)
    if base:
        await dbg.buttons(**{base: True})
        await asyncio.sleep(1.0)
    drift = await settle(dbg, vehicle, base)

    before = transform(await dbg.read(vehicle, MATRIX_SIZE))
    await hold(True)
    await asyncio.sleep(frames / 30.0)
    after_held = transform(await dbg.read(vehicle, MATRIX_SIZE))
    await hold(False)
    return before, after_held, drift


async def do_probe(dbg, frames, use_sticks, use_all, base):
    vehicle = await current_vehicle(dbg)
    if not vehicle:
        sys.exit("not in a vehicle - get in one first")

    cands = await find_model_index(dbg, vehicle)
    print("vehicle at %08x, holding each input for %d frames (~%.1fs)%s"
          % (vehicle, frames, frames / 30.0, ", with %s held" % base if base else ""))
    if cands:
        guess = ", ".join("%s?" % c[2] for c in cands[:3])
        print("model id candidates: %s" % guess)
    print()
    print("%-12s %8s %8s %8s %8s %7s   %s"
          % ("input", "dx", "dy", "dz", "dist", "drift", "rotation of row0/row1/row2 (deg)"))

    inputs = []
    for name in (ALL_BUTTONS if use_all else SAFE_BUTTONS):
        def make(n):
            async def hold(down):
                await dbg.buttons(**{n: down})
            return hold
        inputs.append((name, make(name)))
    if use_sticks:
        for name, x, y in STICKS:
            def make(sx, sy):
                async def hold(down):
                    await dbg.analog(sx if down else 0.0, sy if down else 0.0)
                return hold
            inputs.append((name, make(x, y)))

    for name, hold in inputs:
        # Holding the base input again is pointless and reads as "no effect".
        if base and name == base:
            continue
        try:
            before, after, drift = await measure(dbg, vehicle, hold, frames, base)
        except RuntimeError as e:
            print("%-12s  error: %s" % (name, e))
            continue
        # The vehicle pointer can go away mid-probe (the player got ejected, the game reloaded).
        # Reporting movement from a stale pointer would be worse than stopping.
        if await current_vehicle(dbg) != vehicle:
            print("%-12s  vehicle changed - stopping" % name)
            break
        dpos = sub(after[3], before[3])
        rot = [angle_between(before[i], after[i]) for i in range(3)]
        print("%-12s %8.2f %8.2f %8.2f %8.2f %7.1f   %6.1f %6.1f %6.1f"
              % (name, dpos[0], dpos[1], dpos[2], length(dpos), drift,
                 rot[0], rot[1], rot[2]))

    # Never leave an injected button or a deflected stick behind.
    await dbg.buttons(**{b: False for b in ALL_BUTTONS})
    await dbg.analog(0.0, 0.0)
    print("\nreleased everything")


async def do_watch(dbg, seconds):
    print("watching for %ds - get in and out of vehicles" % seconds)
    start = time.time()
    prev = None
    while time.time() - start < seconds:
        vehicle = await current_vehicle(dbg)
        if vehicle != prev:
            if vehicle:
                cands = await find_model_index(dbg, vehicle)
                names = ", ".join("+0x%03x=%s" % (o, n) for o, _, n in cands[:4])
                print("%6.1fs  entered %08x   %s" % (time.time() - start, vehicle, names or "?"))
            else:
                print("%6.1fs  on foot" % (time.time() - start))
            prev = vehicle
        await asyncio.sleep(0.2)
    print("done")


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=["info", "probe", "record", "diff", "watch"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--frames", type=int, default=60,
                    help="how long probe holds each input, in game frames at 30fps")
    ap.add_argument("--stick", action="store_true", help="probe also deflects the nub")
    ap.add_argument("--all", action="store_true",
                    help="probe Triangle and Start too - they exit the vehicle and pause")
    ap.add_argument("--base", metavar="BUTTON",
                    help="hold this button throughout, e.g. --base cross so a helicopter's "
                         "rotor is turning and pitch/roll have something to act on")
    args = ap.parse_args()

    if args.command == "diff":
        if len(args.args) < 2:
            sys.exit("diff needs at least two recording names")
        return do_diff(args.args)

    async with connect(args.port) as ws:
        dbg = Debugger(ws)
        if args.command == "info":
            await do_info(dbg)
        elif args.command == "probe":
            if args.base and args.base not in ALL_BUTTONS:
                sys.exit("--base must be one of: %s" % ", ".join(ALL_BUTTONS))
            await do_probe(dbg, args.frames, args.stick, args.all, args.base)
        elif args.command == "record":
            if not args.args:
                sys.exit("record needs a label, e.g. `record heli`")
            await do_record(dbg, args.args[0])
        elif args.command == "watch":
            await do_watch(dbg, int(args.args[0]) if args.args else 300)


if __name__ == "__main__":
    asyncio.run(main())
