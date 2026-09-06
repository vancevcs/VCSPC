"""Rewrite the game's tutorial messages to name keyboard keys instead of PSP buttons.

The help lines in the corner of the screen - "To sprint, hold ~k~ ~PDSPR~ while running" - do not
contain a button name at all. They contain a TOKEN, and the game resolves it through a second GXT
entry: `~PDSPR~` looks up `C0PDSPR`, which reads "~X~". So the whole control vocabulary of every
tutorial message is 58 strings in one table, and pointing them at this fork's keyboard bindings is
a data change with no code in it.

    python Tools/vcsgxtkeys.py --verify          # rebuild unchanged, prove the writer is faithful
    python Tools/vcsgxtkeys.py --report          # what maps to what, and what is still unmapped
    python Tools/vcsgxtkeys.py                   # write it where the fork's disc patch reads it
    python Tools/vcsgxtkeys.py --iso IN --iso-out OUT     # or bake a patched copy of the disc

The default output is memstick/PSP/VCS/ENGLISH.GXT, which `VCS::PatchDiscRead` substitutes into
the disc as it is read. The retail ISO is never touched and deleting that file undoes everything.

**A path-based redirect was tried first and cannot work**, which is why the substitution is by
range. Logging every `MetaFileSystem::OpenFile` for a whole boot showed the reason: VCS opens
`disc0:/sce_lbn0x0_size0x65170000` - the whole UMD as one stream - and seeks to its own files by
sector. **No filename is ever requested for anything on the disc.**

`--iso` remains for baking a standalone image, and both routes share one constraint: the game
seeks by sector numbers baked into its own code, so the replacement is padded back to the
original's exact length. Trailing zeroes are inert - a GXT is read through the TABL directory at
its head, which never mentions the file's length.

`C0` through `C3` are the four control CONFIGURATIONS the game's own options screen offers, and it
resolves against whichever is selected. This fork's scheme is fixed, so all four get the same
values; otherwise a player who had changed configuration would see PSP buttons again.

THE MAPPING IS HAND-WRITTEN AND HAS TO BE, and it is the one thing here that can rot: it mirrors
kVCSKeyMappings in Core/VCS/VCSInput.cpp, which is C++ this script cannot read. Change a binding
there and change it here. `--report` lists every token that appears in the game's text with no
mapping, which is what to check after touching either side.
"""

import argparse
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_IN = os.path.join(os.path.dirname(REPO), "disc", "PSP_GAME", "USRDIR", "RUNDATA", "ENGLISH.GXT")
DEFAULT_OUT = os.path.join(REPO, "memstick", "PSP", "VCS", "ENGLISH.GXT")

# Action token -> what this fork's keyboard does instead. Taken row by row from kVCSKeyMappings.
#
# Style follows the strings being replaced: a key is named in capitals the way the controls card
# names it, a device is a lowercase phrase ("the mouse"), because the surrounding sentence reads
# "use ... to look around" and "use THE MOUSE" is the half that has to sound like English.
#
# A value of None means this fork has nothing that does it. The token is then left alone rather
# than filled with a guess - a message naming a PSP button is wrong, but a message naming the
# wrong key is worse, and --report lists them.
KEYS = {
    # On foot
    "PDSPR": "SHIFT",          # sprint
    "PDLT":  "RIGHT MOUSE",    # aim
    "PDFW":  "LEFT MOUSE",     # fire while locked on
    "PDCTL": "Q and E",        # change target
    "PDCWE": "Q and E",        # cycle weapons
    "PDLO1": None,             # look modifier - the mouse needs none, see SENTENCES
    "PDLO2": "the mouse",      # look
    "PDBAK": None,
    "FREE1": "S",              # enter free aim
    "FREE2": "the mouse",      # aim within free aim
    "TGSUB": "G",              # toggle sub-mission: vigilante/taxi/paramedic/fire/air
                               # rescue, every empire action, Trip Skip, cancel a
                               # mission, and recruit. 14 lines use it - grep the token
                               # before assuming one of them is the meaning.
    "CVEIW": "V",              # camera mode
    "SNZI":  "Z",              # scope zoom in
    "SNZO":  "Y",              # scope zoom out
    "SNSLO": "the mouse",      # fine aim
    "ANS":   "TAB",

    # Melee
    "PUNA":  "LEFT MOUSE",     # light attack
    "PUHA":  "SHIFT",          # heavy attack
    "PUBL":  "SPACE",          # block
    "PUGR":  "F",              # grab
    "PUCF":  "TAB",            # take the weapon on the ground
    "PUTE":  "RIGHT MOUSE",

    # Swimming
    "SWIMD": "W A S D",
    "SWIMF": "SHIFT",
    "SWIMO": "SPACE",

    # Shops and menus the game runs in the world
    "AMBUY": "SHIFT",          # the PSP's Cross, which this fork puts on Shift
    "AMEXI": "LEFT MOUSE",     # the PSP's Circle
    "AMLEF": "Q",
    "AMRIG": "E",
    "GOLFU": "SHIFT",

    # Driving
    "VEACC": "W",
    "VEBRK": "S",
    # H_IV_13: "Press ~VEHB~ to use the vehicle's brakes, or reverse if the vehicle has stopped."
    # Retail is "~S~ + ~X~", the same chord as VEWE2 - and here that is the GAME's text being
    # odd, because braking and reversing are Square alone. Left as the single key the sentence is
    # about. Checked rather than assumed: the audit that found VEWE2 flagged this one too, and
    # reading the line it belongs to is what separated them.
    "VEHB":  "S",              # brake / reverse
    "VEEE":  "F",              # get in / out
    "VEHN":  "H",              # horn
    "VECRS": "R and T",        # radio
    "VELL":  "Q",              # glance left
    "VELR":  "E",              # glance right
    "VESTR": "A and D",
    "VETU":  "the mouse",      # tilt the camera - both halves are the mouse, see SENTENCES
    "VETD":  "the mouse",
    "VEMAG": "LEFT MOUSE",
    "VELB":  None,
    "TAXJU": "H",              # taxi boost, the PSP's down button

    # Vehicle weapons and the odd vehicles
    "VEWEP": "LEFT MOUSE",     # fire
    # H_IV_H1, the Hunter's cannon, and the one that was reported. Retail is "~S~ + ~X~" -
    # Square AND Cross, held together, which is a CHORD and not one button. Both contexts that
    # can carry it agree on the keys: W is Cross and S is Square in a vehicle and in an aircraft
    # alike, so "W + S" is deliverable as written.
    #
    # It said "SPACE", which was wrong twice: it collapsed a two-button chord into one key, and
    # the key it picked is explicitly suppressed in the InAircraft context ("no handbrake in the
    # air") - so the line named the one key in that context guaranteed to do nothing.
    #
    # The "+" is kept rather than turned into "and". The game uses "+" for hold-both and "and"
    # for either-one, and the Hunter's cannon is the first.
    "VEWE2": "W + S",          # secondary
    # H_IV_06, the Rhino. Retail is "left button and right button" - the D-PAD, not the
    # triggers, which the game writes as "L button" / "R button". Same pair VECRS above maps,
    # so the same answer: in a vehicle the d-pad's left and right are R and T.
    #
    # It said "the mouse" first, which was a guess about the turret following the camera rather
    # than a reading of the game's own line. The line names a control; use the control.
    "VEWEL": "R and T",        # aim the Rhino's cannon
    "VEWEI": "W and S",        # shift weight on a bike
    "VEWEU": "W and S",
    # H_IV_09, the fire engine. Retail is "analog stick", which VESTR above already maps to
    # "A and D" - in a vehicle those keys ARE the stick, and the mouse is the camera.
    #
    # Worth knowing what this cannot say: a keyboard reaches the vehicle stick's X only, so the
    # cannon's vertical aim has no key at all. Naming the two that exist beats naming a device
    # that does not drive it.
    "FIREH": "A and D",        # firetruck cannon
    # UNRESOLVED, and left alone deliberately. The GXT says FLUP is "left button" and FLDN is
    # "right button"; the mapping rows say CTRL_RIGHT (T) raises the forks and CTRL_LEFT (R)
    # lowers them, marked verified. Those cannot both be true, and the row descriptions were
    # written before R and T were swapped, so they are the likelier to have gone stale.
    #
    # Not guessed either way, because no help line uses these two - only the Controls screen -
    # and a coin-flip here buys nothing. Sit in a forklift, press R, and whichever way the forks
    # go settles it in ten seconds.
    "FLUP":  "T",              # forklift up
    "FLDN":  "R",              # forklift down
    "HERO":  "Q and E",        # helicopter yaw
    "HEPI":  "A and D",        # helicopter roll

    # The Domestobot mission, which has a control scheme of its own
    "DOSLR": "A and D",
    "DOSUD": "W and S",
    "DOSIN": "SHIFT",
    # Retail is "L button and R button" and this names one key, so it is incomplete. Not
    # corrected, because which pair it should be depends on a context nobody has measured - the
    # Domestobot's triggers are TAB and SPACE if it drives as a vehicle and TAB and RIGHT MOUSE
    # if it does not. No help line uses it either. Measure the context before filling it in.
    "DOARM": "TAB",
}

# Whole lines that a token swap cannot save, because the SENTENCE is about the PSP.
#
# Both are the same shape: two PSP controls collapse into one on a mouse, so the original reads
# "use the mouse and the mouse". Rewritten to say the thing once, in the game's own markup.
SENTENCES = {
    ("MAIN", "H_OF_03"): "Use~h~ ~k~ ~PDLO2~ ~w~to look around while on foot.",
    ("MAIN", "H_IV_18"): "Use~h~ ~k~ ~VETU~ ~w~to tilt the game camera.",
}

HEADER = b"TABL"


def parse(data):
    """[(table name, [(key, value)])], in file order."""
    size = struct.unpack_from("<I", data, 4)[0]
    tables = []
    for off in range(8, 8 + size, 12):
        name = data[off:off + 8].split(b"\0")[0].decode("latin-1")
        tables.append((name, struct.unpack_from("<I", data, off + 8)[0]))

    out = []
    for name, pos in tables:
        p = pos if data[pos:pos + 4] == b"TKEY" else pos + 8
        assert data[p:p + 4] == b"TKEY", (name, data[p:p + 8])
        ksize = struct.unpack_from("<I", data, p + 4)[0]
        kstart = p + 8
        dat = kstart + ksize
        assert data[dat:dat + 4] == b"TDAT", (name, data[dat:dat + 8])
        dstart = dat + 8

        entries = []
        for i in range(ksize // 12):
            o = kstart + i * 12
            doff = struct.unpack_from("<I", data, o)[0]
            key = data[o + 4:o + 12].split(b"\0")[0].decode("latin-1")
            s = dstart + doff
            e = s
            while data[e:e + 2] != b"\0\0":
                e += 2
            # The original offset comes along because TKEY is ordered by KEY and TDAT is not:
            # the two orders are independent, and a rebuild has to preserve both.
            entries.append((key, data[s:e].decode("utf-16-le"), doff))
        out.append((name, entries))
    return out


def build(tables):
    """The inverse of parse.

    Two things here were guesses that --verify caught, and both are the kind a naive rebuild gets
    wrong silently:

    Strings are written once each, NOT pooled by value. The original has 2408 entries in MAIN with
    2408 distinct offsets laid out contiguously, so duplicate values really do get duplicate
    copies; pooling them produced a file 8272 bytes short.

    And TKEY is ordered by KEY while TDAT is not. So the pool is packed by walking the entries in
    their ORIGINAL offset order, and the key table is then written back in its own order with the
    new offsets. Writing the strings in key order instead diverged at the very first entry."""
    bodies = []
    for name, entries in tables:
        pool = bytearray()
        placed = {}
        for i in sorted(range(len(entries)), key=lambda n: entries[n][2]):
            placed[i] = len(pool)
            pool += entries[i][1].encode("utf-16-le") + b"\0\0"
        keys = bytearray()
        for i, (key, value, _) in enumerate(entries):
            keys += struct.pack("<I", placed[i]) + key.encode("latin-1").ljust(8, b"\0")
        body = b"TKEY" + struct.pack("<I", len(keys)) + bytes(keys)
        body += b"TDAT" + struct.pack("<I", len(pool)) + bytes(pool)
        bodies.append((name, body))

    tabl = bytearray()
    for name, _ in bodies:
        tabl += name.encode("latin-1").ljust(8, b"\0") + b"\0\0\0\0"  # offset patched below

    out = bytearray(HEADER + struct.pack("<I", len(tabl)) + bytes(tabl))
    for i, (name, body) in enumerate(bodies):
        while len(out) % 4:
            out += b"\0"
        struct.pack_into("<I", out, 8 + i * 12 + 8, len(out))
        if i:  # every table but the first carries its own name again
            out += name.encode("latin-1").ljust(8, b"\0")
        out += body
    return bytes(out)


# "Use the ~VECRS~" has to lose its article, and that is a rule rather than a list of exceptions.
#
# Every PSP name in this table is a noun phrase that wants one - "the up button", "the analog
# stick" - so the help lines were written with "the" in front of the token. Almost nothing on a
# keyboard is: the replacements are either bare key names or already carry their own article, so
# the same sentences come out as "use the the mouse" and "press the LEFT MOUSE".
#
# Seven lines needed it, in five tables including two that only a debug build ever shows, which is
# the argument for doing it by rule: hand-written fixes would have covered the ones somebody
# happened to read.
def strip_article(value, token):
    # The article, then whatever markup sits between it and the token - "the~h~ ~k~ ~FREE2~" is
    # written without a space, so the whitespace after "the" is optional.
    return re.sub(r"(?i)\bthe(\s*(?:~[a-z]~\s*)*)~" + token + r"~", r"\1~" + token + "~", value)


def apply_keys(tables):
    changed = 0
    for name, entries in tables:
        for i, (key, value, off) in enumerate(entries):
            m = re.match(r"^C([0-3])(.+)$", key) if name == "MAIN" else None
            if m and m.group(2) in KEYS and KEYS[m.group(2)] is not None:
                entries[i] = (key, KEYS[m.group(2)], off)
                changed += 1
            elif (name, key) in SENTENCES:
                entries[i] = (key, SENTENCES[(name, key)], off)
                changed += 1
            else:
                # Any other string that names a control: drop the article in front of it. Done
                # after the two cases above rather than beside them, because a C0* value IS a
                # control name and a rewritten sentence was written already correct.
                fixed = value
                for token in KEYS:
                    if ("~%s~" % token) in fixed:
                        fixed = strip_article(fixed, token)
                if fixed != value:
                    entries[i] = (key, fixed, off)
                    changed += 1
    return changed


def report(tables):
    used = {}
    for name, entries in tables:
        for key, value, _ in entries:
            for tok in re.findall(r"~([A-Z0-9]{2,8})~", value):
                used.setdefault(tok, []).append("%s.%s" % (name, key))
    main_table = next(entries for name, entries in tables if name == "MAIN")
    actions = {k[2:] for k, _, _ in main_table if re.match(r"^C0.+", k)}

    print("%-8s %-14s %s" % ("TOKEN", "KEYBOARD", "USED"))
    for tok in sorted(a for a in actions if a in used):
        mapped = KEYS.get(tok, "-- no entry --")
        print("%-8s %-14s %d place(s)" % (tok, mapped if mapped else "** UNMAPPED **", len(used[tok])))
    missing = sorted(a for a in actions if a in used and not KEYS.get(a))
    print("\n%d of %d used tokens have no keyboard name%s" % (
        len(missing), len([a for a in actions if a in used]),
        (": " + ", ".join(missing)) if missing else ""))


def patch_iso(src, dst, original, patched):
    """Copy the disc and overwrite the GXT where it lies.

    The file is found by searching for its own bytes rather than by parsing ISO9660, because the
    thing that has to be right is the OFFSET the game will read from, and matching the whole file
    proves that directly. Padded back to the original length so nothing after it moves - the disc
    is read by sector, and a shorter file would slide every later sector under the game's feet."""
    needle = original[:64]
    offset = -1
    chunk = 1 << 24
    with open(src, "rb") as f:
        pos, prev = 0, b""
        while offset < 0:
            buf = f.read(chunk)
            if not buf:
                break
            hay = prev + buf
            i = hay.find(needle)
            if i >= 0:
                offset = pos - len(prev) + i
            prev = hay[-len(needle):]
            pos += len(buf)
    if offset < 0:
        sys.exit("could not find ENGLISH.GXT inside %s - is it the same build?" % src)

    with open(src, "rb") as f:
        f.seek(offset)
        if f.read(len(original)) != original:
            sys.exit("found the header at 0x%x but the file after it differs - refusing to write" % offset)

    print("ENGLISH.GXT at 0x%x (sector %d); copying the disc..." % (offset, offset // 2048))
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        f.seek(offset)
        f.write(patched + b"\0" * (len(original) - len(patched)))
    print("wrote %d bytes (+%d of padding) -> %s" % (len(patched), len(original) - len(patched), dst))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", default=DEFAULT_IN, help="the game's ENGLISH.GXT")
    ap.add_argument("--out", default=DEFAULT_OUT, help="where the patched copy goes")
    ap.add_argument("--verify", action="store_true", help="rebuild unchanged and compare, byte for byte")
    ap.add_argument("--report", action="store_true", help="print the mapping and what is missing from it")
    ap.add_argument("--iso", help="disc image to take a patched copy of")
    ap.add_argument("--iso-out", dest="iso_out", help="where the patched copy goes")
    args = ap.parse_args()

    if not os.path.exists(args.src):
        sys.exit("no GXT at %s - point --in at a dump of the disc" % args.src)
    data = open(args.src, "rb").read()
    tables = parse(data)

    if args.verify:
        rebuilt = build(tables)
        if rebuilt == data:
            print("round trip is byte-identical (%d bytes, %d tables)" % (len(data), len(tables)))
        else:
            print("round trip DIFFERS: %d in, %d out" % (len(data), len(rebuilt)), file=sys.stderr)
            for i in range(min(len(data), len(rebuilt))):
                if data[i] != rebuilt[i]:
                    print("first difference at 0x%x" % i, file=sys.stderr)
                    break
            sys.exit(1)
        return

    if args.report:
        report(tables)
        return

    changed = apply_keys(tables)
    out = build(tables)
    if len(out) > len(data):
        sys.exit("patched GXT is %d bytes, larger than the original's %d - it cannot be written "
                 "back into the disc's own space. Shorten the longest replacements in KEYS."
                 % (len(out), len(data)))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(out)
    print("%d strings rewritten -> %s (%d bytes, was %d)" % (changed, args.out, len(out), len(data)))

    if args.iso:
        if not args.iso_out:
            sys.exit("--iso needs --iso-out; this does not patch a disc image in place")
        patch_iso(args.iso, args.iso_out, data, out)


if __name__ == "__main__":
    main()
