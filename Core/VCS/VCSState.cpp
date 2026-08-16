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

#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSState.h"

namespace VCS {

static VCSState g_state;

void UpdateState(VCSState *state) {
	state->Clear();

	state->playerBase = ReadAddrU32(VCSAddr::PlayerBase);
	state->health = ReadAddrFloat(VCSAddr::PlayerHealth);
	state->weaponIndex = ReadAddrU32(VCSAddr::WeaponIndex);
	state->isAiming = ReadAddrBool(VCSAddr::IsAiming);
	state->isFreeAiming = ReadAddrBool(VCSAddr::IsFreeAiming);
	state->cameraYaw = ReadAddrFloat(VCSAddr::CameraYaw);
	state->cameraPitch = ReadAddrFloat(VCSAddr::CameraPitch);

	// Vehicle occupancy. The game may expose this either as an explicit on-foot flag or as a
	// vehicle pointer that's null on foot; we support whichever turns up first, and derive the
	// other side from it so callers don't have to care which one we actually found.
	const std::optional<u32> vehicle = ReadAddrU32(VCSAddr::PlayerVehicle);
	if (vehicle) {
		state->playerVehicle = *vehicle != 0 ? vehicle : std::nullopt;
		state->inVehicle = *vehicle != 0;
		state->onFoot = *vehicle == 0;
	}

	// An explicit flag, if we have one, wins over the pointer-derived guess.
	const std::optional<bool> onFootFlag = ReadAddrBool(VCSAddr::PlayerOnFoot);
	if (onFootFlag) {
		state->onFoot = *onFootFlag;
		state->inVehicle = !*onFootFlag;
	}

	state->anyValid =
		state->playerBase.has_value() ||
		state->health.has_value() ||
		state->weaponIndex.has_value() ||
		state->isAiming.has_value() ||
		state->isFreeAiming.has_value() ||
		state->cameraYaw.has_value() ||
		state->cameraPitch.has_value() ||
		state->onFoot.has_value() ||
		state->inVehicle.has_value();
}

const VCSState &GetState() {
	return g_state;
}

void UpdateSharedState() {
	UpdateState(&g_state);
}

void ClearSharedState() {
	g_state.Clear();
}

}  // namespace VCS
