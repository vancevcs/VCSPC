// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Core/VCS/VCSRoute.h"

// Route markers on the radar, made by asking the game for real blips.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS CALLS THE GAME INSTEAD OF WRITING THE STORE
//
// The blip store's layout is known - entries of 0x30 bytes at store+0x270, position at +0x10, type
// at +0x20 - so the obvious thing is to fill a spare entry in and be done. That was tried and it
// does not work: a hand-written entry never appears, and neither does one with the store's active
// count bumped to match. The store tracks its own slots, and a blip is not a struct you own, it is
// a handle the manager issued.
//
// So this calls the game's own creator, which is what `00C3 add_blip_for_coord` does:
//
//     handle = 0x0880e03c(store, 4, &xyz, 5, 3)      ; the 5 is in $a3, the 3 in $t0
//     0x0880e374(store, handle, 3)                   ; make it show
//
// That takes a fifth integer argument, which `hleEnqueueCall` cannot express - it sets $a0..$a3
// and stops. Hence a program in PSP memory, the same answer VCSWorld reached for the same reason,
// and the encoders both use now live in VCSMips.h.
//
// The payoff for going through the game rather than around it is that these are ordinary blips:
// the radar draws them, the pause map draws them, and they survive whatever the game does to its
// own display without us having to know the radar's rect, rotation or zoom.
//
// ---------------------------------------------------------------------------------------------
// WHAT IT DRAWS
//
// A route is thousands of units long and the radar shows a few hundred, so drawing every node
// would spend the blip pool on markers nobody can see. Only the part of the route ahead of the
// player is marked, thinned to a fixed budget - which is also what makes the trail read as a
// direction rather than as a line of dots.
//
// Threading: emu thread only.

namespace VCS {

struct VCSBlipSettings {
	// Off by default until the route markers have been seen to appear and, more importantly, to
	// be cleaned up. Blips the game issued and we then forgot about would sit on the radar for the
	// rest of the session.
	bool showRoute = false;

	// How many markers to keep on the route at once, and how far apart along it.
	//
	// Eight rather than twelve because the store has far less room than it looks: most of its
	// slots are the game's own safe houses and mission markers, and asking for more than are free
	// means a create per marker that returns -1. Those are handled now, but they are still work
	// the game does for nothing.
	int maxMarkers = 8;
	float spacing = 25.0f;
};

VCSBlipSettings &BlipSettings();

// Put the program in PSP memory. Idempotent and cheap once done; safe to call before the game
// module exists, in which case it does nothing and can be retried - the same lifecycle as
// VCSWorld's install, and for the same reason.
void InstallBlipMaker();

bool BlipMakerInstalled();

// Mark this route on the radar, replacing whatever was marked before. An empty route clears.
void ShowRoute(const std::vector<RoutePoint> &route);

// Remove every marker this made. Called on shutdown, and whenever the feature is turned off.
void ClearRouteBlips();

// Per-frame. Retries the install, issues at most one creation call per tick, and reaps handles.
void BlipTick();

// How many of the store's 75 slots are free. The game's own safe houses, pickups and mission
// markers occupy most of them, so this is usually a single-figure number - which is the real limit
// on how long a marker trail can be.
int FreeBlipSlots();

// How many route markers are currently placed, and one short phrase about what the maker is
// doing - for the debugger.
int RouteBlipCount();
const char *BlipStatus();

}  // namespace VCS
