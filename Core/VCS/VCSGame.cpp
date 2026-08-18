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

#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/System.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSState.h"

namespace VCS {

// GTA: Vice City Stories, USA. The PAL (ULES00502/ULES00503) and JP (ULJM05297) releases have
// different builds and therefore different addresses, so they deliberately aren't accepted here
// even though the compat flag could be set for them. Supporting them means a second address
// table, not just another disc ID.
static const char *kVCSDiscIDUSA = "ULUS10160";

static bool g_active = false;
static std::string g_discID;
static u64 g_tickCount = 0;

void Init() {
	g_active = false;
	g_discID.clear();
	g_tickCount = 0;

	ResetHostKeys();
	ClearSharedState();
	CameraReset();

	g_discID = g_paramSFO.GetDiscID();

	// Two independent gates. The compat flag alone isn't enough, because a user can put anything
	// in PSP/System/compat.ini and we'd rather do nothing than read a different game's memory.
	if (!PSP_CoreParameter().compat.flags().VCSInputOverhaul) {
		return;
	}

	if (g_discID != kVCSDiscIDUSA) {
		WARN_LOG(Log::System, "VCSInputOverhaul is set for disc ID %s, but only %s is supported. Staying inactive.",
			g_discID.c_str(), kVCSDiscIDUSA);
		return;
	}

	g_active = true;

	int known = 0;
	for (size_t i = 0; i < ARRAY_SIZE(kVCSAddresses); i++) {
		if (IsAddrSet(kVCSAddresses[i].id)) {
			known++;
		}
	}

	INFO_LOG(Log::System, "VCS input overhaul active for %s (%d/%d addresses known)",
		g_discID.c_str(), known, (int)VCSAddr::Count);
}

void Shutdown() {
	// Release any buttons we were holding before the ctrl module goes away.
	ResetHostKeys();
	ClearSharedState();
	CameraReset();
	// Put the game's own instruction back before anything else tears down.
	RemoveFireHook();

	g_active = false;
	g_discID.clear();
	g_tickCount = 0;
}

void Tick() {
	if (!g_active) {
		return;
	}

	g_tickCount++;

	// Free aim at the fire site. Installed HERE rather than in Init, because Init runs at
	// __KernelInit - before the EBOOT is loaded - so anything written there is overwritten by the
	// module loader. It self-guards on already-installed and on finding the expected instruction,
	// so retrying every tick until the code exists costs a single compare.
	InstallFireHook();

	// Decode first, then map - the context depends on what we just read.
	UpdateSharedState();
	const VCSInputContext context = ResolveContext(GetState());
	ApplyMapping(context);

	// These three MUST stay in this order. All of them want this frame's mouse delta and exactly
	// one of them gets it, decided by context and settings:
	//
	//   ApplyAnalog   takes it when the LEFT stick is the reticle (free aim, stock behaviour)
	//   ApplyAimStick takes it when the RIGHT stick feeds the CLEO plugin (aimViaRightStick)
	//   CameraTick    takes whatever neither of them claimed, and turns it into a rotation
	//
	// The first two are mutually exclusive, so the delta is never spent twice. Reordering these
	// lines would silently break aiming - the camera would consume the movement and the crosshair
	// would never move.
	//   PadStickTick  takes it when the mouse drives the game's own synthesised second stick
	//
	// Opens the aim model's frame. Both ApplyAnalog and PadStickTick will ask it what deflection
	// to write and it must only step once, so this has to come before either of them.
	AimModelBeginFrame();

	ApplyAnalog(context);
	ApplyAimStick(context);
	PadStickTick(context);
	// Before CameraTick, so free aim gets the delta ahead of the camera - same one-consumer rule
	// as the two above.
	AimTick(context);

	// Mouse look last, so it sees the context we just resolved. This writes memory rather than
	// pressing buttons, which is why it isn't part of VCSInput.
	CameraTick(context);

	// After CameraTick, because it steers by the camera yaw and wants this frame's value rather
	// than last frame's. Writes the ped's velocity, so it has to run every frame or be wiped.
	FreeAimMoveTick(context);
}

bool IsActive() {
	return g_active;
}

const std::string &GetDiscID() {
	return g_discID;
}

u64 GetTickCount() {
	return g_tickCount;
}

}  // namespace VCS
