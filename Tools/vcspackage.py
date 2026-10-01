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


def sanitise_vcs_ini(text, force=None):
    force = force or {}
    out = []
    for line in text.splitlines():
        key = line.strip().split("=", 1)[0].strip()
        if key == "GamePath":
            out.append("GamePath = ")
            continue
        if key in force:
            out.append(f"{key} = {force[key]}")
            continue
        out.append(line)
    return "\n".join(out).rstrip() + "\n"


def sanitise_ini(text, extra_force=None, add=None):
    """`add` is {section: {key: value}} for keys the dev ini never wrote at all - a default this
    machine never had a reason to save, like ScreenRotation on Windows. Forcing only rewrites a
    line that is already there, so without it a phone would get whatever its platform defaults to."""
    forced = dict(INI_FORCE, **(extra_force or {}))
    add = {sec: dict(keys) for sec, keys in (add or {}).items()}
    section = None
    for line in text.splitlines():
        # PPSSPP writes a byte order mark, which sits in front of the first section's name.
        stripped = line.strip().lstrip("\ufeff")
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        elif "=" in stripped and section in add:
            add[section].pop(stripped.split("=", 1)[0].strip(), None)
    out = []
    section = None
    for line in text.splitlines():
        stripped = line.strip().lstrip("\ufeff")
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            if section not in INI_DROP_SECTIONS:
                out.append(line)
                out.extend(f"{key} = {value}" for key, value in add.pop(section, {}).items())
                continue
        if section in INI_DROP_SECTIONS:
            continue
        key = stripped.split("=", 1)[0].strip() if "=" in stripped else None
        if key in INI_DROP_KEYS:
            continue
        if key in forced:
            out.append(f"{key} = {forced[key]}")
            continue
        out.append(line)
    if any(add.values()):
        fail(f"no section to add {add} to in ppsspp.ini")
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
# The converted pack, if Tools/vcsktx2.py has been run. A phone cannot read the .dds pack at all
# - see that script - so on a phone this is the only pack worth shipping, and its absence is worth
# saying out loud rather than quietly shipping 748MB the GPU will skip.
KTX2_PACK = ROOT / "dist" / "textures-ktx2" / "ULUS10160"


def stage_memstick(out_dir, with_textures, phone=False, ini_add=None):
    # The keyboard text names keys - LEFT MOUSE, WASD - which on a phone is worse than the PSP's
    # own button names the game falls back to without it. A touch vocabulary replaces it later.
    if not phone:
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
        sanitise_ini(dev_ini.read_text(encoding="utf-8", errors="replace"),
                     PHONE_INI_FORCE if phone else None, ini_add), encoding="utf-8")

    # Everything else the running build keeps in SYSTEM, so a packaged copy is the same program
    # in the same state - see SYSTEM_SKIP_NAMES.
    for entry in sorted(dev_ini.parent.iterdir()):
        if entry.is_dir() or entry.name == "ppsspp.ini":
            continue
        if entry.name in SYSTEM_SKIP_NAMES or ".bak" in entry.name:
            continue
        if entry.name == "vcs.ini":
            (system / "vcs.ini").write_text(
                sanitise_vcs_ini(entry.read_text(encoding="utf-8", errors="replace"),
                                 PHONE_VCS_INI_FORCE if phone else None),
                encoding="utf-8")
            continue
        shutil.copy2(entry, system / entry.name)

    # The HD pack. Big, and the reason ReplaceTextures is worth having on - see sanitise_ini.
    # `new/` is where SaveNewTextures dumps and is referenced by nothing in textures.ini.
    if with_textures:
        src_tex = ROOT / "memstick" / "PSP" / "TEXTURES" / "ULUS10160"
        if phone:
            if not KTX2_PACK.is_dir():
                fail("the converted pack is missing - run Tools/vcsktx2.py, or pass --no-textures."
                     " The .dds pack cannot be used on a phone: the GPU cannot sample BC7 and"
                     " PPSSPP skips every file.")
            src_tex = KTX2_PACK
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


# --- Android ------------------------------------------------------------------------------------
#
# An APK and a GTAVCS folder, shipped as two downloads: the zip carries only the game files, and
# the APK is written beside it rather than inside. The app changes far more often than the 700MB
# of textures, so an update is one small file, and a zip with a copy of the app inside it would go
# stale the first time only the app was replaced. NativeInit on Android looks for exactly that folder at the root of
# the phone's storage and uses its memstick/, and the disc goes in beside it - the PC layout with
# the exe taken out. Built by `./gradlew :android:assembleVcsRelease`, which signs with the repo's
# debug keystore: this is sideloaded, never sent to a store.
ANDROID_APK = ROOT / "android" / "build" / "outputs" / "apk" / "vcs" / "release" / "android-vcs-release.apk"
ANDROID_APK_NAME = "GTA Vice City Stories.apk"
ANDROID_FOLDER = "GTAVCS"

# What a phone needs different from the tuned desktop config. The rule stays "keep everything and
# name what changes" - see INI_FORCE - and these are the changes. iOS takes the same set.
PHONE_INI_FORCE = {
    "CacheFullIsoInRam": "False",    # 1.6 GB of RAM on a PC; the whole phone has 6
    "InternalResolution": "2",       # 960x544. Auto would pick 4x on a 1080p screen; raise it once measured
    "DisplayStretch": "True",        # the widescreen fix renders pre-corrected for this
    "ShowTouchControls": "True",     # the switch the VCS overlay is gated on as well
    "UIScaleFactor": "0",            # the Android default; -1 is the desktop's
    "UseMouse": "False",
}
# Both of these capture and transform the scene on the CPU every frame. Start a phone at the cheap
# end and raise them once the frame rate is known - the same defaults the code gives a phone.
PHONE_VCS_INI_FORCE = {
    "Shadows": "0",
    "Water": "1",
}

ANDROID_README = """GTA: Vice City Stories - Android
=================================

A build of PPSSPP that only plays Vice City Stories, for a phone.  Touch
controls of its own, a menu of its own, and the HD texture pack converted to
a format phone GPUs can actually read.

It needs an arm64 phone with Android 8.1 or later.


Setting it up
-------------

1. Copy the "GTAVCS" folder to the root of the phone's internal storage, so
   you end up with Internal storage/GTAVCS/memstick.  Over USB, or:

       adb push GTAVCS /sdcard/

2. Put your own copy of the game in that GTAVCS folder, beside memstick.
   The USA disc, ULUS10160, as .iso or .cso.  It is not included and cannot
   be.

       adb push "Vice City Stories.iso" /sdcard/GTAVCS/

3. Install "GTA Vice City Stories.apk", which is a separate download beside
   this zip.  Android asks you to allow installs from whichever app opens it.
   An update is usually only a new APK, installed over the old one.  Or:

       adb install "GTA Vice City Stories.apk"

4. Open it and allow access to files when asked.  It needs that to read
   the GTAVCS folder.  The game starts by itself once it finds the disc.

It installs beside a normal PPSSPP and shares nothing with it.


The controls
------------

They change with what you are doing - on foot, aiming, driving, flying, and
on the game's own map and stats pages - so what is on screen is what there
is to press.  A few that are not obvious:

- One button is jump AND sprint.  Hold it to sprint, tap it twice to jump.
- TARGET is a toggle, not a button to hold down.
- In a car, the two buttons on the left look to that side and fire, which is
  how a drive-by works.
- Tap the radar to open the menu.  Tap the weapon in the corner to change
  weapon.
- During a cutscene the controls come off and you get SKIP and PAUSE.

OPTIONS -> TOUCH CONTROLS has size, look speed and steering, and ARRANGE
CONTROLS, which lets you drag every button to wherever your thumbs are.

A Bluetooth controller works as well, with the layout the PC build uses for
a pad: the triggers aim and fire, the right stick looks, Start opens the
menu.  The phone's own back gesture opens it too.

The game's help text still names PSP buttons - "press the X button" - rather
than naming a control on screen.


Graphics
--------

OPTIONS -> DISPLAY SETUP starts with shadows off and water at MEDIUM, which
is the cheap end.  Turn them up once you know how the game runs on yours.

WIDESCREEN fills a screen wider than the PSP's without stretching anything.
CROP trims the top and bottom of the view.  WIDER shows about 20% more of
the world instead, which looks better standing still - but the game only
loads what it expects to draw, so scenery appears and vanishes at the edges
as you turn.


The HD textures
---------------

Included, converted to a format phone GPUs can read (KTX2/UASTC - the .dds
pack the PC build uses cannot be sampled by a Mali or PowerVR GPU at all).
OPTIONS -> DISPLAY SETUP -> TEXTURE QUALITY switches between these and the
PSP's own.
"""


def build_android(out_dir, make_zip, with_textures):
    if not ANDROID_APK.is_file():
        fail(f"{ANDROID_APK.relative_to(ROOT)} not found - run ./gradlew :android:assembleVcsRelease first")

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    stage_memstick(out_dir / ANDROID_FOLDER, with_textures, phone=True)
    finish_package(out_dir, make_zip, ANDROID_README)
    # Beside the package, not in it - see the note at the top of this section.
    apk = out_dir.parent / ANDROID_APK_NAME
    shutil.copy2(ANDROID_APK, apk)
    print(f"copied the APK -> {apk}")


# --- iOS ----------------------------------------------------------------------------------------
#
# An .ipa and a folder of game files, two downloads for the reason the Android ones are two: the app
# changes far more often than the 700MB of textures. The folder is the Android one with GTAVCS taken
# off the top - on iOS the game's folder is the app's own Documents, which NativeInit uses and which
# Finder's file sharing and the Files app both show, so a player copies `memstick` and the disc
# straight into it.
#
# Built by b-ios-vcs.sh, unsigned. Everything that makes it this game rather than PPSSPP happens here,
# the same split the Mac package makes: the name, the icon, the launch screen, and an ad hoc
# signature. Nobody without an Apple developer account can sign for a device, so the .ipa is for a
# sideloading tool - AltStore, SideStore, Sideloadly - which re-signs it with the player's own
# Apple ID. TrollStore installs it as it is.
IOS_BUILT_APP = ROOT / "build-ios" / "Release-iphoneos" / "PPSSPP.app"
IOS_MOLTENVK = ROOT / "ext" / "vulkan" / "iOS" / "Frameworks" / "libMoltenVK.dylib"
IOS_ICONS = ROOT / "ios" / "vcs.xcassets"
IOS_LAUNCH_STORYBOARD = ROOT / "ios" / "Launch Screen.storyboard"
IOS_APP_NAME = "ViceCityStories.app"
# What the home screen shows, and the Files app names the folder after. The Android launcher's name,
# and for the same reason: "GTA Vice City Stories" is cut off under an icon.
IOS_DISPLAY_NAME = "Vice City Stories"
IOS_BUNDLE_ID = "io.github.vancevcs.vcsmobile"
IOS_MIN_OS = "15.0"                  # cmake/Toolchains/ios.cmake - the oldest Xcode 27 builds for
IOS_IPA_NAME = "GTA Vice City Stories.ipa"
IOS_FOLDER = "GTA Vice City Stories iOS"
# Either landscape, never portrait. Android gets that from its platform default; iOS's default is
# to follow the phone round, and the desktop ini never wrote the key down to be forced.
IOS_INI_ADD = {"General": {"ScreenRotation": "5"}}
# get-task-allow is what lets a JIT enabler attach to the app as a debugger, which is the only way
# a sideloaded app gets to generate code. A re-signing tool keeps it from the free developer
# profile anyway; this is for TrollStore, which keeps whatever is here.
IOS_ENTITLEMENTS = {"get-task-allow": True}

IOS_README = """GTA: Vice City Stories - iPhone
===============================

A build of PPSSPP that only plays Vice City Stories, for an iPhone or an
iPad.  Touch controls of its own, a menu of its own, and the HD texture pack
converted to a format the phone's GPU can read.

It needs iOS 15 or later.


Installing the app
------------------

"GTA Vice City Stories.ipa" is a separate download beside this zip.  It is
not signed by Apple, so it goes on with a sideloading tool, which signs it
with your own Apple ID:

- AltStore or SideStore, on the phone
- Sideloadly, from a Mac or a PC with the phone plugged in
- TrollStore, if your iOS version has it

With a free Apple ID the signature lasts seven days.  AltStore and SideStore
refresh it for you; refreshing or installing a newer .ipa over the old one
keeps your saves and settings.


Setting it up
-------------

1. Open the app once, then close it again - swipe it away in the app
   switcher.  That gives it a folder.

2. Copy the "memstick" folder from this zip into that folder, and replace
   the one the app made when it asks:

   - On the phone: unzip this in the Files app, then move "memstick" to
     Files -> On My iPhone -> Vice City Stories.
   - From a Mac: plug the phone in, select it in Finder, open Files, and
     drag "memstick" onto Vice City Stories.  On Windows the Apple Devices
     app has the same list.

3. Put your own copy of the game in the same folder, beside memstick.  The
   USA disc, ULUS10160, as .iso or .cso.  It is not included and cannot
   be.

4. Open the app.  The game starts by itself once it finds the disc.

Copy things in while the app is closed.  It writes its settings into
memstick as it goes to the background, and would write over the ones you
have just copied.


Speed
-----

iOS does not let an app it did not sign generate code, which is how PPSSPP
normally runs a PSP game quickly.  Without that the game runs on PPSSPP's
interpreter, which is slower.  If you have a JIT enabler set up for your
iOS version, such as StikDebug, start the game through it.

OPTIONS -> DISPLAY SETUP starts with shadows off and water at MEDIUM, which
is the cheap end.  Turn them up once you know how the game runs on yours.

WIDESCREEN fills a screen wider than the PSP's without stretching anything.
CROP trims the top and bottom of the view.  WIDER shows about 20% more of
the world instead, which looks better standing still - but the game only
loads what it expects to draw, so scenery appears and vanishes at the edges
as you turn.


The controls
------------

They change with what you are doing - on foot, aiming, driving, flying, and
on the game's own map and stats pages - so what is on screen is what there
is to press.  A few that are not obvious:

- One button is jump AND sprint.  Hold it to sprint, tap it twice to jump.
- TARGET is a toggle, not a button to hold down.
- In a car, the two buttons on the left look to that side and fire, which is
  how a drive-by works.
- Tap the radar to open the menu.  Tap the weapon in the corner to change
  weapon.
- During a cutscene the controls come off and you get SKIP and PAUSE.

OPTIONS -> TOUCH CONTROLS has size, look speed and steering, and ARRANGE
CONTROLS, which lets you drag every button to wherever your thumbs are.

A controller works as well - an Xbox or PlayStation pad over Bluetooth -
with the layout the PC build uses for a pad: the triggers aim and fire, the
right stick looks, Start opens the menu.

The game's help text still names PSP buttons - "press the X button" - rather
than naming a control on screen.


The HD textures
---------------

Included, converted to KTX2/UASTC, which the phone turns into a format its
GPU can read as each texture loads.  OPTIONS -> DISPLAY SETUP -> TEXTURE
QUALITY switches between these and the PSP's own.


Licence
-------

PPSSPP is free software under the GNU GPL, version 2 or later, and so is
this build - see LICENSE.TXT.  That licence entitles you to its complete
source code.

    Upstream PPSSPP:  https://github.com/hrydgard/ppsspp

The app also carries MoltenVK (Apache 2.0).  The Pricedown typeface used by
the menu is by Ray Larabie.  No game data of any kind is included in this
download.
"""


def ios_version():
    """git_version() pared down to what CFBundleVersion accepts: dot-separated numbers."""
    numbers = git_version().split("-")[0].split(".")
    if not all(n.isdigit() for n in numbers):
        return "0"
    return ".".join(str(int(n)) for n in numbers[:3])


def build_ios_app(app):
    """PPSSPP.app as this game: the name, the icon, the launch screen, MoltenVK, a signature."""
    shutil.copytree(IOS_BUILT_APP, app, symlinks=True)

    for skip in ASSET_SKIP:
        shutil.rmtree(app / "assets" / skip, ignore_errors=True)

    # Where VulkanLoader looks first - "@executable_path/Frameworks". The CMake sideload target
    # never copies it in, which is why upstream's b-ios.sh does it by hand too. Without it the
    # app falls back to OpenGL, where the shadows and the water do not exist.
    (app / "Frameworks").mkdir(exist_ok=True)
    shutil.copy2(IOS_MOLTENVK, app / "Frameworks" / IOS_MOLTENVK.name)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        # The launch screen. CMake's Xcode generator lists the storyboard in the target and never
        # puts it in a build phase, so the bundle has none - and an app with no launch storyboard
        # is a legacy app to iOS, run letterboxed in an iPhone 5 sized window. PPSSPP's own is
        # PPSSPP's blue; the game opens on black logos, so this one is black.
        board = IOS_LAUNCH_STORYBOARD.read_text(encoding="utf-8")
        start = board.find('<color key="backgroundColor"')
        end = board.find("/>", start)
        if start < 0 or end < 0:
            fail(f"{IOS_LAUNCH_STORYBOARD.name} has no background colour to replace")
        board = (board[:start] + '<color key="backgroundColor" red="0" green="0" blue="0" '
                 'alpha="1" colorSpace="custom" customColorSpace="sRGB"' + board[end:])
        black_board = tmp / IOS_LAUNCH_STORYBOARD.name
        black_board.write_text(board, encoding="utf-8")
        run("xcrun", "ibtool", "--compile", app / "Launch Screen.storyboardc", black_board,
            "--target-device", "iphone", "--target-device", "ipad",
            "--minimum-deployment-target", IOS_MIN_OS)

        # The icon, compiled from the fork's own catalog over PPSSPP's.
        for old in list(app.glob("AppIcon*.png")) + [app / "Assets.car"]:
            old.unlink(missing_ok=True)
        partial = tmp / "icons.plist"
        run("xcrun", "actool", IOS_ICONS, "--compile", app, "--platform", "iphoneos",
            "--minimum-deployment-target", IOS_MIN_OS, "--app-icon", "AppIcon",
            "--target-device", "iphone", "--target-device", "ipad",
            "--output-partial-info-plist", partial)
        with open(partial, "rb") as f:
            icons = plistlib.load(f)

        plist_path = app / "Info.plist"
        with open(plist_path, "rb") as f:
            plist = plistlib.load(f)
        for key in MAC_PLIST_DROP + ("UILaunchImageFile",):
            plist.pop(key, None)
        version = ios_version()
        plist.update(icons)
        plist.update({
            "CFBundleName": IOS_DISPLAY_NAME,
            "CFBundleDisplayName": IOS_DISPLAY_NAME,
            "CFBundleIdentifier": IOS_BUNDLE_ID,
            "CFBundleShortVersionString": version,
            "CFBundleVersion": version,
            "MinimumOSVersion": IOS_MIN_OS,
            "UILaunchStoryboardName": "Launch Screen",
            # Both of these, so the app's folder shows in the Files app as well as in Finder.
            "UIFileSharingEnabled": True,
            "LSSupportsOpeningDocumentsInPlace": True,
            "UISupportedInterfaceOrientations": [
                "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
            "UISupportedInterfaceOrientations~ipad": [
                "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
        })
        with open(plist_path, "wb") as f:
            plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)

        # Launch images for a 4-inch screen, from before launch storyboards. The storyboard
        # replaces them, and they are PPSSPP's.
        for old in app.glob("Default*.png"):
            old.unlink()

        # Ad hoc: no identity, which is all anyone without a developer account has. MoltenVK first,
        # because signing the bundle seals what is inside it.
        entitlements = tmp / "app.entitlements"
        with open(entitlements, "wb") as f:
            plistlib.dump(IOS_ENTITLEMENTS, f)
        run("xattr", "-cr", app)
        run("codesign", "--force", "--sign", "-", "--timestamp=none",
            app / "Frameworks" / IOS_MOLTENVK.name)
        run("codesign", "--force", "--sign", "-", "--timestamp=none",
            "--entitlements", entitlements, app)
        run("codesign", "--verify", "--strict", app)


def build_ios(out_dir, make_zip, with_textures):
    if not IOS_BUILT_APP.is_dir():
        fail(f"{IOS_BUILT_APP.relative_to(ROOT)} not found - run ./b-ios-vcs.sh first")
    if not IOS_MOLTENVK.is_file():
        fail(f"{IOS_MOLTENVK.relative_to(ROOT)} is missing")

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    # Beside the package, not in it - the APK's reasoning.
    ipa = out_dir.parent / IOS_IPA_NAME
    with tempfile.TemporaryDirectory() as tmp:
        payload = Path(tmp) / "Payload"
        payload.mkdir()
        build_ios_app(payload / IOS_APP_NAME)
        ipa.unlink(missing_ok=True)
        # An .ipa is a zip with Payload/ at its root. No resource forks or extended attributes:
        # they would land in the archive as ._ files inside a signed bundle.
        run("ditto", "-c", "-k", "--norsrc", "--noextattr", "--noqtn", "--keepParent", payload, ipa)
    print(f"wrote the .ipa, {ipa.stat().st_size / (1024 * 1024):.1f} MB -> {ipa}")

    stage_memstick(out_dir, with_textures, phone=True, ini_add=IOS_INI_ADD)
    finish_package(out_dir, make_zip, IOS_README)


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
    ap.add_argument("--android", action="store_true",
                    help="package the Android APK and its GTAVCS folder instead")
    ap.add_argument("--ios", action="store_true",
                    help="package the iOS .ipa and its game files instead")
    args = ap.parse_args()
    if args.ios:
        out = args.out
        if out == ap.get_default("out"):
            out = str(ROOT / "dist" / IOS_FOLDER)
        # No saves package, for the Android reason.
        build_ios(Path(out), args.zip, not args.no_textures)
        return
    if args.android:
        out = args.out
        if out == ap.get_default("out"):
            out = str(ROOT / "dist" / "GTA Vice City Stories Android")
        # No saves package: its README walks through desktop folders.
        build_android(Path(out), args.zip, not args.no_textures)
        return
    (build_mac if mac else build)(Path(args.out), args.zip, not args.no_textures)
    if not args.no_saves:
        # Beside the game's package, not inside it.
        out = Path(args.out)
        build_saves(out.with_name(out.name + " - Saves"), args.zip)


if __name__ == "__main__":
    main()
