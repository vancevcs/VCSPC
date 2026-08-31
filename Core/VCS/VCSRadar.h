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

	// Redraw the game's own player marker over the top of the line.
	//
	// Everything we draw lands on the finished frame, so the line covers the arrow at the radar's
	// centre - which is exactly where the route always passes. Drawing the same texture the game
	// uses, in the same place, puts it back on top. It is the game's own art, out of the texture
	// pack, so it matches whatever the pack replaced it with.
	bool drawPlayerIcon = true;
	float iconSize = 11.0f;

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

// Render thread. Which way to point the player's arrow, in radians clockwise from straight up.
//
// It is NOT always zero, which was the first guess and was wrong. The radar turns with the CAMERA,
// so on a mouse-look build the map swings when the mouse moves while the car keeps going the way
// it was going. The arrow has to show the car's heading relative to the camera's, or it ends up
// tracking the mouse instead of the driving. Returns false when the facing could not be read.
bool GetPlayerFacing(float *angle);

// Render thread. Whether this frame's line is going to the objective marker rather than to a
// dropped waypoint - which colour to draw it in.
bool RouteIsMission();

// Whether the game is drawing its HUD - and so its radar - at all. False during cutscenes, which
// is the one state where everything else this reads stays valid while the radar is gone.
bool RadarOnScreen();

// For the debugger: what the transform last read, and why nothing is drawing if nothing is.
const char *RadarStatus();
bool ReadRadarFrame(float *originX, float *originY, float *fwdX, float *fwdY, float *range);

// Drop the cached route. Shutdown, and whenever the graph goes away.
void RadarReset();

}  // namespace VCS
