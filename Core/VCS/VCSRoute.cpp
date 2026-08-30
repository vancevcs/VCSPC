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

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSRoute.h"
#include "Core/VCS/VCSState.h"

namespace VCS {

namespace {

struct RoadNode {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	int firstLink = 0;
	int linkCount = 0;
};

std::vector<RoadNode> g_nodes;
std::vector<int> g_links;
u32 g_loadedFrom = 0;   // the ThePaths pointer this copy was built from

// Which connected piece each node belongs to.
//
// The road network is not one graph. It comes in THIRTEEN pieces, the largest holding 2399 of the
// 3087 nodes and the rest trailing off to a handful each - and they overlap in space rather than
// sitting on separate islands, so "near the marker" and "reachable from the player" are genuinely
// different questions. The sparse map-wide pieces look like the boat lanes, which the game counts
// among its car nodes.
//
// That was not a parsing fault, which is worth writing down because it looked exactly like one.
// The link ranges are perfectly contiguous - the count at +8 & 0x0f predicts the next node's
// firstLink for all 3086 consecutive pairs - and every single edge has its reverse present, a
// reciprocity of 1.000 over 6370 of them. The parse is right; the graph really is in pieces.
//
// Reachability is therefore exactly undirected connectivity, and one BFS at load makes the routing
// question "the nearest node the player can actually drive to" instead of "the nearest node".
std::vector<int> g_component;
int g_componentCount = 0;

inline float Dist(const RoadNode &a, const RoadNode &b) {
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	return std::sqrt(dx * dx + dy * dy);
}

void Clear() {
	g_nodes.clear();
	g_links.clear();
	g_component.clear();
	g_componentCount = 0;
	g_loadedFrom = 0;
}

// Label every node with its connected piece. Plain BFS over the undirected graph, which is the
// right notion here because the edges come in both directions.
void BuildComponents() {
	const int count = (int)g_nodes.size();
	g_component.assign((size_t)count, -1);
	g_componentCount = 0;
	std::vector<int> queue;
	for (int start = 0; start < count; start++) {
		if (g_component[(size_t)start] >= 0) {
			continue;
		}
		const int id = g_componentCount++;
		g_component[(size_t)start] = id;
		queue.clear();
		queue.push_back(start);
		for (size_t at = 0; at < queue.size(); at++) {
			const RoadNode &here = g_nodes[(size_t)queue[at]];
			for (int k = 0; k < here.linkCount; k++) {
				const int slot = here.firstLink + k;
				if (slot < 0 || slot >= (int)g_links.size()) {
					continue;
				}
				const int next = g_links[(size_t)slot];
				if (next < 0 || g_component[(size_t)next] >= 0) {
					continue;
				}
				g_component[(size_t)next] = id;
				queue.push_back(next);
			}
		}
	}
}

// Copy the road half of the game's node array to the host.
//
// Everything here is bounds-checked against what the game itself declares rather than trusted: the
// road count must fit inside the total, and a link index must name a road node. A path graph read
// mid-load is not a corrupt one, it is an incomplete one, and the difference between refusing it
// and routing through it is a crash.
bool Load(u32 paths) {
	Clear();

	const std::optional<u32> nodeArray = ReadU32(paths + kVCSPathNodeArray);
	const std::optional<u32> total = ReadU32(paths + kVCSPathNodeCount);
	const std::optional<u32> roads = ReadU32(paths + kVCSPathCarNodeCount);
	if (!nodeArray || !total || !roads || *nodeArray == 0) {
		return false;
	}
	// A sane ceiling as well as a consistency check. 8380 is what this game has; anything an order
	// of magnitude past it means the pointer is not a path graph.
	if (*roads == 0 || *roads > *total || *total > 200000) {
		return false;
	}

	const u32 count = *roads;
	g_nodes.resize(count);
	int maxLink = 0;
	for (u32 i = 0; i < count; i++) {
		const u32 rec = *nodeArray + i * kVCSPathNodeStride;
		// x and y are read as two u16 rather than one u32, and that is not a style choice.
		// The stride is TEN bytes, so every odd record is only 2-aligned - and VCSMemory's
		// ReadU32 requires 4-alignment, by way of Memory::IsValid4AlignedRange. Reading the pair
		// as a word failed on node 1 and took the whole graph load down with it, which showed up
		// as "no path graph loaded" with every address in the table correct.
		const std::optional<u16> xw = ReadU16(rec + kVCSPathNodeX);
		const std::optional<u16> yw = ReadU16(rec + kVCSPathNodeY);
		const std::optional<u8> zb = ReadU8(rec + kVCSPathNodeZ);
		const std::optional<u16> firstLink = ReadU16(rec + kVCSPathNodeFirstLink);
		const std::optional<u8> flags = ReadU8(rec + kVCSPathNodeLinkCount);
		if (!xw || !yw || !zb || !firstLink || !flags) {
			Clear();
			return false;
		}
		RoadNode &n = g_nodes[i];
		n.x = (float)(s16)*xw / kVCSPathNodeScale;
		n.y = (float)(s16)*yw / kVCSPathNodeScale;
		n.z = (float)(s8)*zb / kVCSPathNodeScale;
		n.firstLink = (int)*firstLink;
		n.linkCount = (int)(*flags & 0x0f);
		maxLink = std::max(maxLink, n.firstLink + n.linkCount);
	}

	// The links sit immediately after the WHOLE node array - both graphs' worth - and there is no
	// pointer to them anywhere in the header. See the addresses doc for how that was established;
	// the short version is that the gap between the array's end and ThePaths+0x08 is exactly the
	// link count the nodes themselves declare.
	const u32 linkArray = *nodeArray + *total * kVCSPathNodeStride;
	g_links.resize(maxLink);
	for (int i = 0; i < maxLink; i++) {
		const std::optional<u16> v = ReadU16(linkArray + (u32)i * kVCSPathLinkStride);
		if (!v) {
			Clear();
			return false;
		}
		// A link naming something outside the road graph is dropped rather than trusted. About 2%
		// of the array is like this - entries with bit 15 set that are not simply flagged indices -
		// and every one of them would be an out-of-bounds read during the search.
		g_links[i] = (*v < count) ? (int)*v : -1;
	}

	BuildComponents();
	g_loadedFrom = paths;
	return true;
}

}  // namespace

bool EnsureGraph() {
	const std::optional<u32> paths = ReadAddrU32(VCSAddr::ThePaths);
	if (!paths || *paths == 0) {
		Clear();
		return false;
	}
	if (*paths == g_loadedFrom && !g_nodes.empty()) {
		return true;
	}
	return Load(*paths);
}

int RoadNodeCount() {
	return (int)g_nodes.size();
}

int RoadLinkCount() {
	return (int)g_links.size();
}

int NearestRoadNode(float x, float y, float maxDistance, int component) {
	int best = -1;
	float bestSq = maxDistance * maxDistance;
	for (size_t i = 0; i < g_nodes.size(); i++) {
		if (component >= 0 && (i >= g_component.size() || g_component[i] != component)) {
			continue;
		}
		const float dx = g_nodes[i].x - x;
		const float dy = g_nodes[i].y - y;
		const float d = dx * dx + dy * dy;
		if (d < bestSq) {
			bestSq = d;
			best = (int)i;
		}
	}
	return best;
}

int RoadNodeComponent(int node) {
	if (node < 0 || node >= (int)g_component.size()) {
		return -1;
	}
	return g_component[(size_t)node];
}

int RoadComponentCount() {
	return g_componentCount;
}

std::vector<int> FindRoute(int fromNode, int toNode) {
	std::vector<int> path;
	const int count = (int)g_nodes.size();
	if (fromNode < 0 || toNode < 0 || fromNode >= count || toNode >= count) {
		return path;
	}
	if (fromNode == toNode) {
		path.push_back(fromNode);
		return path;
	}

	const float kInf = 1e30f;
	std::vector<float> best((size_t)count, kInf);
	std::vector<int> came((size_t)count, -1);
	std::vector<bool> settled((size_t)count, false);

	// (estimated total, node). Ordered by the estimate, so the first time the goal comes off the
	// queue it is optimal - which holds because the heuristic never overestimates: a straight line
	// is never longer than a road.
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;

	best[(size_t)fromNode] = 0.0f;
	open.push({Dist(g_nodes[fromNode], g_nodes[toNode]), fromNode});

	while (!open.empty()) {
		const int node = open.top().second;
		open.pop();
		if (settled[(size_t)node]) {
			continue;
		}
		settled[(size_t)node] = true;
		if (node == toNode) {
			break;
		}

		const RoadNode &here = g_nodes[(size_t)node];
		for (int k = 0; k < here.linkCount; k++) {
			const int slot = here.firstLink + k;
			if (slot < 0 || slot >= (int)g_links.size()) {
				continue;
			}
			const int next = g_links[(size_t)slot];
			if (next < 0 || settled[(size_t)next]) {
				continue;
			}
			const float cost = best[(size_t)node] + Dist(here, g_nodes[(size_t)next]);
			if (cost < best[(size_t)next]) {
				best[(size_t)next] = cost;
				came[(size_t)next] = node;
				open.push({cost + Dist(g_nodes[(size_t)next], g_nodes[(size_t)toNode]), next});
			}
		}
	}

	if (!settled[(size_t)toNode]) {
		// Genuinely unreachable rather than a failure: this network really does come in pieces,
		// and an empty route is how that is reported.
		return path;
	}
	for (int at = toNode; at != -1; at = came[(size_t)at]) {
		path.push_back(at);
	}
	std::reverse(path.begin(), path.end());
	return path;
}

std::vector<RoutePoint> RouteToWaypoint() {
	std::vector<RoutePoint> out;
	if (!EnsureGraph()) {
		return out;
	}
	float wpx = 0.0f, wpy = 0.0f;
	if (!FindWaypoint(&wpx, &wpy)) {
		return out;
	}
	const std::optional<u32> player = ReadAddrU32(VCSAddr::PlayerBase);
	if (!player || *player == 0) {
		return out;
	}
	const std::optional<float> px = ReadFloat(*player + kVCSEntityPositionOffset);
	const std::optional<float> py = ReadFloat(*player + kVCSEntityPositionOffset + 4);
	if (!px || !py) {
		return out;
	}

	const int from = NearestRoadNode(*px, *py, 150.0f);
	if (from < 0) {
		return out;
	}
	// The destination is picked from the player's own piece of the network rather than from the
	// whole of it. Snapping the marker to the nearest node full stop is what produced "no route"
	// while both ends sat plainly on roads: the marker had landed on one of the smaller pieces,
	// which overlaps the main one in space but shares no edge with it.
	//
	// The radius is generous because a marker is dropped by hand and lands wherever the player
	// pointed - a rooftop, a park, the sea. Failing to route because the nearest ROAD was ninety
	// units away would be a worse answer than routing to the road nearest the thing they meant.
	int to = NearestRoadNode(wpx, wpy, 600.0f, RoadNodeComponent(from));
	if (to < 0) {
		// Nothing reachable anywhere near it. Fall back to the nearest node of any piece, which at
		// least draws the direction, rather than drawing nothing.
		to = NearestRoadNode(wpx, wpy, 600.0f);
	}
	if (to < 0) {
		return out;
	}
	for (int node : FindRoute(from, to)) {
		out.push_back({g_nodes[(size_t)node].x, g_nodes[(size_t)node].y, g_nodes[(size_t)node].z});
	}
	return out;
}

bool RoadNodeAt(int index, RoutePoint *out) {
	if (index < 0 || index >= (int)g_nodes.size()) {
		return false;
	}
	const RoadNode &n = g_nodes[(size_t)index];
	*out = {n.x, n.y, n.z};
	return true;
}

bool RoadLinkAt(int node, int slot, int *otherNode) {
	if (node < 0 || node >= (int)g_nodes.size()) {
		return false;
	}
	const RoadNode &n = g_nodes[(size_t)node];
	if (slot < 0 || slot >= n.linkCount) {
		return false;
	}
	const int at = n.firstLink + slot;
	if (at < 0 || at >= (int)g_links.size() || g_links[(size_t)at] < 0) {
		return false;
	}
	*otherNode = g_links[(size_t)at];
	return true;
}

float RouteLength(const std::vector<RoutePoint> &route) {
	float total = 0.0f;
	for (size_t i = 1; i < route.size(); i++) {
		const float dx = route[i].x - route[i - 1].x;
		const float dy = route[i].y - route[i - 1].y;
		total += std::sqrt(dx * dx + dy * dy);
	}
	return total;
}

void RouteReset() {
	Clear();
}

}  // namespace VCS
