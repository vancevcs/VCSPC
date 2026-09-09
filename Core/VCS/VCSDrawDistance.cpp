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

#include "Core/VCS/VCSDrawDistance.h"

#include <cmath>
#include <cstring>

#include "Common/Log.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSGame.h"

namespace VCS {

static bool s_hookInstalled;

// What the game last computed for itself, and what we last left in its place. Keeping both is what
// stops the scale compounding: the hook runs many times a frame, and multiplying the live value
// each time would take the draw distance to infinity in about a second.
static float s_stock = 1.0f;
static float s_written = -1.0f;

namespace {

float ReadScale() {
	if (!Memory::IsValid4AlignedAddress(kVCSCamLodScale)) {
		return 0.0f;
	}
	float value;
	memcpy(&value, Memory::GetPointerUnchecked(kVCSCamLodScale), sizeof(value));
	return value;
}

void WriteScale(float value) {
	if (!Memory::IsValid4AlignedAddress(kVCSCamLodScale)) {
		return;
	}
	memcpy(Memory::GetPointerWriteUnchecked(kVCSCamLodScale), &value, sizeof(value));
}

float WantedFactor() {
	float factor = GameSettings().drawDistance;
	if (!(factor > 0.0f)) factor = 1.0f;
	if (factor < 1.0f) factor = 1.0f;
	if (factor > kVCSDrawDistanceMax) factor = kVCSDrawDistanceMax;
	return factor;
}

}  // namespace

int Hook_vcs_lod_distance() {
	const float factor = WantedFactor();
	const float live = ReadScale();
	if (live <= 0.0f) {
		return 0;
	}

	// A value that is not the one we left is the game's own, freshly rebuilt for this frame - so
	// that is the number to scale. Anything else and we would be scaling our own scaling.
	if (live != s_written) {
		s_stock = live;
	}

	const float want = s_stock * factor;
	if (live != want) {
		WriteScale(want);
		s_written = want;
	}

	static bool s_said = false;
	if (!s_said && factor > 1.0f) {
		s_said = true;
		INFO_LOG(Log::HLE, "VCS: draw distance x%.1f - the game asked for %.3f, reading %.3f",
			factor, s_stock, want);
	}
	return 0;
}

void DrawDistanceTick() {
	if (s_hookInstalled) {
		return;
	}
	// Nothing to install until somebody asks for more than the game's own distance. This keeps the
	// module completely out of the game's code at the default setting, which is the same rule the
	// rest of this fork's patches follow.
	if (GameSettings().drawDistance <= 1.0f) {
		return;
	}

	const int index = GetReplacementFuncIndexByName("vcs_lod_distance");
	if (index < 0) {
		return;
	}

	// Never patch an address without first seeing the instruction that was measured there. Early
	// ticks legitimately find nothing, because the module has not been loaded yet - so this is a
	// "try again next tick" rather than a failure.
	if (Memory::Read_Instruction(kVCSLodDistanceFn, true).encoding != kVCSLodDistanceFnOp) {
		return;
	}

	if (WriteReplaceInstructionAt(kVCSLodDistanceFn, index)) {
		// A function entry is a block start, so the compiled block has to go or it would carry on
		// running the original instruction out of its own copy.
		currentMIPS->InvalidateICache(kVCSLodDistanceFn, 4);
		s_hookInstalled = true;
		INFO_LOG(Log::HLE, "VCS: draw distance hook installed at %08x", kVCSLodDistanceFn);
	}
}

void RemoveDrawDistanceHook() {
	if (s_hookInstalled) {
		// Invalidate BEFORE restoring: the restorer reads the RAW word and silently declines
		// unless it finds a replacement marker, and at a block start the raw word is the block
		// cache's marker rather than ours.
		currentMIPS->InvalidateICache(kVCSLodDistanceFn, 4);
		RestoreReplacedInstruction(kVCSLodDistanceFn);
		currentMIPS->InvalidateICache(kVCSLodDistanceFn, 4);
		s_hookInstalled = false;
	}
	// And hand the game's own number back, or the last scaled value would sit there until the next
	// frame rebuilt it - which for a savestate taken right now is forever.
	if (s_written > 0.0f && ReadScale() == s_written) {
		WriteScale(s_stock);
	}
	s_stock = 1.0f;
	s_written = -1.0f;
}

bool DrawDistanceInstalled() {
	return s_hookInstalled;
}

float DrawDistanceStock() {
	return s_stock;
}

float DrawDistanceApplied() {
	return s_hookInstalled ? s_stock * WantedFactor() : s_stock;
}

}  // namespace VCS
