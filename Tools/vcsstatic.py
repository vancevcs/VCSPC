#!/usr/bin/env python3
"""Read GTA:VCS's code and memory offline, out of a PPSSPP savestate.

The point of this over Tools/vcsdisasm.py is that nothing has to be running. No emulator, no
WebSocket, and - the part that actually saves the time - no need to manoeuvre the game into the
state you want to look at before you can look at it. A savestate is a complete RAM image at the
addresses the game really uses, so `--disasm` answers "what does this code do" and `--read`
answers "what was in memory at that moment", both from a file.

The entire aim response model in Core/VCS/VCSCamera.cpp was worked out with this, in one sitting,
with the emulator closed. See "Why free aim felt like a thumbstick" in CLAUDE.md.

    pip install zstandard capstone

    # what does the weapon-aim camera do with the look axis?
    python Tools/vcsstatic.py <state.ppst> --disasm 0x089a341c 60

    # who calls it?
    python Tools/vcsstatic.py <state.ppst> --callers 0x0898bb4c

    # what was the camera doing when this state was saved?
    python Tools/vcsstatic.py <state.ppst> --read 0x08bc7f1c:f 0x08bc7f18:f 0x08bade60:b

    # dump RAM for another tool to chew on
    python Tools/vcsstatic.py <state.ppst> --dump ram.bin

Capstone does not decode Allegrex's VFPU opcodes. Those show up as `?vfpu?` with a rough guess at
the family rather than ending the listing - enough to keep reading past them, which is all that
was ever needed. If a value you are tracing disappears into one, that is the limit of this tool.

Savestate layout: a 48-byte SChunkHeader, then a 128-byte title (revision >= 5), then a
zstd-compressed serialized blob. Main RAM is somewhere inside the blob; rather than parse the
whole serialization format, it is located by searching for an instruction pair whose address is
already known - the CameraInputMode test at 0x0898bb4c, which is stable across builds of this
disc and is documented in CLAUDE.md.
"""

import argparse
import struct
import sys

try:
    import zstandard
except ImportError:
    sys.exit("zstandard is required: pip install zstandard")

try:
    from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
except ImportError:
    sys.exit("capstone is required: pip install capstone")

RAM_BASE = 0x08800000
RAM_SIZE = 0x02000000

# 0898bb4c  lbu  a1, -0x3F00(gp)
# 0898bb50  beq  a1, zero, 0x0898bb80
ANCHOR_ADDR = 0x0898BB4C
ANCHOR = struct.pack("<II", 0x9385C100, 0x10A0000B)

_md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)


def load_ram(path, quiet=False):
    blob = open(path, "rb").read()
    if len(blob) < 176:
        sys.exit("not a savestate: too small")
    revision, compress, packed, unpacked = struct.unpack("<iiII", blob[:16])
    git = blob[16:48].split(b"\0")[0].decode(errors="replace")
    title = blob[48:176].split(b"\0")[0].decode(errors="replace")
    if revision < 5:
        sys.exit("savestate revision %d is too old (no title block)" % revision)
    if compress != 2:
        sys.exit("expected zstd (compress=2), got %d" % compress)
    if len(blob) - 176 != packed:
        sys.exit("size mismatch: header says %d, file has %d" % (packed, len(blob) - 176))

    data = zstandard.ZstdDecompressor().decompress(blob[176:], max_output_size=unpacked)

    # The anchor is inside one of four near-identical pad accessors, so it matches more than once.
    # They sit 0x40 apart and the first is the one whose address we know.
    hits, start = [], 0
    while True:
        i = data.find(ANCHOR, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if not hits:
        sys.exit("could not find the anchor instructions - is this a ULUS10160 savestate?")
    expected = [hits[0] + n * 0x40 for n in range(len(hits))]
    if hits != expected:
        print("warning: anchor hits are not the expected 0x40 apart: %s"
              % [hex(h) for h in hits], file=sys.stderr)

    offset = hits[0] - (ANCHOR_ADDR - RAM_BASE)
    if offset < 0:
        sys.exit("anchor found too early in the blob to be main RAM")
    ram = data[offset:offset + RAM_SIZE]

    if not quiet:
        print("%s  (PPSSPP %s)" % (title, git), file=sys.stderr)
        print("RAM: %d bytes at blob offset %#x" % (len(ram), offset), file=sys.stderr)
    return ram


class Image:
    def __init__(self, ram):
        self.ram = ram

    def valid(self, addr, n=4):
        return RAM_BASE <= addr and addr - RAM_BASE + n <= len(self.ram)

    def read(self, addr, n):
        if not self.valid(addr, n):
            sys.exit("%#x is outside the RAM image" % addr)
        return self.ram[addr - RAM_BASE:addr - RAM_BASE + n]

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def disasm(self, addr, count):
        for i in range(count):
            a = addr + i * 4
            w = self.u32(a)
            got = list(_md.disasm(struct.pack("<I", w), a))
            if got:
                print("%08x  %08x  %-10s %s" % (a, w, got[0].mnemonic, got[0].op_str))
            else:
                print("%08x  %08x  %-10s %s" % (a, w, "?vfpu?", vfpu_hint(w)))

    def callers(self, target):
        """Every `jal target` in RAM. The cheapest way to walk a call graph upward."""
        word = struct.pack("<I", (3 << 26) | ((target >> 2) & 0x03FFFFFF))
        out, start = [], 0
        while True:
            i = self.ram.find(word, start)
            if i < 0:
                break
            if i % 4 == 0:
                out.append(RAM_BASE + i)
            start = i + 1
        return out


def vfpu_hint(w):
    """Rough family for the Allegrex-only opcodes capstone rejects. Enough to keep reading."""
    op = w >> 26
    return {
        0x12: "vfpu move to/from (mtv/mfv)",
        0x18: "vfpu arithmetic",
        0x19: "vfpu arithmetic",
        0x1B: "vfpu matrix (vmmul/vtfm)",
        0x1F: "vfpu misc",
        0x34: "lv.s",
        0x36: "lv.q",
        0x3A: "lv/sv family",
        0x3C: "sv.s",
        0x3E: "sv.q",
    }.get(op, "unknown")


def parse_spec(spec):
    """ADDRESS[:TYPE] - f float, b byte, h halfword (signed), w word. Word is the default."""
    if ":" in spec:
        addr, kind = spec.rsplit(":", 1)
    else:
        addr, kind = spec, "w"
    return int(addr, 0), kind


def main():
    ap = argparse.ArgumentParser(
        description="Read VCS code and memory out of a PPSSPP savestate, with nothing running.")
    ap.add_argument("savestate", help="a .ppst from memstick/PSP/PPSSPP_STATE")
    ap.add_argument("--disasm", nargs=2, metavar=("ADDR", "COUNT"),
                    help="disassemble COUNT instructions from ADDR")
    ap.add_argument("--callers", metavar="ADDR",
                    help="every jal targeting ADDR")
    ap.add_argument("--read", nargs="+", metavar="ADDR[:TYPE]",
                    help="read values; TYPE is f/b/h/w, default w")
    ap.add_argument("--dump", metavar="FILE",
                    help="write the raw RAM image to FILE (loads at %#x)" % RAM_BASE)
    args = ap.parse_args()

    if not (args.disasm or args.callers or args.read or args.dump):
        ap.error("nothing to do - pass one of --disasm, --callers, --read, --dump")

    img = Image(load_ram(args.savestate))

    if args.dump:
        open(args.dump, "wb").write(img.ram)
        print("wrote %s (%d bytes, loads at %#x)" % (args.dump, len(img.ram), RAM_BASE))

    if args.disasm:
        img.disasm(int(args.disasm[0], 0), int(args.disasm[1], 0))

    if args.callers:
        target = int(args.callers, 0)
        found = img.callers(target)
        print("%d callers of %#x:" % (len(found), target))
        for a in found:
            print("  %08x" % a)

    for spec in args.read or []:
        addr, kind = parse_spec(spec)
        if kind == "f":
            print("%08x  %g" % (addr, struct.unpack("<f", img.read(addr, 4))[0]))
        elif kind == "b":
            print("%08x  %d" % (addr, img.read(addr, 1)[0]))
        elif kind == "h":
            print("%08x  %d" % (addr, struct.unpack("<h", img.read(addr, 2))[0]))
        else:
            print("%08x  %#010x  (%d)" % (addr, img.u32(addr), img.u32(addr)))


if __name__ == "__main__":
    main()
