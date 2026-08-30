// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include <cmath>
#include <mutex>
#include <vector>

#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSRadar.h"
#include "Core/VCS/VCSRoute.h"

namespace VCS {

namespace {

VCSRadarSettings g_settings;

std::mutex g_lock;
std::vector<RadarSegment> g_segments;   // guarded by g_lock
float g_facing = 0.0f;                  // guarded by g_lock
bool g_haveFacing = false;              // guarded by g_lock

// The route, in world units. Held between recomputes because A* over three thousand nodes is not
// a per-frame cost, and the projection that IS per-frame is a dozen multiplies a point.
std::vector<RoutePoint> g_route;
int g_recompute = 0;

const char *g_status = "idle";

// How often to ask for the route again. Four times a second is far quicker than a driver can
// invalidate one - a wrong turn shows up within a couple of car lengths - and it keeps the search
// off the frame that has to draw.
constexpr int kRecomputeTicks = 15;

// Clip one segment of the route to the radar's unit disc.
//
// Segments rather than a polyline, and a real clip rather than dropping whole segments whose ends
// are outside: at a range of 96 world units the road ahead is often a single segment with both
// ends past the edge and the middle straight across the radar. Dropping those would blank the
// line exactly when it matters most.
bool ClipToDisc(float x0, float y0, float x1, float y1,
                float *ox0, float *oy0, float *ox1, float *oy1) {
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float a = dx * dx + dy * dy;
	const float c = x0 * x0 + y0 * y0 - 1.0f;
	if (a < 1e-9f) {
		// A degenerate segment is a point; it draws nothing either way.
		return false;
	}
	const float b = 2.0f * (x0 * dx + y0 * dy);
	const float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f) {
		return false;   // the whole line misses the radar
	}
	const float sq = std::sqrt(disc);
	const float tA = (-b - sq) / (2.0f * a);
	const float tB = (-b + sq) / (2.0f * a);
	const float lo = std::max(0.0f, tA);
	const float hi = std::min(1.0f, tB);
	if (lo >= hi) {
		return false;
	}
	*ox0 = x0 + lo * dx;
	*oy0 = y0 + lo * dy;
	*ox1 = x0 + hi * dx;
	*oy1 = y0 + hi * dy;
	return true;
}

}  // namespace

VCSRadarSettings &RadarSettings() {
	return g_settings;
}

bool ReadRadarFrame(float *originX, float *originY, float *fwdX, float *fwdY, float *range) {
	const std::optional<u32> origin = ResolveAddr(VCSAddr::RadarOrigin);
	const std::optional<u32> fwd = ResolveAddr(VCSAddr::RadarForward);
	const std::optional<u32> radarObj = ReadAddrU32(VCSAddr::BlipManager);
	if (!origin || !fwd || !radarObj || *radarObj == 0) {
		return false;
	}
	const std::optional<float> ox = ReadFloat(*origin);
	const std::optional<float> oy = ReadFloat(*origin + 4);
	const std::optional<float> fx = ReadFloat(*fwd);
	const std::optional<float> fy = ReadFloat(*fwd + 4);
	const std::optional<float> r = ReadFloat(*radarObj + kVCSRadarRange);
	if (!ox || !oy || !fx || !fy || !r) {
		return false;
	}
	// The forward vector really is normalised - it measured 1.0000 - so a length far off one means
	// the read landed somewhere else, which is worth refusing rather than drawing a line at a
	// wrong scale and calling it a bug in the routing.
	const float len = std::sqrt(*fx * *fx + *fy * *fy);
	if (len < 0.9f || len > 1.1f || *r < 1.0f || *r > 10000.0f) {
		return false;
	}
	*originX = *ox; *originY = *oy;
	*fwdX = *fx;    *fwdY = *fy;
	*range = *r;
	return true;
}

void RadarTick() {
	if (!g_settings.drawRoute) {
		std::lock_guard<std::mutex> guard(g_lock);
		g_segments.clear();
		g_status = "off";
		return;
	}

	// The radar is not on screen while the game's own menu is, and a line drawn over the pause map
	// would be placed by the radar's transform, which does not describe the map.
	if (GameMenuActive()) {
		std::lock_guard<std::mutex> guard(g_lock);
		g_segments.clear();
		g_status = "menu up";
		return;
	}

	if (--g_recompute <= 0) {
		g_recompute = kRecomputeTicks;
		g_route = RouteToWaypoint();
	}

	float ox, oy, fx, fy, range;
	if (g_route.size() < 2 || !ReadRadarFrame(&ox, &oy, &fx, &fy, &range)) {
		std::lock_guard<std::mutex> guard(g_lock);
		g_segments.clear();
		g_status = g_route.size() < 2 ? "no route" : "no radar frame";
		return;
	}

	// Where the player's arrow should point, in the radar's own frame.
	//
	// The vehicle's heading when there is one, the ped's when on foot - what the arrow means is
	// "the way you are travelling", and in a car that is the car's business, not the driver's.
	float facing = 0.0f;
	bool haveFacing = false;
	{
		std::optional<u32> entity = ReadAddrU32(VCSAddr::PlayerVehicle);
		if (!entity || *entity == 0) {
			entity = ReadAddrU32(VCSAddr::PlayerBase);
		}
		if (entity && *entity != 0) {
			if (const std::optional<float> yaw = ReadEntityHeading(*entity)) {
				// Negated, and that is measured rather than reasoned: with the plain cos/sin the
				// arrow came out pointing left when the car went right AND backwards when it went
				// forwards. Both axes flipped at once is a half turn, not a mirror and not a sign
				// error in the rotation below - so ReadEntityHeading's yaw names the direction
				// opposite to travel. It is only ever used for a difference of angles elsewhere,
				// where a consistent half turn cancels and nobody would have noticed.
				const float dirX = -std::cos(*yaw);
				const float dirY = -std::sin(*yaw);
				// Through the same rotation the line goes through, so the arrow and the road it is
				// on can never disagree.
				const float rx = fy * dirX + fx * dirY;
				const float ry = fy * dirY - fx * dirX;
				// Clockwise from straight up, which is the angle the draw wants: rotating "up" by
				// this lands on (sin, -cos), and the screen's y runs the other way to the radar's.
				facing = std::atan2(rx, ry);
				haveFacing = true;
			}
		}
	}

	std::vector<RadarSegment> built;
	built.reserve(g_route.size());
	const float inv = 1.0f / range;

	float px = 0.0f, py = 0.0f;
	bool havePrev = false;
	for (const RoutePoint &p : g_route) {
		const float dx = p.x - ox;
		const float dy = p.y - oy;
		// The game's own arithmetic, from 0x0880edb0.
		const float rx = (fy * dx + fx * dy) * inv;
		const float ry = (fy * dy - fx * dx) * inv;
		if (havePrev) {
			float a0, b0, a1, b1;
			if (ClipToDisc(px, py, rx, ry, &a0, &b0, &a1, &b1)) {
				RadarSegment seg;
				// Radar space is +x right and +y forward; the screen's y grows downward, which is
				// the one sign that has to be flipped on the way out.
				seg.x1 = g_settings.centreX + a0 * g_settings.radius;
				seg.y1 = g_settings.centreY - b0 * g_settings.radius;
				seg.x2 = g_settings.centreX + a1 * g_settings.radius;
				seg.y2 = g_settings.centreY - b1 * g_settings.radius;
				built.push_back(seg);
			}
		}
		px = rx; py = ry;
		havePrev = true;
	}

	{
		std::lock_guard<std::mutex> guard(g_lock);
		g_segments.swap(built);
		g_facing = facing;
		g_haveFacing = haveFacing;
		g_status = g_segments.empty() ? "route off radar" : "drawing";
	}
}

void GetRouteSegments(std::vector<RadarSegment> *out) {
	std::lock_guard<std::mutex> guard(g_lock);
	*out = g_segments;
}

bool GetPlayerFacing(float *angle) {
	std::lock_guard<std::mutex> guard(g_lock);
	if (!g_haveFacing) {
		return false;
	}
	*angle = g_facing;
	return true;
}

const char *RadarStatus() {
	return g_status;
}

void RadarReset() {
	std::lock_guard<std::mutex> guard(g_lock);
	g_segments.clear();
	g_route.clear();
	g_haveFacing = false;
	g_facing = 0.0f;
	g_recompute = 0;
	g_status = "idle";
}

}  // namespace VCS
