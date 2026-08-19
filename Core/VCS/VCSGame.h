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

// The disc ID we booted with, for display. Empty when no game is running.
const std::string &GetDiscID();

// Number of ticks since Init, so the debugger can show that the hook is actually firing. This
// is genuinely useful while bringing the module up - a frozen counter means the vblank hook
// isn't wired.
u64 GetTickCount();

}  // namespace VCS
