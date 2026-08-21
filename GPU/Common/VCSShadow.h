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

namespace Draw {
class DrawContext;
class Framebuffer;
}

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
	// Every enabled directional light seen this frame, not just the one that won. VCS turns more
	// than one on and they are not all the sun: a capture came back with channel 0 pointing at
	// exactly (1, 0, 0) at full white - axis-aligned, dead level with the horizon, and the sort of
	// value a channel has when nobody has set it rather than one a solar position produces.
	// Brightness alone cannot separate that from the real thing when both are full white, so the
	// candidates are listed and the pick is explained rather than asserted.
	struct SunCandidate {
		bool valid;
		float dir[3];
		float diffuse[3];
		int draws;
		bool aboveHorizon;
	};
	SunCandidate sunCandidates[4];

	bool sunValid;
	float sunDir[3];
	float sunDiffuse[3];
	int sunChannel;


	// Where the caster world matrices actually put their geometry, as a bounding box over their
	// translations. "World space" to the GE is whatever space the game feeds the hardware, which
	// is not obliged to be the absolute world space the game keeps in its own structures - and
	// the shadow projection has to live in the former. Comparing this box against both the
	// recovered camera and the player position read from PSP memory says which space is which,
	// without anyone having to guess.
	//
	// Caveat: for a skinned mesh the bones are already applied during decode and the world matrix
	// is often identity, so those entries sit at the origin and mean nothing. The rigid world
	// geometry, which is the bulk of the casters, is what makes this box meaningful.
	bool casterBoundsValid;
	float casterMin[3];
	float casterMax[3];

	// How many times the view matrix changed after the frame's first caster. Anything above zero
	// means the frame holds more than one camera - a render-to-texture pass, the radar - and
	// "take the matrix from the first caster" is the wrong heuristic.
	int viewMatrixChanges;
	int lightingDraws;   // draws with hardware lighting enabled at all
	int dirLightDraws;   // ...of those, ones with an enabled directional channel
};

// Where the shadow projection ends up looking, recomputed once a frame. Milestone 2a fills this
// in and shows it; nothing renders from it yet.
//
// Conventions, because getting one of these backwards costs a day: VCS world space is Z-up. That
// is not inferred from the sun vector - CWorld::ProcessVerticalLine answers "what is the height
// at this point" by writing z, and the vault code reads ledge heights out of z, both already
// verified against the running game. Matrices follow PPSSPP's row-vector convention throughout,
// so a point is transformed as clip = vec4(worldPos, 1.0) * m, and gstate's 4x3 matrices store
// the translation in the last row.
struct ShadowView {
	bool valid;

	// Recovered from gstate.viewMatrix by inverting it. The panel checks this against the
	// player position read out of PSP memory, which is the cheapest way to catch a transposed
	// matrix - a wrong convention puts the camera somewhere absurd rather than subtly off.
	float cameraPos[3];
	float cameraRight[3];
	float cameraUp[3];
	float cameraForward[3];

	float lightDir[3];    // the direction light travels, i.e. the negated sun vector
	float lightRight[3];
	float lightUp[3];

	float centre[3];      // cascade centre in world space, after texel snapping
	float radius;
	float texelWorldSize;

	float lightViewProj[16];

	// The recovered camera position pushed back through the view matrix the way the vertex
	// shader would push a vertex. It should land on the origin. This cannot catch a wrong
	// convention - the round trip uses the same belief twice - but it does catch an algebra
	// slip, which is the other half of how these go wrong.
	float viewResidual[3];
};

// Player-facing knobs. Two of these exist because the answer is not known yet rather than because
// anyone should want to change them.
struct Settings {
	// Cascade 0 is anchored on the camera with a fixed radius rather than fitted to the view
	// frustum. Frustum fitting is strictly better and belongs in milestone 4; this gets a shadow
	// map on screen without first having to decompose the game's projection matrix.
	float cascadeRadius;
	float centreDistance;
	int mapSize;

	// Which view-space axis points into the scene. The GL convention is -Z and that is the
	// default, but the PSP's is worth confirming rather than assuming: with this wrong the
	// cascade sits behind the camera and shadows land on nothing. One run settles it.
	bool forwardIsNegativeZ;
};

// The frame's caster geometry, baked to the space the GE is fed and flattened to one triangle
// list. Building it this way rather than replaying the game's draw calls into the shadow map is
// the difference between one draw call a frame and one per caster - 184 of them in a typical
// frame here - and it costs a vertex transform each, which at ~38k vertices is nothing. It also
// keeps the whole thing free of any backend's buffer lifetime rules.
struct CaptureStats {
	int draws;
	int vertices;
	int indices;
	size_t bytes;
	bool overflowed;   // hit the cap; the map will be missing geometry rather than corrupt
	bool rendered;     // the depth pass actually ran this frame
	int batches;       // draws issued - one per 64k vertices, because thin3d indices are 16-bit

	// Captured vertices that land inside the cascade's clip volume, counted on the CPU with the
	// same matrix the shader uses. A depth map is a poor thing to judge by eye - it is mostly a
	// smooth gradient whether or not it is right - and this answers the only question that
	// picture was being asked: is the cascade catching the world, and how much of it.
	int verticesInCascade;

	// The bounds of the actual vertices, not of their world matrices. This is the honest answer
	// to "is the cascade radius anywhere near right" - a 28-unit cascade against a caster set
	// hundreds of units across covers a small fraction of what is on screen, by design, but the
	// numbers should be in a believable relation to each other.
	bool boundsValid;
	float min[3];
	float max[3];

	// How many captured draws are themselves enormous. The bounds above came back 2048 units
	// square, which is either the map-spanning ground or water quad - one draw, harmless, and
	// flat - or a sign that positions are being read at the wrong stride and the numbers are
	// noise. Those two look identical in a bounding box and completely different in this count:
	// a handful means the former, most of them means the latter.
	int largeDraws;
	int largestDrawVerts;
};

const CaptureStats &LastCapture();

// Appends one flush's worth of caster geometry, transformed to world space by `world` and
// expanded from whatever topology it arrived in to a plain triangle list.
//
// Safe to call with `indices` null for a non-indexed draw. `numDecodedVerts` is the decoded
// vertex count for the whole flush, which is what the index buffer indexes into.
void AddCaster(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, GEPrimitiveType prim, const float world[12]);

// The accumulated triangle list, valid until the next BeginFrame.
Settings &GetSettings();
const ShadowView &View();

// Set once per boot, after the compat flags are known. Read on every draw, so it lives in the
// header to stay inlineable rather than costing a call per draw call.
extern bool g_active;

inline bool IsActive() { return g_active; }

void Init();
void Shutdown();

// Per-frame reset, and the one place the depth pass runs.
//
// Publishing, projecting and rendering all happen here, in that order, against the frame that
// just ended - so the view matrix, the sun and the geometry all come from the same frame rather
// than from three different ones. The shadow map is therefore one frame behind what is on screen,
// which at these frame rates nobody can see, and in exchange the pass never has to interleave
// itself with the game's own render passes.
void BeginFrame(Draw::DrawContext *draw);

// The depth target, for the debugger to preview. Null until the pass has run once.
Draw::Framebuffer *ShadowMap();

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
