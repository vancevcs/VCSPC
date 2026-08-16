#!/usr/bin/env python3
"""Watch the aim-related state while someone plays, and print only what changes.

Answers the question the debugger window can't: which value actually distinguishes locked-on
from free aim, and which pad channel carries the aim once you're there. Four signals were keyed
on by inference and all four were wrong; this settles it by observation instead.

Run PPSSPP with --debugger=1337, get in game, then:

    python Tools/vcsaimwatch.py [seconds] [port]

...and while it runs: hold aim on an NPC until it locks, then break the lock into free aim, then
move the mouse. Every transition gets a timestamped line.
"""

import asyncio
import json
import base64
import struct
import sys
import time

try:
    import websockets
except ImportError:
    sys.exit("websockets is required: pip install websockets")

SUBPROTOCOL = "debugger.ppsspp.org"

PAD = 0x08BDE610

# (label, address, struct format, size)
WATCH = [
    ("IsAiming",     0x08BB32A0, "<I", 4),
    ("IsFreeAiming", 0x08BAFB54, "<I", 4),
    ("CamInputMode", 0x08BADE60, "<B", 1),
    ("nubX",         PAD + 0x02, "<h", 2),
    ("nubY",         PAD + 0x04, "<h", 2),
    ("dpadLeft",     PAD + 0x16, "<h", 2),
    ("dpadRight",    PAD + 0x18, "<h", 2),
    ("dpadUp",       PAD + 0x12, "<h", 2),
    ("dpadDown",     PAD + 0x14, "<h", 2),
]


async def call(ws, event, ticket, **kw):
    kw.update({"event": event, "ticket": ticket})
    await ws.send(json.dumps(kw))
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=20.0))
        if msg.get("ticket") == ticket:
            return msg


async def sample(ws, n):
    out = {}
    for label, addr, fmt, size in WATCH:
        r = await call(ws, "memory.read", "%s%d" % (label, n), address=addr, size=size)
        out[label] = struct.unpack(fmt, base64.b64decode(r["base64"]))[0]
    return out


async def main(seconds, port):
    url = "ws://127.0.0.1:%d/debugger" % port
    async with websockets.connect(url, subprotocols=[SUBPROTOCOL], max_size=None,
                                  ping_interval=None, ping_timeout=None,
                                  open_timeout=10, close_timeout=5) as ws:
        print("watching for %ds - hold aim, lock on, then break into free aim" % seconds)
        print("%-8s %s" % ("time", "  ".join("%-12s" % w[0] for w in WATCH)))
        prev = None
        start = time.time()
        n = 0
        while time.time() - start < seconds:
            n += 1
            cur = await sample(ws, n)
            # Only the nub and d-pad move continuously; treat them as changed only when they
            # cross zero, or the output is one line per frame and unreadable.
            key = {k: (v if k in ("IsAiming", "IsFreeAiming", "CamInputMode") else (v != 0))
                   for k, v in cur.items()}
            if key != prev:
                print("%7.2fs %s" % (time.time() - start,
                      "  ".join("%-12s" % str(cur[w[0]]) for w in WATCH)))
                prev = key
            await asyncio.sleep(0.05)
        print("done")


if __name__ == "__main__":
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 90
    p = int(sys.argv[2]) if len(sys.argv) > 2 else 1337
    asyncio.run(main(secs, p))
