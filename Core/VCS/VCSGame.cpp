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
#include "Core/HLE/sceCtrl.h"
#include "Core/System.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSCheats.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSRadar.h"
#include "Core/VCS/VCSRoute.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSSettings.h"
#include "Core/VCS/VCSState.h"
#include "Core/VCS/VCSVault.h"
#include "Core/VCS/VCSWorld.h"

namespace VCS {

// GTA: Vice City Stories, USA. The PAL (ULES00502/ULES00503) and JP (ULJM05297) releases have
// different builds and therefore different addresses, so they deliberately aren't accepted here
// even though the compat flag could be set for them. Supporting them means a second address
// table, not just another disc ID.
static const char *kVCSDiscIDUSA = "ULUS10160";

static bool g_active = false;
static std::string g_discID;
static u64 g_tickCount = 0;

// --- Boot phase ---
//
// Measured: FrameCounter is 0 for the whole of the logos and credits, and becomes nonzero - a
// fixed 30602, deterministically - the instant the world starts. That edge is the seam we hang
// the startup menu on.
//
// This is NOT the reverted "logic stopped means menu" idea from 03c1f3cfe0. That watched the
// counter stall *continuously* during play, where loading screens and cutscenes make it wrong.
// This reads the first 0 -> nonzero transition after boot, once. It is monotonic, so the
// ambiguity that sank the other one cannot arise.
static BootPhase g_bootPhase = BootPhase::Intro;
static int g_introSkipFrames = 0;

// Long enough for the game to notice the press across its 30Hz logic rate.
static constexpr int kIntroSkipHoldFrames = 8;

BootPhase GetBootPhase() {
	return g_bootPhase;
}

void NotifyMenuDismissed() {
	g_bootPhase = BootPhase::Playing;
}

void RequestIntroSkip() {
	if (g_bootPhase == BootPhase::Intro) {
		g_introSkipFrames = kIntroSkipHoldFrames;
	}
}

static void UpdateBootPhase() {
	if (g_bootPhase != BootPhase::Intro) {
		return;
	}

	// Cross is what skips the credits - measured, and Start on its own does not do it.
	if (g_introSkipFrames > 0) {
		g_introSkipFrames--;
		__CtrlUpdateButtons(CTRL_CROSS, 0);
	}

	const std::optional<u32> frames = ReadAddrU32(VCSAddr::FrameCounter);
	if (frames && *frames != 0) {
		g_bootPhase = BootPhase::AtMenu;
		g_introSkipFrames = 0;
	}
}

void Init() {
	g_active = false;
	g_discID.clear();
	g_tickCount = 0;
	g_bootPhase = BootPhase::Intro;
	g_introSkipFrames = 0;

	ResetHostKeys();
	ClearSharedState();
	CameraReset();
	CheatReset();
	FrontEndReset();
	MapCursorReset();
	RouteReset();
	RadarReset();

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

	// Only now, once we know this is actually VCS. The settings apply to nothing otherwise, and
	// reading the file for every game booted would be work done for no one.
	LoadSettings();

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
	// A combination half typed into a game that is going away is not worth finishing, and the
	// queue must not survive into the next boot.
	CheatReset();
	FrontEndReset();
	// Put the game's own instruction back before anything else tears down.
	RemoveFireHook();
	RemoveClimbSplashHook();
	VaultReset();
	RemoveWorldQuery();

	g_active = false;
	g_discID.clear();
	g_tickCount = 0;
}

void Tick() {
	if (!g_active) {
		return;
	}

	g_tickCount++;

	// Cheap, and the front end needs it before anything else this tick.
	UpdateBootPhase();

	// Free aim at the fire site. Installed HERE rather than in Init, because Init runs at
	// __KernelInit - before the EBOOT is loaded - so anything written there is overwritten by the
	// module loader. It self-guards on already-installed and on finding the expected instruction,
	// so retrying every tick until the code exists costs a single compare.
	InstallFireHook();

	// The world query, installed on the same terms and for the same reason - it writes a small
	// program into PSP memory, which cannot happen before there is a game to write it next to.
	//
	// Gated on vaulting being ON, unlike the fire hook, because unlike the fire hook it TAKES
	// something: 512 bytes out of the game's own user memory partition. That is almost certainly
	// harmless - the game sizes its pools from a fixed budget rather than from what is left - but
	// "almost certainly harmless" is not a reason to do it to someone who has not asked for the
	// feature. Nothing gives the block back until shutdown; a call could still be in flight.
	if (VaultSettings().enabled) {
		InstallWorldQuery();
		// And the hook that keeps the climb-out's water splash off dry land. It takes nothing from
		// the game and writes one instruction, but it is only ever about vaulting - so it goes on
		// the same switch.
		InstallClimbSplashHook();
	}

	// Decode first, then map - the context depends on what we just read.
	UpdateSharedState();
	const VCSInputContext context = ResolveContext(GetState());

	// Collect any answer the game left us, then let vaulting ask its next question and drive
	// whatever climb is running.
	//
	// BEFORE ApplyMapping, and that ordering is the whole trigger: a vault starts on the tick the
	// jump key goes down, and the mapping has to already know that so it can send the game nothing
	// instead of a jump. Reversed, every vault would begin with a hop.
	WorldQueryTick();
	VaultTick(context);

	// Advance any cheat combination the menu queued. BEFORE ApplyMapping for the same reason
	// VaultTick is: it decides on this tick whether it owns the pad, and the mapping has to
	// already know that so it can send the game the sequencer's press instead of the player's
	// keys. Reversed, the first press of every combination would go out with a held W beside it.
	CheatTick();

	// And the bridge into the game's own front end, on the same terms and in the same place: it
	// decides on this tick whether it owns the pad, and ApplyMapping has to already know.
	FrontEndTick();

	// The map's own cursor: put the game's full-screen cross away while the map is up, and work
	// out where ours goes. Emu thread, because it patches an instruction and reads the widget.
	MapCursorTick();

	// The GPS line. RadarTick is the one that matters: it reads the radar's origin, facing and
	// range, projects the route through the game's own transform and leaves screen-space segments
	// for the UI thread to draw. It writes no PSP memory.
	RadarTick();

	ApplyMapping(context);

	// The pad's right stick becomes look movement here, and it has to be BEFORE the three
	// consumers below rather than beside them: it FILLS the accumulator they drain. Run after
	// them and the stick would be one frame behind in every context, which on a camera reads as
	// lag rather than as nothing happening.
	ApplyPadLook(context);

	// These MUST stay in this order. Both want this frame's mouse delta and exactly one of them
	// gets it, decided by context and settings:
	//
	//   ApplyAnalog   takes it when the LEFT stick is the reticle (free aim)
	//   CameraTick    takes whatever it did not claim, and turns it into a rotation
	//
	// Reordering these lines would silently break aiming - the camera would consume the movement
	// and the crosshair would never move.
	//
	// Opens the aim model's frame. ApplyAnalog asks it what deflection to write and it must only
	// step once, so this has to come first.
	AimModelBeginFrame();

	ApplyAnalog(context);
	// Before CameraTick, so free aim gets the delta ahead of the camera - same one-consumer rule
	// as the line above.
	AimTick(context);

	// Mouse look last, so it sees the context we just resolved. This writes memory rather than
	// pressing buttons, which is why it isn't part of VCSInput.
	CameraTick(context);

	// After CameraTick, because it steers by the camera yaw and wants this frame's value rather
	// than last frame's. Writes the ped's velocity, so it has to run every frame or be wiped.
	FreeAimMoveTick(context);

	// Also after CameraTick, and for the same reason: it turns the character to the camera's yaw
	// and wants the value CameraTick just wrote, not the one from before it ran.
	PedAimTick(context);
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
