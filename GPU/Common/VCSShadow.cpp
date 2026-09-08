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
#include "Common/Log.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/ShaderWriter.h"
#include "Core/System.h"
#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/Common/VCSShadow.h"

namespace VCSShadow {

bool g_available = false;
bool g_active = false;
// Off until switched on. This is an experiment inside a working emulator: the cost of it
// being on by default is paid by someone who did not ask for it.
static bool s_enabled = false;

static FrameStats s_current;
static FrameStats s_published;

// Luminance of the brightest directional light seen so far this frame. Kept out of FrameStats
// because it is scratch for picking the sun, not something worth showing.
static float s_sunLuminance;

// Roughly three degrees. Below this there is no useful sun and no useful shadow.
static const float kMinSunElevation = 0.05f;

static ShadowView s_view;
static Settings s_settings = {
	28.0f,    // cascadeRadius, world units. GTA is roughly one unit to the metre, so this is a
	          // block or so around the player - enough for cascade 0 to prove itself.
	16.0f,    // centreDistance ahead of the camera
	1024,     // mapSize
	true,     // forwardIsNegativeZ
	480, 272, // mask size - the PSP's own resolution; the mask is sampled, not looked at
	0.0025f,  // depthBias
	false,    // flipShadowV
	0,        // debugView
	false,    // countCascadeCoverage
};

// gstate's view matrix as of the first caster of the frame. Captured rather than read at end of
// frame because the game leaves whatever matrix the last draw used in place, and the last draw is
// usually 2D.
static float s_frameViewMatrix[12];
static float s_frameProjMatrix[16];
static bool s_haveViewMatrix;

// The frame's caster geometry. Kept as std::vector so the capacity settles after a few frames
// and then stops allocating - clear() keeps the storage.
static std::vector<float> s_positions;

// thin3d's DrawIndexed is 16-bit only, so the triangle list is cut into batches of under 64k
// vertices with indices relative to each batch's first vertex. A typical frame here captures
// ~30k vertices and so produces exactly one batch; a busy one produces two.
struct Batch {
	u32 firstVertex;
	int vertexCount;
	u32 firstIndex;
	int indexCount;
};
static const int kMaxBatchVertices = 65535;
static std::vector<u16> s_indices;
static std::vector<Batch> s_batches;
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

// Row-major, row-vector: C = A * B, so a point transformed by C is the point through A then B.
void Mul4x4(const float a[16], const float b[16], float out[16]) {
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += a[row * 4 + k] * b[k * 4 + col];
			}
			out[row * 4 + col] = sum;
		}
	}
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

	// World to camera clip. The 4x3 view matrix widens to 4x4 with the translation staying in the
	// last row, exactly as ConvertMatrix4x3To4x4 does it, and then meets the projection matrix.
	{
		const float *v = s_frameViewMatrix;
		const float view4[16] = {
			v[0], v[1], v[2],  0.0f,
			v[3], v[4], v[5],  0.0f,
			v[6], v[7], v[8],  0.0f,
			v[9], v[10], v[11], 1.0f,
		};
		Mul4x4(view4, s_frameProjMatrix, s_view.cameraViewProj);
	}

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

	// A single caster never approaches 64k vertices - the largest measured here is about a
	// thousand - so starting a fresh batch whenever this one would overflow is always enough.
	if (s_batches.empty() || s_batches.back().vertexCount + numDecodedVerts > kMaxBatchVertices) {
		Batch fresh;
		fresh.firstVertex = (u32)(s_positions.size() / 3);
		fresh.vertexCount = 0;
		fresh.firstIndex = (u32)s_indices.size();
		fresh.indexCount = 0;
		s_batches.push_back(fresh);
	}
	Batch &batch = s_batches.back();

	// Two different bases, and mixing them up costs a frame of confusion: indices are relative to
	// the batch because they are 16-bit, while the position write goes to the end of the shared
	// buffer. They coincide while there is only one batch, which is most frames - so this only
	// ever breaks once a scene gets busy enough to need a second one.
	const u32 base = (u32)batch.vertexCount;
	const size_t writeOffset = s_positions.size();
	s_positions.resize(s_positions.size() + (size_t)numDecodedVerts * 3);
	float *out = s_positions.data() + writeOffset;

	// Row-vector, matching the vertex shader: world = vec4(pos, 1.0) * m, with the 4x3 matrix
	// holding its translation in the last row. The decoded position is always three floats -
	// the decoder guarantees that regardless of how the game encoded it - and for a skinned mesh
	// the bones have already been folded in here, which is what gets peds into pose for free.
	float drawMin[3] = { 1e30f, 1e30f, 1e30f };
	float drawMax[3] = { -1e30f, -1e30f, -1e30f };

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
		for (int c = 0; c < 3; c++) {
			if (v[c] < drawMin[c]) drawMin[c] = v[c];
			if (v[c] > drawMax[c]) drawMax[c] = v[c];
		}
	}

	// 500 units is far larger than any building here and far smaller than the map, so this
	// separates "one map-spanning ground or water quad" from "positions are being read at the
	// wrong stride". A bounding box cannot tell those apart; this count can.
	if (drawMax[0] - drawMin[0] > 500.0f || drawMax[1] - drawMin[1] > 500.0f) {
		s_capture.largeDraws++;
		if (numDecodedVerts > s_capture.largestDrawVerts) {
			s_capture.largestDrawVerts = numDecodedVerts;
		}
	}

	// Everything becomes a triangle list, because the whole cascade is going out as a single
	// draw and one draw cannot carry three topologies. Strips and fans are cheap to expand and
	// the alternative is a draw call per caster.
	const auto index = [&](int i) -> u16 {
		return (u16)(base + (indices ? (u32)indices[i] : (u32)i));
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

	batch.vertexCount += numDecodedVerts;
	batch.indexCount = (int)(s_indices.size() - batch.firstIndex);

	s_capture.draws++;
	s_capture.batches = (int)s_batches.size();
	s_capture.vertices = (int)(s_positions.size() / 3);
	s_capture.indices = (int)s_indices.size();
	s_capture.bytes = s_positions.size() * sizeof(float) + s_indices.size() * sizeof(u16);
}

const CaptureStats &LastCapture() {
	return s_capturePublished;
}

Settings &GetSettings() {
	return s_settings;
}

const ShadowView &View() {
	return s_view;
}

// --- the depth pass ---------------------------------------------------------------------------
//
// One render pass, one pipeline, and one draw per 64k vertices. All of the complexity that a
// shadow pass normally carries - a pipeline per vertex format, per-draw world matrices, buffer
// lifetimes - was spent up front in the capture instead, which bakes everything to world space
// and flattens it to a single triangle list.

struct ShadowUB {
	float lightViewProj[16];
};

static const UniformBufferDesc s_shadowUBDesc{ sizeof(ShadowUB), {
	{ "u_lightViewProj", 0, -1, UniformType::MATRIX4X4, 0 },
} };

static Draw::Framebuffer *s_fbo;
static Draw::Pipeline *s_pipeline;
static int s_fboSize;
static bool s_depthSetupFailed;

static void ReleaseResources() {
	if (s_pipeline) {
		s_pipeline->Release();
		s_pipeline = nullptr;
	}
	if (s_fbo) {
		s_fbo->Release();
		s_fbo = nullptr;
	}
	s_fboSize = 0;
}

static bool EnsureResources(Draw::DrawContext *draw) {
	if (s_fbo && s_pipeline && s_fboSize == s_settings.mapSize) {
		return true;
	}
	// Setup that failed once will fail identically every frame. Retrying it rebuilds shaders at
	// frame rate, which costs far more than the feature it is trying to enable.
	if (s_depthSetupFailed) {
		return false;
	}
	ReleaseResources();

	using namespace Draw;
	const int size = s_settings.mapSize;

	// thin3d always gives a colour attachment alongside the depth one. We never write to it -
	// the blend state masks every channel off - so it costs memory and nothing else.
	s_fbo = draw->CreateFramebuffer({ size, size, 1, 1, 0, true, "vcs_shadow" });
	if (!s_fbo) {
		return false;
	}
	s_fboSize = size;

	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();

	char *vsCode = new char[4096];
	{
		ShaderWriter writer(vsCode, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec3", "a_position", Draw::SEM_POSITION } };
		static const UniformDef uniforms[] = { { "mat4", "u_lightViewProj", 0 } };
		writer.BeginVSMain(inputs, uniforms, Slice<VaryingDef>::empty());
		// Row-vector, same as every other matrix in this file and the same as the game's own
		// vertex path. ShaderWriter's mul() is defined as (x * y), which in GLSL is exactly that.
		writer.C("  gl_Position = mul(vec4(a_position, 1.0), u_lightViewProj);\n");
		writer.EndVSMain(Slice<VaryingDef>::empty());
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)vsCode, strlen(vsCode), "vcs_shadow_vs");
	delete[] vsCode;

	char *fsCode = new char[4096];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.BeginFSMain(Slice<UniformDef>::empty(), Slice<VaryingDef>::empty());
		// Nothing is written here. The pass exists for the depth it leaves behind, but a pipeline
		// still needs a fragment stage.
		writer.C("  vec4 outColor = vec4(1.0, 1.0, 1.0, 1.0);\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *fs = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "vcs_shadow_fs");
	delete[] fsCode;

	if (!vs || !fs) {
		if (vs) vs->Release();
		if (fs) fs->Release();
		ReleaseResources();
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		12,
		{ { SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 } },
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);

	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = true;
	dsDesc.depthWriteEnabled = true;
	dsDesc.depthCompare = Comparison::LESS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);

	// No colour writes at all.
	BlendState *blend = draw->CreateBlendState({ false, 0x0 });

	// Deliberately no face culling for now. Culling back faces is the usual first move against
	// shadow acne, but it depends on the PSP's winding matching the host's front-face convention,
	// and that is worth confirming against a working map rather than assuming into a broken one.
	RasterState *raster = draw->CreateRasterState({});

	PipelineDesc desc{
		Primitive::TRIANGLE_LIST,
		{ vs, fs },
		inputLayout,
		depthStencil,
		blend,
		raster,
		&s_shadowUBDesc,
	};
	s_pipeline = draw->CreateGraphicsPipeline(desc, "vcs_shadow");

	vs->Release();
	fs->Release();
	inputLayout->Release();
	depthStencil->Release();
	blend->Release();
	raster->Release();

	if (!s_pipeline) {
		ReleaseResources();
		s_depthSetupFailed = true;
		return false;
	}
	return true;
}

static void RenderCascade(Draw::DrawContext *draw) {
	s_capture.rendered = false;
	if (!draw || !s_view.valid || s_batches.empty()) {
		return;
	}
	if (!EnsureResources(draw)) {
		return;
	}

	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(s_fbo,
		{ RPAction::CLEAR, RPAction::CLEAR, RPAction::DONT_CARE, 0, 1.0f, 0, "vcs_shadow" },
		"vcs_shadow");

	const float size = (float)s_fboSize;
	Viewport viewport{ 0.0f, 0.0f, size, size, 0.0f, 1.0f };
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, s_fboSize, s_fboSize);
	draw->BindPipeline(s_pipeline);

	// Transpose on the way into the uniform. lightViewProj is row-major, because that is the
	// convention everything else in this file reasons in and what the panel prints - but GLSL
	// reads a mat4 uniform column-major, so mul(v, M) there is really v times the transpose.
	// PPSSPP hits the same wall and solves it the same way: every matrix it hands its own vertex
	// shader goes through ConvertMatrix4x3To3x4Transposed first. Uploading this raw scrambles the
	// basis, which draws as long thin triangles radiating from a point rather than as a scene.
	// Count what actually falls in the box, before handing the same matrix to the GPU.
	s_capture.verticesInCascade = -1;
	if (s_settings.countCascadeCoverage) {
		const float *m = s_view.lightViewProj;
		int inside = 0;
		const size_t count = s_positions.size() / 3;
		for (size_t i = 0; i < count; i++) {
			const float *p = s_positions.data() + i * 3;
			const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
			const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
			const float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
			if (x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f && z >= 0.0f && z <= 1.0f) {
				inside++;
			}
		}
		s_capture.verticesInCascade = inside;
	}

	ShadowUB ub;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			ub.lightViewProj[col * 4 + row] = s_view.lightViewProj[row * 4 + col];
		}
	}
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	for (const Batch &batch : s_batches) {
		if (batch.indexCount < 3) {
			continue;
		}
		draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3, batch.vertexCount,
			s_indices.data() + batch.firstIndex, batch.indexCount);
	}
	s_capture.rendered = true;
}

Draw::Framebuffer *ShadowMap() {
	return s_fbo;
}

// --- the shadow mask --------------------------------------------------------------------------
//
// A screen-space mask of what the sun reaches, rendered by drawing the captured casters a second
// time from the camera. The obvious alternative - reconstruct world position from the game's
// depth buffer - was rejected on inspection: PPSSPP rewrites gl_Position.z through u_minZmaxZ and
// a further doubling step depending on backend and render state, so reconstruction would mean
// replicating a moving target and would break in ways that look like shadow bugs. Drawing the
// geometry again costs one extra pass over ~32k triangles, which is nothing, and hands the
// fragment shader an exact world position as a varying instead of a reconstructed one.

struct MaskUB {
	float cameraViewProj[16];
	float lightViewProj[16];
	float params[4];   // bias, flipV, unused, unused
};

static const UniformBufferDesc s_maskUBDesc{ sizeof(MaskUB), {
	{ "u_cameraViewProj", 0, -1, UniformType::MATRIX4X4, 0 },
	{ "u_lightViewProj", 1, 0, UniformType::MATRIX4X4, 64 },
	{ "u_shadowParams", 2, 1, UniformType::FLOAT4, 128 },
} };

static Draw::Framebuffer *s_maskFbo;
static Draw::Pipeline *s_maskPipeline;
static Draw::SamplerState *s_maskSampler;
static int s_maskWidth;
static int s_maskHeight;
static bool s_maskSetupFailed;

static void ReleaseMaskResources() {
	if (s_maskPipeline) {
		s_maskPipeline->Release();
		s_maskPipeline = nullptr;
	}
	if (s_maskSampler) {
		s_maskSampler->Release();
		s_maskSampler = nullptr;
	}
	if (s_maskFbo) {
		s_maskFbo->Release();
		s_maskFbo = nullptr;
	}
	s_maskWidth = 0;
	s_maskHeight = 0;
}

static bool EnsureMaskResources(Draw::DrawContext *draw) {
	const int width = s_settings.maskWidth;
	const int height = s_settings.maskHeight;
	if (s_maskFbo && s_maskPipeline && s_maskWidth == width && s_maskHeight == height) {
		s_capture.maskStep = CaptureStats::MaskStep::Ok;
		return true;
	}
	if (s_maskSetupFailed) {
		s_capture.maskStep = CaptureStats::MaskStep::Pipeline;
		return false;
	}
	ReleaseMaskResources();

	using namespace Draw;
	s_maskFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, true, "vcs_shadow_mask" });
	if (!s_maskFbo) {
		s_capture.maskStep = CaptureStats::MaskStep::Framebuffer;
		s_maskSetupFailed = true;
		return false;
	}
	s_maskWidth = width;
	s_maskHeight = height;

	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	static const SamplerDef samplers[] = { { 0, "shadowMap", SamplerFlags(0) } };
	// The light-space position is computed per vertex, not per fragment. Two reasons, one of them
	// forced: mul() is a ShaderWriter #define emitted in the vertex and geometry preambles but not
	// the fragment one, so a matrix multiply in a fragment shader simply will not compile on
	// Vulkan GLSL. The other is that it is cheaper - once a vertex rather than once a pixel - and
	// costs nothing in accuracy, because the light projection is orthographic and therefore
	// affine, so interpolating its output linearly is exact.
	static const VaryingDef varyings[] = {
		{ "vec3", "v_lightClip", Draw::SEM_TEXCOORD0, 0, "highp" },
	};

	// Every uniform is declared in both stages, even though each stage only reads some of them.
	// On Vulkan the block is a single descriptor shared by the two shaders, so declaring different
	// subsets generates two different block layouts for one binding and the pipeline will not
	// build. Unused members cost nothing; a mismatched block costs the whole pass.
	static const UniformDef uniforms[] = {
		{ "mat4", "u_cameraViewProj", 0 },
		{ "mat4", "u_lightViewProj", 1 },
		{ "vec4", "u_shadowParams", 2 },
	};

	const size_t kShaderBufferSize = 8192;
	char *vsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(vsCode, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec3", "a_position", Draw::SEM_POSITION } };
		writer.BeginVSMain(inputs, uniforms, varyings);
		writer.C("  v_lightClip = mul(vec4(a_position, 1.0), u_lightViewProj).xyz;\n");
		writer.C("  gl_Position = mul(vec4(a_position, 1.0), u_cameraViewProj);\n");
		writer.EndVSMain(varyings);
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)vsCode, strlen(vsCode), "vcs_mask_vs");
	if (!vs) {
		ERROR_LOG(Log::G3D, "VCS mask VS (%d bytes) failed:\n%s", (int)strlen(vsCode), vsCode);
	}
	delete[] vsCode;

	char *fsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(samplers);
		writer.BeginFSMain(uniforms, varyings);
		writer.C("  vec2 shadowUV = v_lightClip.xy * 0.5 + 0.5;\n");
		// Which way up the shadow map's V axis runs depends on the backend's clip convention, and
		// getting it wrong puts the right shapes in the wrong places. Left as a toggle.
		writer.C("  if (u_shadowParams.y > 0.5) { shadowUV.y = 1.0 - shadowUV.y; }\n");
		// Sampled unconditionally and clamped, so the debug views below show something even where
		// the bounds test would have rejected the fragment.
		writer.C("  vec2 clampedUV = clamp(shadowUV, 0.0, 1.0);\n");
		writer.C("  float mapDepth = ").SampleTexture2D("shadowMap", "clampedUV").C(".r;\n");
		writer.C("  float lit = 1.0;\n");
		writer.C("  if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 && shadowUV.y >= 0.0 && shadowUV.y <= 1.0 &&\n");
		writer.C("      v_lightClip.z >= 0.0 && v_lightClip.z <= 1.0) {\n");
		writer.C("    if (v_lightClip.z - u_shadowParams.x > mapDepth) { lit = 0.0; }\n");
		writer.C("  }\n");
		// The debug views. An all-black mask has at least two causes that look identical from
		// outside - the sample returning nothing, or the comparison being the wrong way round -
		// and these separate them in one run rather than one per guess.
		writer.C("  vec4 outColor = vec4(lit, lit, lit, 1.0);\n");
		writer.C("  if (u_shadowParams.z > 0.5 && u_shadowParams.z < 1.5) {\n");
		writer.C("    outColor = vec4(mapDepth, mapDepth, mapDepth, 1.0);\n");
		writer.C("  } else if (u_shadowParams.z > 1.5 && u_shadowParams.z < 2.5) {\n");
		writer.C("    outColor = vec4(v_lightClip.z, v_lightClip.z, v_lightClip.z, 1.0);\n");
		writer.C("  } else if (u_shadowParams.z > 2.5) {\n");
		writer.C("    outColor = vec4(clampedUV.x, clampedUV.y, 0.0, 1.0);\n");
		writer.C("  }\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *fs = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "vcs_mask_fs");
	if (!fs) {
		ERROR_LOG(Log::G3D, "VCS mask FS (%d bytes) failed:\n%s", (int)strlen(fsCode), fsCode);
	}
	delete[] fsCode;

	if (!vs || !fs) {
		// Log the source, not just the failure - a shader that will not compile is only
		// diagnosable if you can see what was handed to the compiler.
		s_capture.maskStep = vs ? CaptureStats::MaskStep::FragmentShader
			: CaptureStats::MaskStep::VertexShader;
		ERROR_LOG(Log::G3D, "VCS shadow mask shader failed to compile (%s)",
			vs ? "fragment" : "vertex");
		if (vs) vs->Release();
		if (fs) fs->Release();
		ReleaseMaskResources();
		s_maskSetupFailed = true;
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		12,
		{ { SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 } },
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);

	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = true;
	dsDesc.depthWriteEnabled = true;
	dsDesc.depthCompare = Comparison::LESS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);
	BlendState *blend = draw->CreateBlendState({ false, 0xF });
	RasterState *raster = draw->CreateRasterState({});

	SamplerStateDesc sampDesc{};
	sampDesc.magFilter = TextureFilter::NEAREST;
	sampDesc.minFilter = TextureFilter::NEAREST;
	sampDesc.mipFilter = TextureFilter::NEAREST;
	sampDesc.wrapU = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapV = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	s_maskSampler = draw->CreateSamplerState(sampDesc);

	PipelineDesc desc{
		Primitive::TRIANGLE_LIST,
		{ vs, fs },
		inputLayout,
		depthStencil,
		blend,
		raster,
		&s_maskUBDesc,
		samplers,
	};
	s_maskPipeline = draw->CreateGraphicsPipeline(desc, "vcs_shadow_mask");

	vs->Release();
	fs->Release();
	inputLayout->Release();
	depthStencil->Release();
	blend->Release();
	raster->Release();

	if (!s_maskPipeline) {
		s_capture.maskStep = CaptureStats::MaskStep::Pipeline;
		ERROR_LOG(Log::G3D, "VCS shadow mask pipeline failed to create");
		ReleaseMaskResources();
		s_maskSetupFailed = true;
		return false;
	}
	s_capture.maskStep = CaptureStats::MaskStep::Ok;
	return true;
}

static void RenderMask(Draw::DrawContext *draw) {
	s_capture.maskRendered = false;
	s_capture.maskStep = CaptureStats::MaskStep::NotAttempted;
	if (!s_capture.rendered || !s_fbo || s_batches.empty()) {
		return;
	}
	if (!EnsureMaskResources(draw)) {
		return;
	}

	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(s_maskFbo,
		{ RPAction::CLEAR, RPAction::CLEAR, RPAction::DONT_CARE, 0xFFFFFFFF, 1.0f, 0, "vcs_mask" },
		"vcs_mask");
	draw->BindFramebufferAsTexture(s_fbo, 0, Aspect::DEPTH_BIT, 0);
	draw->BindSamplerStates(0, 1, &s_maskSampler);

	Viewport viewport{ 0.0f, 0.0f, (float)s_maskWidth, (float)s_maskHeight, 0.0f, 1.0f };
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, s_maskWidth, s_maskHeight);
	draw->BindPipeline(s_maskPipeline);

	MaskUB ub;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			ub.cameraViewProj[col * 4 + row] = s_view.cameraViewProj[row * 4 + col];
			ub.lightViewProj[col * 4 + row] = s_view.lightViewProj[row * 4 + col];
		}
	}
	ub.params[0] = s_settings.depthBias;
	ub.params[1] = s_settings.flipShadowV ? 1.0f : 0.0f;
	ub.params[2] = (float)s_settings.debugView;
	ub.params[3] = 0.0f;
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	for (const Batch &batch : s_batches) {
		if (batch.indexCount < 3) {
			continue;
		}
		draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3, batch.vertexCount,
			s_indices.data() + batch.firstIndex, batch.indexCount);
	}
	s_capture.maskRendered = true;
}

Draw::Framebuffer *ShadowMask() {
	return s_maskFbo;
}

void Init() {
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	s_sunLuminance = 0.0f;
	memset(&s_view, 0, sizeof(s_view));
	s_haveViewMatrix = false;

	// The flag is only set for ULUS10160 in compat.ini, so this is the disc-ID check as well.
	g_available = PSP_CoreParameter().compat.flags().VCSDynamicShadows;
	g_active = g_available && s_enabled;
}

void SetEnabled(bool enabled) {
	s_enabled = enabled;
	g_active = g_available && s_enabled;
	if (enabled) {
		// Switching it back on is the one place a fresh attempt makes sense - the alternative is
		// a latched failure that can only be cleared by restarting the emulator.
		s_depthSetupFailed = false;
		s_maskSetupFailed = false;
	}
	if (!g_active) {
		// Drop the frame's work immediately rather than leaving a stale map on screen in the
		// debugger and a buffer full of geometry nobody will draw.
		s_positions.clear();
		s_indices.clear();
		s_batches.clear();
		memset(&s_capture, 0, sizeof(s_capture));
		memset(&s_capturePublished, 0, sizeof(s_capturePublished));
	}
}

bool IsEnabled() {
	return s_enabled;
}

void Shutdown() {
	ReleaseResources();
	ReleaseMaskResources();
	s_depthSetupFailed = false;
	s_maskSetupFailed = false;
	g_available = false;
	g_active = false;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	memset(&s_view, 0, sizeof(s_view));
	s_sunLuminance = 0.0f;
	s_haveViewMatrix = false;
}

void BeginFrame(Draw::DrawContext *draw) {
	if (!g_active) {
		return;
	}
	s_published = s_current;
	ComputeShadowView(s_published);

	// Project and render before anything is cleared, so the matrix, the sun and the geometry all
	// belong to the same frame.
	//
	// This deliberately does NOT run from the draw engine's BeginFrame. GPU_Vulkan::BeginHostFrame
	// calls that before framebufferManager_->BeginFrame, so binding a render target there happens
	// before the framebuffer manager has started its frame - behind its back and too early. The
	// call site is now immediately after that, and immediately before the Dirty(DIRTY_ALL) that
	// makes the game re-establish all of its own state, so nothing this binds can leak into it.
	RenderCascade(draw);
	RenderMask(draw);

	s_capturePublished = s_capture;
	memset(&s_capture, 0, sizeof(s_capture));
	s_positions.clear();
	s_indices.clear();
	s_batches.clear();

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

		// Record every candidate, whether or not it wins, so the panel can show what the game
		// actually offered rather than only what got chosen.
		{
			FrameStats::SunCandidate &candidate = s_current.sunCandidates[i];
			candidate.valid = true;
			candidate.draws++;
			for (int c = 0; c < 3; c++) {
				const float normalized = dir[c] * invLength;
				if (candidate.draws > 1 && fabsf(normalized - candidate.dir[c]) > 0.01f) {
					candidate.directionChanges++;
				}
				candidate.dir[c] = normalized;
			}
			candidate.diffuse[0] = r;
			candidate.diffuse[1] = g;
			candidate.diffuse[2] = b;
			candidate.aboveHorizon = candidate.dir[2] > kMinSunElevation;
		}

		// The sun is above the horizon. Z is up here, so a light pointing level or below is not
		// the sun whatever its colour, and taking one produces a shadow map that reads as an
		// elevation of the city rather than a view of it from the sky. Every genuine sun this has
		// measured sat between 0.17 and 0.54 in Z; the impostor sat at exactly 0.
		if (dir[2] * invLength <= kMinSunElevation) {
			continue;
		}

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
		memcpy(s_frameProjMatrix, gstate.projMatrix, sizeof(s_frameProjMatrix));
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
