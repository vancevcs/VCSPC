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

// What the PSP's savedata dialog is showing, so this fork can drive it closed-loop.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS EXISTS
//
// The auto-save and the auto-load both end up at a screen the GAME does not own. VCS asks for the
// firmware's savedata utility - the slot list, the "overwrite?" prompt, the progress bar - and
// PPSSPP draws all of it. So the doctrine the rest of this fork follows, drive the mechanic and
// watch the state, runs out of state to watch: `FrontEndMenuManager` freezes at whatever page it
// was on and nothing in PSP memory says which of the dialog's screens is up or which slot is
// highlighted.
//
// The alternative was pressing Cross on a timer and hoping, which is exactly the class of thing
// that works on the machine it was written on. This reports what is on screen instead. It is
// READ-ONLY and changes nothing about how the dialog behaves - the fork still presses the same
// buttons a player would, it just knows what it is looking at.
//
// ---------------------------------------------------------------------------------------------
// WHAT THE DIALOG DOES, WHICH IS WHY THESE FIELDS AND NOT OTHERS
//
// Save:  list -> (slot has data) overwrite prompt -> writing -> done
// Load:  list -> loading -> the world restarts under you
//
// The prompt starts on NO (`yesnoChoice` is set to 0 on entry) and LEFT is what moves it to YES,
// which is worth knowing before pressing anything: a Cross sent at the prompt without that press
// cancels the save it was meant to confirm.
//
// `busy` covers both the IO and the fades. Pressing during either is how a sequence ends up one
// press ahead of the screen and answering a question that has not been asked yet.

namespace VCS {

struct SaveDialogPeek {
	bool active = false;    // a savedata dialog is up at all
	bool save = false;      // saving, as opposed to loading or deleting
	bool list = false;      // the slot list has focus
	bool confirm = false;   // a yes/no prompt is up - overwrite, or "do you want to save"
	bool busy = false;      // reading, writing, or fading; press nothing
	bool done = false;      // finished, waiting to be acknowledged
	int selected = -1;      // index into the slot list, which is the game's own slot order
	int count = 0;          // how many slots the list has
	int yesno = 0;          // 1 while YES is highlighted on a prompt, 0 for NO

	// The PSP buttons this dialog treats as OK and as Back, as CTRL_ masks.
	//
	// Asked rather than assumed, because which is which is a SETTING - the firmware's O/X
	// preference, which PPSSPP follows - and because the two screens want different ones anyway:
	// "Save completed" offers Back alone (DS_SAVE_DONE accepts only the cancel button), so a
	// sequence that pressed OK at it would sit there until it timed out with the save already
	// written and the dialog still on screen.
	u32 okButton = 0;
	u32 cancelButton = 0;
};

// False when no savedata dialog is up, in which case `out` is left in its default state.
bool PeekSaveDialog(SaveDialogPeek *out);

// Whether THIS boot's silent autoload should find nothing, asked once and answered once.
//
// **VCS loads a save by itself at boot.** Not through its menu and not through this fork: the
// game asks the firmware for a silent AUTOLOAD of a save it names, and the world that comes up is
// that save's. It is why a first run - an empty memory stick - starts the story and a later one
// does not, and it is what defeated the first two attempts at a NEW GAME row: whatever the menu
// did, the next boot quietly continued from a save again.
//
// So a new game is "boot the disc, and let that one autoload find nothing", which is exactly the
// state a first run is in. The dialog asks this on its way into an autoload; RequestNewGame is
// what makes it true, once.
bool TakeNewGameBoot();

}  // namespace VCS
