#!/usr/bin/env python3
"""Cross-reference GTA:VCS code by what it touches, not by what it is called.

vcsstatic.py answers "what does the code at X do" once you know X. This answers "which X".
It works on a RAM dump (`vcsstatic.py <state.ppst> --dump ram.bin`) and finds functions by
structural fingerprints, which is the only handle available with no symbols.

    python Tools/vcsxref.py ram.bin --scanfamily
    python Tools/vcsxref.py ram.bin --callers 0x0889786c --bracketed

`--scanfamily` finds every function that advances CWorld's sector scan code (the u16 at
gp-0x6394, bumped and reset at 0xffff exactly as re3's CWorld::AdvanceCurrentScanCode does).
That set is the collision/line-of-sight family, and sorting it by caller count puts the two
general raycasts at the top. It also reports which incoming argument registers each one masks
to a byte, which is what separates the forms:

    a0,a1 pointers + everything else bool   ->  GetIsLineOfSightClear-shaped (yes/no)
    a0..a3 untouched + t0..t3 bool          ->  ProcessLineOfSight-shaped (returns colPoint, entity)

`--bracketed` filters call sites to those wrapped in `sb <nonzero>, off(gp)` before and
`sb zero, off(gp)` after - re3's CWorld::bIncludeCarTyres / bIncludeDeadPeds / bIncludeBikers
pattern. Weapon fire does that; camera clipping and AI visibility checks do not, so it picks
the shooting code out of ~70 callers without needing to recognise anything about weapons.

That is how CWeapon::FireInstantHit was found - see "The weapon fire path" in
docs/VCS_ADDRESSES.md.
"""

import argparse
import struct
import sys
from collections import defaultdict

RAM_BASE = 0x08800000
# The usable code span. Functions really do run past 0x089f0000 - 0x08adca6c and 0x08b65de8
# are both real - so don't narrow this without checking.
CODE_LO, CODE_HI = 0x08804000, 0x08B70000

SCANCODE_IMM = 0x9C6C  # gp-0x6394, CWorld's current scan code
ARG_REGS = {4: "a0", 5: "a1", 6: "a2", 7: "a3", 8: "t0", 9: "t1", 10: "t2", 11: "t3"}
LOAD_STORE = (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B)


class Ram:
    def __init__(self, path):
        self.data = open(path, "rb").read()

    def word(self, addr):
        return struct.unpack_from("<I", self.data, addr - RAM_BASE)[0]


op = lambda x: x >> 26
rs = lambda x: (x >> 21) & 0x1F
rt = lambda x: (x >> 16) & 0x1F
imm = lambda x: x & 0xFFFF


def simm(x):
    """Sign-extended 16-bit immediate. Do NOT just subtract 0x10000 - positive offsets exist
    (the weapon's bInclude* global is at gp+0x1f88) and get printed as nonsense if you do."""
    v = x & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def func_start(ram, addr, limit=8192):
    """Walk back to the enclosing function's `addiu $sp, $sp, -N`.

    Verify the result before trusting it: a real start is preceded by the delay slot of a
    `jr $ra`. Leaf functions that never touch $sp have no such prologue, so a walk that
    crosses one lands too far back.
    """
    a = addr
    for _ in range(limit):
        x = ram.word(a)
        if op(x) == 0x09 and rs(x) == 29 and rt(x) == 29 and simm(x) < 0:
            return a
        a -= 4
    return None


def arg_profile(ram, start):
    """Which incoming arg registers get masked to a byte (bools) vs dereferenced (pointers)."""
    boolish, ptrish = set(), set()
    for i in range(64):
        x = ram.word(start + i * 4)
        if op(x) == 0x0C and rs(x) in ARG_REGS and imm(x) == 0xFF:  # andi aX, aX, 0xff
            boolish.add(ARG_REGS[rs(x)])
        elif op(x) == 0x00 and (x & 0x3F) == 0x25 and rt(x) in ARG_REGS:  # move sX, aY
            ptrish.add(ARG_REGS[rt(x)])
        elif op(x) in (0x23, 0x31) and rs(x) in ARG_REGS:  # lw / lwc1 off(aY)
            ptrish.add(ARG_REGS[rs(x)])
        elif x == 0x03E00008:
            break
    return sorted(ptrish), sorted(boolish)


def jal_word(addr):
    return (3 << 26) | ((addr >> 2) & 0x03FFFFFF)


def find_callers(ram, target):
    j = jal_word(target)
    return [a for a in range(CODE_LO, CODE_HI, 4) if ram.word(a) == j]


def scan_family(ram):
    hits = []
    for addr in range(CODE_LO, CODE_HI, 4):
        x = ram.word(addr)
        if imm(x) == SCANCODE_IMM and rs(x) == 28 and op(x) in LOAD_STORE:
            hits.append(addr)

    funcs = defaultdict(list)
    for h in hits:
        f = func_start(ram, h)
        if f:
            funcs[f].append(h)

    targets = {jal_word(f): f for f in funcs}
    callers = defaultdict(int)
    for addr in range(CODE_LO, CODE_HI, 4):
        t = targets.get(ram.word(addr))
        if t:
            callers[t] += 1

    print(f"{len(hits)} scan-code accesses in {len(funcs)} functions\n")
    print(f"{'function':<12} {'callers':>7}  {'pointer args':<20} bool args")
    for f in sorted(funcs, key=lambda f: -callers[f]):
        p, b = arg_profile(ram, f)
        print(f"{f:08x}     {callers[f]:>5}  {','.join(p):<20} {','.join(b)}")


def gp_bracket(ram, call, back=18, fwd=12):
    """gp-relative byte globals set non-zero before the call and zeroed after it."""
    before, after = set(), set()
    for i in range(1, back + 1):
        x = ram.word(call - i * 4)
        if op(x) == 0x28 and rs(x) == 28 and rt(x) != 0:
            before.add(imm(x))
    for i in range(1, fwd + 1):
        x = ram.word(call + i * 4)
        if op(x) == 0x28 and rs(x) == 28 and rt(x) == 0:
            after.add(imm(x))
    return sorted(before & after)


def report_callers(ram, target, bracketed):
    calls = find_callers(ram, target)
    print(f"{len(calls)} call sites of {target:08x}\n")
    rows = []
    for c in calls:
        g = gp_bracket(ram, c) if bracketed else []
        if bracketed and not g:
            continue
        rows.append((c, func_start(ram, c), g))

    if bracketed:
        print(f"{len(rows)} bracketed by gp byte globals (the re3 bInclude* pattern)\n")
    print(f"{'call site':<12} {'function':<12} globals")
    for c, f, g in rows:
        fs = f"{f:08x}" if f else "?"
        # Offsets are signed 16-bit but frequently positive - print them as the game sees them.
        off = ", ".join(f"gp{simm(i):+#x} (={0x08BB1D60 + simm(i):08x})" for i in g)
        print(f"{c:08x}     {fs:<12} {off}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ram", help="RAM dump from vcsstatic.py --dump")
    ap.add_argument("--scanfamily", action="store_true", help="find CWorld's collision-scan functions")
    ap.add_argument("--callers", type=lambda s: int(s, 0), help="list call sites of an address")
    ap.add_argument("--bracketed", action="store_true", help="with --callers, keep only bInclude*-bracketed sites")
    args = ap.parse_args()

    ram = Ram(args.ram)
    if args.scanfamily:
        scan_family(ram)
    elif args.callers is not None:
        report_callers(ram, args.callers, args.bracketed)
    else:
        ap.error("give --scanfamily or --callers")


if __name__ == "__main__":
    main()
