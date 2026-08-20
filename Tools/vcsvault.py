#!/usr/bin/env python3
"""Read the vault probe's last question and answer - live, or out of a savestate.

The Vault tab in the debugger window shows the same numbers, but reading six floats off a screen
and typing them into a conversation is both slow and lossy. The world-query block lives in PSP
memory, so it can be read directly: the exact sample points that were fired, the exact heights
that came back, and the handshake words that say whether the call reached the game at all.

    python Tools/vcsvault.py --live                 # PPSSPP running with --debugger=1337
    python Tools/vcsvault.py --live --watch         # keep printing while you walk around
    python Tools/vcsvault.py <state.ppst>           # from a savestate, no emulator needed

The block only exists while "Vaulting" is ticked in the Vault tab - the install is gated on it, so
that PPSSPP takes nothing from the game's memory partition for a feature nobody switched on.

Its layout is defined in Core/VCS/VCSWorld.cpp. The magic word is what finds it, since the address
comes from userMemory and is different every run.
"""

import argparse
import asyncio
import base64
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vcsstatic import load_ram, RAM_BASE   # noqa: E402

MAGIC = 0x56435357          # 'VCSW', written last so a half-built block never matches
SEQ_OFF, DONE_OFF, COUNT_OFF = 0x04, 0x08, 0x0c
DISPATCHES_OFF, THREAD_OFF, HOST_OFF = 0x14, 0x18, 0x1c
REFUSED_THREAD_OFF, REFUSED_WHY_OFF, REFUSALS_OFF = 0x1a0, 0x1a4, 0x1a8
IN_OFF, IN_STRIDE = 0x20, 12
OUT_OFF, OUT_STRIDE = 0x80, 8
CODE_OFF = 0xc0
BLOCK_SIZE = 0x200
STUB_FIRST_OP = 0x27BDFFE0  # addiu $sp, $sp, -0x20, the program's own first instruction

PLAYER_BASE = 0x08bc8170    # -> the player ped
ENTITY_POS = 0x30
PED_HEADING = 0x8d0
PED_SPAN = 0x900            # enough of the ped struct to cover position and heading

SAMPLE_NAMES = ["footing", "near", "mid", "far", "landing"]


class Image:
    """Random access over one or more (base, bytes) chunks.

    Both paths produce one of these - a savestate is a single 32 MB chunk, a live read is three
    small ones - so the decoding below never has to know which it is looking at.
    """

    def __init__(self, chunks):
        self.chunks = chunks

    def _at(self, addr, n):
        for base, blob in self.chunks:
            if base <= addr and addr + n <= base + len(blob):
                return blob, addr - base
        raise KeyError("%08x is outside every chunk" % addr)

    def word(self, addr):
        blob, off = self._at(addr, 4)
        return struct.unpack_from("<I", blob, off)[0]

    def flt(self, addr):
        blob, off = self._at(addr, 4)
        return struct.unpack_from("<f", blob, off)[0]


def looks_like_block(blob, off=0):
    """A single magic word can collide - it is one word out of eight million. The head of a real
    block cannot: it also carries a plausible sample count and the program's first instruction.

    Or a JIT block marker where that instruction should be. Once PPSSPP has compiled the program -
    which it does the first time the game runs it - the live first word is 0x68xxxxxx, not the
    instruction we wrote. Exactly the trap that kept the install check failing, one level up.
    """
    if struct.unpack_from("<I", blob, off + COUNT_OFF)[0] > 8:
        return False
    head = struct.unpack_from("<I", blob, off + CODE_OFF)[0]
    return head == STUB_FIRST_OP or (head & 0xff000000) == 0x68000000


def find_block_offline(ram):
    needle = struct.pack("<I", MAGIC)
    start = 0
    while True:
        i = ram.find(needle, start)
        if i < 0:
            return None
        start = i + 4
        if i % 4 == 0 and i + BLOCK_SIZE <= len(ram) and looks_like_block(ram, i):
            return RAM_BASE + i


# --- live ---------------------------------------------------------------------------------------

async def _read(dbg, addr, size):
    resp = await dbg.call("memory.read", address=addr, size=size, timeout=30.0)
    return base64.b64decode(resp["base64"])


async def _live_snapshot(port, known_block):
    """One connection, one instant: the block, the player pointer and the ped struct.

    Fetching them together matters - the five samples are meaningless next to a ped position read
    a second later, which is exactly what a per-value read loop would produce.
    """
    from vcsscan import connect, Debugger, detect_ram_size   # only --live needs websockets

    async with connect(port) as ws:
        dbg = Debugger(ws)

        block = known_block
        blob = None
        if block is not None:
            blob = await _read(dbg, block, BLOCK_SIZE)
            if not looks_like_block(blob):
                block, blob = None, None    # it moved, or the feature was toggled off and on

        if block is None:
            ram = await detect_ram_size(dbg)
            resp = await dbg.call("memory.search", address=RAM_BASE, size=ram, type="u32",
                                  value=MAGIC, maxResults=32, timeout=60.0)
            for addr in resp.get("matches", []):
                cand = await _read(dbg, addr, BLOCK_SIZE)
                if looks_like_block(cand):
                    block, blob = addr, cand
                    break
        if block is None:
            return None, None

        chunks = [(block, blob)]
        player = await _read(dbg, PLAYER_BASE, 4)
        chunks.append((PLAYER_BASE, player))
        ped = struct.unpack_from("<I", player, 0)[0]
        if ped:
            chunks.append((ped, await _read(dbg, ped, PED_SPAN)))
        return block, Image(chunks)


# --- reporting ----------------------------------------------------------------------------------

def tag_text(tag):
    """Four-character tags are written as a little-endian word so they read as text in a dump."""
    return "".join(chr((tag >> (8 * i)) & 0xff) for i in range(4)).strip(chr(0))


def report(img, block):
    seq = img.word(block + SEQ_OFF)
    done = img.word(block + DONE_OFF)
    count = img.word(block + COUNT_OFF)
    print("block %08x   seq %d   done %d   count %d" % (block, seq, done, count))

    # The handshake, mirrored into the block by the host side - see Core/VCS/VCSWorld.cpp. seq is
    # how many questions were prepared, dispatches how many were carried into the game by a
    # syscall, done how many came back. Any two of those disagreeing names the broken step.
    dispatches = img.word(block + DISPATCHES_OFF)
    thread = img.word(block + THREAD_OFF)
    host = tag_text(img.word(block + HOST_OFF)) or "nothing"
    print("prepared %d   dispatched %d   answered %d   carried by %s   main thread %s"
          % (seq, dispatches, done, host,
             "unknown" if thread == 0xffffffff else "0x%08x" % thread))
    # One question in flight at any instant is normal - it is prepared on one tick and carried by
    # the next display-list submit. More than one means nothing is carrying them.
    if seq - dispatches > 1:
        print("  requests are being prepared and not carried into the game.")
        why = tag_text(img.word(block + REFUSED_WHY_OFF))
        refusals = img.word(block + REFUSALS_OFF)
        rthread = img.word(block + REFUSED_THREAD_OFF)
        if refusals:
            reason = {
                "intr": "a host fired inside an INTERRUPT handler, which is not a thread",
                "thrd": "a host fired on the wrong thread",
                "noth": "the game's main loop thread was never identified",
            }.get(why, "refused: " + why)
            print("  %d attempt(s) refused - %s" % (refusals, reason))
            print("  last attempt came from thread %s"
                  % ("unknown" if rthread == 0xffffffff else "0x%08x" % rthread))
        else:
            print("  no host syscall fired at all while a question was waiting")

    if seq == 0:
        print("  the block is installed but nothing has ever asked it anything -")
        print("  the probe only runs in the OnFoot context")
        return
    if done != seq:
        # One outstanding request at any instant is normal, the query is asynchronous. A done that
        # stays behind means the call is not reaching the game.
        print("  NOTE: the last request was still outstanding when this was read")

    ped = img.word(PLAYER_BASE)
    px, py, pz = (img.flt(ped + ENTITY_POS + 0), img.flt(ped + ENTITY_POS + 4),
                  img.flt(ped + ENTITY_POS + 8))
    print("ped %08x at (%.3f, %.3f, %.3f)  heading %.3f rad"
          % (ped, px, py, pz, img.flt(ped + PED_HEADING)))

    rows = []
    for i in range(min(count, 8)):
        ix = block + IN_OFF + i * IN_STRIDE
        ox = block + OUT_OFF + i * OUT_STRIDE
        rows.append({
            "name": SAMPLE_NAMES[i] if i < len(SAMPLE_NAMES) else str(i),
            "from": (img.flt(ix), img.flt(ix + 4), img.flt(ix + 8)),
            "z": img.flt(ox),
            "found": img.word(ox + 4) != 0,
        })
    if not rows:
        return

    footing = rows[0]
    print()
    print("%-9s %-28s %-10s %-10s %s"
          % ("sample", "line dropped from", "hit z", "over foot", "distance"))
    for i, r in enumerate(rows):
        fx, fy, fz = r["from"]
        dist = ((fx - rows[0]["from"][0]) ** 2 + (fy - rows[0]["from"][1]) ** 2) ** 0.5
        if not r["found"]:
            height = "-"
        elif footing["found"]:
            height = "%+.3f" % (r["z"] - footing["z"])
        else:
            height = "?"
        print("%-9s (%8.2f,%8.2f,%7.2f) %-10s %-10s %s"
              % (r["name"], fx, fy, fz, "%.3f" % r["z"] if r["found"] else "nothing",
                 height, "%.2f ahead" % dist if i else ""))

    if footing["found"]:
        print()
        print("ped origin sits %.3f above its footing" % (pz - footing["z"]))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("state", nargs="?", help="a .ppst savestate; omit when using --live")
    ap.add_argument("--live", action="store_true", help="read a running PPSSPP instead")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--watch", action="store_true", help="keep reading (--live only)")
    ap.add_argument("--every", type=float, default=1.0, help="seconds between watch reads")
    args = ap.parse_args()

    if args.live:
        block = None
        while True:
            block, img = asyncio.run(_live_snapshot(args.port, block))
            if img is None:
                sys.exit("no world-query block in memory - is 'Vaulting' ticked in the Vault tab?")
            report(img, block)
            if not args.watch:
                return
            print("-" * 78)
            time.sleep(args.every)

    if not args.state:
        ap.error("pass a savestate, or --live")
    ram = load_ram(args.state)
    block = find_block_offline(ram)
    if block is None:
        sys.exit("no world-query block in this state - is 'Vaulting' ticked in the Vault tab?")
    report(Image([(RAM_BASE, ram)]), block)


if __name__ == "__main__":
    main()
