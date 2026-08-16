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

#include <optional>

#include "Common/CommonTypes.h"
#include "Core/VCS/VCSAddresses.h"

// The game state we care about, decoded once per frame from PSP memory.
//
// Every field is an optional. "Not found yet" and "found, and the value is 0/false" are
// genuinely different things here - conflating them would make the input layer act on an
// address table that is still empty. Anything reading this struct must handle nullopt.

namespace VCS {

struct VCSState {
	// Mutually exclusive in practice, but kept separate because early on we may be able to
	// determine one and not the other.
	std::optional<bool> onFoot;
	std::optional<bool> inVehicle;

	// Locked on to a target. Stays false during free aim.
	std::optional<bool> isAiming;
	// Free-aiming (sniper / RPG). Stays false when locked on, so the two are independent and
	// together give three states: neither, locked on, free aim.
	std::optional<bool> isFreeAiming;
	std::optional<u32> weaponIndex;

	// Radians. Sign convention is whatever the game uses; task 2 will pin it down once
	// CameraYaw is known.
	std::optional<float> cameraYaw;
	std::optional<float> cameraPitch;

	std::optional<float> health;

	// Raw pointer values, useful in the debugger for chasing struct offsets.
	std::optional<u32> playerBase;
	std::optional<u32> playerVehicle;

	// True if we managed to read anything at all this frame. False with an empty table.
	bool anyValid = false;

	void Clear() { *this = VCSState(); }
};

// Reads the current state out of PSP memory via the address table.
//
// Must be called on the emu thread. Safe to call with any subset of the table filled in,
// including none of it - unset entries simply leave their fields as nullopt.
void UpdateState(VCSState *state);

// The state decoded on the most recent tick. Returns a stable reference; the contents are
// refreshed by VCSGame::Tick. Reading this from the ImGui debugger is fine because that runs
// on the same thread as the CPU (see AGENTS.md), so it can never observe a torn update.
const VCSState &GetState();

// Drive the shared instance returned by GetState. Called by VCSGame only - everyone else gets
// the read-only view above.
void UpdateSharedState();
void ClearSharedState();

}  // namespace VCS
