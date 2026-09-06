#!/usr/bin/env python3
"""Build a distributable folder for the VCS "PC version" fork.

    python Tools/vcspackage.py            # stage into dist/
    python Tools/vcspackage.py --zip      # stage and zip it

What goes in is decided by one question: what does a person who has never seen this repository
need in order to play, and nothing else.

THE MEMORY STICK IS THE WHOLE LAYOUT.  Windows/main.cpp picks `exePath / "memstick"` unless a
file called `installed.txt` sits beside the exe, so a folder that ships its own `memstick/` and
no `installed.txt` is portable by construction - it keeps its saves and settings inside itself
and touches nothing in Documents.  That is also what makes `memstick/PSP/VCS/ENGLISH.GXT`
findable, which is not optional: the keyboard help text is delivered by patching the disc as it
is read, and without that file the game names PSP buttons again.

WHAT IS DELIBERATELY LEFT OUT, because each one is a way to ship something that is not ours or
not the player's:

  the ISO            copyrighted game data.  Everyone brings their own, dropped into this
                     folder - CreateStartScreen boots a single disc image found beside the exe,
                     and falls back to PPSSPP's file browser when there is none or several.
  memstick/SAVEDATA  somebody else's saves, and eight slots of them
  memstick/PSP/PLUGINS  the CLEO plugin is a third-party binary of unknown licence
  memstick/PPSSPP_STATE  savestates, which are development scratch
  memstick/PSP/TEXTURES  the HD pack is gigabytes and is its own release
  vcs.ini            holds GamePath, so shipping it would point at a disc nobody else has
  controls.ini       PPSSPP writes its defaults on first run, and the defaults are what this
                     fork was built against - arrows on the d-pad, Escape on pause
  assets/debugger    the WebSocket debugger's web UI, which is exactly the "debug stuff" a
                     release build should not carry
  the dev ppsspp.ini a machine's graphics tuning, a recent-files list of local paths, and a
                     play-time record.  A minimal one is written instead - see below.

Nothing here strips a debugger out of the binary, because there is none to strip: the ImGui
debugger is Release-gated (VCS::PresentAsGame), the menu bar that carried Debug is gated on the
BUILD rather than the session (VCS::IsGameBuild, which is what a first run needs), and the
WebSocket server only listens when --debugger is passed on the command line.  A Release build IS
the release build.
"""

import argparse
import os
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE_NAME = "GTA Vice City Stories.exe"

# Only the settings this fork actually needs, so the rest arrive as PPSSPP's own defaults. A
# distributed build should carry no opinion it cannot justify.
#
# UseMouse is the one that is NOT optional. PPSSPP gates mouse-delta delivery on it, so with it
# off there is no mouse look and no mouse aiming at all - and the game build has no way into
# PPSSPP's own settings screen to turn it on. A player would meet a port whose headline feature
# is silently absent, with nowhere to look.
#
# CheckForNewVersion is off because this is a fork: pointing it at PPSSPP's update feed would
# offer somebody an "upgrade" that replaces this build with a different program.
MINIMAL_INI = """[General]
CheckForNewVersion = False

[Control]
UseMouse = True
"""

README = """GTA: Vice City Stories - PC version
===================================

A build of PPSSPP that plays only one game, as if it had been made for a PC:
mouse look, mouse aiming, WASD, a menu of its own, and help text that names
keys instead of PSP buttons.


Running it
----------

1. Put your own copy of the game - the USA disc, ULUS10160 - in this folder,
   next to the exe.  It is not included and cannot be.
2. Run "GTA Vice City Stories.exe".

That is all.  A single disc image sitting beside the exe is taken as the one
you meant, and the game starts.

If you would rather keep the disc somewhere else, leave this folder without
one and the first run opens a file browser instead.  Either way the choice is
remembered, so every run after the first goes straight in.

Everything it writes - your saves, your settings - stays in the "memstick"
folder next to the exe.  Move the folder and your saves move with it; delete
it and you are back to a first run.  Nothing is written to Documents.


Controls
--------

Escape opens the menu, and CONTROLS -> BINDINGS lists every control for the
keyboard, an Xbox pad and a PlayStation pad side by side.  The game's own
tutorial messages name keys too.

A few that are not obvious:

  Alt + WASD    walk instead of run
  G             sub-missions: vigilante, taxi, paramedic, empire sites
  Tab           pick up a weapon you are standing on
  L             hold the game's lock-on instead of free aiming


Saves
-----

The game auto-saves after each story mission, and the save list marks those
"(Autosave)".  A star marks the newest, which is the one a launch comes back
to.  You can still save by hand at a safe house, exactly as on the PSP.


Licence
-------

PPSSPP is free software under the GNU GPL, version 2 or later, and so is
this fork - see LICENSE.TXT.  That licence entitles you to the complete
source code for this build.

  Upstream PPSSPP:  https://github.com/hrydgard/ppsspp

The Pricedown typeface used by the menu is by Ray Larabie.  No game data of
any kind is included in this download.
"""

# Everything under assets/ is needed except this - fonts, atlases, shaders, the compatibility
# list that turns this fork's own behaviour on, and the flash0 PSP fonts the game itself uses.
ASSET_SKIP = {"debugger"}


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def copy_assets(dst):
    src = ROOT / "assets"
    if not src.is_dir():
        fail("assets/ is missing - run this from a checkout")
    dst.mkdir(parents=True, exist_ok=True)
    for entry in sorted(src.iterdir()):
        if entry.name in ASSET_SKIP:
            continue
        # A stray editing backup is not an asset. Named rather than pattern-matched, so a real
        # file that happens to have a long name is never silently dropped.
        if entry.suffix in (".bak", ".bak-flat") or entry.name.endswith(".bak-flat"):
            continue
        if entry.is_dir():
            shutil.copytree(entry, dst / entry.name, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, dst / entry.name)


def build(out_dir, make_zip):
    exe = ROOT / EXE_NAME
    if not exe.is_file():
        fail(f"{EXE_NAME} not found - build PPSSPPWindows in Release x64 first")

    gxt = ROOT / "memstick" / "PSP" / "VCS" / "ENGLISH.GXT"
    if not gxt.is_file():
        fail("memstick/PSP/VCS/ENGLISH.GXT is missing - run Tools/vcsgxtkeys.py first")

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    shutil.copy2(exe, out_dir / EXE_NAME)
    copy_assets(out_dir / "assets")

    vcs_dir = out_dir / "memstick" / "PSP" / "VCS"
    vcs_dir.mkdir(parents=True)
    shutil.copy2(gxt, vcs_dir / "ENGLISH.GXT")

    system = out_dir / "memstick" / "PSP" / "SYSTEM"
    system.mkdir(parents=True)
    (system / "ppsspp.ini").write_text(MINIMAL_INI, encoding="utf-8")

    licence = ROOT / "LICENSE.TXT"
    if licence.is_file():
        shutil.copy2(licence, out_dir / "LICENSE.TXT")
    else:
        print("warning: LICENSE.TXT not found - the GPL requires it to travel with the binary")
    (out_dir / "README.txt").write_text(README, encoding="utf-8")

    # An installed.txt beside the exe would send the memory stick to Documents and orphan the
    # ENGLISH.GXT shipped here. It is never created by this script; this is the check that says
    # so out loud if one ever arrives by another route.
    if (out_dir / "installed.txt").exists():
        fail("installed.txt in the package would break the portable layout")

    total = sum(f.stat().st_size for f in out_dir.rglob("*") if f.is_file())
    count = sum(1 for f in out_dir.rglob("*") if f.is_file())
    print(f"staged {count} files, {total / (1024 * 1024):.1f} MB -> {out_dir}")

    if make_zip:
        archive = out_dir.with_suffix(".zip")
        if archive.exists():
            archive.unlink()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(out_dir.rglob("*")):
                if f.is_file():
                    z.write(f, Path(out_dir.name) / f.relative_to(out_dir))
        print(f"zipped {archive.stat().st_size / (1024 * 1024):.1f} MB -> {archive}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "dist" / "GTA Vice City Stories PC"),
                    help="where the staged folder goes")
    ap.add_argument("--zip", action="store_true", help="also write a .zip beside it")
    args = ap.parse_args()
    build(Path(args.out), args.zip)


if __name__ == "__main__":
    main()
