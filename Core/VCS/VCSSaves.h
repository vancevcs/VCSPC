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

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// What is on the memory stick, so this fork's own menu can show a save list instead of walking
// the player into the firmware's one.
//
// ---------------------------------------------------------------------------------------------
// WHY READ THEM OURSELVES
//
// The game's LOAD GAME ends at the PSP savedata dialog: a list drawn by the firmware, in the
// firmware's font, on a black background, with the PSP's own button glyphs. It works - the bridge
// drives it and the auto-load takes the entry it offers - but it is the one screen in this port
// that still looks like a handheld, and it is the screen a player uses most often after the map.
//
// Everything it displays is in the save's own PARAM.SFO, which is a documented format PPSSPP
// already parses. So the list can be OURS, in the menu's own face, with the game's own words in
// it: `SAVEDATA_TITLE` is the last mission passed and `SAVEDATA_DETAIL` is the district, the safe
// house, the day, the money and the percentage, written by the game when it saved.
//
// Reading is all this does. Loading still goes through the game - see FrontEndTarget::Load - for
// the same reason the map does: the emulator can enumerate a directory, but only the game can put
// a world back together.
//
// ---------------------------------------------------------------------------------------------
// THE SLOTS ARE FIXED, AND THAT IS THE GAME'S DOING
//
// VCS ships an eight-entry name list, `S92F0` to `S92F7`, at 0x08ba30d8 in the EBOOT. There is no
// ninth slot to grow into and no "new save" entry to select - a save always overwrites one of the
// eight - which is why an index here is the game's own slot number and not a position in a list
// that happens to be sorted this way today. `EnumerateSaves` always returns exactly eight, empty
// ones included, so the menu can show a slot that has nothing in it rather than a shorter list.

namespace VCS {

// How many the game offers. Not a guess: it is the length of the name list in the EBOOT.
inline constexpr int kVCSSaveSlots = 8;

struct SaveSlot {
	int index = 0;            // 0..7, the game's own slot order and the dialog's list order
	bool present = false;     // false for a slot with nothing in it

	// Straight out of PARAM.SFO, as the game wrote them.
	std::string title;        // "Brawn of the Dead" - the last mission passed
	std::string detail;       // "Beach.\nVice Point safehouse.\nDay 20, $29403.\n14.4% complete."

	// Flattened onto one line, which is what a menu row's help line can show: the same words with
	// the newlines turned into separators, and the date on the end.
	std::string summary;

	std::string directory;    // ULUS10160S92F3
	int64_t modified = 0;     // seconds, for "the most recent save"

	// Written by this port's auto-save rather than by a player walking into a save pickup.
	//
	// NOT derivable from anything on the memory stick, which is why it is recorded rather than
	// read: the auto-save opens the game's own save menu and lets the game write the file, so the
	// PARAM.SFO it produces is identical in every field to one the player asked for. Nothing
	// distinguishes them but the knowledge of who started it, and only this process has that.
	bool autoSave = false;
};

// All eight, in slot order, whether or not they hold anything. Reads the filesystem, so this is
// for a menu being built rather than for anything per-frame.
std::vector<SaveSlot> EnumerateSaves();

// Record that the save now in this slot is one the auto-save wrote. Called once, when the
// sequence ends having seen the firmware say the file was written - see GoIdle in VCSFrontEnd.cpp
// for why that is the moment rather than when the request was made.
void NoteAutoSave(int index);

// Which slot holds the newest save, or -1 when the memory stick is empty. The same answer the
// firmware's own list arrives on, worked out from the same timestamps.
int NewestSaveSlot();

// Delete one, directory and all.
//
// Done here rather than through the game's DELETE SAVE DATA, which is a third walk into a third
// firmware dialog to remove a directory this process can remove itself. The game re-enumerates
// the memory stick every time it opens a list, so it never sees a stale one.
bool DeleteSave(int index);

}  // namespace VCS
