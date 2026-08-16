#!/usr/bin/env python3
"""Memory scanner for the GTA: Vice City Stories address hunt.

PPSSPP's own `memory.search` handles the easy case - scanning for a value you already know,
like health being exactly 100.0. It cannot do the hard case: narrowing down a value you
*can't* read, such as a vehicle pointer. That needs Cheat-Engine-style snapshot and diff,
which is what this adds.

The idea: dump RAM at moments when you know something about the value, then intersect.
For the vehicle pointer, you know it is 0 on foot and a valid pointer while driving:

    python Tools/vcsscan.py snapshot foot1        # standing around
    python Tools/vcsscan.py snapshot car          # now sitting in a car
    python Tools/vcsscan.py snapshot foot2        # back out on foot
    python Tools/vcsscan.py search foot1:zero car:ptr foot2:zero

Three constraints usually cuts 6 million words down to a handful. Put the survivors in the
VCS debugger window's scratchpad and watch them while you get in and out of a car.

Setup:
    Run PPSSPP with --debugger=1337, then pass --port 1337 (the default).
    Needs the `websockets` and `numpy` packages.

See docs/VCS_ADDRESSES.md for what to hunt for and in what order.
"""

import argparse
import asyncio
import base64
import json
import os
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: pip install numpy")

try:
    import websockets
except ImportError:
    sys.exit("websockets is required: pip install websockets")


SUBPROTOCOL = "debugger.ppsspp.org"


class DebuggerTimeout(RuntimeError):
    """The debugger accepted the request but never answered.

    Distinct from a normal error reply because do_snapshot treats a plain failure as
    'past the end of RAM' and stops - a timeout must NOT silently truncate a dump.
    """

# PSP user RAM. The kernel region below 0x08800000 is not interesting for game state, and
# including it would just add noise to every scan.
USER_RAM_BASE = 0x08800000
USER_RAM_MAX = 0x02000000  # 32 MB; we stop early if the emulated console has less.

# Read in chunks - one 32 MB base64 blob in a single JSON message is needlessly hostile to
# both ends.
CHUNK = 1 << 20

SNAP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".vcsscan")


def snap_path(name):
    return os.path.join(SNAP_DIR, name + ".bin")


class Debugger:
    def __init__(self, ws):
        self.ws = ws
        self.ticket = 0

    async def send_only(self, event, **params):
        """Fire and forget. cpu.stepping and cpu.resume send no direct reply - they only
        broadcast later - so waiting for a matching ticket would hang forever."""
        self.ticket += 1
        await self.ws.send(json.dumps(dict(event=event, ticket=self.ticket, **params)))

    async def wait_stepping(self, want, timeout=5.0):
        """Poll cpu.status until the CPU reaches the requested run state."""
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            if (await self.call("cpu.status")).get("stepping") == want:
                return True
            await asyncio.sleep(0.1)
        return False

    async def call(self, event, timeout=5.0, **params):
        self.ticket += 1
        ticket = self.ticket
        await self.ws.send(json.dumps(dict(event=event, ticket=ticket, **params)))

        # Broadcast events (logs, cpu status) arrive unsolicited on the same socket, so keep
        # reading until our own ticket comes back - but bounded. With no game loaded the
        # debugger accepts the connection and then simply never answers a memory read, which
        # without this deadline hangs the whole tool with no output at all.
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise DebuggerTimeout("%s: no response in %.0fs - is a game actually running?"
                                      % (event, timeout))
            try:
                msg = json.loads(await asyncio.wait_for(self.ws.recv(), timeout=remaining))
            except asyncio.TimeoutError:
                raise DebuggerTimeout("%s: no response in %.0fs - is a game actually running?"
                                      % (event, timeout))
            if msg.get("ticket") != ticket:
                continue
            if msg.get("event") == "error":
                raise RuntimeError("%s: %s" % (event, msg.get("message")))
            return msg


def connect(port):
    """Returns the connect object itself, which is the async context manager.

    ping_interval=None matters: PPSSPP's debugger does not answer WebSocket pings, so the
    library's default keepalive tears the connection down after ~20 seconds with a confusing
    "keepalive ping timeout". That kills any long watch, and `live` runs for minutes.
    """
    url = "ws://127.0.0.1:%d/debugger" % port
    # open_timeout matters too: the port is open whenever PPSSPP is running, but if no game is
    # loaded the upgrade can simply never complete, which would otherwise hang before any of
    # the per-request timeouts below could fire - producing no output at all.
    return websockets.connect(url, subprotocols=[SUBPROTOCOL], max_size=None,
                              ping_interval=None, ping_timeout=None,
                              open_timeout=10, close_timeout=5)


async def do_snapshot(port, name):
    os.makedirs(SNAP_DIR, exist_ok=True)
    async with connect(port) as ws:
        dbg = Debugger(ws)

        # Freeze the CPU for the duration of the dump. A 24 MB read takes long enough that a
        # running game would change memory underneath us, giving a snapshot that never actually
        # existed at any single instant - which quietly produces wrong answers when diffed.
        was_stepping = (await dbg.call("cpu.status")).get("stepping", False)
        if not was_stepping:
            await dbg.send_only("cpu.stepping")
            if not await dbg.wait_stepping(True):
                print("warning: could not pause the CPU; snapshot may be inconsistent",
                      file=sys.stderr)

        try:
            out = bytearray()
            addr = USER_RAM_BASE
            end = USER_RAM_BASE + USER_RAM_MAX
            while addr < end:
                size = min(CHUNK, end - addr)
                try:
                    resp = await dbg.call("memory.read", address=addr, size=size, timeout=60.0)
                except DebuggerTimeout:
                    # A hang is NOT the end of RAM - never let it silently truncate a dump.
                    raise
                except RuntimeError:
                    # Past the end of this console's RAM. Everything so far is still valid.
                    break
                out += base64.b64decode(resp["base64"])
                addr += size
                print("\r  %.1f MB" % (len(out) / (1 << 20)), end="", file=sys.stderr)
            print(file=sys.stderr)
        finally:
            # Always hand the game back, even if the dump failed halfway.
            if not was_stepping:
                await dbg.send_only("cpu.resume")
                await dbg.wait_stepping(False)

    if not out:
        sys.exit("read nothing - is a game running, and is the debugger enabled?")

    with open(snap_path(name), "wb") as f:
        f.write(out)
    print("snapshot '%s': %.1f MB from 0x%08x" % (name, len(out) / (1 << 20), USER_RAM_BASE))


def load(name, dtype):
    path = snap_path(name)
    if not os.path.exists(path):
        sys.exit("no snapshot named '%s' (looked in %s)" % (name, SNAP_DIR))
    raw = np.fromfile(path, dtype=np.uint8)
    # Truncate to a whole number of elements so the view is always well formed.
    itemsize = np.dtype(dtype).itemsize
    raw = raw[: (len(raw) // itemsize) * itemsize]
    return raw.view(dtype)


def parse_int(text):
    return int(text, 16) if text.lower().startswith("0x") else int(text)


def apply_term(term, dtype, mask, cache):
    """AND one NAME:PRED term into the running candidate mask."""
    if ":" not in term:
        sys.exit("bad term %r - expected NAME:PRED, e.g. foot1:zero" % term)
    name, pred = term.split(":", 1)

    if name not in cache:
        cache[name] = load(name, dtype)
    values = cache[name]

    if len(mask) == 0:
        mask = np.ones(len(values), dtype=bool)
    n = min(len(mask), len(values))
    mask, values = mask[:n], values[:n]

    if pred == "zero":
        cond = values == 0
    elif pred == "nonzero":
        cond = values != 0
    elif pred == "ptr":
        # A plausible pointer into user RAM. Bound it by how much RAM this console ACTUALLY
        # has, taken from the snapshot's own length - USER_RAM_MAX is only the ceiling we try
        # to read, and using it here would accept addresses past the end of a 24 MB console.
        ram_end = USER_RAM_BASE + len(values) * np.dtype(dtype).itemsize
        # Alignment matters too: real struct pointers are 4-aligned, and requiring it removes
        # a lot of coincidental matches.
        cond = (values >= USER_RAM_BASE) & (values < ram_end) & (values % 4 == 0)
    elif pred.startswith("="):
        cond = values == np.array(parse_int(pred[1:]), dtype=dtype)
    elif pred.startswith("!="):
        cond = values != np.array(parse_int(pred[2:]), dtype=dtype)
    elif pred.startswith("f="):
        target = float(pred[2:])
        # Most of RAM reinterpreted as float is inf/NaN garbage, which makes isclose emit
        # warnings that look like errors but aren't. Compare quietly.
        with np.errstate(invalid="ignore", over="ignore"):
            cond = np.isclose(values, target, rtol=1e-4, atol=1e-4)
    elif pred.startswith("frange:"):
        # Value within a numeric range. An angle in radians lives in about -PI..PI or 0..2PI,
        # which throws out the vast majority of RAM reinterpreted as float.
        lo, hi = (float(x) for x in pred.split(":")[1:3])
        with np.errstate(invalid="ignore", over="ignore"):
            cond = np.isfinite(values) & (values >= lo) & (values <= hi)
    elif pred.startswith("delta:"):
        # |this - other| falls within a range. The sharp filter for an angle: if you rotated the
        # camera by a known amount, the variable holding it must have moved by that much.
        parts = pred.split(":")
        other, lo, hi = parts[1], float(parts[2]), float(parts[3])
        if other not in cache:
            cache[other] = load(other, dtype)
        with np.errstate(invalid="ignore", over="ignore"):
            d = np.abs(values.astype(np.float64) - cache[other][:n].astype(np.float64))
            cond = np.isfinite(d) & (d >= lo) & (d <= hi)
    elif pred.startswith("changed:"):
        other = pred.split(":", 1)[1]
        if other not in cache:
            cache[other] = load(other, dtype)
        cond = values != cache[other][:n]
    elif pred.startswith("same:"):
        other = pred.split(":", 1)[1]
        if other not in cache:
            cache[other] = load(other, dtype)
        cond = values == cache[other][:n]
    else:
        sys.exit("unknown predicate %r" % pred)

    return mask & cond, cache


def do_search(terms, type_name, limit):
    dtype = {"u32": np.uint32, "u16": np.uint16, "u8": np.uint8, "float": np.float32}[type_name]
    itemsize = np.dtype(dtype).itemsize

    mask = np.array([], dtype=bool)
    cache = {}
    for term in terms:
        mask, cache = apply_term(term, dtype, mask, cache)
        print("  after %-24s %d candidates" % (term, int(mask.sum())), file=sys.stderr)

    idx = np.flatnonzero(mask)
    print("\n%d matches" % len(idx))
    if len(idx) == 0:
        print("Nothing survived. Try dropping the last constraint, or take fresh snapshots -")
        print("a value that moves between sessions is inside a heap allocation, not a global.")
        return

    any_name = next(iter(cache))
    values = cache[any_name]
    for i in idx[:limit]:
        addr = USER_RAM_BASE + int(i) * itemsize
        shown = " ".join(
            "%s=%s" % (n, fmt(cache[n][i], dtype)) for n in cache if i < len(cache[n])
        )
        print("  0x%08x  %s" % (addr, shown))
    if len(idx) > limit:
        print("  ... %d more (use --limit)" % (len(idx) - limit))


def fmt(value, dtype):
    if dtype == np.float32:
        return "%.4f" % value
    return "0x%08x" % int(value) if int(value) > 9 else str(int(value))


async def detect_ram_size(dbg):
    """How much user RAM this console actually has. A PSP-1000 has 24 MB, later models more,
    and asking memory.search for a range past the end is rejected outright."""
    for size in (0x02000000, 0x01800000, 0x01000000, 0x00800000):
        try:
            await dbg.call("memory.read_u32", address=USER_RAM_BASE + size - 4)
            return size
        except RuntimeError:
            continue
    sys.exit("could not determine RAM size - is a game running?")


async def do_value(port, type_name, value, limit):
    """Server-side exact-value scan. Fast, but only for values you already know."""
    async with connect(port) as ws:
        dbg = Debugger(ws)
        ram = await detect_ram_size(dbg)
        params = dict(address=USER_RAM_BASE, size=ram, type=type_name,
                      maxResults=min(limit, 100000))
        # float takes a string so integers aren't mistaken for floats, same as cpu.setReg.
        params["value"] = str(value) if type_name == "float" else parse_int(value)
        resp = await dbg.call("memory.search", **params)
        matches = resp.get("matches", [])
        print("%d matches%s" % (len(matches), " (truncated)" if resp.get("truncated") else ""))
        for addr in matches[:limit]:
            print("  0x%08x" % addr)


async def do_live(port, addrs, seconds, every):
    """Watch addresses and report only when one of them CHANGES.

    Printing every sample is useless for the thing you actually want to catch - a value moving
    when you do something in game. You cannot both stare at a scrolling terminal and play. So
    this prints a baseline, then stays quiet until something moves, and says which one moved.
    """
    async with connect(port) as ws:
        dbg = Debugger(ws)

        async def sample():
            out = []
            for addr in addrs:
                try:
                    out.append((await dbg.call("memory.read_u32", address=addr))["value"])
                except RuntimeError:
                    out.append(None)
            return out

        def show(raw):
            if raw is None:
                return "unreadable"
            f = struct.unpack("<f", struct.pack("<I", raw))[0]
            return "%.4f (u32 %d)" % (f, raw)

        prev = await sample()
        print("baseline:")
        for a, v in zip(addrs, prev):
            print("  0x%08x = %s" % (a, show(v)))
        print("\nWatching for %d seconds - go do the thing in game now." % seconds)
        print("Nothing will print until a value changes.\n")

        loop = asyncio.get_event_loop()
        deadline = loop.time() + seconds
        changes = 0
        while loop.time() < deadline:
            await asyncio.sleep(every)
            cur = await sample()
            for a, old, new in zip(addrs, prev, cur):
                if old != new:
                    changes += 1
                    print("  CHANGED  0x%08x : %s  ->  %s" % (a, show(old), show(new)))
            prev = cur

        print("\n%s" % ("done - %d change(s) seen" % changes if changes
                        else "done - nothing changed. Either it wasn't one of these, "
                             "or the thing you did didn't affect them."))


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=1337, help="PPSSPP debugger port (--debugger=PORT)")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("snapshot", help="dump user RAM to a named snapshot")
    s.add_argument("name")

    s = sub.add_parser("search", help="intersect predicates across snapshots")
    s.add_argument("terms", nargs="+", metavar="NAME:PRED",
                   help="zero | nonzero | ptr | =N | !=N | f=N | frange:LO:HI | "
                        "delta:OTHER:MIN:MAX | changed:OTHER | same:OTHER")
    s.add_argument("--type", default="u32", choices=["u32", "u16", "u8", "float"])
    s.add_argument("--limit", type=int, default=40)

    s = sub.add_parser("value", help="live exact-value scan via PPSSPP's memory.search")
    s.add_argument("type", choices=["u32", "u16", "u8", "float"])
    s.add_argument("value")
    s.add_argument("--limit", type=int, default=40)

    s = sub.add_parser("live", help="watch addresses, print only when one changes")
    s.add_argument("addrs", nargs="+")
    s.add_argument("--seconds", type=int, default=180, help="how long to watch (default 180)")
    s.add_argument("--every", type=float, default=0.25, help="poll interval in seconds")

    args = p.parse_args()

    if args.cmd == "snapshot":
        asyncio.run(do_snapshot(args.port, args.name))
    elif args.cmd == "search":
        do_search(args.terms, args.type, args.limit)
    elif args.cmd == "value":
        asyncio.run(do_value(args.port, args.type, args.value, args.limit))
    elif args.cmd == "live":
        asyncio.run(do_live(args.port, [parse_int(a) for a in args.addrs],
                            args.seconds, args.every))


if __name__ == "__main__":
    main()
