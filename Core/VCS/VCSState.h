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

// How the game flies/drives this vehicle, which is what decides the bindings.
//
// The names are the game's own - it keeps a type-name table at 0x08bafc6c reading
// "car boat jetski train heli plane bike ferry bmx quad". Only the distinctions the control
// scheme actually cares about are modelled here: a jetski steers like a boat and a bmx like a
// bike, so they collapse. Heli and plane stay separate only because a plane may yet turn out
// to need something a helicopter doesn't; today they get identical bindings.
enum class VehicleClass {
	Unknown,
	Car,
	Bike,
	Boat,
	Heli,
	Plane,
};

const char *VehicleClassName(VehicleClass klass);

// Model id -> class, from the game's own model-name table (stride 0x1c, entry 213 is
// "maverick") cross-checked against gtamods' VCS vehicle list. Anything unrecognised is
// Unknown, which the input layer treats as a car - the safe default, since that is what the
// single in-vehicle binding set has always assumed.
VehicleClass VehicleClassForModel(u32 model);

// Whether this class flies, i.e. whether the aircraft bindings apply.
bool VehicleClassIsAircraft(VehicleClass klass);

// Whether the selected weapon is fists or melee - i.e. the player is holding nothing that shoots,
// so there is no free aim to hand the mouse. Takes the SLOT, not the weapon id.
bool WeaponSlotIsMelee(u32 slot);

// Whether this is the binoculars. Takes the weapon ID, not the slot - the two are different
// numbers and only the id says WHICH weapon. See the definition for where 39 comes from.
bool WeaponTypeIsBinoculars(u32 type);

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
	// The selected weapon SLOT (0-9), not a weapon id - see WeaponIndex in VCSAddresses.h. Slots
	// are ordered by category, which is what makes the melee test below possible.
	std::optional<u32> weaponIndex;
	// The weapon id sitting in that slot, read out of the ped's weapon array. The slot alone does
	// not say what the weapon is, and the aim layer has to tell melee from a gun.
	std::optional<u32> weaponType;

	// Radians. Sign convention is whatever the game uses; task 2 will pin it down once
	// CameraYaw is known.
	std::optional<float> cameraYaw;
	std::optional<float> cameraPitch;

	std::optional<float> health;

	// Raw pointer values, useful in the debugger for chasing struct offsets.
	std::optional<u32> playerBase;
	std::optional<u32> playerVehicle;

	// Non-zero while the player is committed to entering a vehicle, before playerVehicle is set.
	// See PedEnteringVehicle in VCSAddresses.h - this is the window pitch must be normalised in.
	std::optional<u32> enteringVehicle;

	// The occupied vehicle's model id, and what that model is. Both stay unset/Unknown on foot.
	std::optional<u32> vehicleModel;
	VehicleClass vehicleClass = VehicleClass::Unknown;

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
