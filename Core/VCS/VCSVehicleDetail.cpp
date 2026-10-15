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

#include <cmath>

#include "Common/Common.h"
#include "Common/Log.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSVehicleDetail.h"

namespace VCS {

namespace {

// The distances SetRslCamera multiplies CCamera+0x7a0 by, in metres, in the order of kDistances: a
// car's lod0, lod1 and fade, a big vehicle's lod0 and lod1. PSP is the game's own; HIGH the LCS
// Android port's dvLODSettingsHigh_* table (VehicleDist0 / 1, VehicleFadeDist, BigVehicleDist0 / 1)
// - the same engine, and the only table Rockstar shipped for it; ULTRA twice that.
const u32 kDistances[] = {
	kVCSVehicleLod0Dist, kVCSVehicleLod1Dist, kVCSVehicleFadeDist, kVCSBigVehicleLod0Dist, kVCSBigVehicleLod1Dist,
};
const float kMetres[][ARRAY_SIZE(kDistances)] = {
	{ 50.0f, 70.0f, 90.0f, 50.0f, 110.0f },     // PSP
	{ 120.0f, 140.0f, 160.0f, 120.0f, 140.0f },  // HIGH
	{ 240.0f, 280.0f, 320.0f, 240.0f, 280.0f },  // ULTRA
};

bool g_installed = false;
int g_level = kVCSVehicleDetailPSP;
bool g_said = false;

int WantedLevel() {
	const int level = GameSettings().vehicleDetail;
	return level >= 0 && level < (int)ARRAY_SIZE(kMetres) ? level : kVCSVehicleDetailPSP;
}

void Unhook() {
	if (!g_installed || !currentMIPS) {
		return;
	}
	// Invalidate BEFORE restoring: at a block start the raw word is the block cache's marker, and
	// the restorer declines unless it finds a replacement there.
	currentMIPS->InvalidateICache(kVCSVisSetCameraDone, 4);
	RestoreReplacedInstruction(kVCSVisSetCameraDone);
	// Written outright as well: restoring an instruction this session never saved writes a nop.
	Memory::WriteUnchecked_U32(kVCSVisSetCameraDoneOp, kVCSVisSetCameraDone);
	currentMIPS->InvalidateICache(kVCSVisSetCameraDone, 4);
	g_installed = false;
}

}  // namespace

int Hook_vcs_vehicle_detail() {
	const int level = g_level;
	if (!g_installed || level == kVCSVehicleDetailPSP) {
		return 0;
	}
	// SetRslCamera has just stored this frame's values, (multiplier * the PSP's metres) squared, so
	// one multiply each by the squared ratio is the table's distance at the game's own multiplier -
	// and nothing compounds, because the function recomputes them before every pass through here.
	float before = 0.0f, after = 0.0f;
	for (size_t i = 0; i < ARRAY_SIZE(kDistances); i++) {
		const float stored = ReadFloat(kDistances[i]).value_or(0.0f);
		if (!(stored > 0.0f) || !std::isfinite(stored)) {
			continue;
		}
		const float ratio = kMetres[level][i] / kMetres[kVCSVehicleDetailPSP][i];
		WriteFloat(kDistances[i], stored * ratio * ratio);
		if (i == 0) {
			before = stored;
			after = stored * ratio * ratio;
		}
	}
	// Said once, on the first frame with a value: during the boot the camera's multiplier is still 0.
	if (!g_said && after > 0.0f) {
		g_said = true;
		NOTICE_LOG(Log::HLE, "VCS: vehicle detail %s - a car keeps its full model to %.1f m (the game's %.1f)",
			level == kVCSVehicleDetailUltra ? "ULTRA" : "HIGH", std::sqrt(after), std::sqrt(before));
	}
	return 0;
}

void VehicleDetailTick() {
	if (!IsActive() || !currentMIPS) {
		return;
	}
	const int level = WantedLevel();
	g_level = level;
	if (level == kVCSVehicleDetailPSP) {
		if (g_installed) {
			Unhook();
			NOTICE_LOG(Log::HLE, "VCS: vehicle detail back to the PSP's distances");
		}
		return;
	}

	const int index = GetReplacementFuncIndexByName("vcs_vehicle_detail");
	if (index < 0) {
		return;
	}
	const u32 replaceOp = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)index;
	// Through Read_Instruction, never a raw read: the raw word of code that has run can be a JIT
	// block marker, and comparing against it would reinstall every tick.
	const u32 now = Memory::Read_Instruction(kVCSVisSetCameraDone, false).encoding;
	if (g_installed && now == replaceOp) {
		return;
	}
	if (g_installed) {
		// A savestate keeps code but not replacement ops.
		NOTICE_LOG(Log::HLE, "VCS: vehicle detail hook changed (a savestate?) - reinstalling");
		g_installed = false;
	}
	// Never patch without first seeing the instructions that were measured. Early ticks find
	// nothing because the module is not loaded yet - "try again next tick", not a failure.
	if (now != kVCSVisSetCameraDoneOp ||
			Memory::Read_Instruction(kVCSVisSetCameraLastStore, false).encoding != kVCSVisSetCameraLastStoreOp ||
			Memory::Read_Instruction(kVCSVisSetCameraLod0Lui, false).encoding != kVCSVisSetCameraLod0LuiOp) {
		return;
	}
	currentMIPS->InvalidateICache(kVCSVisSetCameraDone, 4);
	// WriteReplaceInstructionAt's result is not the answer (it reports false for an identical
	// replacement already there too), so the word is read back instead.
	WriteReplaceInstructionAt(kVCSVisSetCameraDone, index);
	currentMIPS->InvalidateICache(kVCSVisSetCameraDone, 4);
	if (Memory::Read_Instruction(kVCSVisSetCameraDone, false).encoding != replaceOp) {
		return;
	}
	g_installed = true;
	g_said = false;
	NOTICE_LOG(Log::HLE, "VCS: vehicle detail hook installed at %08x", kVCSVisSetCameraDone);
}

void RemoveVehicleDetail() {
	Unhook();
	g_level = kVCSVehicleDetailPSP;
	g_said = false;
}

}  // namespace VCS
