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

const char *VehicleClassName(VehicleClass klass) {
	switch (klass) {
	case VehicleClass::Car:   return "car";
	case VehicleClass::Bike:  return "bike";
	case VehicleClass::Boat:  return "boat";
	case VehicleClass::Heli:  return "heli";
	case VehicleClass::Plane: return "plane";
	case VehicleClass::Unknown:
	default:
		return "unknown";
	}
}

bool VehicleClassIsAircraft(VehicleClass klass) {
	return klass == VehicleClass::Heli || klass == VehicleClass::Plane;
}

VehicleClass VehicleClassForModel(u32 model) {
	// Only the non-car models are listed. Cars are the overwhelming majority and the default,
	// so listing them would be 80 lines that all say the same thing and one of them would
	// eventually be wrong.
	switch (model) {
	// Helicopters. The Sparrow and Sea Sparrow belong here despite the "sparrow" name - both are
	// light helicopters, not fixed-wing, and "coastg" is the Coastguard Maverick rather than a
	// coastguard boat.
	case 213:  // maverick
	case 260:  // vcnmav      - VCN Maverick
	case 261:  // polmav      - VCPD Maverick
	case 262:  // sparrow
	case 263:  // sesparow    - Sea Sparrow
	case 275:  // hunter
	case 277:  // coastg      - Coastguard Maverick
	case 279:  // chopper     - the other police helicopter
		return VehicleClass::Heli;

	// Fixed-wing. The autogyro (Little Willie) is a rotorcraft, but it flies like the planes and
	// gets identical bindings either way.
	case 173:  // autogyro
	case 222:  // biplane
	case 278:  // skimmer     - seaplane
	case 280:  // airtrain
		return VehicleClass::Plane;

	// Boats and personal watercraft.
	case 185:  // speeder2
	case 198:  // hovercr
	case 214:  // reefer
	case 215:  // speeder
	case 219:  // predator
	case 233:  // jetski
	case 247:  // dinghy
	case 248:  // marquis
	case 249:  // rio
	case 250:  // tropic
	case 257:  // squalo
	case 258:  // jetmax
		return VehicleClass::Boat;

	// Two-wheelers and quads. Grouped as Bike because they steer like one; the bindings match
	// a car's today, and the class exists so that stops being an assumption.
	case 170:  // 6atv
	case 178:  // bmxboy
	case 179:  // bmxgirl
	case 205:  // sanchez
	case 212:  // pcj600
	case 229:  // faggio
	case 230:  // quad
	case 231:  // angel
	case 232:  // freeway
	case 252:  // streetfi
	case 256:  // pheonix
		return VehicleClass::Bike;

	default:
		// 170..280 is the vehicle id range; outside it we were handed something that isn't a
		// vehicle at all, and saying "car" would be a claim rather than a default.
		return (model >= 170 && model <= 280) ? VehicleClass::Car : VehicleClass::Unknown;
	}
}

bool WeaponSlotIsMelee(u32 slot) {
	// Slots are category-ordered, as in every GTA of this era: 0 is empty-handed, 1 is the melee
	// slot, everything above it shoots. That is evidence here rather than analogy - in a live
	// gameplay state slot 0 read type 0 (no weapon at all), slot 1 held type 10 with 0/0 ammo
	// (melee carries none), and slots 2..8 held types 13, 19, 23, 26, 28, 32, 31, every one of them
	// with a real ammo count. Confirmed in play afterwards: holding a melee weapon read slot 1 and
	// the overlay tagged it (melee), with WASD working normally through a lock-on.
	//
	// If a melee weapon ever turns up above slot 1 this is the one place to fix, and the overlay
	// shows the slot live so it is cheap to check.
	return slot <= 1;
}

bool WeaponTypeIsBinoculars(u32 type) {
	// 39, and read out of the game's own script rather than guessed. The mission that hands the
	// binoculars over checks `has_char_got_weapon $PLAYER_CHAR weapon 39` and then immediately
	// does `set_current_char_weapon $PLAYER_CHAR to 39` (scm/MAIN.txt, BRY_B3_18510), which pins
	// the id to the item by the game's own account of what it just gave you.
	//
	// Confirmed live: with the binoculars in hand the overlay reads weaponType 39 in slot 9.
	//
	// The ID and not the slot, unlike the melee test above. Slot 9 is the last slot rather than
	// the binocular slot - whatever else lands there would answer yes - and this question is
	// asked about a specific ITEM with specific handling, not about a category.
	return type == 39;
}

void UpdateState(VCSState *state) {
	state->Clear();

	state->playerBase = ReadAddrU32(VCSAddr::PlayerBase);
	state->health = ReadAddrFloat(VCSAddr::PlayerHealth);
	// ReadAddrAsU32, not ReadAddrU32: this one is a single byte at PlayerBase + 0x789, which is not
	// 4-aligned, so a u32 read fails IsValid4AlignedRange and yields nullopt every single frame.
	state->weaponIndex = ReadAddrAsU32(VCSAddr::WeaponIndex);
	// The weapon id behind that slot lives at PlayerBase + 0x574 + slot * 28 + 4, lifted from the
	// handler for script command 02C0 get_current_char_weapon - see docs/VCS_ADDRESSES.md. The
	// address table can't express it (one level of indirection, no arithmetic), so compute it here.
	//
	// The slot is a SIGNED byte in the game and arrives here through a u8 read, so a -1 "no weapon"
	// would show up as 255. Range-check before using it as an index rather than reading 255 records
	// past the array.
	if (state->playerBase && state->weaponIndex && *state->weaponIndex <= 9) {
		state->weaponType = ReadU32(*state->playerBase + kVCSWeaponRecordsOffset +
			*state->weaponIndex * kVCSWeaponRecordStride + kVCSWeaponRecordTypeOffset);
	}
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

	// Non-zero only while committed to entering a vehicle, and set about 1.65s before PlayerVehicle
	// catches up. Left as nullopt rather than zero when idle so the overlay reads "unset" instead of
	// implying we looked and the answer was "not entering".
	const std::optional<u32> entering = ReadAddrU32(VCSAddr::PedEnteringVehicle);
	if (entering && *entering != 0) {
		state->enteringVehicle = entering;
	}

	// Only meaningful while actually in one - the entry is based on PlayerVehicle, so on foot it
	// resolves to a bad address and reads as nullopt anyway. Reading it unconditionally is fine.
	// AsU32, not U32: the entry is a U16 and +0x56 is only 2-byte aligned, so a word read would
	// both grab the wrong bytes and fail the 4-aligned bounds check.
	state->vehicleModel = ReadAddrAsU32(VCSAddr::VehicleModel);
	if (state->vehicleModel) {
		state->vehicleClass = VehicleClassForModel(*state->vehicleModel);
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
