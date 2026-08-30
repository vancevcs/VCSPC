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

#include <cstddef>

#include "Common/CommonTypes.h"

// The cheat menu: VCS's own button-combo cheats, typed by us instead of by the player.
//
// VCS has no cheat entry screen. Every cheat is an eight-press combination entered on the pad
// during ordinary play - Up, Down, Left, Right, Circle, Circle, L, R for full health - which is
// unpleasant on a keyboard and impossible to remember. The menu turns each one into a row.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS TYPES THE COMBINATION RATHER THAN CALLING THE GAME
//
// The other route is to find each cheat's handler and call it through VCSWorld's thunk, and it is
// tempting: 36 addresses, and no timing to get right. It is the wrong trade here, for three
// reasons that all point the same way:
//
//   - A cheat is not just its effect. Entering one flags the save, prints the game's own
//     confirmation, and in several cases toggles a flag that other systems read. Typing the
//     combination gets all of that by construction; calling a handler gets whatever that one
//     function happens to do and silently skips the rest.
//   - The sequences are data we already have and the addresses are 36 hunts we have not done.
//   - A wrong sequence costs one row. A wrong address costs a crash.
//
// So this layer knows nothing about what any cheat DOES. It presses buttons.
//
// ---------------------------------------------------------------------------------------------
// TIMING, AND WHY IT COUNTS GAME FRAMES RATHER THAN VBLANKS
//
// The game samples the pad once per logic frame, at 30Hz, while VCS::Tick runs at vblank - 60Hz,
// and neither rate is guaranteed during streaming or a loading screen. A press held for a fixed
// number of vblanks is therefore a press held for an unpredictable number of *samples*, and a
// sample missed in the middle of an eight-press combination fails the whole thing with nothing on
// screen to say why.
//
// So the sequencer steps on `FrameCounter` - the same clock VCSCamera's handback walks on, and for
// the same reason. Each press is held for a couple of game frames and released for a couple more,
// which is the shortest thing the game can reliably see as a press followed by a release. That
// matters for the repeats: "Circle, Circle" is two presses, and without the gap between them the
// game sees one long one.
//
// A whole combination takes about a second. That is not a problem to optimise away - GTA's cheat
// matchers keep a rolling history of presses rather than a timed window, so there is no deadline
// to beat.
//
// ---------------------------------------------------------------------------------------------
// OWNERSHIP
//
// While a combination is being typed the player's own controls are suppressed, exactly as they
// are during a vault and for the same reason: a held W is a Cross, and a Cross arriving in the
// middle of the sequence is a ninth press the game did not expect. The suppression lives in
// `ApplyMapping`, which stays the only thing in this fork that pushes buttons - this file decides
// WHAT is pressed and never presses it. See `CheatButtonMask`.
//
// Threading: `RequestCheat` is called from the UI thread when a menu row is clicked, so the queue
// is mutex-guarded. Everything else is emu thread only.

namespace VCS {

// Which page of the cheat menu a cheat appears on. A page is built by filtering the table, so the
// order of the rows in the table is the order on screen.
enum class CheatGroup {
	Player,
	Vehicles,
	Pedestrians,
	World,
};

// The longest combination the sequencer can type. Every VCS cheat is exactly eight presses; the
// slack is there so that adding one that is not does not need a second edit.
inline constexpr int kMaxCheatPresses = 12;

struct VCSCheat {
	CheatGroup group;

	const char *name;  // the menu row
	const char *help;  // one line, shown under the list while the row is selected

	// The combination, as CTRL_* masks, one per press, zero-terminated. A cheat that filled the
	// array would have no terminator, which is why `CheatLength` stops at the array bound too.
	u32 press[kMaxCheatPresses];

	// Cannot be turned off by entering it again - the only way back is a save from before it.
	// Purely so the menu can say so; nothing here treats a sticky cheat differently.
	bool sticky;
};

// Tunables. In this header rather than in the menu's option table because the numbers describe
// the game's pad sampling, not a preference: they are the kind of knob that needs a paragraph of
// measurement to interpret, which by the rule in CLAUDE.md means the debugger window, not a
// player-facing row.
struct VCSCheatSettings {
	// Game frames each press is held, and each gap between presses lasts. Two is the shortest
	// that survived testing; one relies on the game sampling the pad on every logic frame, which
	// it does not obviously promise.
	int holdFrames = 2;
	int gapFrames = 2;

	// Nothing pressed, before the first press and after the last. The lead-in exists because the
	// player may be holding a direction when the menu closes, and the game has to see that
	// released before our first press or the two merge into one.
	int leadInFrames = 3;
	int tailFrames = 2;
};

VCSCheatSettings &CheatSettings();

// The table, in menu order.
const VCSCheat *Cheats();
size_t CheatCount();

// How many presses a cheat actually has.
int CheatLength(const VCSCheat &cheat);

// Queue a cheat to be typed, by index into the table. Safe to call from the UI thread, which is
// where it is called from - a menu row's click handler.
//
// Queued rather than started, because the menu pauses the emulator: nothing can be typed until
// the player is back in the game, and the row that starts it is the last thing they touch before
// that happens. Returns false only when the index is out of range or the queue is full.
bool RequestCheat(int index);

// Whether a combination is being typed right now, including the silent lead-in and tail. While
// this is true the player's own bindings are suppressed - see the note above.
bool CheatEntryInProgress();

// What to press this frame, as a mask of CTRL_* bits. Zero during the gaps, and zero when nothing
// is being typed. `ApplyMapping` applies it; this file never touches sceCtrl.
u32 CheatButtonMask();

// Advance the sequencer by one tick. Emu thread, once per vblank, and it MUST run before
// `ApplyMapping` - that is what lets the mapping stand down on the same tick the first press goes
// out, rather than one tick late with the player's own buttons still asserted.
void CheatTick();

// Abandon anything in flight and empty the queue. Called on init and shutdown; a combination half
// typed into a game that is going away is not worth finishing.
void CheatReset();

// What the sequencer is doing, in one short phrase, for the debugger window. "idle" when there is
// nothing to say.
const char *CheatStatus();

// The cheat currently being typed, or -1. For the debugger.
int ActiveCheat();

}  // namespace VCS
