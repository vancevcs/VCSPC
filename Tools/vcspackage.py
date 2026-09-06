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
  SYSTEM/*.bak*, CACHE, DUMP, vcs_autosaves.txt  editing backups, caches, and a ledger that
                     names this machine's saves
  assets/debugger    the WebSocket debugger's web UI, which is exactly the "debug stuff" a
                     release build should not carry
  memstick/PSP/TEXTURES/*/new  where SaveNewTextures DUMPS to.  The pack itself ships; this
                     one folder is output, referenced by nothing in textures.ini.
  ppsspp.ini's [Recent], [PlayTime], CurrentDirectory, the window position, and the logging
                     and remote-debugger switches.  Everything ELSE in that file ships - see
                     the note over sanitise_ini for why a minimal one was wrong.

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

# The dev ppsspp.ini, with the personal and the machine-specific taken out - NOT a minimal one
# written from scratch.
#
# A minimal ini shipped first, on the reasoning that a distributed build should carry no opinion it
# cannot justify. That reasoning threw away the settings that make this build good, and the package
# went out with no fullscreen and no texture replacement, reported as "you already did a release
# build for me a while ago and it worked so well". Both were one line in this file: FullScreen and
# ReplaceTextures were True in the config every test had been run against, and neither survived.
#
# The lesson is about what "default" means. PPSSPP's defaults are right for an emulator that plays
# anything; this is a build of ONE game that has been tuned against it for weeks, and the tuning IS
# the product. Shipping upstream defaults is not neutrality, it is shipping a different program.
#
# So the rule inverts: keep everything, and name what comes out.
INI_DROP_SECTIONS = {"Recent", "PlayTime"}

# Personal, machine-specific, or a development switch. Each is set rather than deleted where a
# value is wanted, and dropped where absence is the right answer.
INI_FORCE = {
    "CurrentDirectory": "",          # this machine's Documents folder
    "FileLogging": "False",          # writes a log beside the exe all session
    "RemoteDebuggerOnStartup": "False",
    "CheckForNewVersion": "False",   # a fork: that feed offers a different program
    "SaveNewTextures": "False",      # dumps every texture to disk as it is drawn
    # Left True by any dev session that had the debugger open, and persisted. The build refuses
    # to draw it now regardless (see ImDebugger::Frame), but shipping a release whose config says
    # "debugger on" is wrong on its own terms.
    "ShowImDebugger": "False",
    # The developer DISPLAY settings, as a class rather than one at a time. Each of these turns on
    # something drawn over the game, each defaults off upstream, and each ends up True in a dev ini
    # simply because it was useful once. DebugOverlay is the one that shipped: it is CfgFlag
    # DONT_SAVE upstream - PPSSPP will not write it - but it is still READ, so a stale value sat in
    # the file and painted syscall timings over the game in a release package.
    "DebugOverlay": "0",
    "iShowStatusFlags": "0",         # the Debug build's speed counter; Release has its own row
    "ShowDeveloperMenu": "False",
    "AchievementsUserName": "",
}

# Dropped outright: a window rectangle from another monitor layout can put the window somewhere
# with no screen under it.
INI_DROP_KEYS = {"WindowX", "WindowY"}

# The rest of memstick/PSP/SYSTEM ships as it is, and that is the point rather than an oversight.
#
# vcs.ini is the fork's OWN settings - every camera, aim, vault and menu value on the options
# pages. Leaving it out did not give a player "the defaults"; it gave them the compiled-in values
# rather than the ones this build has been tuned to over weeks, which is the same mistake the
# minimal ppsspp.ini made one file over. controls.ini is PPSSPP's own mapping, and the fork's
# tables are written against it.
#
# Only GamePath comes out, because it names a disc on this machine. Everything else is the tuning.
SYSTEM_SKIP_NAMES = {"vcs_autosaves.txt"}     # a ledger of which of THIS machine's saves are auto
SYSTEM_SKIP_DIRS = {"CACHE", "DUMP"}


def sanitise_vcs_ini(text):
    out = []
    for line in text.splitlines():
        if line.strip().split("=", 1)[0].strip() == "GamePath":
            out.append("GamePath = ")
            continue
        out.append(line)
    return "\n".join(out).rstrip() + "\n"


def sanitise_ini(text):
    out = []
    section = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        if section in INI_DROP_SECTIONS:
            continue
        key = stripped.split("=", 1)[0].strip() if "=" in stripped else None
        if key in INI_DROP_KEYS:
            continue
        if key in INI_FORCE:
            out.append(f"{key} = {INI_FORCE[key]}")
            continue
        out.append(line)
    return "\n".join(out).rstrip() + "\n"


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

The HD texture pack is included and on.  If you would rather have the
PSP's own textures - it is a lot of video memory - the menu has a row for
it: Escape, SETTINGS, GRAPHICS, TEXTURE QUALITY.

Fullscreen is on, and the same GRAPHICS page turns it off.

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


def build(out_dir, make_zip, with_textures):
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
    dev_ini = ROOT / "memstick" / "PSP" / "SYSTEM" / "ppsspp.ini"
    if not dev_ini.is_file():
        fail("memstick/PSP/SYSTEM/ppsspp.ini is missing - it is what the package ships")
    (system / "ppsspp.ini").write_text(
        sanitise_ini(dev_ini.read_text(encoding="utf-8", errors="replace")), encoding="utf-8")

    # Everything else the running build keeps in SYSTEM, so a packaged copy is the same program
    # in the same state - see SYSTEM_SKIP_NAMES.
    for entry in sorted(dev_ini.parent.iterdir()):
        if entry.is_dir() or entry.name == "ppsspp.ini":
            continue
        if entry.name in SYSTEM_SKIP_NAMES or ".bak" in entry.name:
            continue
        if entry.name == "vcs.ini":
            (system / "vcs.ini").write_text(
                sanitise_vcs_ini(entry.read_text(encoding="utf-8", errors="replace")),
                encoding="utf-8")
            continue
        shutil.copy2(entry, system / entry.name)

    # The HD pack. Big, and the reason ReplaceTextures is worth having on - see sanitise_ini.
    # `new/` is where SaveNewTextures dumps and is referenced by nothing in textures.ini.
    if with_textures:
        src_tex = ROOT / "memstick" / "PSP" / "TEXTURES" / "ULUS10160"
        if not src_tex.is_dir():
            fail("the texture pack is missing - pass --no-textures to build without it")
        dst_tex = out_dir / "memstick" / "PSP" / "TEXTURES" / "ULUS10160"
        # `new` is the dump folder; the *.bak-* are editing backups of textures.ini, and a second
        # ini in that folder is a second answer to "which pack is this".
        shutil.copytree(src_tex, dst_tex,
                        ignore=shutil.ignore_patterns("new", "*.bak", "*.bak-*"),
                        dirs_exist_ok=True)
        if not (dst_tex / "textures.ini").is_file():
            fail("textures.ini did not come with the pack - replacement would silently do nothing")

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
    ap.add_argument("--no-textures", action="store_true",
                    help="leave the HD pack out (much smaller, PSP textures only)")
    args = ap.parse_args()
    build(Path(args.out), args.zip, not args.no_textures)


if __name__ == "__main__":
    main()
