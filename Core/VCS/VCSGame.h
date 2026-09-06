// Copyright (c) 2026- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <string>

#include "Common/CommonTypes.h"
// For kVCSVolumeMax, which is the range the two volume settings below are expressed in.
#include "Core/VCS/VCSAddresses.h"

// Lifecycle and per-frame driver for the GTA: Vice City Stories input overhaul.
//
// This is the only entry point the rest of PPSSPP knows about. Three call sites, all of them
// no-ops for every other game:
//
//   __KernelInit()      -> VCS::Init()      (Core/HLE/sceKernel.cpp)
//   __KernelShutdown()  -> VCS::Shutdown()  (Core/HLE/sceKernel.cpp)
//   hleEnterVblank()    -> VCS::Tick()      (Core/HLE/sceDisplay.cpp)
//
// Everything is gated on the VCSInputOverhaul compat flag AND on the disc ID, so with the flag
// off - which is every game except ULUS10160 - Tick returns on its first line and no other VCS
// code is ever reached.

namespace VCS {

// Called once per game boot, from __KernelInit. Decides whether this is VCS and whether the
// compat flag is on; if either is false, everything else in this module stays dormant.
void Init();

// Called from __KernelShutdown. Safe to call when Init decided not to activate.
void Shutdown();

// Called once per vblank from hleEnterVblank, on the emu thread. Decodes game state and applies
// the input mapping. Returns immediately when inactive.
// Where the game is in its boot sequence, which the front end needs because VCS has no menu of
// its own - it goes logos, credits, straight into the story. See "The boot sequence, measured"
// in CLAUDE.md for the measurements behind this.
enum class BootPhase {
	Intro,    // logos and credits are playing; FrameCounter reads 0
	AtMenu,   // the world has just started - the moment to put our menu up
	Playing,  // the menu has been dismissed and the player is in the game
};

BootPhase GetBootPhase();

// Called once the startup menu has been dismissed, so it is not shown again this run.
void NotifyMenuDismissed();

// Skip the credits. Presses the game's own skip button rather than trying to fast-forward it,
// which is why the seam still takes about four seconds to arrive afterwards.
void RequestIntroSkip();

void Tick();

// True when this is VCS and the compat flag is on. Used by the debugger window to decide
// whether to show live data or an explanation of why there isn't any.
bool IsActive();

// True when this build should present as the GAME rather than as an emulator: no PPSSPP logo,
// no menu bar, no ImGui debugger, no speed counter. Release builds only, and only for VCS -
// the Debug build keeps every tool, and no other game is ever affected, per the fork's
// zero-behaviour-change rule.
//
// Safe to call before a game boots, which is what the logo-screen decision needs.
bool PresentAsGame();

// Whether this BINARY is the game's rather than the workshop's: Release in this fork, whatever
// disc it has or has not been pointed at yet.
//
// A different question from PresentAsGame, and the difference only shows on the first run. That
// one asks "is this session the game", which needs a disc, and is right for the things it gates -
// the speed counter, the pause key, the logo screen. The MENU BAR is not one of them: it carries
// Debug, Emulation and Game settings, and a build somebody downloaded to play one game should not
// put those on the only screen they have to use, purely because it has not been told where their
// disc is yet. Measured on a packaged build: the first screen offered a Debug menu.
bool IsGameBuild();

// Presentation settings this module owns, as opposed to the ones that belong to a mechanic.
//
// showFps deliberately does NOT edit g_Config.iShowStatusFlags, and that is not a duplicate
// home for one setting: PPSSPP's own settings screen is unreachable in the game build, so the
// two govern different builds. This row is the counter in Release; iShowStatusFlags is still
// the counter in Debug, set from PPSSPP's Graphics page as it always was.
//
// Off by default. The point of the game build is that it looks like a game, and a player who
// wants the number now has a row to turn it on with.
//
// The two below are a different kind of row and worth telling apart from showFps: they do not
// belong to this fork at all. Subtitles and the HUD are settings the PSP game already has, on the
// Display page of its own front end - which this fork's menu does not offer a way into, so
// without these rows they became unreachable, the same loss the map and the save list took.
//
// They are mirrored into plain bools here rather than edited in place because the menu runs on
// the UI thread and PSP memory may only be touched from the emu thread. ApplyDisplayPrefs pushes
// them across once a tick; see the note there for why that is a write-when-different rather than
// a write-every-frame.
struct VCSGameSettings {
	bool showFps = false;

	// Both default to what the game ships with, so a player who never opens the page gets the
	// retail behaviour rather than this fork's opinion of it.
	bool subtitles = true;

	// The health, armour, money, weapon and clock panel. NOT the radar, which the game keeps on a
	// separate RADAR MODE setting - turning this off leaves the radar drawn, exactly as the
	// retail row does.
	bool hud = true;

	// The game's own two volumes, 0..kVCSVolumeMax. `radioVolume` is what its own Audio page
	// calls MUSIC VOLUME - in this game the music IS the radio, and the row is named after what
	// a player hears rather than after the mixer channel.
	//
	// Full by default, which is this fork's choice rather than the game's: nothing here can ask
	// the game what it shipped with, and a menu that owns a setting has to have an answer for
	// "restore defaults". The cost is one-off and worth stating - the first boot after these rows
	// appeared overrides whatever the player had set in the game's own Audio page, and every boot
	// after that uses what they set here.
	int sfxVolume = kVCSVolumeMax;
	int radioVolume = kVCSVolumeMax;
};

VCSGameSettings &GameSettings();

// Substitute a file on the UMD as it is read, by BYTE RANGE rather than by name.
//
// Called from ISOFileSystem::ReadFile with the absolute position on the disc that was just read
// into `data`. Overwrites any part of it that falls inside a patched file. Returns immediately
// for every other game, and for VCS whenever the read is nowhere near one.
//
// **By range because there is no name to match.** VCS opens `disc0:/sce_lbn0x0_size0x65170000` -
// the whole UMD as one stream - and seeks to its own files by sector, so nothing on the disc is
// ever requested by filename. A redirect keyed on the path was built first and never fired once;
// this is the same substitution one layer down, where the game's own addressing lives.
//
// What it buys over patching the ISO: the retail disc is untouched and stays the boot path, the
// replacement is a file on the memory stick that can be deleted to undo it, and there is no
// 1.6 GB copy. What it does NOT change is the size limit - the game seeks by sector numbers baked
// into its own code, so a replacement has to fit the original's span and is padded to it.
void PatchDiscRead(u64 positionOnIso, u8 *data, size_t bytes);

// The disc ID we booted with, for display. Empty when no game is running.
const std::string &GetDiscID();

// Number of ticks since Init, so the debugger can show that the hook is actually firing. This
// is genuinely useful while bringing the module up - a frozen counter means the vblank hook
// isn't wired.
u64 GetTickCount();

}  // namespace VCS
