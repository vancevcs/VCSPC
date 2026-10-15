// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#pragma once

#include "Common/CommonTypes.h"

// How far vehicles keep their full model - the vehicle half of the old draw-distance work, which
// found that the camera's LOD multiplier governs peds and vehicles rather than the map
// (docs/vcs/draw-distance.md, "The LOD multiplier was patched properly").
//
// The game sets its vehicle LOD distances every frame in CVisibilityComponents::SetRslCamera. A
// HOOKENTER on the instruction after its last store scales them to the Vehicle detail row's table
// (GameSettings().vehicleDetail), so the game's own multiplier still applies and the peds and the
// map are left alone. Installed from Tick and re-checked every tick: a savestate brings back the
// game's op. docs/vcs/vehicle-detail.md.

namespace VCS {

void VehicleDetailTick();
void RemoveVehicleDetail();

// The replacement (Core/HLE/ReplaceTables.cpp), at kVCSVisSetCameraDone.
int Hook_vcs_vehicle_detail();

}  // namespace VCS
