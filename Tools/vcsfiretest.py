#!/usr/bin/env python3
"""Decide whether VCS's shot can be redirected at the raycast, the way re3 does free aim.

re3 and reVC do free aim entirely at the fire site: CWeapon::FireInstantHit picks the bullet's
source and target from the CAMERA (Find3rdPersonCamTargetVector) instead of from the ped's aim,
and everything else - the crosshair, the arm IK - is presentation. Porting that here needs one
question answered first, and it is not answerable by reading code:

    does overwriting source/target at the raycast actually move the shot?

If it does, the model ports and the whole aim-response inversion in VCSCamera.cpp becomes
unnecessary for free aim. If it doesn't, the shot is resolved somewhere downstream and we have
saved ourselves building the hook.

This breaks at the call site, rewrites the target vector, resumes, and reads back where the
raycast said it hit - see "The weapon fire path" in docs/VCS_ADDRESSES.md for how those
addresses were found.

    # arm it, then fire in game a few times
    python Tools/vcsfiretest.py --shots 6            # observe only, write nothing
    python Tools/vcsfiretest.py --shots 6 --yaw 25   # alternate control / deflected shots

Run PPSSPP with --debugger=1337. Prefer the interpreter CPU backend: breakpoints are most
reliable there, per AGENTS.md.

**Reading the result.** A hit point that swings with the written target is the answer we want,
and it is a necessary condition, not a sufficient one - it proves the raycast followed us, not
that the damage and the tracer did. Confirm those by eye at a large --yaw, where the impact
should visibly land off to one side.
"""

import argparse
import asyncio
import base64
import json
import math
import struct
import sys

try:
    import websockets
except ImportError:
    sys.exit("websockets is required: pip install websockets")

SUBPROTOCOL = "debugger.ppsspp.org"

# docs/VCS_ADDRESSES.md, "The weapon fire path".
CALL_SITE = 0x08ACC844  # jal CWorld::ProcessLineOfSight
AFTER_CALL = 0x08ACC84C  # first instruction after the delay slot; v0 holds the return
SP_SOURCE = 0x440
SP_TARGET = 0x450
SP_COLPOINT = 0x460


class Debugger:
    def __init__(self, ws):
        self.ws = ws
        self.ticket = 0
        self.pending = {}

    async def call(self, event, **params):
        self.ticket += 1
        tag = f"t{self.ticket}"
        await self.ws.send(json.dumps({"event": event, "ticket": tag, **params}))
        while True:
            msg = json.loads(await self.ws.recv())
            if msg.get("ticket") != tag:
                self.stash(msg)
                continue
            if msg.get("event") == "error":
                raise RuntimeError(f"{event}: {msg.get('message')}")
            return msg

    def stash(self, msg):
        """Broadcasts arrive interleaved with replies; keep the ones we care about."""
        if msg.get("event") in ("cpu.stepping", "cpu.resume"):
            self.pending.setdefault(msg["event"], []).append(msg)

    async def wait_stepping(self, timeout):
        queued = self.pending.pop("cpu.stepping", [])
        if queued:
            return queued[0]
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), timeout=timeout))
            if msg.get("event") == "cpu.stepping":
                return msg

    async def reg(self, name):
        return (await self.call("cpu.getReg", name=name))["uintValue"]

    async def read(self, address, size):
        r = await self.call("memory.read", address=address, size=size, replacements=False)
        return base64.b64decode(r["base64"])

    async def write(self, address, data):
        await self.call("memory.write", address=address, base64=base64.b64encode(data).decode())


def vec(buf, off=0):
    return struct.unpack_from("<3f", buf, off)


def deflect(source, target, yaw_deg):
    """Rotate the shot direction about Z, keeping its length - the same thing a crosshair
    offset does in re3, just imposed rather than derived from the camera."""
    d = [t - s for t, s in zip(target, source)]
    a = math.radians(yaw_deg)
    ca, sa = math.cos(a), math.sin(a)
    return (
        source[0] + d[0] * ca - d[1] * sa,
        source[1] + d[0] * sa + d[1] * ca,
        source[2] + d[2],
    )


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


async def run(port, shots, yaw, timeout):
    url = f"ws://127.0.0.1:{port}/debugger"
    async with websockets.connect(url, subprotocols=[SUBPROTOCOL], max_size=None,
                                  ping_interval=None, ping_timeout=None,
                                  open_timeout=10, close_timeout=5) as ws:
        dbg = Debugger(ws)
        await dbg.call("cpu.breakpoint.add", address=CALL_SITE, enabled=True)
        await dbg.call("cpu.breakpoint.add", address=AFTER_CALL, enabled=True)
        print(f"armed at {CALL_SITE:08x} and {AFTER_CALL:08x}")
        print(f"fire in game - {shots} shots wanted"
              + (f", every other one deflected {yaw:+g} deg\n" if yaw else ", observing only\n"))

        header = f"{'#':>2} {'mode':<9} {'hit point':<26} {'moved':>7}  hit"
        print(header)
        print("-" * len(header))

        control_hit = None
        for shot in range(1, shots + 1):
            deflected = bool(yaw) and shot % 2 == 0

            brk = await dbg.wait_stepping(timeout)
            if brk.get("pc") != CALL_SITE:
                # The post-call breakpoint from a previous shot, or something else stopped us.
                await dbg.call("cpu.resume")
                continue

            sp = await dbg.reg("sp")
            buf = await dbg.read(sp + SP_SOURCE, 0x20)
            source = vec(buf, 0)
            target = vec(buf, SP_TARGET - SP_SOURCE)

            written = target
            if deflected:
                written = deflect(source, target, yaw)
                await dbg.write(sp + SP_TARGET, struct.pack("<3f", *written))

            await dbg.call("cpu.resume")

            post = await dbg.wait_stepping(timeout)
            if post.get("pc") != AFTER_CALL:
                print(f"{shot:>2}  unexpected stop at {post.get('pc', 0):08x}")
                await dbg.call("cpu.resume")
                continue

            hit_flag = await dbg.reg("v0")
            colpoint = vec(await dbg.read(sp + SP_COLPOINT, 12))
            await dbg.call("cpu.resume")

            if not deflected:
                control_hit = colpoint
                moved = 0.0
            else:
                moved = dist(colpoint, control_hit) if control_hit else float("nan")

            mode = "deflected" if deflected else "control"
            pt = "(%8.2f %8.2f %8.2f)" % colpoint
            print(f"{shot:>2} {mode:<9} {pt:<26} {moved:>7.2f}  {'y' if hit_flag else 'n'}")

        await dbg.call("cpu.breakpoint.remove", address=CALL_SITE)
        await dbg.call("cpu.breakpoint.remove", address=AFTER_CALL)
        print("\nbreakpoints removed")
        if yaw:
            print("A 'moved' distance that tracks the deflection means the raycast follows the\n"
                  "written target - the necessary condition for porting re3's model. Confirm the\n"
                  "tracer and the damage follow too, by eye, before trusting it.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--shots", type=int, default=6)
    ap.add_argument("--yaw", type=float, default=0.0,
                    help="degrees to rotate the shot on alternate shots; 0 observes only")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="seconds to wait for you to pull the trigger")
    args = ap.parse_args()
    try:
        asyncio.run(run(args.port, args.shots, args.yaw, args.timeout))
    except asyncio.TimeoutError:
        sys.exit("timed out waiting for a shot - is the breakpoint being reached?\n"
                 "If the game is running and you did fire, suspect the JIT: set CPUCore = 0\n"
                 "in memstick/PSP/SYSTEM/ppsspp.ini and relaunch.")
    except (websockets.exceptions.ConnectionClosed, ConnectionResetError, OSError) as e:
        # Almost always PPSSPP going away underneath us. A traceback here buries the one fact
        # that matters, which is that the emulator is no longer there.
        sys.exit(f"lost the debugger connection ({type(e).__name__}) - PPSSPP has probably "
                 "exited.\nAny rows printed above are still valid; anything after the last one "
                 "never happened.")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
