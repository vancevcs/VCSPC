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
// Three passes, all of them inside the frame they belong to:
//
//   1. A depth map of the city as seen from the sun, into a cascade anchored ahead of the camera.
//   2. A screen-space mask - white where the sun reaches - drawn by putting the same captured
//      geometry through the camera's own transform a second time.
//   3. One triangle over the game's framebuffer, multiplying what is there by that mask.
//
// All three run at the moment the frame turns from 3D to 2D, so the HUD drawn afterwards is not
// shadowed and nothing is a frame stale. See OnFlush.
//
// Three assumptions underneath it, each checked against the running game before anything was
// rendered, and each still visible as a count in the debugger's Shadows tab:
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
	OffscreenTarget,// a render-to-texture pass - its own camera, and nobody looks at it directly
	Primitive,      // lines, points, sprites - nothing that bounds a volume
	NoDepthWrite,   // particles, coronas, and the vanilla blob shadow
	Blended,        // blending that genuinely composites: glass, smoke, water
	Cutout,         // alpha-tested, and the texture is what cuts the shape out
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

	// Triangles kept and dropped by the back-face test. Roughly half and half on closed geometry;
	// a count of nearly zero either way means the winding convention is wrong.
	int trianglesFacingAway;
	int trianglesFacingLight;

	// Whether the light this frame is the sun or the mirrored one that stands in for the moon.
	bool sunIsMoon;

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

		// VCS reuses a light channel for different things inside one frame, so a channel index is
		// not a stable identity for "the sun" and the direction shown here is only the last one
		// the channel held. The pick is a maximum across the whole frame, which is why the chosen
		// sun can differ from every row below it.
		int directionChanges;
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

// Where the shadow projection ends up looking, recomputed once a frame.
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

	// How far up-sun of the centre the depth map still holds casters - see casterReach. The box
	// is `radius` wide and `casterReach + radius` deep, which is why the two are separate.
	float casterReach;
	float texelWorldSize;

	float lightViewProj[16];

	// World to the pixel the game drew, for the mask pass. Built from the same captured view
	// matrix, the projection matrix beside it, AND everything PPSSPP's own vertex shader does
	// after those two - the PSP viewport transform, the raster offset, the remap into the render
	// target's NDC. Stopping at view*proj gets a mask that resembles the frame and does not line
	// up with it, and a shadow mask that is a few pixels out is worse than none.
	float cameraViewProj[16];

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

	// How far UP-SUN of the cascade the depth pass still accepts casters.
	//
	// This is the whole of "the building behind me casts no shadow", and it was never the cache -
	// the cache had the building. A shadow travels along the light and nowhere else, so in light
	// space a caster sits at the same place as the shadow it throws and differs only in depth.
	// The box was pulled back by exactly one cascade radius, so anything more than seventy units
	// towards the sun was clipped out of the map before it could cast into it - which is most of
	// a street when the sun is behind you.
	//
	// Widening the cascade would have cost resolution everywhere to fix it. Moving the near plane
	// costs only depth range, because the SIDES of the box are already exactly right: a caster
	// outside them lands outside them too.
	float casterReach;

	int mapSize;

	// Which view-space axis points into the scene. The GL convention is -Z and that is the
	// default, but the PSP's is worth confirming rather than assuming: with this wrong the
	// cascade sits behind the camera and shadows land on nothing. One run settles it.
	bool forwardIsNegativeZ;

	// The screen-space mask is built at the game's own render resolution times this, so it lines
	// up with the frame it will be multiplied into. Below 1.0 it is cheaper and softer: the
	// composite samples it bilinearly, so a half-scale mask is a free blur rather than a stairstep.
	float maskScale;

	float depthBias;

	// Bias proportional to how steeply the light grazes the surface. A constant bias alone has to
	// be set for the worst case - ground almost edge-on to the sun - and that much of it detaches
	// every shadow from the thing casting it. This term comes from the depth derivatives in the
	// mask shader, so it is large exactly where acne appears and near zero everywhere else.
	float slopeBias;

	// Half-width of the PCF kernel in shadow-map texels. 0 is a single tap and a hard stencil
	// edge; 1 is the 3x3 that makes a 2048 map read as a shadow rather than as a cutout.
	int pcfRadius;

	// How much of the cascade's outer edge fades back to unshadowed, as a fraction of its half
	// width. Without this the cascade ends in a straight line ruled across the road.
	float edgeFade;

	// How dark a shadowed pixel goes, and what colour it goes towards. Scaled by the sun's own
	// brightness, so shadows thin out towards dusk without anything having to know the time.
	float strength;
	float tint[3];

	// The game culls what it draws to the camera's own frustum - that is how a PSP ran this at
	// all - and the capture only ever sees what reaches the GPU. So a building just off the left
	// edge stops casting the moment it leaves the view, and its shadow blinks out of the middle
	// of the road. Keeping recently-seen scenery and re-submitting it to the DEPTH pass fixes
	// that without asking the game to draw one triangle more.
	//
	// Casters only. A receiver has to be on screen to be shaded, so a cached one would be
	// shading nothing.
	bool cacheCasters;

	// How long a caster keeps casting after it was last drawn. This is also the ghost window: an
	// object that both moves and leaves the view in the same frame holds its old shadow for this
	// long. Everything that moves in this game is either skinned or gets caught by the
	// same-model-different-matrix eviction, so it is a backstop rather than the usual path.
	float cacheHoldSeconds;

	// Cached casters further than this from the camera are dropped. The cascade is 50 units, so
	// there is nothing to be gained by remembering the far side of the island - and this is what
	// keeps the cache from growing into the whole city as you drive across it.
	float cacheRadius;

	// Only the faces turned AWAY from the light go into the depth map.
	//
	// This is the textbook answer to self-shadowing and it is worth more here than the usual
	// bias tuning, because the two passes read the same captured vertices: a surface compares
	// against ITSELF, and the only error is where the shadow map's texel grid falls. Storing the
	// far side of every object puts a whole object's thickness between the two, which no
	// rasterisation difference can cross. The stripes across the road were that error.
	//
	// It also halves the geometry the depth pass draws, which is free frame time.
	bool castBackFacesOnly;

	// Which way round the captured winding runs. Wrong, and the map holds the near side of
	// everything instead of the far side - shadows stay roughly where they should be, because a
	// silhouette is the same either way, and the acne comes back worse. One run settles it.
	bool flipCasterWinding;

	// At night the game's brightest directional light is below the horizon, and a light below the
	// horizon casts nothing. Rather than switch the feature off for half the day, the vector is
	// mirrored back above the horizon - which is roughly where the moon is - and the shadows it
	// casts are scaled by this. Zero turns night shadows off.
	float moonStrength;

	// The game's own blob shadow - a flat alpha-blended quad under peds and vehicles - is not
	// wanted once there are real ones, or everything that moves has two shadows.
	//
	// The textures it uses are LEARNED rather than named: a small flat blended quad that writes
	// no depth and lies directly underneath something the capture accepted is that object's
	// shadow, whatever texture it happens to be using today. Its texture address goes into a
	// small set and every later draw using one is dropped. A decal on open road has nothing
	// above it and is left alone, which is the distinction a render-state test cannot make.
	bool hideBlobShadows;

	// A draw whose whole footprint sits within this distance of the camera is thrown away, caster
	// and receiver both. It is the game's full-screen overlays - the colour filter, the fades -
	// which VCS draws as 3D geometry rather than in through mode, so no render-state test can
	// see them for what they are. They cover every pixel and sit AT the camera, so once the mask
	// keeps the nearest surface they win every pixel of it, and the mask comes back as one flat
	// value: measured at 0.6 units across, centred on the recovered camera, filling the frame.
	//
	// The camera is never inside real geometry, and the third-person camera sits 4.6 units behind
	// the player, so there is a wide gap to put this in.
	float nearCameraCutoff;

	// A draw whose world-space footprint is wider than this is captured as a shadow RECEIVER only
	// and never enters the depth pass. The sky and the map-spanning ground and water quads are
	// what this is for: geometry that covers the whole cascade would fill the shadow map with one
	// surface and put the entire city in its shadow. Nothing that reads as a building comes near
	// this span.
	float maxCasterSpan;

	// Which way up the shadow map's V axis runs depends on the backend's clip convention. Wrong,
	// and the shadows track the right shapes in the wrong places - so it is a toggle to be
	// settled in one run rather than a guess compiled into the shader.
	bool flipShadowV;

	// 0 shadow term, 1 sampled shadow-map depth, 2 light-space Z, 3 shadow UV as red/green.
	int debugView;

	// Draw the mask straight over the frame instead of multiplying by it. The one view that
	// answers "is the mask lined up with what the game drew" with nothing else in the way.
	bool showMask;

	// The cascade coverage count runs the light matrix over every captured vertex on the CPU,
	// a second full pass purely to produce one diagnostic number. It earned its place while the
	// projection was unproven; it has no business costing frames once it is.
	bool countCascadeCoverage;
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
	bool maskRendered; // ...and so did the screen-space mask
	bool composited;   // ...and the mask actually reached the game's own framebuffer

	// Indices in each of the two streams. Everything captured is a receiver; the subset small
	// enough to be believable as a caster is what the depth pass draws - see maxCasterSpan.
	int casterIndices;
	int receiverIndices;
	int receiverOnlyDraws;

	// Draws thrown out for sitting on top of the camera - see nearCameraCutoff. One or two a
	// frame is the colour filter and is expected; zero means the filter is not finding it, and
	// the mask will be a flat colour.
	int nearCameraDraws;

	// The caster cache: how many entries it holds, how many of them this frame's shadow map got
	// from it rather than from the game, and how much memory that is. A cached count of zero
	// while shadows still blink out means nothing is being recognised as scenery.
	int cachedEntries;
	int cachedDraws;
	int cachedVertices;
	size_t cachedBytes;

	// The blob-shadow suppressor: how many textures it has learned, and how many draws it
	// dropped this frame. Learned staying at zero means the "sits under something" test never
	// fires and the game's own shadows are still on screen.
	int blobTextures;
	int blobDraws;

	// The size the mask was built at, which follows the game's render target rather than being
	// configured. Shown because a mask that is not the size of the frame cannot line up with it.
	int maskWidth;
	int maskHeight;

	// Which step of the mask setup failed, if it did. "Did not run" on its own says nothing -
	// four different things can fail there and they need four different fixes.
	enum class MaskStep : u8 {
		Ok = 0,
		NotAttempted,
		Framebuffer,
		VertexShader,
		FragmentShader,
		Pipeline,
	};
	MaskStep maskStep;
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

// Offered every draw the caster filter threw out for writing no depth, with its geometry, so the
// blob-shadow suppressor can see where it is. Cheap by construction - it is only called for small
// draws, and it returns immediately unless hideBlobShadows is on.
void NoteGroundQuad(const u8 *decoded, int numDecodedVerts, int stride, int posOffset,
	const float world[12], u32 textureAddr);

// Whether the draw about to be issued is one of the game's own blob shadows, and should not be
// drawn at all. Called once per flush from the draw engine, after classification.
bool ShouldSkipDraw();

// The accumulated triangle list, valid until the next BeginFrame.
Settings &GetSettings();
const ShadowView &View();

// Read on every draw, so these live in the header to stay inlineable rather than costing a call
// per draw call.
//
// Two flags, not one. g_available is the compat flag, fixed at boot: it says this is a disc the
// feature knows about at all. g_active is that AND the runtime switch, and it is the one the hot
// path tests - so turning the feature off costs nothing anywhere, including the capture, rather
// than quietly continuing to bake 30k vertices a frame for a pass nobody is looking at.
extern bool g_available;
extern bool g_active;

inline bool IsAvailable() { return g_available; }
inline bool IsActive() { return g_active; }

// Runtime on/off. This is an experiment living inside a working emulator, so it needs to be
// switchable without a rebuild and without touching compat.ini.
void SetEnabled(bool enabled);
bool IsEnabled();

void Init();
void Shutdown();

// Per-frame publish and reset. Nothing renders from here.
//
// An earlier version DID render from here, against the frame that had just ended, to keep the
// passes out of the way of the game's own. That is fine for the shadow map, which is geometry and
// barely moves in a frame - and wrong for the screen-space mask, which is the camera's own view
// and slides bodily across the picture the moment the camera turns. The passes moved into the
// frame; this kept the bookkeeping.
void BeginFrame(Draw::DrawContext *draw);

// Called from the draw engine at the top of every vertex flush, before it establishes any state.
//
// `through` is whether the batch about to be drawn is 2D. The first 2D batch of a frame that has
// captured any 3D is the seam between the world and the HUD, and that is when all three passes
// run and the mask is multiplied into `target` - the framebuffer the game is currently drawing
// into. Returns true if anything was drawn, which is the caller's cue to rebind its own render
// target; false is the ordinary case and costs one bool test.
bool OnFlush(Draw::DrawContext *draw, bool through, Draw::Framebuffer *target);

// The depth target, for the debugger to preview. Null until the pass has run once.
Draw::Framebuffer *ShadowMap();

// The screen-space mask: white where the sun reaches, black where it does not. This is what the
// composite multiplies the game's colour by.
Draw::Framebuffer *ShadowMask();

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
