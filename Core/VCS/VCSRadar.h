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

// The GPS line on the radar, drawn by us.
//
// ---------------------------------------------------------------------------------------------
// WHY NOT BLIPS
//
// The first version of this borrowed the game's own blips, one per route point, through a MIPS
// thunk into the creator at 0x0880e03c. It worked, in the sense that markers appeared and tracked
// the waypoint - but they were triangles at fixed spacing, which reads as a trail of dots rather
// than as a route, and every batch faulted six times inside the creator for reasons that never
// yielded to inspection. Breakpoints do not fire under the JIT here, so pinning it needed an
// interpreter run that would have bought a diagnosis for a feature about to be replaced.
//
// Drawing the line ourselves is strictly better on every axis that matters: it is a line rather
// than dots, its colour and thickness are ours, it costs the game's blip pool nothing, and it
// writes no PSP memory at all - which is what makes the faults go away rather than get fixed.
// That first version is gone as of this change - it was dead code with a memory fault in it, which
// is a trap for whoever reads this next. The technique it proved out survives in VCSMips.h, which
// VCSWorld still uses for the ground probe.
//
// ---------------------------------------------------------------------------------------------
// THE TRANSFORM IS THE GAME'S
//
// Read out of `0x0880edb0`, which is reVC's CRadar::TransformRealWorldPointToRadarSpace:
//
//     dx = wx - originX ;  dy = wy - originY
//     rx = ( fwdY*dx + fwdX*dy ) / range
//     ry = ( fwdY*dy - fwdX*dx ) / range
//
// with origin at gp+0x1744, the forward vector at gp+0x174c (a unit vector - measured as 1.0000),
// and the range a float in the radar object at +0x1AB8, which reads 96.0 at the default zoom. The
// radar object is the same object as the blip store, `*(0x08bb343c)`.
//
// That lands a world point in the radar's unit disc, which is the hard half and is exact.
//
// The other half - unit disc to screen - is NOT read from the game. Its own version, at
// 0x0880eb64, reaches the radar's rect through a viewport object at gp+0x16D8 by way of getter
// calls rather than plain fields, so lifting it would mean either emulating those calls or
// thunking into them every frame. The rect is a constant of the HUD layout: four numbers, settled
// once by eye and kept here. `showCalibration` draws them so they can be checked against the
// radar the game itself is drawing.
//
// ---------------------------------------------------------------------------------------------
// THREADING
//
// RadarTick runs on the emu thread and is the only part that reads PSP memory. It leaves behind
// screen-space segments under a lock; the UI thread draws those and reads nothing. That split is
// what keeps the "PSP memory on the emu thread only" rule intact through a feature whose whole
// output is drawn by the host.

namespace VCS {

struct VCSRadarSettings {
	bool drawRoute = true;

	// The radar's circle, in the PSP's 480x272 screen coordinates. Not the game's 512x320 - the
	// game works in a larger virtual screen and scales down, and the overlay is placed against
	// what is actually displayed.
	float centreX = 44.1f;
	float centreY = 228.6f;
	float radius = 33.0f;

	// Plain 0xRRGGBB; the draw converts it. Vice City's own red, which is what the map marker and
	// the mission arrows are drawn in, so the route reads as part of the HUD rather than as
	// something bolted over it.
	u32 lineColor = 0xCE3C37;

	// The objective marker gets its own colour, so the line says which of the two it is going to
	// without anyone having to open the debugger to find out. Matched to the pink the game draws
	// that marker in.
	u32 missionLineColor = 0xC7719A;
	float thickness = 1.0f;

	// Leave the player's blip a hole in the line rather than painting across it.
	//
	// Everything we draw lands on the finished frame, so the line covers the marker the game draws
	// at the radar's centre - which is exactly where the route always begins, that being where the
	// player is. The first fix for that redrew the marker on top out of the texture pack, and it
	// worked for as long as the pack was PNGs: the pack is BC7 DDS now and the UI's loader reads
	// PNG, JPEG and ZIM, so the load quietly failed and the line went straight back over the arrow.
	// Cutting the centre out of the line instead needs no art at all, and what shows through is the
	// game's own marker - right size, right rotation, and already replaced by whatever the pack
	// replaced it with.
	//
	// In the same 480x272 screen units as the radius above, and a radius. Measured off the marker
	// on screen rather than guessed: it draws about 6 by 7 of those units, so 3.5 is its own half
	// extent and the line ends exactly where the marker starts covering it. The first value here
	// was 5.5 - half the icon size the old redraw used - and that left two units of daylight
	// between the line and the arrow, which reads as the line stopping short rather than as the
	// line going under. Zero draws it straight through.
	float playerHoleRadius = 3.5f;

	// Draw the assumed radar circle and its centre, for lining the four numbers above up with the
	// radar the game draws. Off in normal play.
	bool showCalibration = false;
};

VCSRadarSettings &RadarSettings();

// One drawable piece of the route, in PSP screen coordinates, already clipped to the radar. A
// route crossing the radar's edge comes back as several of these rather than one polyline, which
// is also how a segment that leaves and re-enters stays broken instead of cutting the corner.
struct RadarSegment {
	float x1 = 0.0f, y1 = 0.0f;
	float x2 = 0.0f, y2 = 0.0f;
};

// Emu thread. Recomputes the route occasionally and re-projects it every frame.
void RadarTick();

// Render thread. Copies out this frame's segments; empty when there is nothing to draw.
void GetRouteSegments(std::vector<RadarSegment> *out);

// Render thread. Whether this frame's line is going to the objective marker rather than to a
// dropped waypoint - which colour to draw it in.
bool RouteIsMission();

// Whether the game is drawing its HUD - and so its radar - at all. False during cutscenes, which
// is the one state where everything else this reads stays valid while the radar is gone.
bool RadarOnScreen();

// The line is drawn while DRIVING a road vehicle and at no other time - not on foot, not in a
// boat, not in the air. See the gate in RadarTick: the route is a path through the road graph, so
// anywhere those roads are not the way to travel it is not merely useless but wrong about the
// route. Cars, bikes and unrecognised models count as driving.

// For the debugger: what the transform last read, and why nothing is drawing if nothing is.
const char *RadarStatus();
bool ReadRadarFrame(float *originX, float *originY, float *fwdX, float *fwdY, float *range);

// Drop the cached route. Shutdown, and whenever the graph goes away.
void RadarReset();

}  // namespace VCS
