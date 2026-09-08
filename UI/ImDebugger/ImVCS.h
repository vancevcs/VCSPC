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

#include "Common/CommonTypes.h"

struct ImConfig;

// The VCS debugger window. Lives in UI/ rather than Core/VCS/ so that Core keeps no dependency
// on Dear ImGui.
//
// This is the tool for finding the addresses that Core/VCS/VCSAddresses.h is missing: it shows
// the live decoded state, the address table itself, and a scratchpad where you can watch a
// handful of candidate addresses interpreted every which way, refreshed every frame.
//
// Safe to run on the ImGui thread because PPSSPP guarantees the ImGui debugger runs on the same
// thread as CPU emulation - see the "Debugger threading model" section of AGENTS.md.

class ImVCSWindow {
public:
	void Draw(ImConfig &cfg);

private:
	void DrawStatus();
	void DrawDecodedState();
	void DrawAddressTable();
	void DrawScratchpad();
	void DrawInputTester();
	void DrawCamera();
	void DrawVault();
	void DrawShadows();
	void DrawFrontEnd();
	void DrawRoute();

	// Scratchpad rows. Fixed size on purpose - this is a scratchpad, not a watch list, and
	// keeping it a plain array means no allocation while the emulator is running.
	static constexpr int kScratchRows = 8;

	struct ScratchRow {
		char addressText[16] = "";
		bool freeze = false;   // Stop refreshing so a value can be read while it changes fast.
		u32 frozenRaw = 0;
		bool hasFrozen = false;
	};

	ScratchRow scratch_[kScratchRows];

	// Simulated host keys for testing the mapping table before the real input path is wired up
	// in task 2. Index into VCS::kVCSKeyMappings.
	int simulatedKeyIndex_ = -1;
};
