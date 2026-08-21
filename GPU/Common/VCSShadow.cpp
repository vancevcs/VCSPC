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

#include <cmath>
#include <cstring>
#include <vector>

#include "Common/CommonFuncs.h"
#include "Core/System.h"
#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/Common/VCSShadow.h"

namespace VCSShadow {

bool g_active = false;

static FrameStats s_current;
static FrameStats s_published;

// Luminance of the brightest directional light seen so far this frame. Kept out of FrameStats
// because it is scratch for picking the sun, not something worth showing.
static float s_sunLuminance;

static ShadowView s_view;
static Settings s_settings = {
	28.0f,    // cascadeRadius, world units. GTA is roughly one unit to the metre, so this is a
	          // block or so around the player - enough for cascade 0 to prove itself.
	16.0f,    // centreDistance ahead of the camera
	1024,     // mapSize
	true,     // forwardIsNegativeZ
};

// gstate's view matrix as of the first caster of the frame. Captured rather than read at end of
// frame because the game leaves whatever matrix the last draw used in place, and the last draw is
// usually 2D.
static float s_frameViewMatrix[12];
static bool s_haveViewMatrix;

// The frame's caster geometry. Kept as std::vector so the capacity settles after a few frames
// and then stops allocating - clear() keeps the storage.
static std::vector<float> s_positions;
static std::vector<u32> s_indices;
static CaptureStats s_capture;
static CaptureStats s_capturePublished;

// A frame that wants more than this is not a frame we can help. At 12 bytes a vertex this is
// about 12 MB of positions, against a measured ~38k vertices a frame - roughly 25x headroom, so
// hitting it means something has gone wrong rather than that the scene got busy.
static const size_t kMaxCapturedVertices = 1024 * 1024;

namespace {

inline float Dot(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void Cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

inline bool Normalize(float v[3]) {
	const float lengthSq = Dot(v, v);
	if (lengthSq < 1e-12f) {
		return false;
	}
	const float invLength = 1.0f / sqrtf(lengthSq);
	v[0] *= invLength;
	v[1] *= invLength;
	v[2] *= invLength;
	return true;
}

// Recovers the camera's world-space position and axes from gstate's 4x3 view matrix.
//
// The view matrix is row-vector and world-to-view, so viewPos = worldPos * R + t with R at
// m[0..8] and t at m[9..11]. R is a rotation, so its inverse is its transpose: the world-space
// direction of view axis c is column c of R, and the camera position is -t * R^T. Getting this
// transposed is the classic way to lose a day, which is why the panel prints the result next to
// the player position rather than trusting it.
void DecomposeViewMatrix(const float m[12], ShadowView *view) {
	view->cameraRight[0] = m[0];
	view->cameraRight[1] = m[3];
	view->cameraRight[2] = m[6];

	view->cameraUp[0] = m[1];
	view->cameraUp[1] = m[4];
	view->cameraUp[2] = m[7];

	float viewZ[3] = { m[2], m[5], m[8] };
	const float sign = s_settings.forwardIsNegativeZ ? -1.0f : 1.0f;
	view->cameraForward[0] = viewZ[0] * sign;
	view->cameraForward[1] = viewZ[1] * sign;
	view->cameraForward[2] = viewZ[2] * sign;

	const float t[3] = { m[9], m[10], m[11] };
	view->cameraPos[0] = -(t[0] * m[0] + t[1] * m[1] + t[2] * m[2]);
	view->cameraPos[1] = -(t[0] * m[3] + t[1] * m[4] + t[2] * m[5]);
	view->cameraPos[2] = -(t[0] * m[6] + t[1] * m[7] + t[2] * m[8]);
}

// Builds world-to-light-clip as one row-vector 4x4, so a caster's world position becomes shadow
// clip space with a single multiply and nothing downstream has to know how it was assembled.
//
// The light half is an orthographic box: the sun is far enough away that perspective is wrong.
// Depth maps to [0, 1] rather than [-1, 1] because that is Vulkan's clip convention.
void BuildLightViewProj(ShadowView *view) {
	const float *right = view->lightRight;
	const float *up = view->lightUp;
	const float *forward = view->lightDir;

	// Pull back along the light by the cascade radius, so the whole sphere is in front of the
	// near plane and nothing that should cast gets clipped away behind it.
	const float origin[3] = {
		view->centre[0] - forward[0] * view->radius,
		view->centre[1] - forward[1] * view->radius,
		view->centre[2] - forward[2] * view->radius,
	};

	const float invRadius = 1.0f / view->radius;
	const float invDepth = 1.0f / (2.0f * view->radius);

	float *m = view->lightViewProj;
	m[0]  = right[0] * invRadius;  m[1]  = up[0] * invRadius;  m[2]  = forward[0] * invDepth;  m[3]  = 0.0f;
	m[4]  = right[1] * invRadius;  m[5]  = up[1] * invRadius;  m[6]  = forward[1] * invDepth;  m[7]  = 0.0f;
	m[8]  = right[2] * invRadius;  m[9]  = up[2] * invRadius;  m[10] = forward[2] * invDepth;  m[11] = 0.0f;
	m[12] = -Dot(origin, right) * invRadius;
	m[13] = -Dot(origin, up) * invRadius;
	m[14] = -Dot(origin, forward) * invDepth;
	m[15] = 1.0f;
}

}  // namespace

// Recomputes where the shadow projection is looking, from the view matrix and sun direction the
// frame that just ended left behind.
static void ComputeShadowView(const FrameStats &stats) {
	s_view.valid = false;
	if (!s_haveViewMatrix || !stats.sunValid) {
		return;
	}

	DecomposeViewMatrix(s_frameViewMatrix, &s_view);

	// Push the recovered position back through the view matrix exactly as the vertex shader
	// would push a vertex: out.c = sum_r p.r * m[r*3+c] + m[9+c].
	{
		const float *m = s_frameViewMatrix;
		const float *p = s_view.cameraPos;
		for (int c = 0; c < 3; c++) {
			s_view.viewResidual[c] = p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + m[9 + c];
		}
	}

	// The sun vector points towards the light; light travels the other way.
	s_view.lightDir[0] = -stats.sunDir[0];
	s_view.lightDir[1] = -stats.sunDir[1];
	s_view.lightDir[2] = -stats.sunDir[2];
	if (!Normalize(s_view.lightDir)) {
		return;
	}

	// Any up vector will do as long as it is not parallel to the light. World up is the natural
	// choice and only fails with the sun directly overhead, which is what the fallback is for.
	float up[3] = { 0.0f, 0.0f, 1.0f };
	if (fabsf(Dot(s_view.lightDir, up)) > 0.99f) {
		up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
	}
	Cross(up, s_view.lightDir, s_view.lightRight);
	if (!Normalize(s_view.lightRight)) {
		return;
	}
	Cross(s_view.lightDir, s_view.lightRight, s_view.lightUp);
	Normalize(s_view.lightUp);

	s_view.radius = s_settings.cascadeRadius;
	s_view.texelWorldSize = (2.0f * s_view.radius) / (float)s_settings.mapSize;

	float centre[3] = {
		s_view.cameraPos[0] + s_view.cameraForward[0] * s_settings.centreDistance,
		s_view.cameraPos[1] + s_view.cameraForward[1] * s_settings.centreDistance,
		s_view.cameraPos[2] + s_view.cameraForward[2] * s_settings.centreDistance,
	};

	// Snap the centre to whole shadow-map texels along the light's own axes. Without this the
	// map's sampling grid slides under the geometry as the camera moves and every shadow edge
	// crawls - the single most obvious artefact in a cascade that is otherwise correct.
	const float texel = s_view.texelWorldSize;
	if (texel > 0.0f) {
		const float x = floorf(Dot(centre, s_view.lightRight) / texel) * texel;
		const float y = floorf(Dot(centre, s_view.lightUp) / texel) * texel;
		const float z = Dot(centre, s_view.lightDir);
		for (int i = 0; i < 3; i++) {
			centre[i] = s_view.lightRight[i] * x + s_view.lightUp[i] * y + s_view.lightDir[i] * z;
		}
	}
	s_view.centre[0] = centre[0];
	s_view.centre[1] = centre[1];
	s_view.centre[2] = centre[2];

	BuildLightViewProj(&s_view);
	s_view.valid = true;
}

void AddCaster(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, GEPrimitiveType prim, const float world[12]) {
	if (numDecodedVerts <= 0 || indexCount < 3) {
		return;
	}
	if (s_positions.size() / 3 + (size_t)numDecodedVerts > kMaxCapturedVertices) {
		s_capture.overflowed = true;
		return;
	}

	const u32 base = (u32)(s_positions.size() / 3);
	s_positions.resize(s_positions.size() + (size_t)numDecodedVerts * 3);
	float *out = s_positions.data() + (size_t)base * 3;

	// Row-vector, matching the vertex shader: world = vec4(pos, 1.0) * m, with the 4x3 matrix
	// holding its translation in the last row. The decoded position is always three floats -
	// the decoder guarantees that regardless of how the game encoded it - and for a skinned mesh
	// the bones have already been folded in here, which is what gets peds into pose for free.
	for (int i = 0; i < numDecodedVerts; i++) {
		const float *p = (const float *)(decoded + (size_t)i * stride + posOffset);
		const float x = p[0] * world[0] + p[1] * world[3] + p[2] * world[6] + world[9];
		const float y = p[0] * world[1] + p[1] * world[4] + p[2] * world[7] + world[10];
		const float z = p[0] * world[2] + p[1] * world[5] + p[2] * world[8] + world[11];
		out[i * 3 + 0] = x;
		out[i * 3 + 1] = y;
		out[i * 3 + 2] = z;

		const float v[3] = { x, y, z };
		if (!s_capture.boundsValid) {
			s_capture.boundsValid = true;
			for (int c = 0; c < 3; c++) {
				s_capture.min[c] = v[c];
				s_capture.max[c] = v[c];
			}
		} else {
			for (int c = 0; c < 3; c++) {
				if (v[c] < s_capture.min[c]) s_capture.min[c] = v[c];
				if (v[c] > s_capture.max[c]) s_capture.max[c] = v[c];
			}
		}
	}

	// Everything becomes a triangle list, because the whole cascade is going out as a single
	// draw and one draw cannot carry three topologies. Strips and fans are cheap to expand and
	// the alternative is a draw call per caster.
	const auto index = [&](int i) -> u32 {
		return base + (indices ? (u32)indices[i] : (u32)i);
	};
	switch (prim) {
	case GE_PRIM_TRIANGLES:
		for (int i = 0; i + 2 < indexCount; i += 3) {
			s_indices.push_back(index(i));
			s_indices.push_back(index(i + 1));
			s_indices.push_back(index(i + 2));
		}
		break;
	case GE_PRIM_TRIANGLE_STRIP:
		// Every other triangle in a strip has reversed winding. Keeping that consistent matters
		// because the depth pass culls faces to halve the acne problem.
		for (int i = 0; i + 2 < indexCount; i++) {
			if (i & 1) {
				s_indices.push_back(index(i + 1));
				s_indices.push_back(index(i));
			} else {
				s_indices.push_back(index(i));
				s_indices.push_back(index(i + 1));
			}
			s_indices.push_back(index(i + 2));
		}
		break;
	case GE_PRIM_TRIANGLE_FAN:
		for (int i = 1; i + 1 < indexCount; i++) {
			s_indices.push_back(index(0));
			s_indices.push_back(index(i));
			s_indices.push_back(index(i + 1));
		}
		break;
	default:
		break;
	}

	s_capture.draws++;
	s_capture.vertices = (int)(s_positions.size() / 3);
	s_capture.indices = (int)s_indices.size();
	s_capture.bytes = s_positions.size() * sizeof(float) + s_indices.size() * sizeof(u32);
}

const CaptureStats &LastCapture() {
	return s_capturePublished;
}

const float *CapturedPositions() {
	return s_positions.empty() ? nullptr : s_positions.data();
}

const u32 *CapturedIndices() {
	return s_indices.empty() ? nullptr : s_indices.data();
}

Settings &GetSettings() {
	return s_settings;
}

const ShadowView &View() {
	return s_view;
}

void Init() {
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	s_sunLuminance = 0.0f;
	memset(&s_view, 0, sizeof(s_view));
	s_haveViewMatrix = false;

	// The flag is only set for ULUS10160 in compat.ini, so this is the disc-ID check as well.
	g_active = PSP_CoreParameter().compat.flags().VCSDynamicShadows;
}

void Shutdown() {
	g_active = false;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	memset(&s_view, 0, sizeof(s_view));
	s_sunLuminance = 0.0f;
	s_haveViewMatrix = false;
}

void BeginFrame() {
	if (!g_active) {
		return;
	}
	s_published = s_current;
	ComputeShadowView(s_published);
	s_capturePublished = s_capture;
	memset(&s_capture, 0, sizeof(s_capture));
	s_positions.clear();
	s_indices.clear();

	memset(&s_current, 0, sizeof(s_current));
	s_sunLuminance = 0.0f;
	s_haveViewMatrix = false;
}

// Finds the sun among the four GE light channels. GTA hands the hardware a directional light for
// the sun and, depending on the scene, dimmer directional fills alongside it; brightness is what
// tells them apart, so the brightest wins and the rest are ignored.
static void NoteLights() {
	if (!gstate.isLightingEnabled()) {
		return;
	}
	s_current.lightingDraws++;

	bool anyDirectional = false;
	for (int i = 0; i < 4; i++) {
		if (!gstate.isLightChanEnabled(i) || !gstate.isDirectionalLight(i)) {
			continue;
		}
		anyDirectional = true;

		// PSP colours are packed ABGR, so red is the low byte.
		const u32 diffuse = gstate.getDiffuseColor(i);
		const float r = (float)(diffuse & 0xFF) * (1.0f / 255.0f);
		const float g = (float)((diffuse >> 8) & 0xFF) * (1.0f / 255.0f);
		const float b = (float)((diffuse >> 16) & 0xFF) * (1.0f / 255.0f);
		const float luminance = r * 0.299f + g * 0.587f + b * 0.114f;
		if (s_current.sunValid && luminance <= s_sunLuminance) {
			continue;
		}

		float dir[3] = {
			getFloat24(gstate.lpos[i * 3 + 0]),
			getFloat24(gstate.lpos[i * 3 + 1]),
			getFloat24(gstate.lpos[i * 3 + 2]),
		};
		const float lengthSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
		if (lengthSq < 1e-12f) {
			// A zero direction is a light that is enabled but not meaningfully set up. Taking it
			// would give the shadow projection no axis at all, so leave the previous pick alone.
			continue;
		}
		const float invLength = 1.0f / sqrtf(lengthSq);

		s_sunLuminance = luminance;
		s_current.sunValid = true;
		s_current.sunChannel = i;
		s_current.sunDir[0] = dir[0] * invLength;
		s_current.sunDir[1] = dir[1] * invLength;
		s_current.sunDir[2] = dir[2] * invLength;
		s_current.sunDiffuse[0] = r;
		s_current.sunDiffuse[1] = g;
		s_current.sunDiffuse[2] = b;
	}

	if (anyDirectional) {
		s_current.dirLightDraws++;
	}
}

// Whether the current blend setup can actually change what is already in the framebuffer.
//
// This is the whole lesson of the first run of the counters. Testing gstate.isAlphaBlendEnabled()
// threw out 170 of 241 draws and left zero casters - the entire city. Bucketing them showed one
// setup, srcalpha/invsrcalpha/add, on 170 draws whose vertex colours were fully opaque all 170
// times. That expands to src*1 + dst*0: the game switches blending on for the opaque pass and
// specifies a blend that does nothing. Asking "is blending enabled" is the wrong question;
// asking "can this blend alter the destination" is the right one.
//
// Caveat worth writing down: on a modulating texture the final alpha is vertex alpha times
// texture alpha, and PPSSPP does not track the texture half, so a fully opaque vertex colour
// over a texture with holes reads as opaque here. That errs towards including a draw as a
// caster, which costs a slightly too-dark shadow - much cheaper than the error in the other
// direction, and alpha-tested foliage and fences are draws we want as casters anyway.
static bool BlendAltersDestination() {
	// Anything other than a straight multiply-and-add is doing something deliberate.
	if (gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_ADD) {
		return true;
	}

	const GEBlendSrcFactor funcA = gstate.getBlendFuncA();
	const GEBlendDstFactor funcB = gstate.getBlendFuncB();

	// Fixed factors: src*fixA + dst*fixB. White over black is a copy.
	if (funcA == GE_SRCBLEND_FIXA && funcB == GE_DSTBLEND_FIXB) {
		return !(gstate.getFixA() == 0xFFFFFF && gstate.getFixB() == 0x000000);
	}

	// The standard alpha blend, which is a copy whenever the source is fully opaque.
	if (funcA == GE_SRCBLEND_SRCALPHA && funcB == GE_DSTBLEND_INVSRCALPHA) {
		return !gstate_c.vertexFullAlpha;
	}

	return true;
}

// The caster filter. There is no "this is a car" bit in a display list, so everything here is
// render state, and the order matters: the first failure wins and gets the blame in the counts.
static Reject TestDraw(GEPrimitiveType prim, int vertexCount) {
	// Through mode is the 2D path - the HUD, the radar, menus. The vertices are already in
	// screen space, so there is no world transform to project from a light at all.
	if (gstate.isModeThrough()) {
		return Reject::Through;
	}

	switch (prim) {
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_TRIANGLE_STRIP:
	case GE_PRIM_TRIANGLE_FAN:
		break;
	default:
		// Lines, points and sprites. Sprites are the interesting exclusion: they are how the
		// game draws billboards, and a camera-facing quad casts a shadow that swings as you
		// turn, which reads worse than no shadow at all.
		return Reject::Primitive;
	}

	// Depth write off means the game itself considers the draw not part of the solid scene.
	// That catches particles, coronas, and - usefully - the vanilla blob shadow, which has to
	// go anyway or every ped ends up with two shadows.
	if (!gstate.isDepthWriteEnabled()) {
		return Reject::NoDepthWrite;
	}

	// Blending that genuinely composites catches glass, smoke and water. A blend that cannot
	// change the destination is not transparency at all - see BlendAltersDestination, which
	// is where the entire city was being thrown away. Alpha *testing* is deliberately not a
	// rejection either: fences and foliage are alpha-tested opaque geometry and their cut-out
	// shape is most of what makes their shadow look right.
	if (gstate.isAlphaBlendEnabled() && BlendAltersDestination()) {
		return Reject::Blended;
	}

	if (vertexCount < 3) {
		return Reject::TooFewVerts;
	}

	return Reject::None;
}

// Records what the blend setup actually was on a draw the filter threw out for blending, so the
// question "is that really transparency?" gets answered with the equation rather than a guess.
static void NoteBlend(int vertexCount) {
	if (vertexCount >= 3) {
		s_current.castersIfBlendIgnored++;
	}

	const u8 funcA = (u8)gstate.getBlendFuncA();
	const u8 funcB = (u8)gstate.getBlendFuncB();
	const u8 eq = (u8)gstate.getBlendEq();

	for (int i = 0; i < s_current.blendBucketCount; i++) {
		BlendBucket &bucket = s_current.blendBuckets[i];
		if (bucket.funcA == funcA && bucket.funcB == funcB && bucket.eq == eq) {
			bucket.count++;
			if (gstate_c.vertexFullAlpha) {
				bucket.fullAlpha++;
			}
			return;
		}
	}

	// More than kMaxBlendBuckets distinct setups in one frame would mean the game is doing
	// something far more varied than expected, and the tail is not worth growing the struct for.
	if (s_current.blendBucketCount >= kMaxBlendBuckets) {
		return;
	}
	BlendBucket &bucket = s_current.blendBuckets[s_current.blendBucketCount++];
	bucket.funcA = funcA;
	bucket.funcB = funcB;
	bucket.eq = eq;
	bucket.count = 1;
	bucket.fullAlpha = gstate_c.vertexFullAlpha ? 1 : 0;
}

Reject ClassifyDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount) {
	s_current.draws++;
	NoteLights();

	const Reject reject = TestDraw(prim, vertexCount);
	if (reject != Reject::None) {
		s_current.rejected[(int)reject]++;
		if (reject == Reject::Blended) {
			NoteBlend(vertexCount);
		}
		return reject;
	}

	s_current.casters++;
	s_current.castersIfBlendIgnored++;

	// The first caster of the frame is the one to take the camera from: by the last draw the
	// game has usually moved on to 2D and left a matrix behind that means nothing here.
	if (!s_haveViewMatrix) {
		memcpy(s_frameViewMatrix, gstate.viewMatrix, sizeof(s_frameViewMatrix));
		s_haveViewMatrix = true;
	} else if (memcmp(s_frameViewMatrix, gstate.viewMatrix, sizeof(s_frameViewMatrix)) != 0) {
		s_current.viewMatrixChanges++;
	}

	// The world matrix translation is where this draw's geometry sits in whatever space the GE
	// is being fed. Bounding it over the frame says what that space actually is.
	const float origin[3] = { gstate.worldMatrix[9], gstate.worldMatrix[10], gstate.worldMatrix[11] };
	if (!s_current.casterBoundsValid) {
		s_current.casterBoundsValid = true;
		for (int i = 0; i < 3; i++) {
			s_current.casterMin[i] = origin[i];
			s_current.casterMax[i] = origin[i];
		}
	} else {
		for (int i = 0; i < 3; i++) {
			if (origin[i] < s_current.casterMin[i]) s_current.casterMin[i] = origin[i];
			if (origin[i] > s_current.casterMax[i]) s_current.casterMax[i] = origin[i];
		}
	}
	s_current.casterVerts += vertexCount;
	if ((vertTypeID & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE) {
		s_current.castersSkinned++;
	}
	if ((vertTypeID & GE_VTYPE_NRM_MASK) != GE_VTYPE_NRM_NONE) {
		s_current.castersWithNormals++;
	}
	return Reject::None;
}

const FrameStats &LastFrameStats() {
	return s_published;
}

const char *BlendSrcName(u8 funcA) {
	static const char *kNames[] = {
		"dstcolor", "invdstcolor", "srcalpha", "invsrcalpha", "dstalpha", "invdstalpha",
		"2*srcalpha", "2*invsrcalpha", "2*dstalpha", "2*invdstalpha", "fixA",
	};
	return funcA < (u8)ARRAY_SIZE(kNames) ? kNames[funcA] : "?";
}

const char *BlendDstName(u8 funcB) {
	static const char *kNames[] = {
		"srccolor", "invsrccolor", "srcalpha", "invsrcalpha", "dstalpha", "invdstalpha",
		"2*srcalpha", "2*invsrcalpha", "2*dstalpha", "2*invdstalpha", "fixB",
	};
	return funcB < (u8)ARRAY_SIZE(kNames) ? kNames[funcB] : "?";
}

const char *BlendEqName(u8 eq) {
	static const char *kNames[] = { "add", "sub", "revsub", "min", "max", "abs" };
	return eq < (u8)ARRAY_SIZE(kNames) ? kNames[eq] : "?";
}

const char *RejectName(Reject r) {
	switch (r) {
	case Reject::None: return "caster";
	case Reject::Through: return "2D / through";
	case Reject::Primitive: return "not a triangle";
	case Reject::NoDepthWrite: return "no depth write";
	case Reject::Blended: return "alpha blended";
	case Reject::TooFewVerts: return "too few verts";
	default: return "?";
	}
}

}  // namespace VCSShadow
