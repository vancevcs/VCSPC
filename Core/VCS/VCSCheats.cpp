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

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "Common/System/OSD.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSCheats.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

// Short names for the presses, so a row below reads like the combination a player would be told
// to enter. `kL`/`kR` are the shoulder buttons and `kLf`/`kRt` the d-pad, which is the one
// distinction worth spelling out - published cheat lists write both as "L" and "R" and "LEFT" and
// "RIGHT", and confusing the two is the easiest way to enter a table full of near misses.
namespace {
constexpr u32 kUp = CTRL_UP;
constexpr u32 kDn = CTRL_DOWN;
constexpr u32 kLf = CTRL_LEFT;
constexpr u32 kRt = CTRL_RIGHT;
constexpr u32 kX = CTRL_CROSS;
constexpr u32 kO = CTRL_CIRCLE;
constexpr u32 kSq = CTRL_SQUARE;
constexpr u32 kTr = CTRL_TRIANGLE;
constexpr u32 kL = CTRL_LTRIGGER;
constexpr u32 kR = CTRL_RTRIGGER;
}  // namespace

// The cheats, in menu order within each group.
//
// These are the combinations as the game's published lists give them, NOT as read out of the
// game's own table - so a row that does nothing in play is a transcription to fix here, and
// nothing more serious than that. The authoritative version is in the EBOOT: the matcher that
// compares the pad history against these lives near whatever writes the flag that opcode `02A4
// are_any_car_cheats_activated` reads, and mining it would replace this table with measured data.
// Worth doing; not worth blocking a working menu on.
static const VCSCheat kCheats[] = {
	// --- Player ---
	{ CheatGroup::Player, "FULL HEALTH", "Refills the health bar.",
		{ kUp, kDn, kLf, kRt, kO, kO, kL, kR }, false },
	{ CheatGroup::Player, "FULL ARMOR", "Refills body armour.",
		{ kUp, kDn, kLf, kRt, kSq, kSq, kL, kR }, false },
	{ CheatGroup::Player, "GET $250,000", "Adds a quarter of a million dollars.",
		{ kUp, kDn, kLf, kRt, kX, kX, kL, kR }, false },
	{ CheatGroup::Player, "WEAPON SET 1", "Melee and the light end of the arsenal.",
		{ kLf, kRt, kX, kUp, kDn, kSq, kLf, kRt }, false },
	{ CheatGroup::Player, "WEAPON SET 2", "The middle tier - assault weapons.",
		{ kLf, kRt, kSq, kUp, kDn, kTr, kLf, kRt }, false },
	{ CheatGroup::Player, "WEAPON SET 3", "The heavy tier - explosives and the sniper.",
		{ kLf, kRt, kTr, kUp, kDn, kO, kLf, kRt }, false },
	{ CheatGroup::Player, "NEVER WANTED", "Locks the wanted level at zero.",
		{ kUp, kRt, kTr, kTr, kDn, kLf, kX, kX }, false },
	{ CheatGroup::Player, "RAISE WANTED LEVEL", "Adds two stars each time it is entered.",
		{ kUp, kRt, kSq, kSq, kDn, kLf, kO, kO }, false },
	{ CheatGroup::Player, "COMMIT SUICIDE", "Kills Vic. There is no confirmation.",
		{ kRt, kRt, kO, kO, kL, kR, kDn, kX }, false },

	// --- Vehicles ---
	{ CheatGroup::Vehicles, "SPAWN RHINO TANK", "Drops a tank next to you.",
		{ kUp, kL, kDn, kR, kLf, kL, kRt, kR }, false },
	{ CheatGroup::Vehicles, "SPAWN TRASHMASTER", "Drops a garbage truck next to you.",
		{ kDn, kUp, kRt, kTr, kL, kTr, kL, kTr }, false },
	{ CheatGroup::Vehicles, "PERFECT HANDLING", "Cars grip the road and cannot roll.",
		{ kDn, kLf, kUp, kL, kR, kTr, kO, kX }, false },
	{ CheatGroup::Vehicles, "CHROME CARS", "Every vehicle gets a mirror finish.",
		{ kRt, kUp, kLf, kDn, kTr, kTr, kL, kR }, false },
	{ CheatGroup::Vehicles, "BLACK TRAFFIC", "Every traffic car spawns black.",
		{ kL, kR, kL, kR, kLf, kO, kUp, kX }, false },
	{ CheatGroup::Vehicles, "DESTROY ALL CARS", "Blows up every vehicle around you.",
		{ kL, kR, kR, kLf, kRt, kSq, kDn, kR }, false },
	{ CheatGroup::Vehicles, "AGGRESSIVE DRIVERS", "Traffic drives at you rather than past you.",
		{ kUp, kUp, kRt, kLf, kTr, kO, kO, kSq }, false },
	{ CheatGroup::Vehicles, "ALL GREEN LIGHTS", "Every traffic light stays green.",
		{ kUp, kDn, kTr, kX, kL, kR, kLf, kO }, false },
	{ CheatGroup::Vehicles, "UPSIDE DOWN", "Flips the screen over.",
		{ kSq, kSq, kSq, kL, kL, kR, kLf, kRt }, false },
	{ CheatGroup::Vehicles, "REVERSE UPSIDE DOWN", "Flips the screen the other way.",
		{ kLf, kLf, kLf, kR, kR, kL, kRt, kLf }, false },

	// --- Pedestrians ---
	{ CheatGroup::Pedestrians, "PEDESTRIANS ATTACK", "Everyone turns on you. CANNOT BE UNDONE.",
		{ kDn, kTr, kUp, kX, kL, kR, kL, kR }, true },
	{ CheatGroup::Pedestrians, "PEDESTRIANS RIOT", "The city turns on itself. CANNOT BE UNDONE.",
		{ kR, kL, kL, kDn, kLf, kO, kDn, kL }, true },
	{ CheatGroup::Pedestrians, "PEDESTRIANS HAVE WEAPONS", "Arms everyone on the street.",
		{ kUp, kL, kDn, kR, kLf, kO, kRt, kTr }, false },
	{ CheatGroup::Pedestrians, "PASSENGER PEDESTRIANS", "Pedestrians climb into your car.",
		{ kDn, kUp, kRt, kL, kL, kSq, kUp, kL }, false },
	{ CheatGroup::Pedestrians, "GUY MAGNET", "Men follow you around.",
		{ kRt, kL, kDn, kL, kO, kUp, kL, kSq }, false },

	// --- World ---
	{ CheatGroup::World, "CLEAR WEATHER", "Cloudless sky.",
		{ kLf, kDn, kR, kL, kRt, kUp, kLf, kX }, false },
	{ CheatGroup::World, "SUNNY WEATHER", "Bright and hazy.",
		{ kLf, kDn, kR, kL, kRt, kUp, kLf, kO }, false },
	{ CheatGroup::World, "OVERCAST WEATHER", "Heavy cloud.",
		{ kLf, kDn, kL, kR, kRt, kUp, kLf, kSq }, false },
	{ CheatGroup::World, "RAINY WEATHER", "Storm.",
		{ kLf, kDn, kL, kR, kRt, kUp, kLf, kTr }, false },
	{ CheatGroup::World, "FOGGY WEATHER", "Thick fog.",
		{ kLf, kDn, kTr, kX, kRt, kUp, kLf, kL }, false },
	{ CheatGroup::World, "FASTER CLOCK", "The game's clock runs faster.",
		{ kR, kL, kL, kDn, kUp, kX, kDn, kL }, false },
	{ CheatGroup::World, "FASTER GAMEPLAY", "Speeds everything up.",
		{ kLf, kLf, kR, kR, kUp, kTr, kDn, kX }, false },
	{ CheatGroup::World, "SLOWER GAMEPLAY", "Slows everything down.",
		{ kLf, kLf, kO, kO, kDn, kUp, kTr, kX }, false },

};

const VCSCheat *Cheats() {
	return kCheats;
}

size_t CheatCount() {
	return ARRAY_SIZE(kCheats);
}

int CheatLength(const VCSCheat &cheat) {
	for (int i = 0; i < kMaxCheatPresses; i++) {
		if (cheat.press[i] == 0) {
			return i;
		}
	}
	return kMaxCheatPresses;
}

VCSCheatSettings &CheatSettings() {
	static VCSCheatSettings settings;
	return settings;
}

// --- The sequencer -----------------------------------------------------------------------------

namespace {

enum class Phase {
	Idle,
	LeadIn,   // nothing pressed, letting go of whatever the player was holding
	Hold,     // one press asserted
	Gap,      // released, so the next press reads as a new one
	TailOut,  // nothing pressed, letting the last release land
};

Phase g_phase = Phase::Idle;
int g_active = -1;
int g_press = 0;
int g_remaining = 0;
u32 g_mask = 0;

// The game logic clock we step on. `g_haveFrame` distinguishes "no frame seen yet" from "frame
// zero", which matters on the very first tick after a boot.
u32 g_lastFrame = 0;
bool g_haveFrame = false;
int g_vblanks = 0;

std::mutex g_queueMutex;
std::vector<int> g_queue;
constexpr size_t kMaxQueued = 4;

// Whether the game has advanced a logic frame since the last tick.
//
// `FrameCounter` is the real answer and the one that stays correct when the game drops below
// 30Hz - a press measured in vblanks would be held for fewer samples exactly when the game is
// busiest. The vblank fallback below is deliberately cruder: it assumes the shipped 2:1 ratio,
// which is right in ordinary play and approximate everywhere else. It exists so that an unset
// FrameCounter degrades the timing rather than removing the feature.
bool GameFrameAdvanced() {
	const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
	if (frame) {
		g_vblanks = 0;
		if (!g_haveFrame) {
			g_haveFrame = true;
			g_lastFrame = *frame;
			return false;
		}
		if (*frame == g_lastFrame) {
			return false;
		}
		g_lastFrame = *frame;
		return true;
	}

	g_haveFrame = false;
	if (++g_vblanks < 2) {
		return false;
	}
	g_vblanks = 0;
	return true;
}

int PhaseFrames(Phase phase) {
	const VCSCheatSettings &s = CheatSettings();
	int frames = 1;
	switch (phase) {
	case Phase::LeadIn: frames = s.leadInFrames; break;
	case Phase::Hold: frames = s.holdFrames; break;
	case Phase::Gap: frames = s.gapFrames; break;
	case Phase::TailOut: frames = s.tailFrames; break;
	case Phase::Idle: return 0;
	}
	// A phase of zero frames would be skipped entirely, which for a gap means two presses merging
	// into one - the failure this whole file is timed to avoid. One is the floor.
	return std::max(frames, 1);
}

void EnterPhase(Phase phase) {
	g_phase = phase;
	g_remaining = PhaseFrames(phase);
	// Every phase but Hold is silence; Hold sets its own mask before calling this.
	if (phase != Phase::Hold) {
		g_mask = 0;
	}
}

void BeginPress() {
	const VCSCheat &cheat = kCheats[g_active];
	g_mask = cheat.press[g_press];
	EnterPhase(Phase::Hold);
}

void Finish() {
	g_phase = Phase::Idle;
	g_active = -1;
	g_press = 0;
	g_remaining = 0;
	g_mask = 0;
}

// Take the next queued cheat, or -1.
int PopQueued() {
	std::lock_guard<std::mutex> guard(g_queueMutex);
	if (g_queue.empty()) {
		return -1;
	}
	const int index = g_queue.front();
	g_queue.erase(g_queue.begin());
	return index;
}

}  // namespace

bool RequestCheat(int index) {
	if (index < 0 || (size_t)index >= ARRAY_SIZE(kCheats)) {
		return false;
	}
	std::lock_guard<std::mutex> guard(g_queueMutex);
	if (g_queue.size() >= kMaxQueued) {
		return false;
	}
	g_queue.push_back(index);
	return true;
}

bool CheatEntryInProgress() {
	return g_phase != Phase::Idle;
}

u32 CheatButtonMask() {
	return g_mask;
}

void CheatTick() {
	if (g_phase == Phase::Idle) {
		const int next = PopQueued();
		if (next < 0) {
			return;
		}
		g_active = next;
		g_press = 0;
		EnterPhase(Phase::LeadIn);

		// Said here rather than when the row was clicked, because the click happens over a paused
		// game where nothing can be drawn over the world yet. It is also the only feedback there
		// is when a combination is wrong: the game prints its own confirmation on success, so
		// this message alone, with nothing following it, is the symptom of a bad table row.
		g_OSD.Show(OSDType::MESSAGE_INFO,
			std::string("Cheat: ") + kCheats[g_active].name, 2.0f, "vcs_cheat");
		return;
	}

	// Hold whatever we are holding until the game has actually looked at the pad again.
	if (!GameFrameAdvanced()) {
		return;
	}

	if (--g_remaining > 0) {
		return;
	}

	const int length = CheatLength(kCheats[g_active]);

	switch (g_phase) {
	case Phase::LeadIn:
		BeginPress();
		break;

	case Phase::Hold:
		g_press++;
		if (g_press >= length) {
			EnterPhase(Phase::TailOut);
		} else {
			EnterPhase(Phase::Gap);
		}
		break;

	case Phase::Gap:
		BeginPress();
		break;

	case Phase::TailOut:
		Finish();
		break;

	case Phase::Idle:
		break;
	}
}

void CheatReset() {
	Finish();
	g_haveFrame = false;
	g_lastFrame = 0;
	g_vblanks = 0;
	std::lock_guard<std::mutex> guard(g_queueMutex);
	g_queue.clear();
}

const char *CheatStatus() {
	switch (g_phase) {
	case Phase::LeadIn: return "lead-in";
	case Phase::Hold: return "press";
	case Phase::Gap: return "gap";
	case Phase::TailOut: return "tail";
	case Phase::Idle: break;
	}
	return "idle";
}

int ActiveCheat() {
	return g_active;
}

}  // namespace VCS
