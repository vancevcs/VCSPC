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
#include "GPU/ge_constants.h"

// Dynamic sun shadows for GTA: Vice City Stories.
//
// Gated on the VCSDynamicShadows compat flag, which assets/compat.ini sets for ULUS10160 and
// nothing else. With the flag off, IsActive() is false, every entry point returns on its first
// line, and no other game executes a single instruction of this.
//
// This is milestone 1 of the shadow work, and it renders nothing at all. It classifies draws
// and counts them, so that three things can be checked against the running game before any
// render target exists:
//
//   1. Does the caster filter actually separate world/vehicles/peds from HUD and particles?
//      The filter is pure render state - there is no "this is a car" bit in a display list -
//      so the only way to know it works is to watch the counts move as you play.
//   2. Do peds reach the filter still skinned? The counts say how many casters carry bone
//      weights. If that number is zero while pedestrians are on screen, they are being drawn
//      some other way and a depth pass would render them in bind pose.
//   3. Does the game hand the GE a directional light? If it does, that is the sun, in world
//      space, already following the time of day - and the projection needs no address hunting.
//
// Every one of those was a guess before it showed a number.

namespace VCSShadow {

// Why a draw is not a shadow caster. The tests run in this order and the first failure wins,
// so the counts partition the frame's draws exactly - draws == casters + sum(rejected).
enum class Reject : u8 {
	None = 0,       // it is a caster
	Through,        // 2D: HUD, radar, menus. No world transform to speak of.
	Primitive,      // lines, points, sprites - nothing that bounds a volume
	NoDepthWrite,   // particles, coronas, and the vanilla blob shadow
	Blended,        // blending that genuinely composites: glass, smoke, water
	TooFewVerts,    // degenerate; not worth a draw call in the shadow pass
	Count,
};

// One bucket of alpha-blended draws sharing a blend setup.
//
// This exists because the first run of the counters came back with three quarters of the frame
// rejected as "alpha blended" and zero casters, which is not what transparency looks like. The
// buckets settled it: 170 draws of srcalpha/invsrcalpha/add, fully opaque vertex colours on all
// 170, which is a blend that copies. VCS switches blending on for the opaque pass and never
// clears it. BlendAltersDestination is the answer; this table is kept because it is now the
// thing that shows what real transparency in this game looks like, and milestone 2 needs to
// know that before it starts skipping draws.
struct BlendBucket {
	u8 funcA;
	u8 funcB;
	u8 eq;
	int count;
	int fullAlpha;   // ...of which the vertex colours were fully opaque
};

static const int kMaxBlendBuckets = 8;

struct FrameStats {
	int draws;
	int casters;
	int casterVerts;

	// Casters carrying bone weights. These are the skinned meshes - pedestrians and the player -
	// and they matter because the shadow pass has to run them through the same vertex decoder to
	// get them in pose rather than splayed out in bind pose.
	int castersSkinned;

	// Casters carrying vertex normals. PSP world geometry frequently ships without them, and
	// without normals there is no normal-offset bias, which is the good answer to shadow acne.
	// This count decides whether milestone 4 can use it or has to fall back to slope-scaled
	// depth bias for everything.
	int castersWithNormals;

	int rejected[(int)Reject::Count];

	// The blend setups behind the Blended rejects, most-recent-first insertion order.
	BlendBucket blendBuckets[kMaxBlendBuckets];
	int blendBucketCount;

	// What the caster count would be if the blend test were dropped. The single number that says
	// whether the filter is one line away from working or needs rethinking.
	int castersIfBlendIgnored;

	// The brightest enabled directional light seen this frame. On a directional channel the GE
	// treats lpos as a world-space vector pointing *towards* the light - VertexShaderGenerator
	// feeds it straight into `toLight` - so this is a sun direction, for free, already tracking
	// the time of day.
	bool sunValid;
	float sunDir[3];
	float sunDiffuse[3];
	int sunChannel;

	int lightingDraws;   // draws with hardware lighting enabled at all
	int dirLightDraws;   // ...of those, ones with an enabled directional channel
};

// Set once per boot, after the compat flags are known. Read on every draw, so it lives in the
// header to stay inlineable rather than costing a call per draw call.
extern bool g_active;

inline bool IsActive() { return g_active; }

void Init();
void Shutdown();

// Per-frame reset. Publishes the frame that just ended so the debugger has a whole frame to
// read rather than a half-built one.
void BeginFrame();

// Called from the draw engine for every draw that reaches the GPU. `vertTypeID` is the decoder's
// vertex type, not gstate's, because those can differ.
Reject ClassifyDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount);

// The last completed frame. Read by the debugger on the same thread as CPU emulation - see the
// threading note in ImVCS.h - so no marshalling is needed.
const FrameStats &LastFrameStats();

const char *RejectName(Reject r);
const char *BlendSrcName(u8 funcA);
const char *BlendDstName(u8 funcB);
const char *BlendEqName(u8 eq);

}  // namespace VCSShadow
