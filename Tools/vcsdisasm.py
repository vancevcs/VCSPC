#!/usr/bin/env python3
"""Disassemble a range of VCS code over PPSSPP's WebSocket debugger.

Run PPSSPP with --debugger=1337 and a game loaded, then:
    python vcsdisasm.py 0x0898BB4C 40

Exists to read the pad/camera functions PSPRecomp identified, so their CPad base address and
field offsets can be lifted out of the instruction stream rather than guessed.
"""

import asyncio
import json
import sys

try:
    import websockets
except ImportError:
    sys.exit("websockets is required: pip install websockets")

SUBPROTOCOL = "debugger.ppsspp.org"


async def main(port, addr, count):
    url = "ws://127.0.0.1:%d/debugger" % port
    async with websockets.connect(url, subprotocols=[SUBPROTOCOL], max_size=None,
                                  ping_interval=None, ping_timeout=None,
                                  open_timeout=10, close_timeout=5) as ws:
        req = {"event": "memory.disasm", "address": addr, "count": count,
               "displaySymbols": True, "ticket": "d1"}
        await ws.send(json.dumps(req))
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=20.0))
            if msg.get("ticket") != "d1":
                continue
            if msg.get("event") == "error":
                sys.exit("error: %s" % msg.get("message"))
            for line in msg.get("lines", []):
                print("%08x  %-10s %-8s %s" % (
                    line.get("address", 0),
                    line.get("encoding", ""),
                    line.get("name", ""),
                    line.get("params", "")))
            return


if __name__ == "__main__":
    a = int(sys.argv[1], 0)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    p = int(sys.argv[3]) if len(sys.argv) > 3 else 1337
    asyncio.run(main(p, a, n))
