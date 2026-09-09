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

// How far away the game keeps its map objects.
//
// Every map object's draw distance is `info[0x28 + info[0x38] * 4]` multiplied by ONE global float
// at `CCamera + 0x7a8`, which reads exactly 1.0 in ordinary play. That multiply is the last thing
// `0x08aae0ec` does before returning the distance, and the call stack above it is the world-add
// path - so this is the number that decides, at the moment an entity is brought in, how far away it
// is still worth existing at. Scale it and the whole map stretches at once: no table walk, no
// per-model bookkeeping, nothing to keep in step with the streamer.
//
// It cannot be written from outside, because the game rebuilds it every frame - assigned at
// 0x08a240a0, clamped down at 0x08a24104, copied to +0x7a0 for the haze, multiplied again at
// 0x08a2413c. Hammered from the debugger it reads back 1.0 every time. So the scale is applied by a
// hook on the READER instead, which runs two instructions before the load and therefore cannot be
// raced by the frame that is rebuilding it.
//
// The haze is deliberately left alone: it is copied from this value BEFORE the final multiply, so
// the horizon fog stays where the artists put it while the geometry reaches past it.
namespace VCS {

// Applies, or takes back, whatever `VCSGameSettings::drawDistance` currently says. Called once a
// tick; installing the hook is a one-off and the ordinary cost after that is nothing at all.
void DrawDistanceTick();

// Puts the game's own instruction back. Must run before the module goes away, or a savestate taken
// afterwards carries a hook with no code left to explain it.
void RemoveDrawDistanceHook();

// For the debugger: whether the hook is in, what the game last computed, and what it is being
// scaled to. A stock value that is not 1.0 is worth seeing rather than assuming.
bool DrawDistanceInstalled();
float DrawDistanceStock();
float DrawDistanceApplied();

// The hook itself. Registered in Core/HLE/ReplaceTables.cpp as "vcs_lod_distance" and installed by
// address; it writes the scaled value into CCamera + 0x7a8 and returns, so the game's own function
// runs unchanged and reads what we left there.
int Hook_vcs_lod_distance();

}  // namespace VCS
