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
  memstick/SAVEDATA  the saves are their OWN download - a second folder and a second zip
                     beside this one - so the game is the game and somebody else's progress is
                     an opt-in. They were never installed into the memory stick either, for a
                     reason the note inside them gives: the game boot-loads the newest save it
                     can find, so installing them would drop a first-time player into a 29%
                     game instead of the start of the story.
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
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
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
# with no screen under it. The rest belong to this machine rather than to the build: how many
# times the dev copy was started, and the MAC address PPSSPP generated for it on first run - a
# player who gets none generates their own, which also keeps two players apart in ad hoc play.
INI_DROP_KEYS = {"WindowX", "WindowY", "WindowWidth", "WindowHeight", "RunCount", "MacAddress"}

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


# Raw, because the paths in it are Windows paths and a lone backslash before a U or an N is an
# escape as far as Python is concerned.
SAVES_README = r"""Save files
==========

Eight saves from a play-through, between 16% and 30% of the story.

They are NOT installed - copy them in only if you want them.  The game loads
the newest save it can find when it starts, so with these in place you would
begin in the middle of somebody else's game rather than at the beginning of
your own.

To use them, copy the ULUS10160S92F* folders into

    memstick\PSP\SAVEDATA

next to the game, so you end up with

    memstick\PSP\SAVEDATA\ULUS10160S92F0\  (and so on)

They then appear in the game's own LOAD GAME list, named after the last
mission each one passed.  Delete the folders again to be rid of them.
"""

README = """GTA: Vice City Stories - PC version
==================================================

Vice City Stories, playing like a PC game: mouse look, mouse aiming, WASD,
a menu of its own, an HD texture pack, and the game's own tutorial messages
naming keys instead of PSP buttons.

It is a build of PPSSPP that only plays this one game.  No emulator to set
up, nothing to configure - everything is already set the way it was tuned.


Setting it up
-------------

1. Unzip this folder anywhere you like.  Your Documents, a games drive, a
   USB stick - it does not matter, and nothing is installed.

2. Put your own copy of the game in this folder, next to
   "GTA Vice City Stories.exe".  The USA disc, ULUS10160, as .iso or .cso.
   It is not included and cannot be.

3. Run "GTA Vice City Stories.exe".  It starts fullscreen, straight into
   the game.

That is the whole setup.  A single disc image sitting beside the exe is
taken as the one you meant.  If you would rather keep your disc somewhere
else, leave this folder without one and the first run opens a file browser
instead; either way the choice is remembered.


Making a shortcut
-----------------

Right-click "GTA Vice City Stories.exe" and choose

    Show more options  ->  Send to  ->  Desktop (create shortcut)

The shortcut works from anywhere - the game always reads its own folder, so
"Start in" does not matter.  What DOES matter is that the folder stays
together: the exe, "assets" and "memstick" are one thing.  Move the folder
and everything moves with it, saves included.


Playing
-------

Escape opens the menu.  CONTROLS -> BINDINGS lists every control for the
keyboard, an Xbox pad and a PlayStation pad, side by side.

A few that are not obvious:

    Alt + WASD    walk instead of run
    G             sub-missions - vigilante, taxi, paramedic, empire sites
    Tab           pick up a weapon you are standing on
    L             hold the game's lock-on instead of free aiming
    Q / E         glance left and right while driving

Mouse look and mouse aiming are on by default, and so is the HD texture
pack.  SETTINGS -> GRAPHICS has TEXTURE QUALITY if you would rather have the
PSP's own textures, and FULLSCREEN if you would rather play in a window.
It also has SHADOW QUALITY and WATER QUALITY.  Shadows start at the top
setting, and turning them down is the first thing to try if the game
runs slowly.


Saves
-----

The game saves after each story mission by itself, and the save list marks
those "(Autosave)".  A star marks the newest, which is the one the game
comes back to when you start it.  You can still save by hand at a safe
house, exactly as on the PSP.

Everything it writes lives in the "memstick" folder next to the exe.  Delete
that folder and you are back to a fresh start; copy it and your saves come
with you.  Nothing is written anywhere else on your machine.


If something is wrong
---------------------

Nothing happens when you run it, or Windows complains about a missing DLL
    Install the Microsoft Visual C++ Redistributable (x64) from Microsoft.

It takes a long time to start
    The whole disc image is read into memory before the game boots, so
    the city streams in without stalling while you play.  From a slow
    drive that can take a minute or two, and it needs about 3 GB of free
    memory.

It opens a file browser instead of the game
    There is no disc image in the folder, or there is more than one.  With
    two it cannot know which you meant, so it asks.

The game runs but the textures look like the PSP's
    SETTINGS -> GRAPHICS -> TEXTURE QUALITY should read HIGH.


Licence
-------

PPSSPP is free software under the GNU GPL, version 2 or later, and so is
this build - see LICENSE.TXT.  That licence entitles you to its complete
source code.

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


def check_memstick():
    gxt = ROOT / "memstick" / "PSP" / "VCS" / "ENGLISH.GXT"
    if not gxt.is_file():
        fail("memstick/PSP/VCS/ENGLISH.GXT is missing - run Tools/vcsgxtkeys.py first")


def build(out_dir, make_zip, with_textures):
    exe = ROOT / EXE_NAME
    if not exe.is_file():
        fail(f"{EXE_NAME} not found - build PPSSPPWindows in Release x64 first")
    check_memstick()

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    shutil.copy2(exe, out_dir / EXE_NAME)
    copy_assets(out_dir / "assets")
    stage_memstick(out_dir, with_textures)
    finish_package(out_dir, make_zip, README)


# The memory stick is the same on every platform, and so is everything said about it above: the
# keyboard text, the tuned settings with this machine taken out of them, and the HD pack.
def stage_memstick(out_dir, with_textures):
    gxt = ROOT / "memstick" / "PSP" / "VCS" / "ENGLISH.GXT"
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



def finish_package(out_dir, make_zip, readme):
    licence = ROOT / "LICENSE.TXT"
    if licence.is_file():
        shutil.copy2(licence, out_dir / "LICENSE.TXT")
    else:
        print("warning: LICENSE.TXT not found - the GPL requires it to travel with the binary")
    (out_dir / "README.txt").write_text(readme, encoding="utf-8")

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
        if sys.platform == "darwin":
            # ditto is what Finder's own Compress runs. It keeps an app bundle's symlinks, file
            # modes and code signature exactly as they are, which zipfile does not promise.
            subprocess.run(["ditto", "-c", "-k", "--keepParent", str(out_dir), str(archive)], check=True)
        else:
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for f in sorted(out_dir.rglob("*")):
                    if f.is_file():
                        z.write(f, Path(out_dir.name) / f.relative_to(out_dir))
        print(f"zipped {archive.stat().st_size / (1024 * 1024):.1f} MB -> {archive}")


def build_saves(out_dir, make_zip):
    """The saves as their own folder and zip, next to the game's.

    Separate on purpose. The game is one thing and somebody else's progress is another, and
    keeping them apart means the download that plays the game does not silently carry a decision
    about where in the story you start.
    """
    src = ROOT / "memstick" / "PSP" / "SAVEDATA"
    if not src.is_dir():
        print("warning: no SAVEDATA directory - skipping the saves package")
        return

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    copied = 0
    for slot in sorted(src.iterdir()):
        # A save is a directory with a PARAM.SFO in it. An empty slot is a directory too, and
        # copying one would put a save in the list that has nothing behind it.
        if not slot.is_dir() or not (slot / "PARAM.SFO").is_file():
            continue
        shutil.copytree(slot, out_dir / slot.name, dirs_exist_ok=True)
        copied += 1

    if not copied:
        shutil.rmtree(out_dir)
        print("warning: no saves found")
        return

    (out_dir / "README.txt").write_text(SAVES_README, encoding="utf-8")
    total = sum(f.stat().st_size for f in out_dir.rglob("*") if f.is_file())
    print(f"staged {copied} saves, {total / (1024 * 1024):.1f} MB -> {out_dir}")

    if make_zip:
        archive = out_dir.with_suffix(".zip")
        if archive.exists():
            archive.unlink()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(out_dir.rglob("*")):
                if f.is_file():
                    z.write(f, Path(out_dir.name) / f.relative_to(out_dir))
        print(f"zipped {archive.stat().st_size / (1024 * 1024):.1f} MB -> {archive}")


# --- macOS -------------------------------------------------------------------------------------
#
# Run on a Mac, this packages the Mac build instead: the same folder, with an app where the exe
# was. Everything above about the memory stick still holds - NativeApp takes a `memstick` beside
# the app the way Windows/main.cpp takes one beside the exe - and assets/ is inside the bundle.
#
# The bundle b-macos.sh builds is PPSSPP's, and it is renamed and re-dressed here rather than in
# CMakeLists, so the fork's diff to upstream's build files stays at nothing.

MAC_BUILT_APP = ROOT / "build" / "PPSSPPSDL.app"
MAC_APP_NAME = "GTA Vice City Stories.app"
MAC_DISPLAY_NAME = "GTA Vice City Stories"
MAC_BUNDLE_ID = "io.github.vancevcs.vcspc"
MAC_MIN_OS = "11.0"                  # b-macos.sh's DEPLOYMENT_TARGET
MAC_FRAMEWORKS_RPATH = "@executable_path/../Frameworks"
# The two libraries the app takes from outside macOS, both linked as @rpath/<name>. MoltenVK is
# already inside the bundle.
MAC_DEPS = ["libSDL3.0.dylib", "libSDL3_ttf.0.dylib"]

# Removed from Info.plist. They are how PPSSPP offers to open any .iso, .cso or .pbp on the Mac, and
# a game has no business claiming the player's disc images - or changing what opens them.
MAC_PLIST_DROP = ("CFBundleDocumentTypes", "CFBundleURLTypes", "UTExportedTypeDeclarations")

MAC_README = """GTA: Vice City Stories - PC version, for Mac
==================================================

Vice City Stories, playing like a PC game: mouse look, mouse aiming, WASD,
a menu of its own, an HD texture pack, and the game's own tutorial messages
naming keys instead of PSP buttons.

It is a build of PPSSPP that only plays this one game.  No emulator to set
up, nothing to configure - everything is already set the way it was tuned.

It needs a Mac with Apple Silicon (M1 or newer) and macOS 11 or later.


Setting it up
-------------

1. Unzip this folder anywhere you like.  Your Documents, Applications, a
   games drive - it does not matter, and nothing is installed.

2. Put your own copy of the game in this folder, next to
   "GTA Vice City Stories".  The USA disc, ULUS10160, as .iso or .cso.
   It is not included and cannot be.

3. Open "GTA Vice City Stories".  It starts fullscreen, straight into the
   game.

The first time, macOS says it cannot check the app for malicious software.
That is because it is not signed with a paid Apple developer certificate,
not because anything is wrong with it.  Click Done, open System Settings ->
Privacy & Security, scroll down to the line about "GTA Vice City Stories",
and click Open Anyway.  You only do this once.

If the folder is in Downloads, Documents or Desktop, macOS may also ask
whether the game can use files in that folder.  Allow it - your disc and
your saves are there.

A single disc image sitting beside the app is taken as the one you meant.
If you would rather keep your disc somewhere else, leave this folder without
one and the first run offers CHOOSE DISC instead; either way the choice is
remembered.

Keep the folder together: the app and "memstick" are one thing.  Move the
folder and everything moves with it, saves included.


Playing
-------

Escape opens the menu.  CONTROLS -> BINDINGS lists every control for the
keyboard, an Xbox pad and a PlayStation pad, side by side.  Cmd+Q quits.

A few that are not obvious:

    Option + WASD  walk instead of run
    G              sub-missions - vigilante, taxi, paramedic, empire sites
    Tab            pick up a weapon you are standing on
    L              hold the game's lock-on instead of free aiming
    Q / E          glance left and right while driving

Mouse look and mouse aiming are on by default, and so is the HD texture
pack.  SETTINGS -> GRAPHICS has TEXTURE QUALITY if you would rather have the
PSP's own textures, and FULLSCREEN if you would rather play in a window.
It also has SHADOW QUALITY and WATER QUALITY.  Shadows start at the top
setting, and turning them down is the first thing to try if the game
runs slowly.


Saves
-----

The game saves after each story mission by itself, and the save list marks
those "(Autosave)".  A star marks the newest, which is the one the game
comes back to when you start it.  You can still save by hand at a safe
house, exactly as on the PSP.

Everything it writes lives in the "memstick" folder next to the app.  Delete
that folder and you are back to a fresh start; copy it and your saves come
with you.  Nothing is written anywhere else on your Mac.


If something is wrong
---------------------

macOS says the app is damaged, or there is no Open Anyway button
    Open Terminal, type the line below with a space at the end, drag this
    folder onto the Terminal window, and press Return:

        xattr -dr com.apple.quarantine

    Then open the app again.

It takes a long time to start
    The whole disc image is read into memory before the game boots, so
    the city streams in without stalling while you play.  From a slow
    drive that can take a minute or two, and it needs about 3 GB of free
    memory.

It offers CHOOSE DISC instead of starting the game
    There is no disc image in the folder, or there is more than one.  With
    two it cannot know which you meant, so it asks.

The game runs but the textures look like the PSP's
    SETTINGS -> GRAPHICS -> TEXTURE QUALITY should read HIGH.


Licence
-------

PPSSPP is free software under the GNU GPL, version 2 or later, and so is
this build - see LICENSE.TXT.  That licence entitles you to its complete
source code.

    Upstream PPSSPP:  https://github.com/hrydgard/ppsspp

The app also carries SDL (zlib licence) and MoltenVK (Apache 2.0).  The
Pricedown typeface used by the menu is by Ray Larabie.  No game data of any
kind is included in this download.
"""


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True)


def output(*cmd):
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, text=True).stdout


def git_version():
    try:
        # The game's own release tags only - saves-* and mac-* tags sit on the same commits.
        return output("git", "-C", ROOT, "describe", "--tags", "--match", "v*", "--always").strip().lstrip("v")
    except (OSError, subprocess.CalledProcessError):
        return "0"


def rpaths_of(binary):
    lines = output("otool", "-l", binary).splitlines()
    paths = []
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for following in lines[i + 1:i + 4]:
                following = following.strip()
                if following.startswith("path "):
                    paths.append(following[len("path "):].rsplit(" (offset", 1)[0])
    return paths


def ico_to_icns(ico, icns):
    """vcs.ico, the Windows build's icon, as the .icns a bundle wants.

    The .ico already holds PNGs from 16 to 256 pixels, so this repacks rather than resamples.
    512 and 1024 are left out instead of being scaled up into blur.
    """
    data = ico.read_bytes()
    count = struct.unpack("<HHH", data[:6])[2]
    pngs = {}
    for i in range(count):
        width, _, _, _, _, _, size, offset = struct.unpack("<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
        blob = data[offset:offset + size]
        if blob[:4] == b"\x89PNG":
            pngs[width or 256] = blob
    iconset_names = {
        "icon_16x16.png": 16, "icon_16x16@2x.png": 32,
        "icon_32x32.png": 32, "icon_32x32@2x.png": 64,
        "icon_128x128.png": 128, "icon_128x128@2x.png": 256,
        "icon_256x256.png": 256,
    }
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "vcs.iconset"
        iconset.mkdir()
        for name, px in iconset_names.items():
            if px not in pngs:
                fail(f"{ico.name} has no {px}x{px} image for {name}")
            (iconset / name).write_bytes(pngs[px])
        run("iconutil", "-c", "icns", iconset, "-o", icns)


def build_mac(out_dir, make_zip, with_textures):
    if not MAC_BUILT_APP.is_dir():
        fail(f"{MAC_BUILT_APP.relative_to(ROOT)} not found - run ./b-macos.sh first")
    for lib in MAC_DEPS:
        if not (ROOT / ".deps" / "lib" / lib).exists():
            fail(f".deps/lib/{lib} not found - run ./b-macos.sh first")
    check_memstick()

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    app = out_dir / MAC_APP_NAME
    shutil.copytree(MAC_BUILT_APP, app, symlinks=True)
    contents = app / "Contents"
    exe = contents / "MacOS" / "PPSSPPSDL"
    resources = contents / "Resources"
    frameworks = contents / "Frameworks"

    # The same assets rule as Windows.
    for skip in ASSET_SKIP:
        shutil.rmtree(resources / "assets" / skip, ignore_errors=True)

    # The game's icon and names in place of PPSSPP's.
    (resources / "ppsspp.icns").unlink(missing_ok=True)
    ico_to_icns(ROOT / "Windows" / "vcs.ico", resources / "vcs.icns")
    plist_path = contents / "Info.plist"
    with open(plist_path, "rb") as f:
        plist = plistlib.load(f)
    for key in MAC_PLIST_DROP:
        plist.pop(key, None)
    version = git_version()
    plist.update({
        "CFBundleName": MAC_DISPLAY_NAME,
        "CFBundleDisplayName": MAC_DISPLAY_NAME,
        "CFBundleIdentifier": MAC_BUNDLE_ID,
        "CFBundleIconFile": "vcs.icns",
        "CFBundleShortVersionString": version.split("-")[0],
        "CFBundleVersion": version,
        "LSMinimumSystemVersion": MAC_MIN_OS,
    })
    with open(plist_path, "wb") as f:
        plistlib.dump(plist, f)

    # SDL3 and SDL3_ttf into the bundle. Both are linked as @rpath/<name> already, so the only
    # change is where @rpath points: away from this machine's .deps, to the bundle's own Frameworks.
    frameworks.mkdir(exist_ok=True)
    for lib in MAC_DEPS:
        shutil.copy2((ROOT / ".deps" / "lib" / lib).resolve(), frameworks / lib)
    binaries = [exe] + sorted(frameworks.glob("*.dylib"))
    for binary in binaries:
        for rpath in rpaths_of(binary):
            if rpath.startswith("/"):
                run("install_name_tool", "-delete_rpath", rpath, binary)
    if MAC_FRAMEWORKS_RPATH not in rpaths_of(exe):
        run("install_name_tool", "-add_rpath", MAC_FRAMEWORKS_RPATH, exe)

    # Nothing in the bundle may still name a path on this machine: it would load on this Mac and
    # on no other, which is the one failure testing here cannot see.
    for binary in binaries:
        # otool names the file it is reading on lines ending in a colon - once more per
        # architecture, for MoltenVK - and those are the bundle's own paths, not links.
        linked = [line.strip() for line in output("otool", "-L", binary).splitlines()
                  if not line.rstrip().endswith(":")]
        stray = [line for line in linked if line.startswith(("/Users/", str(ROOT)))]
        stray += [p for p in rpaths_of(binary) if p.startswith("/")]
        if stray:
            fail(f"{binary.name} still points at this machine: {stray}")

    # install_name_tool has invalidated the signatures, and Apple Silicon will not run an unsigned
    # binary at all. Ad hoc - no identity - which is what "Open Anyway" in the README is about.
    # Extended attributes go first, because codesign refuses a bundle that carries them.
    run("xattr", "-cr", app)
    for lib in sorted(frameworks.glob("*.dylib")):
        run("codesign", "--force", "--sign", "-", lib)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--strict", app)

    stage_memstick(out_dir, with_textures)
    finish_package(out_dir, make_zip, MAC_README)


def main():
    mac = sys.platform == "darwin"
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "dist" / ("GTA Vice City Stories Mac" if mac else "GTA Vice City Stories PC")),
                    help="where the staged folder goes")
    ap.add_argument("--zip", action="store_true", help="also write a .zip beside it")
    ap.add_argument("--no-textures", action="store_true",
                    help="leave the HD pack out (much smaller, PSP textures only)")
    ap.add_argument("--no-saves", action="store_true",
                    help="skip the separate saves package")
    args = ap.parse_args()
    (build_mac if mac else build)(Path(args.out), args.zip, not args.no_textures)
    if not args.no_saves:
        # Beside the game's package, not inside it.
        out = Path(args.out)
        build_saves(out.with_name(out.name + " - Saves"), args.zip)


if __name__ == "__main__":
    main()
