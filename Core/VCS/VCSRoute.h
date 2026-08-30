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

#include <vector>

#include "Common/CommonTypes.h"

// Routing over the game's own road network - the middle of a GPS, between where the player is and
// where they dropped the map marker.
//
// ---------------------------------------------------------------------------------------------
// THE GRAPH IS THE GAME'S, NOT OURS
//
// The first plan for this was to learn the roads by watching the player drive. That would have
// taken hours of driving to produce a worse map with holes in it, because the game already has the
// road network in memory: it is what the AI traffic drives on, which is the definition of "where
// a car can go" and is authored rather than inferred.
//
// `ThePaths` (0x08badb40) holds 8380 nodes of ten bytes each. Positions are s16 stored times 8; a
// u16 at +6 indexes a link array that sits immediately after the nodes, and the low nibble at +8
// is how many links that node has. See docs/VCS_ADDRESSES.md for how each of those was pinned
// down, and for the three independent checks that say the parsing is right.
//
// **Only the first 3087 nodes are roads.** The array is two graphs with no link between them, and
// the second is the pavement network. That was measured rather than reasoned: parked on a road,
// the car sat 1.46 units from a road-graph segment and 5.63 from the nearest pavement one.
// Routing over the wrong half would send the player down alleys and through parks.
//
// ---------------------------------------------------------------------------------------------
// READ ONCE, NOT EVERY FRAME
//
// The graph is static for a level, so it is copied to the host on first use and kept. That is
// ~3000 nodes and ~6000 links - trivial memory, and it turns every later route into arithmetic on
// host memory rather than thousands of PSP reads through the bounds-checked accessors.
//
// It is dropped whenever the pointer at `ThePaths` changes, which is what a level swap looks like
// from here. Not on a timer, and not never: a stale graph would route across a city that is no
// longer loaded, and rebuilding every frame would be the same work forever.
//
// Threading: emu thread only, like everything else that touches PSP memory.

namespace VCS {

// One point on a route, in world units - the same units as the player's position and the map
// marker, so a caller never has to know the graph stores them scaled.
struct RoutePoint {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// Make sure the host copy matches the graph the game currently has loaded. Cheap once warm - it
// compares one pointer. Returns false when the game has no path data yet, which is the normal
// state during a load.
bool EnsureGraph();

// How many road nodes and links the host copy holds. Zero before the first successful load, which
// is what the debugger shows when the address is wrong or the level has not finished loading.
int RoadNodeCount();
int RoadLinkCount();

// The road node nearest a world position, or -1 if the graph is empty. `maxDistance` rejects
// points nowhere near a road - out at sea, or inside a building's interior - so that a route
// cannot silently begin at the far side of the city.
// `component` restricts the search to one connected piece of the network; -1 searches all of them.
// That parameter is the difference between "the node nearest the marker" and "the node nearest the
// marker that the player can actually drive to", and the network really does come in pieces - see
// the note over BuildComponents in the .cpp.
int NearestRoadNode(float x, float y, float maxDistance = 120.0f, int component = -1);

// Which piece a node belongs to, and how many pieces there are. Two nodes are mutually reachable
// exactly when these agree, the edges all being bidirectional.
int RoadNodeComponent(int node);
int RoadComponentCount();

// Shortest path along the roads, from one node to another, as node indices. Empty when there is
// no route - the two are in different components, which this network genuinely has.
//
// Plain A* with straight-line distance as the heuristic. That is admissible here because an edge
// is never shorter than the gap it spans, so the search cannot overshoot the true cost and the
// first time the destination is settled it is settled optimally.
std::vector<int> FindRoute(int fromNode, int toNode);

// The whole job in one call: from where the player is to where the marker is, as world points
// ready to draw. Empty when there is no marker, no graph, or no path between them.
std::vector<RoutePoint> RouteToWaypoint();

// Read one road node's position, and one of its links. For drawing the network itself - the
// debugger's map view uses these to show the roads the route was found over, which is what turns
// "the numbers look plausible" into "that is the road I would have taken".
//
// Both return false for an index the graph does not have, so a caller can walk them without
// knowing the counts first.
bool RoadNodeAt(int index, RoutePoint *out);
bool RoadLinkAt(int node, int slot, int *otherNode);

// Total length of a route in world units, for the debugger and for whatever eventually shows a
// distance to the player.
float RouteLength(const std::vector<RoutePoint> &route);

// Drop the host copy. Called on shutdown; also the thing to call if the graph is ever suspected
// of being stale for a reason EnsureGraph cannot see.
void RouteReset();

}  // namespace VCS
