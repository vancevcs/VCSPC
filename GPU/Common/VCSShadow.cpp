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

void Init() {
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	s_sunLuminance = 0.0f;

	// The flag is only set for ULUS10160 in compat.ini, so this is the disc-ID check as well.
	g_active = PSP_CoreParameter().compat.flags().VCSDynamicShadows;
}

void Shutdown() {
	g_active = false;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	s_sunLuminance = 0.0f;
}

void BeginFrame() {
	if (!g_active) {
		return;
	}
	s_published = s_current;
	memset(&s_current, 0, sizeof(s_current));
	s_sunLuminance = 0.0f;
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
