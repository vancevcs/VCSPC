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

// The player-facing settings of the VCS front end, and the only thing that persists them.
//
// This is a port of the one idea in reVC's menu worth copying wholesale: re3's CFO ("Custom
// Frontend Options"), where a menu row is described by the variable it edits plus the ini key it
// saves to. Adding an option is one row in one table - no renderer change, no switch statement
// arm, no new save code.
//
// The values themselves are NOT here. VCSCameraSettings and VCSFireHookSettings own them, and go
// on owning them; this table owns their names, ranges and persistence. The split is deliberate.
// Those structs hold the research instruments as well as the player settings, each documented at
// the point it is used, and the ImGui debugger goes on binding to them directly. What appears
// below is only the subset a player would ever set - if a knob needs a paragraph of measurements
// to interpret, it belongs in the debugger window, not in the menu.
//
// Nothing in here reads or writes PSP memory, so it is safe to touch from the UI thread, unlike
// most of Core/VCS.

namespace VCS {

enum class OptionType {
	Bool,
	Float,
	Int,     // a number within a range, moved a step at a time
	Choice,  // an int index into a list of labels
};

// Which page an option appears on. A page is built by filtering this table, so the order of the
// rows below is the order on screen.
enum class OptionPage {
	Mouse,
	Controller,
	Aiming,
	Audio,
	Graphics,
	// The things the fork ADDS to the game, as opposed to the ways it lets you drive it. Every
	// row here turns one of this port's own inventions off and hands that behaviour back to the
	// PSP game, which is the honest test for what belongs on the page.
	Gameplay,
};

struct Option {
	OptionPage page;
	OptionType type;

	// Stable id, and the key under [Settings] in vcs.ini. Renaming one silently resets that
	// setting for everyone who already has a file, so don't.
	const char *iniKey;

	const char *label;  // the row itself
	const char *help;   // one line, shown under the list while the row is selected

	// Exactly one of these is set, according to type.
	bool *boolValue;
	float *floatValue;
	int *intValue;

	// Choice only: the labels, one per index.
	const char *const *choices;
	int numChoices;

	// Set for settings that belong to PPSSPP rather than to us - graphics and audio live in
	// g_Config, which has its own file and its own save. Persisting them here as well would give
	// one setting two homes and let them disagree, so Load/Save skip these entirely.
	bool external;

	// Some settings need something to happen when they change, not just a new value - changing
	// the internal resolution has to tell the GPU to resize. Null for everything else.
	void (*onChange)();

	// A row that is only live while some other setting is on, greyed out and inert when it is
	// not. Null for the ones that always apply.
	//
	// Master volume is why this exists, and it earned it: PPSSPP's sound has a master switch
	// above the volume, so a muted emulator leaves a volume row that moves, reads back the new
	// value, and changes nothing you can hear. A row that lies about having done something is
	// worse than one that is visibly unavailable. PPSSPP's own settings screen greys the same
	// row against the same flag.
	bool *enabledBy;

	// How the value reads, for the one case a label or a format string cannot say it. Resolution
	// is that case: "2x" is the setting and "(960x544)" is what it produces, and the second half
	// has to be computed rather than written down, because Auto's depends on the window. Null
	// everywhere else, and the rules below apply instead.
	std::string (*valueText)(const Option &opt);

	// Float only, and deliberately the same ranges the debugger window uses - two UIs onto one
	// variable disagreeing about its range is a bug waiting to be reported as "the slider does
	// nothing past halfway".
	float minValue;
	float maxValue;

	// Int only. Separate from the float pair rather than shared with it, because the whole point
	// of the type is that the value is an integer the rest of the emulator already owns - volume
	// is 0..100 in g_Config, and rounding it through a float is how it stops round-tripping.
	// stepInt is what one press moves; it is also the granularity the block strip snaps to.
	int minInt;
	int maxInt;
	int stepInt;

	// How the value reads on screen. For a Float, null means "show a 0-100 position within the
	// range", which is what you want for something like 0.00128 radians per mouse count: a number
	// that means a great deal to the aim solver and nothing at all to a player. Set it for the
	// few values that are better read raw, e.g. "%.1fx". An Int reads out as itself either way -
	// null there only means the row draws blocks instead of a number.
	const char *format;

	// Captured from the live variable the first time the table is built - which is before any
	// ini is read - so "restore defaults" restores what the code shipped with, not what happened
	// to be saved last.
	bool defaultBool;
	float defaultFloat;
	int defaultInt;
};

// Every exposed option, in menu order. Built on first call.
//
// That first call has to happen before the ini is read, or the captured defaults would be
// whatever was loaded rather than the compiled-in values. LoadSettings() guarantees it by
// calling this itself, first thing.
const std::vector<Option> &Options();

// vcs.ini in the system directory, next to imdebugger.ini.
//
// Deliberately not ppsspp.ini. Keeping these in our own file means Core/Config.cpp - a file
// upstream edits constantly - stays untouched, which is the difference between a clean rebase
// and a merge conflict on every pull.
void LoadSettings();
void SaveSettings();

// Put one page back to the values the code shipped with.
void ResetPage(OptionPage page);

// The VCS disc the front end boots, remembered so the main menu can start the game without
// sending the player through a file browser. Empty until a VCS disc has actually been booted
// once - there is no way to know which ISO is the right one before then, and guessing at the
// recent-games list would mean opening every entry to read its disc ID.
//
// Lives in the same vcs.ini, but not in the option table: it is a path, not a setting, and
// nothing in the menu edits it.
std::string GamePath();
void SetGamePath(std::string_view path);

// Position of a Float or Int option within its range, 0..1. The menu edits through these rather
// than through the variable, so that one arrow press means the same fraction of the range
// everywhere, and so the block strip has one thing to draw for both types.
float GetNormalized(const Option &opt);
void SetNormalized(const Option &opt, float t);

// The write path for Int and Choice: clamps to the option's own range and fires onChange, but
// only when the value really moved. Anything editing one of those goes through here rather than
// assigning through intValue, which is how a setting that needs to tell the GPU about itself
// stays correct no matter which control moved it.
void SetInt(const Option &opt, int value);

// The same write path for Bool, and it exists for the same reason. A toggle that flips
// boolValue directly changes what the menu reads back without telling anything else, which for
// a setting the GPU has to hear about -- texture quality is the one -- means the label moves to
// LOW while the frame carries on being drawn with the HD pack.
void SetBool(const Option &opt, bool value);

// What the right-hand column shows: "ON", "OFF", "63", or a formatted value.
std::string ValueText(const Option &opt);

}  // namespace VCS
