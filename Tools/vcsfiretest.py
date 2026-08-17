#!/usr/bin/env python3
"""Decide whether VCS's shot can be redirected at the raycast, the way re3 does free aim.

re3 and reVC do free aim entirely at the fire site: CWeapon::FireInstantHit picks the bullet's
source and target from the CAMERA (Find3rdPersonCamTargetVector) instead of from the ped's aim,
and everything else - the crosshair, the arm IK - is presentation. Porting that here needs one
question answered, and no amount of reading answers it:

    does overwriting source/target at the raycast actually move the shot?

If it does, the model ports and the aim-response inversion in VCSCamera.cpp stops being needed
for free aim. If it doesn't, the shot resolves downstream and the hook is not worth building.

    # arm every candidate site, observe only, write nothing
    python Tools/vcsfiretest.py --shots 4

    # alternate control / deflected shots
    python Tools/vcsfiretest.py --shots 6 --yaw 25

**Finding the sites.** CWeapon::Fire is 0x08a45338 - identified by reVC's `m_nAmmoTotal <
25000` check, caught with a write breakpoint on clip ammo. It calls 0x08a4e3a0, which holds the
two raycasts below; re3's FireInstantHit has the same split between its pointed-gun branch and
its free-fire branch. Note Fire does NOT reach 0x08ac811c, the site a bInclude*-bracket search
originally turned up - that is a different fire path and a pistol never goes near it.

Source and target are read from a0/a1 at the breakpoint rather than from fixed stack offsets,
so this works unchanged at any ProcessLineOfSight call site you point it at.

**Reading the result.** A hit point that swings with the written target is necessary, not
sufficient: it shows the raycast followed us, not that the damage and the tracer did. Confirm
those by eye at a large --yaw, where the impact should visibly land off to one side.
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

# CWorld::ProcessLineOfSight. Break HERE, conditioned on the return address, rather than on the
# `jal` at the call site: a breakpoint on the call site never once fired, while this is the
# mechanism that identified the site in the first place and tripped reliably every time.
LOS_ENTRY = 0x0889786C

# Where a free-fired pistol raycasts from, established live: break on CWeapon::Fire (0x08a45338,
# found via a write breakpoint on clip ammo), arm the collision family behind an `ra` condition,
# and read back which one it reaches. Reproduced on consecutive shots.
DEFAULT_SITES = [0x08A41D74]


class Debugger:
    """PPSSPP does not reliably echo the ticket on every reply - its error reply to cpu.resume
    does not - so match on ticket OR event name. Matching on ticket alone silently hangs until
    a timeout, which is what made two earlier runs look like the breakpoint never fired."""

    def __init__(self, ws):
        self.ws = ws
        self.n = 0
        self.queue = []

    async def call(self, event, **params):
        self.n += 1
        tag = f"t{self.n}"
        await self.ws.send(json.dumps({"event": event, "ticket": tag, **params}))
        try:
            for _ in range(32):
                msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
                if msg.get("event") == "cpu.stepping":
                    self.queue.append(msg)
                    continue
                if msg.get("ticket") == tag or msg.get("event") in (event, "error"):
                    if msg.get("event") == "error":
                        raise RuntimeError(f"{event}: {msg.get('message')}")
                    return msg
        except asyncio.TimeoutError:
            raise RuntimeError(f"{event}: no reply in 15s")
        raise RuntimeError(f"{event}: no matching reply")

    async def stepping(self, timeout):
        if self.queue:
            return self.queue.pop(0)
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), timeout))
            if msg.get("event") == "cpu.stepping":
                return msg

    async def reg(self, name):
        return (await self.call("cpu.getReg", name=name))["uintValue"]

    async def readv(self, addr):
        r = await self.call("memory.read", address=addr, size=12, replacements=False)
        return struct.unpack("<3f", base64.b64decode(r["base64"]))

    async def writev(self, addr, v):
        await self.call("memory.write", address=addr,
                        base64=base64.b64encode(struct.pack("<3f", *v)).decode())


def deflect(source, target, yaw_deg):
    """Rotate the shot about Z, keeping its length - what a crosshair offset does in re3."""
    dx, dy, dz = (t - s for t, s in zip(target, source))
    a = math.radians(yaw_deg)
    ca, sa = math.cos(a), math.sin(a)
    return (source[0] + dx * ca - dy * sa, source[1] + dx * sa + dy * ca, source[2] + dz)


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


async def run(port, sites, shots, yaw, timeout):
    async with websockets.connect(f"ws://127.0.0.1:{port}/debugger", subprotocols=[SUBPROTOCOL],
                                  max_size=None, ping_interval=None, ping_timeout=None,
                                  open_timeout=10, close_timeout=5) as ws:
        dbg = Debugger(ws)
        posts = {s + 8: s for s in sites}
        try:
            # One conditional breakpoint on the raycast's entry per site, plus a plain one at
            # each return address to read the colPoint back once the call has filled it in.
            cond = " || ".join(f"ra == 0x{s + 8:x}" for s in sites)
            await dbg.call("cpu.breakpoint.add", address=LOS_ENTRY, enabled=True, condition=cond)
            for s in sites:
                await dbg.call("cpu.breakpoint.add", address=s + 8, enabled=True)
            print(f"armed {LOS_ENTRY:08x} when {cond}")
            print(f"fire {shots} shots"
                  + (f", every other one deflected {yaw:+g} deg\n" if yaw else ", observing only\n"))

            head = f"{'#':>2} {'site':<10} {'mode':<9} {'hit point':<27} {'moved':>7}  hit"
            print(head + "\n" + "-" * len(head))

            control = None
            for shot in range(1, shots + 1):
                deflected = bool(yaw) and shot % 2 == 0

                while True:
                    brk = await dbg.stepping(timeout)
                    if brk.get("pc") == LOS_ENTRY:
                        break
                    await dbg.call("cpu.resume")  # a stale post-break, or something else

                # We broke on the raycast's entry, so the caller identifies the site.
                pc = (await dbg.reg("ra")) - 8
                a0, a1, a2 = [await dbg.reg(r) for r in ("a0", "a1", "a2")]
                source, target = await dbg.readv(a0), await dbg.readv(a1)
                if deflected:
                    await dbg.writev(a1, deflect(source, target, yaw))
                await dbg.call("cpu.resume")

                post = await dbg.stepping(timeout)
                if posts.get(post.get("pc")) != pc:
                    print(f"{shot:>2}  unexpected stop at {post.get('pc', 0):08x}")
                    await dbg.call("cpu.resume")
                    continue
                hit = await dbg.reg("v0")
                colpoint = await dbg.readv(a2)
                await dbg.call("cpu.resume")

                if not deflected:
                    control, moved = colpoint, 0.0
                else:
                    moved = dist(colpoint, control) if control else float("nan")

                print(f"{shot:>2} {pc:08x}   {'deflected' if deflected else 'control':<9} "
                      f"{'(%8.2f %8.2f %8.2f)' % colpoint:<27} {moved:>7.2f}  "
                      f"{'y' if hit else 'n'}")
        finally:
            # Always disarm. A breakpoint left behind stops the game dead on the next shot.
            for a in [LOS_ENTRY] + [s + 8 for s in sites]:
                try:
                    await dbg.call("cpu.breakpoint.remove", address=a)
                except Exception:
                    pass
            try:
                await dbg.call("cpu.resume")
            except Exception:
                pass
            print("\nbreakpoints removed")

        if yaw:
            print("A 'moved' distance tracking the deflection means the raycast follows the\n"
                  "written target - necessary for porting re3's model, not sufficient. Check\n"
                  "that the tracer and the damage follow too, by eye.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--shots", type=int, default=6)
    ap.add_argument("--yaw", type=float, default=0.0,
                    help="degrees to rotate the shot on alternate shots; 0 observes only")
    ap.add_argument("--sites", default=",".join(hex(s) for s in DEFAULT_SITES),
                    help="comma-separated ProcessLineOfSight call sites to instrument")
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()
    sites = [int(s, 0) for s in args.sites.split(",")]
    try:
        asyncio.run(run(args.port, sites, args.shots, args.yaw, args.timeout))
    except asyncio.TimeoutError:
        sys.exit("timed out waiting for a shot - no raycast from those sites happened.")
    except RuntimeError as e:
        # Distinct from the above on purpose: an earlier version reported every timeout as
        # "no shot", which hid a breakpoint that HAD fired and a reply that never arrived.
        sys.exit(f"debugger call failed: {e}")
    except (websockets.exceptions.ConnectionClosed, ConnectionResetError, OSError) as e:
        sys.exit(f"lost the debugger connection ({type(e).__name__}) - PPSSPP has probably "
                 "exited.\nRows printed above are still valid.")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
