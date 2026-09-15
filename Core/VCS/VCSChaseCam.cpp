// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

#include "Common/Input/KeyCodes.h"
#include "Common/Log.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSChaseCam.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSMips.h"

namespace VCS {

namespace {

const float kPi = 3.14159265f;
const float kTwoPi = 6.28318531f;

// The game's own orbit angles are only written on foot, and pitch only inside the band the on-foot
// camera has always accepted. They exist there so aiming starts from the view the player had.
const float kGamePitchLimit = 0.7f;

struct CamVec {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

CamVec Add(CamVec a, CamVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CamVec Sub(CamVec a, CamVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CamVec Mul(CamVec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(CamVec a, CamVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(CamVec a) { return std::sqrt(Dot(a, a)); }
CamVec Lerp(CamVec a, CamVec b, float t) { return Add(a, Mul(Sub(b, a), t)); }

CamVec Normalize(CamVec a, CamVec fallback) {
	const float len = Length(a);
	return len > 1e-5f ? Mul(a, 1.0f / len) : fallback;
}

// `up` made exactly perpendicular to `front`, which the game's matrix build assumes and does not do.
CamVec Orthonormal(CamVec up, CamVec front, CamVec fallback) {
	return Normalize(Sub(up, Mul(front, Dot(up, front))), fallback);
}

float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

float WrapPi(float a) {
	a = std::fmod(a + kPi, kTwoPi);
	if (a < 0.0f) {
		a += kTwoPi;
	}
	return a - kPi;
}

float SmoothStep(float t) {
	t = Clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// The fraction of a gap an exponential ease closes in dt.
float EaseFactor(float rate, float dt) {
	return rate > 0.0f ? 1.0f - std::exp(-rate * dt) : 1.0f;
}

std::optional<CamVec> ReadVec(u32 address) {
	const std::optional<float> x = ReadFloat(address);
	const std::optional<float> y = ReadFloat(address + 4);
	const std::optional<float> z = ReadFloat(address + 8);
	if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
		return std::nullopt;
	}
	return CamVec{*x, *y, *z};
}

bool WriteVec(u32 address, CamVec v) {
	return WriteFloat(address, v.x) && WriteFloat(address + 4, v.y) && WriteFloat(address + 8, v.z);
}

// --- The block ------------------------------------------------------------------------------------
//
//   +0x000  magic
//   +0x004  ray count this frame - written by the prepare hook, zero whenever it declines
//   +0x008  ProcessLineOfSight's flags: $t0..$t3, then the five stack words
//   +0x02c  a look command from outside - see kOffCommand
//   +0x040  the rays, kRayStride each:
//             +0x00 start   +0x10 end   +0x20 CColPoint   +0x48 entity out   +0x4c hit
//   +0x240  the program
const u32 kMagic = 0x4D414356;  // "VCAM"
const u32 kOffMagic = 0x000;
const u32 kOffCount = 0x004;
const u32 kOffFlags = 0x008;
// A look command, for a test with nobody at the mouse. There is no input a script can send that
// turns this camera - the debugger's analog injection goes straight to the PSP stick - so the block
// takes one instead: write the angles, then the flags, over the WebSocket debugger's memory.write.
// Bit 0 sets the yaw at +0x30, bit 1 the pitch at +0x34, both in radians, and the prepare hook clears
// the flags once it has taken them. The block's address is in the log line the install writes.
const u32 kOffCommand = 0x02c;
const u32 kOffCommandYaw = 0x030;
const u32 kOffCommandPitch = 0x034;
// The near clip plane the finish hook wants this frame, and whether it wants one at all. The program
// calls the game's own setter with it - see kVCSRwCameraSetNearClip.
const u32 kOffNear = 0x038;
const u32 kOffNearSet = 0x03c;
const u32 kOffRays = 0x040;
const u32 kRayStride = 0x60;
const u32 kRayStart = 0x00;
const u32 kRayEnd = 0x10;
const u32 kRayCol = 0x20;
const u32 kRayEntity = 0x48;
const u32 kRayHit = 0x4c;
const int kMaxRays = 5;
const u32 kOffCode = 0x240;
const u32 kBlockSize = 0x400;

// The program the patched jal lands in. At entry $a0..$a2 hold the cross product's arguments and $ra
// points back into the camera update; the program leaves both exactly as it found them and tail-calls
// the cross product, so from the game's side the jal simply did what it always did.
//
// It keeps nothing but $s0 (the block) and $s1 (the ray counter) of its own, and saves both. The
// caller's $fp, $s6 and $s7 - the three vectors the finish hook writes - are callee-saved, so neither
// this program nor ProcessLineOfSight moves them.
std::vector<u32> BuildProgram(u32 block, int *finishIndex) {
	std::vector<u32> c;
	auto emit = [&c](u32 op) {
		c.push_back(op);
		return (int)c.size() - 1;
	};

	emit(Addiu(kRegSP, kRegSP, -0x40));
	emit(Sw(kRegRA, kRegSP, 0x20));
	emit(Sw(kRegA0, kRegSP, 0x24));
	emit(Sw(kRegA1, kRegSP, 0x28));
	emit(Sw(kRegA2, kRegSP, 0x2c));
	emit(Sw(kRegS0, kRegSP, 0x30));
	emit(Sw(kRegS1, kRegSP, 0x34));
	emit(Lui(kRegS0, block >> 16));
	emit(Ori(kRegS0, kRegS0, block & 0xffff));
	emit(Lw(kRegS1, kRegS0, kOffCount));

	const int loop = emit(Nop());  // beq $s1, $zero, done
	emit(Nop());
	emit(Addiu(kRegS1, kRegS1, -1));
	// $t4 = block + s1 * 0x60
	emit(Sll(kRegT4, kRegS1, 5));
	emit(Sll(kRegT5, kRegS1, 6));
	emit(Addu(kRegT4, kRegT4, kRegT5));
	emit(Addu(kRegT4, kRegT4, kRegS0));
	emit(Addiu(kRegA0, kRegT4, (int)(kOffRays + kRayStart)));
	emit(Addiu(kRegA1, kRegT4, (int)(kOffRays + kRayEnd)));
	emit(Addiu(kRegA2, kRegT4, (int)(kOffRays + kRayCol)));
	emit(Addiu(kRegA3, kRegT4, (int)(kOffRays + kRayEntity)));
	emit(Lw(kRegT0, kRegS0, kOffFlags + 0x00));
	emit(Lw(kRegT1, kRegS0, kOffFlags + 0x04));
	emit(Lw(kRegT2, kRegS0, kOffFlags + 0x08));
	emit(Lw(kRegT3, kRegS0, kOffFlags + 0x0c));
	for (int i = 0; i < 4; i++) {
		emit(Lw(kRegV0, kRegS0, (int)kOffFlags + 0x10 + i * 4));
		emit(Sw(kRegV0, kRegSP, i * 4));
	}
	emit(Lw(kRegV0, kRegS0, kOffFlags + 0x20));
	emit(Jal(kVCSProcessLineOfSight));
	emit(Sw(kRegV0, kRegSP, 0x10));  // delay slot: runs before the call, with the fifth flag
	emit(Sll(kRegT4, kRegS1, 5));
	emit(Sll(kRegT5, kRegS1, 6));
	emit(Addu(kRegT4, kRegT4, kRegT5));
	emit(Addu(kRegT4, kRegT4, kRegS0));
	const int back = emit(Nop());  // b loop
	emit(Sw(kRegV0, kRegT4, (int)(kOffRays + kRayHit)));

	const int done = emit(Nop());  // the finish hook replaces this nop
	// The near clip plane, through the game's own setter, on the frames the finish hook asked for one.
	emit(Lw(kRegT4, kRegS0, kOffNearSet));
	const int noNear = emit(Nop());  // beq $t4, $zero, skip
	emit(Nop());
	emit(Lui(kRegA0, kVCSSceneCameraPtr >> 16));
	emit(Lw(kRegA0, kRegA0, (int)(kVCSSceneCameraPtr & 0xffff)));
	const int noCamera = emit(Nop());  // beq $a0, $zero, skip
	emit(Nop());
	emit(Jal(kVCSRwCameraSetNearClip));
	emit(Lwc1(kRegF12, kRegS0, kOffNear));  // delay slot: the argument
	const int skip = (int)c.size();
	emit(Lw(kRegA0, kRegSP, 0x24));
	emit(Lw(kRegA1, kRegSP, 0x28));
	emit(Lw(kRegA2, kRegSP, 0x2c));
	emit(Lw(kRegS0, kRegSP, 0x30));
	emit(Lw(kRegS1, kRegSP, 0x34));
	emit(Lw(kRegRA, kRegSP, 0x20));
	emit(J(kVCSCamBasisFunc));
	emit(Addiu(kRegSP, kRegSP, 0x40));

	c[loop] = Beq(kRegS1, kRegZero, done - (loop + 1));
	c[back] = B(loop - (back + 1));
	c[noNear] = Beq(kRegT4, kRegZero, skip - (noNear + 1));
	c[noCamera] = Beq(kRegA0, kRegZero, skip - (noCamera + 1));
	*finishIndex = done;
	return c;
}

enum class CamKind {
	None,
	OnFoot,
	Vehicle,
};

const char *KindName(CamKind kind) {
	switch (kind) {
	case CamKind::OnFoot: return "on foot";
	case CamKind::Vehicle: return "vehicle";
	default: return "-";
	}
}

// --- Installation ---
u32 g_block = 0;
bool g_hooked = false;
u32 g_finishAddr = 0;
const char *g_status = "not installed";

// --- The view the player asked for ---
float g_yaw = 0.0f;      // the direction the camera looks: atan2(Front.y, Front.x)
float g_pitch = 0.0f;    // up is positive: asin(Front.z)
float g_sinceLook = 1e3f;

// --- One frame, carried from the prepare hook to the finish hook ---
bool g_prepared = false;
CamKind g_kind = CamKind::None;
int g_mode = -1;
float g_dt = 0.0f;
CamVec g_pivot;
CamVec g_front;
CamVec g_up;
// The direction from the camera to the pivot, which is the view direction except where a vehicle's
// orbit is capped - see vehicleOrbitUp.
CamVec g_orbit;
float g_clearance = 0.0f;
float g_near = 0.0f;
CamVec g_preSource;
CamVec g_preFront;
CamVec g_preUp;
float g_wantDistance = 0.0f;
int g_rayCount = 0;

// --- Kept between frames ---
bool g_owned = false;
CamKind g_lastKind = CamKind::None;
CamVec g_lastPivot;
CamVec g_pivotOffset;
float g_gameDistance = 0.0f;
float g_vehicleLength = 4.5f;
float g_distance = 0.0f;
float g_allowed = 0.0f;
int g_hits = 0;
float g_blend = 1.0f;
bool g_wasGlancing = false;
bool g_glancing = false;

// What the finish hook last wrote, which a blend out starts from.
CamVec g_outSource;
CamVec g_outFront;
CamVec g_outUp;
bool g_blendingOut = false;
float g_blendOutT = 0.0f;

u32 g_vehicle = 0;
CamVec g_vehiclePos;
bool g_haveVehiclePos = false;
float g_speed = 0.0f;

u64 g_frames = 0;
u64 g_ownedFrames = 0;

// A dev hatch, read once: VCS_CAM_TRACE=1 writes one NOTICE line a game second - mode, distances,
// hits - which is how a run with nobody watching the screen finds out what the camera did.
bool g_envRead = false;
bool g_trace = false;
float g_traceTimer = 0.0f;

void ReadEnvironment() {
	if (g_envRead) {
		return;
	}
	g_envRead = true;
	if (const char *trace = getenv("VCS_CAM_TRACE")) {
		g_trace = atoi(trace) != 0;
	}
}

void PitchLimits(CamKind kind, float *lo, float *hi) {
	const VCSChaseCamSettings &s = ChaseCamSettings();
	const float up = kind == CamKind::Vehicle ? s.vehicleLookUp : s.footLookUp;
	const float down = kind == CamKind::Vehicle ? s.vehicleLookDown : s.footLookDown;
	// Never quite vertical: Up is built from the pitch and degenerates at +/-90 degrees.
	*lo = -Clamp(down, 0.0f, 1.5f);
	*hi = Clamp(up, 0.0f, 1.5f);
}

// Seconds of game time in the frame being processed. TimeStep is in 50fps units - 1.668 at this
// game's 30fps - and 0.0 at the menu, which correctly stops every ease.
float GameSeconds() {
	const float step = ReadAddrFloat(VCSAddr::TimeStep).value_or(0.0f);
	if (!(step > 0.0f)) {
		return 0.0f;
	}
	return Clamp(step, 0.0f, 5.0f) / 50.0f;
}

u32 ActiveCam() {
	u8 index = ReadU8(kVCSCCamera + kVCSCCameraActiveCamOffset).value_or(0);
	if (index > 2) {
		index = 0;
	}
	return kVCSCCamera + kVCSCCameraCamsOffset + index * kVCSCamStride;
}

// The vehicle's collision box, which is what mode 18 frames the vehicle by: its top for the pivot,
// its length for the distance.
struct VehicleBox {
	CamVec min{-1.0f, -2.25f, -0.6f};
	CamVec max{1.0f, 2.25f, 1.0f};
	float top = 1.0f;
	float length = 4.5f;
};

std::optional<VehicleBox> ReadVehicleBox() {
	const std::optional<u32> model = ReadAddrAsU32(VCSAddr::VehicleModel);
	const std::optional<u32> table = ReadPointer(kVCSModelInfoTablePtr);
	if (!model || !table || *model > 0x2000) {
		return std::nullopt;
	}
	const std::optional<u32> info = ReadPointer(*table + *model * 4);
	if (!info) {
		return std::nullopt;
	}
	const std::optional<u32> col = ReadPointer(*info + kVCSModelInfoColModelOffset);
	if (!col) {
		return std::nullopt;
	}
	const std::optional<CamVec> lo = ReadVec(*col + kVCSColModelBoxMinOffset);
	const std::optional<CamVec> hi = ReadVec(*col + kVCSColModelBoxMaxOffset);
	if (!lo || !hi || !(hi->z > 0.1f && hi->z < 12.0f)) {
		return std::nullopt;
	}
	const float length = hi->y - lo->y;
	if (!(length > 0.3f && length < 60.0f) || !(hi->x > lo->x) || !(hi->z > lo->z)) {
		return std::nullopt;
	}
	return VehicleBox{*lo, *hi, hi->z, length};
}

// What the camera must stay out of this frame: the player, or the vehicle he is in. Set by the prepare
// hook, read by the finish hook.
CamVec g_subjectPos;
CamVec g_vehRight{1.0f, 0.0f, 0.0f};
CamVec g_vehForward{0.0f, 1.0f, 0.0f};
CamVec g_vehUp{0.0f, 0.0f, 1.0f};
VehicleBox g_box;

// The player's body as a capsule: around his axis, from his feet to just over his head. His origin is
// about 1.04 above the ground.
const float kBodyBelow = 1.05f;
const float kBodyAbove = 0.85f;

// How far `p` is from the surface of the player or the vehicle - negative inside.
float SubjectClearance(CamKind kind, CamVec p) {
	const VCSChaseCamSettings &s = ChaseCamSettings();
	if (kind == CamKind::Vehicle) {
		const CamVec d = Sub(p, g_subjectPos);
		const CamVec local{Dot(d, g_vehRight), Dot(d, g_vehForward), Dot(d, g_vehUp)};
		const CamVec out{
			std::max({g_box.min.x - local.x, 0.0f, local.x - g_box.max.x}),
			std::max({g_box.min.y - local.y, 0.0f, local.y - g_box.max.y}),
			std::max({g_box.min.z - local.z, 0.0f, local.z - g_box.max.z}),
		};
		const float outside = Length(out);
		if (outside > 0.0f) {
			return outside;
		}
		// Inside: the distance to the nearest face, as a negative number.
		return -std::min({local.x - g_box.min.x, g_box.max.x - local.x, local.y - g_box.min.y,
			g_box.max.y - local.y, local.z - g_box.min.z, g_box.max.z - local.z});
	}
	const float z = Clamp(p.z, g_subjectPos.z - kBodyBelow, g_subjectPos.z + kBodyAbove);
	const CamVec axis{g_subjectPos.x, g_subjectPos.y, z};
	return Length(Sub(p, axis)) - s.footBodyRadius;
}

// Lift the camera up and out over the top of the player or the vehicle if it has ended up inside -
// backed into a corner, or pulled in under a low ceiling of bodywork. The view keeps looking where the
// player asked; only the position moves.
CamVec KeepOutOfSubject(CamKind kind, CamVec p) {
	const VCSChaseCamSettings &s = ChaseCamSettings();
	const float margin = s.subjectMargin;
	if (kind == CamKind::Vehicle) {
		const CamVec d = Sub(p, g_subjectPos);
		const CamVec local{Dot(d, g_vehRight), Dot(d, g_vehForward), Dot(d, g_vehUp)};
		const bool inside = local.x > g_box.min.x - margin && local.x < g_box.max.x + margin &&
			local.y > g_box.min.y - margin && local.y < g_box.max.y + margin &&
			local.z > g_box.min.z - margin && local.z < g_box.max.z + margin;
		if (inside) {
			// Along world up, scaled by how upright the vehicle is so a tilted one still gets cleared.
			const float rise = (g_box.max.z + margin - local.z) / std::max(g_vehUp.z, 0.3f);
			p.z += rise;
		}
		return p;
	}
	const float dx = p.x - g_subjectPos.x;
	const float dy = p.y - g_subjectPos.y;
	const float top = g_subjectPos.z + kBodyAbove + margin;
	if (dx * dx + dy * dy < (s.footBodyRadius + margin) * (s.footBodyRadius + margin) &&
	    p.z > g_subjectPos.z - kBodyBelow && p.z < top) {
		p.z = top;
	}
	return p;
}

bool GlanceHeld(bool left) {
	return left ? (IsHostKeyDown(NKCODE_Q) || IsPadButtonDown(NKCODE_BUTTON_L1))
	            : (IsHostKeyDown(NKCODE_E) || IsPadButtonDown(NKCODE_BUTTON_R1));
}

void Unhook() {
	if (!g_hooked) {
		return;
	}
	// The jal first: once it is back, nothing reaches the program, and the rest is bookkeeping.
	if (Memory::IsValid4AlignedAddress(kVCSCamBasisCall)) {
		currentMIPS->InvalidateICache(kVCSCamBasisCall, 4);
		Memory::WriteUnchecked_U32(kVCSCamBasisCallOp, kVCSCamBasisCall);
		currentMIPS->InvalidateICache(kVCSCamBasisCall, 4);
	}
	RestoreReplacedInstruction(kVCSCamPreShake);
	currentMIPS->InvalidateICache(kVCSCamPreShake, 4);
	if (g_finishAddr) {
		RestoreReplacedInstruction(g_finishAddr);
		currentMIPS->InvalidateICache(g_finishAddr, 4);
	}
	g_hooked = false;
	g_prepared = false;
	g_owned = false;
	g_blendingOut = false;
}

}  // namespace

VCSChaseCamSettings &ChaseCamSettings() {
	static VCSChaseCamSettings settings;
	return settings;
}

void InstallChaseCam() {
	if (!IsActive() || !currentMIPS) {
		return;
	}
	if (!ChaseCamSettings().enabled) {
		if (g_hooked) {
			Unhook();
			g_status = "switched off";
			INFO_LOG(Log::HLE, "VCS: chase camera switched off, the game's camera is back");
		}
		return;
	}

	const int prepareIndex = GetReplacementFuncIndexByName("vcs_camera_prepare");
	const int finishIndex = GetReplacementFuncIndexByName("vcs_camera_finish");
	if (prepareIndex < 0 || finishIndex < 0) {
		g_status = "no replacement entries";
		return;
	}
	const u32 replacePrepare = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)prepareIndex;
	const u32 replaceFinish = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)finishIndex;

	if (g_hooked) {
		// Every tick, one read each. A savestate replaces PSP memory wholesale and this file's idea of
		// what is installed with it, so the only trustworthy answer is what is actually there.
		//
		// All three through Read_Instruction, which resolves a JIT block marker back to the op it
		// covers - and with replacements left unresolved, that op is the replacement itself. Reading
		// the two hooks raw looked right and was not: both sit on branch targets, which is where the
		// JIT starts a block, so the raw word is a block marker the moment the code has run once. The
		// first build did that, and reinstalled everything on every tick.
		const bool ok = ReadU32(g_block + kOffMagic).value_or(0) == kMagic &&
			Memory::Read_Instruction(kVCSCamBasisCall, false).encoding == Jal(g_block + kOffCode) &&
			Memory::Read_Instruction(kVCSCamPreShake, false).encoding == replacePrepare &&
			Memory::Read_Instruction(g_finishAddr, false).encoding == replaceFinish;
		if (ok) {
			return;
		}
		g_hooked = false;
		g_block = 0;
		g_status = "the patch was lost - reinstalling";
	}

	if (!Memory::IsValid4AlignedAddress(kVCSCamBasisCall) ||
	    ReadU32(kVCSCamClipLosCall).value_or(0) != kVCSCamClipLosCallOp ||
	    ReadU32(kVCSRwCameraSetNearClip + 0x14).value_or(0) != kVCSRwCameraSetNearClipOp) {
		g_status = "waiting for the camera code";
		return;
	}

	currentMIPS->InvalidateICache(kVCSCamPreShake, 4);
	currentMIPS->InvalidateICache(kVCSCamBasisCall, 4);
	const u32 preRaw = Memory::ReadUnchecked_U32(kVCSCamPreShake);
	if (preRaw != kVCSCamPreShakeOp && preRaw != replacePrepare) {
		g_status = "the pre-shake instruction is not the one measured";
		return;
	}

	// The jal is either the game's own, or - after loading a savestate taken with this installed - a
	// jal into a block of ours that came back with the state. Adopt that block rather than leave the
	// game calling into memory this file has no record of.
	const u32 basisRaw = Memory::ReadUnchecked_U32(kVCSCamBasisCall);
	u32 block = 0;
	if (basisRaw != kVCSCamBasisCallOp) {
		if ((basisRaw >> 26) == 3) {
			const u32 target = (kVCSCamBasisCall & 0xf0000000u) | ((basisRaw & 0x03ffffffu) << 2);
			if (ReadU32(target - kOffCode + kOffMagic).value_or(0) == kMagic) {
				block = target - kOffCode;
			}
		}
		if (!block) {
			g_status = "the matrix-build call is not the one measured";
			return;
		}
	}

	if (!block) {
		u32 size = kBlockSize;
		block = userMemory.Alloc(size, true, "VCS chase camera");
		if (block == (u32)-1) {
			g_status = "no room in the game's memory partition";
			return;
		}
	}
	if (((block + kOffCode) & 0xf0000000u) != (kVCSCamBasisCall & 0xf0000000u)) {
		userMemory.Free(block);
		g_status = "the block landed out of jal range";
		return;
	}

	int finishOffset = 0;
	const std::vector<u32> code = BuildProgram(block, &finishOffset);
	if (kOffCode + code.size() * 4 > kBlockSize) {
		g_status = "the program outgrew its block";
		return;
	}

	bool ok = WriteU32(block + kOffCount, 0);
	// The flags, the look command and its angles all start zeroed.
	for (u32 off = kOffFlags; ok && off < kOffRays; off += 4) {
		ok = WriteU32(block + off, 0);
	}
	for (size_t i = 0; ok && i < code.size(); i++) {
		ok = WriteU32(block + kOffCode + (u32)i * 4, code[i]);
	}
	ok = ok && WriteU32(block + kOffMagic, kMagic);
	if (!ok) {
		g_status = "could not write the block";
		return;
	}
	currentMIPS->InvalidateICache(block + kOffCode, (u32)code.size() * 4);

	// The finish hook sits on a nop the program was just written with, so the instruction saved
	// under it is that nop.
	g_finishAddr = block + kOffCode + (u32)finishOffset * 4;
	WriteReplaceInstructionAt(g_finishAddr, finishIndex);
	currentMIPS->InvalidateICache(g_finishAddr, 4);

	// The prepare hook sits on a game instruction. Put the measured original back first, whatever
	// is there: a replacement that arrived with a savestate has no saved instruction behind it in
	// this session, and without one the JIT would run a nop where the game reads its shake.
	Memory::WriteUnchecked_U32(kVCSCamPreShakeOp, kVCSCamPreShake);
	WriteReplaceInstructionAt(kVCSCamPreShake, prepareIndex);
	currentMIPS->InvalidateICache(kVCSCamPreShake, 4);

	// The jal last, once everything it reaches is ready.
	Memory::WriteUnchecked_U32(Jal(block + kOffCode), kVCSCamBasisCall);
	currentMIPS->InvalidateICache(kVCSCamBasisCall, 4);

	g_block = block;
	g_hooked = true;
	g_prepared = false;
	g_owned = false;
	g_status = "installed";
	ReadEnvironment();
	NOTICE_LOG(Log::HLE, "VCS: chase camera installed - block %08x, %d instructions, finish at %08x",
		block, (int)code.size(), g_finishAddr);
}

void RemoveChaseCam() {
	Unhook();
	if (g_block) {
		WriteU32(g_block + kOffMagic, 0);
		userMemory.Free(g_block);
		g_block = 0;
	}
	g_finishAddr = 0;
	g_status = "not installed";
	ChaseCamReset();
}

void ChaseCamReset() {
	g_prepared = false;
	g_owned = false;
	g_kind = CamKind::None;
	g_lastKind = CamKind::None;
	g_blendingOut = false;
	g_blend = 1.0f;
	g_sinceLook = 1e3f;
	g_pivotOffset = CamVec{};
	g_distance = 0.0f;
	g_vehicle = 0;
	g_haveVehiclePos = false;
	g_speed = 0.0f;
	g_wasGlancing = false;
	g_glancing = false;
	g_frames = 0;
	g_ownedFrames = 0;
}

bool ChaseCamTakesLook(VCSInputContext context) {
	if (!g_hooked || !ChaseCamSettings().enabled) {
		return false;
	}
	if (context != VCSInputContext::OnFoot && context != VCSInputContext::InVehicle &&
	    context != VCSInputContext::InAircraft) {
		return false;
	}
	// The mounted gun reports no vehicle, so it looks like standing on foot - and its camera measures
	// yaw from the vehicle's nose. CameraTick has that case.
	return ReadAddrU32(VCSAddr::PedAttachedTo).value_or(0) == 0;
}

void ChaseCamAddLook(float yawRadians, float pitchRadians) {
	if (yawRadians == 0.0f && pitchRadians == 0.0f) {
		return;
	}
	g_yaw = WrapPi(g_yaw + yawRadians);
	float lo, hi;
	PitchLimits(g_lastKind, &lo, &hi);
	g_pitch = Clamp(g_pitch + pitchRadians, lo, hi);
	g_sinceLook = 0.0f;
}

int Hook_vcs_camera_prepare() {
	g_prepared = false;
	if (!g_hooked || !g_block || !IsActive()) {
		return 0;
	}
	// Cheap, and the only defence against writing into memory a savestate has handed to someone else:
	// the hook can run before the next tick's install check has noticed the block is gone.
	if (ReadU32(g_block + kOffMagic).value_or(0) != kMagic) {
		return 0;
	}
	WriteU32(g_block + kOffCount, 0);
	WriteU32(g_block + kOffNearSet, 0);
	g_rayCount = 0;

	const VCSChaseCamSettings &s = ChaseCamSettings();
	g_frames++;
	g_dt = GameSeconds();

	const u32 sourcePtr = currentMIPS->r[MIPS_REG_FP];
	const u32 frontPtr = currentMIPS->r[MIPS_REG_S7];
	const u32 upPtr = currentMIPS->r[MIPS_REG_S6];
	const std::optional<CamVec> preSource = ReadVec(sourcePtr);
	const std::optional<CamVec> preFront = ReadVec(frontPtr);
	const std::optional<CamVec> preUp = ReadVec(upPtr);
	if (!preSource || !preFront || !preUp) {
		return 0;
	}
	g_preSource = *preSource;
	g_preFront = *preFront;
	g_preUp = *preUp;

	g_mode = (int)ReadU16(ActiveCam()).value_or(0xffff);
	const VCSInputContext context = GetCurrentContext();

	CamKind kind = CamKind::None;
	if (s.enabled && ReadAddrU32(VCSAddr::PedAttachedTo).value_or(0) == 0) {
		if (context == VCSInputContext::OnFoot && g_mode == kVCSCamModeFollowPed) {
			kind = CamKind::OnFoot;
		} else if ((context == VCSInputContext::InVehicle || context == VCSInputContext::InAircraft) &&
		           (g_mode == kVCSCamModeOnAString || g_mode == kVCSCamModeBehindBoat) &&
		           !DriveByAimActive(context)) {
			kind = CamKind::Vehicle;
		}
	}
	g_kind = kind;

	if (kind == CamKind::None) {
		if (g_owned) {
			g_owned = false;
			g_lastKind = CamKind::None;
			// Ease into whatever the game switched to - unless it cut somewhere else entirely, which is
			// a cut the game meant.
			g_blendingOut = s.blendOut > 0.0f && Length(Sub(g_outSource, g_preSource)) < 15.0f;
			g_blendOutT = 0.0f;
		}
		g_prepared = g_blendingOut;
		return 0;
	}
	g_blendingOut = false;

	// --- The pivot ---
	CamVec pivot;
	float vehicleHeading = 0.0f;
	if (kind == CamKind::OnFoot) {
		const std::optional<float> px = ReadAddrFloat(VCSAddr::PlayerPosX);
		const std::optional<float> py = ReadAddrFloat(VCSAddr::PlayerPosY);
		const std::optional<float> pz = ReadAddrFloat(VCSAddr::PlayerPosZ);
		if (!px || !py || !pz) {
			return 0;
		}
		pivot = CamVec{*px, *py, *pz + s.footPivotHeight};
		g_subjectPos = CamVec{*px, *py, *pz};
		g_haveVehiclePos = false;
		g_speed = 0.0f;
	} else {
		const u32 vehicle = ReadAddrU32(VCSAddr::PlayerVehicle).value_or(0);
		const std::optional<CamVec> pos = vehicle ? ReadVec(vehicle + kVCSEntityPositionOffset) : std::nullopt;
		const std::optional<CamVec> forward = vehicle ? ReadVec(vehicle + kVCSEntityForwardOffset) : std::nullopt;
		const std::optional<CamVec> rightRow = vehicle ? ReadVec(vehicle + kVCSEntityRightOffset) : std::nullopt;
		const std::optional<CamVec> upRow = vehicle ? ReadVec(vehicle + kVCSEntityUpOffset) : std::nullopt;
		if (!pos || !forward || !rightRow || !upRow) {
			return 0;
		}
		const VehicleBox box = ReadVehicleBox().value_or(VehicleBox{});
		g_vehicleLength = box.length;
		g_box = box;
		g_subjectPos = *pos;
		g_vehRight = *rightRow;
		g_vehForward = *forward;
		g_vehUp = *upRow;
		// World up, never the vehicle's: a helicopter tipped forward must not tip the pivot with it.
		pivot = CamVec{pos->x, pos->y, pos->z + box.top * s.vehiclePivotScale};
		vehicleHeading = std::atan2(forward->y, forward->x);
		if (vehicle == g_vehicle && g_haveVehiclePos && g_dt > 0.0f) {
			g_speed = Length(Sub(*pos, g_vehiclePos)) / g_dt;
		} else {
			g_speed = 0.0f;
		}
		g_vehicle = vehicle;
		g_vehiclePos = *pos;
		g_haveVehiclePos = true;
	}

	g_gameDistance = Length(Sub(g_preSource, pivot));

	const bool starting = !g_owned;
	if (starting) {
		// Take the view from wherever the game has the camera this frame - after aiming, a cutscene
		// or a load, that is what the player was just looking at.
		const CamVec front = Normalize(g_preFront, CamVec{1.0f, 0.0f, 0.0f});
		g_yaw = std::atan2(front.y, front.x);
		g_pitch = std::asin(Clamp(front.z, -1.0f, 1.0f));
		// From the game's distance, so the blend in starts where the game's camera is and the distance
		// eases out to ours rather than jumping there.
		g_distance = g_gameDistance;
		g_blend = s.blendIn > 0.0f ? 0.0f : 1.0f;
		g_pivotOffset = CamVec{};
		g_wasGlancing = false;
		static bool announcedFoot = false, announcedVehicle = false;
		bool &announced = kind == CamKind::OnFoot ? announcedFoot : announcedVehicle;
		if (!announced) {
			announced = true;
			NOTICE_LOG(Log::HLE, "VCS: chase camera took the %s view (mode %d)", KindName(kind), g_mode);
		}
	} else if (kind != g_lastKind) {
		// Straight from a player to a car or back, with no frame in between: keep the view, and let
		// the pivot slide across rather than jump.
		g_pivotOffset = Add(g_pivotOffset, Sub(g_lastPivot, pivot));
		g_wasGlancing = false;
	}
	g_owned = true;
	g_ownedFrames++;

	// A look command from outside, taken after the anchor so it is not overwritten by it.
	const u32 command = ReadU32(g_block + kOffCommand).value_or(0);
	if (command != 0) {
		WriteU32(g_block + kOffCommand, 0);
		if (command & 1) {
			g_yaw = WrapPi(ReadFloat(g_block + kOffCommandYaw).value_or(g_yaw));
		}
		if (command & 2) {
			g_pitch = ReadFloat(g_block + kOffCommandPitch).value_or(g_pitch);
		}
		g_sinceLook = 0.0f;
		NOTICE_LOG(Log::HLE, "VCS: chase camera look command - yaw %.3f pitch %.3f", g_yaw, g_pitch);
	}

	g_lastPivot = pivot;
	g_pivotOffset = Mul(g_pivotOffset, 1.0f - EaseFactor(s.pivotSettleRate, g_dt));
	g_pivot = Add(pivot, g_pivotOffset);

	// --- How far back ---
	//
	// Not from the game's camera: it starts every frame from the one this file wrote, so its distance
	// is ours handed back - see footDistance.
	if (kind == CamKind::Vehicle) {
		const float zoom = ReadFloat(kVCSCCamera + kVCSCCameraCarZoomOffset).value_or(2.0f);
		const float zoomScale = zoom < 1.5f ? s.vehicleZoomNear : (zoom > 2.5f ? s.vehicleZoomFar : 1.0f);
		g_wantDistance = (s.vehicleDistanceBase + s.vehicleDistancePerLength * g_vehicleLength) *
			zoomScale * s.vehicleDistanceScale;
	} else {
		g_wantDistance = s.footDistance;
	}
	g_wantDistance = Clamp(g_wantDistance, std::max(s.minDistance, 0.1f), 60.0f);

	// --- Where to look ---
	g_sinceLook += g_dt;
	float yaw = g_yaw;
	float pitch = g_pitch;
	g_glancing = false;
	if (kind == CamKind::Vehicle) {
		const bool canGlance = s.glances && context == VCSInputContext::InVehicle;
		const bool left = canGlance && GlanceHeld(true);
		const bool right = canGlance && GlanceHeld(false);
		if (left || right) {
			g_glancing = true;
			yaw = vehicleHeading + (left && right ? kPi : (left ? kPi * 0.5f : -kPi * 0.5f));
			pitch = s.vehicleRestPitch;
		} else {
			if (g_wasGlancing) {
				// Letting go of a glance is asking for the default view back.
				g_yaw = vehicleHeading;
				g_pitch = s.vehicleRestPitch;
			} else if (s.vehicleRecentre && g_sinceLook > s.recentreDelay && g_speed > s.recentreMinSpeed) {
				const float urgency = Clamp(g_speed / (s.recentreMinSpeed * 4.0f), 0.25f, 1.0f);
				const float k = EaseFactor(s.recentreRate * urgency, g_dt);
				g_yaw = WrapPi(g_yaw + WrapPi(vehicleHeading - g_yaw) * k);
				g_pitch += (s.vehicleRestPitch - g_pitch) * k;
			}
			yaw = g_yaw;
			pitch = g_pitch;
		}
		g_wasGlancing = g_glancing;
	}
	float lo, hi;
	PitchLimits(kind, &lo, &hi);
	g_pitch = Clamp(g_pitch, lo, hi);
	pitch = Clamp(pitch, lo, hi);
	g_lastKind = kind;

	const float cy = std::cos(yaw), sy = std::sin(yaw);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	g_front = CamVec{cy * cp, sy * cp, sp};
	g_up = CamVec{-cy * sp, -sy * sp, cp};
	const CamVec right = CamVec{sy, -cy, 0.0f};

	// Where the camera sits is the orbit, which in a vehicle stops just below the pivot: looking further
	// up tilts the view rather than swinging the camera down through the car. On foot the two are one.
	const float orbitPitch = kind == CamKind::Vehicle ? std::min(pitch, s.vehicleOrbitUp) : pitch;
	const float co = std::cos(orbitPitch), so = std::sin(orbitPitch);
	g_orbit = CamVec{cy * co, sy * co, so};
	const CamVec orbitUp = CamVec{-cy * so, -sy * so, co};

	// --- The rays ---
	//
	// All from the pivot, fanning out to coneRadius at the far end. Starting them at the pivot rather
	// than offset from it keeps every start on the player's side of a wall he is standing against.
	const float reach = g_wantDistance + s.collisionMargin;
	const CamVec end = Sub(g_pivot, Mul(g_orbit, reach));
	static const float kFan[kMaxRays][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	const int count = std::clamp(s.rays, 0, kMaxRays);
	for (int i = 0; i < count; i++) {
		const u32 ray = g_block + kOffRays + (u32)i * kRayStride;
		const CamVec e = Add(end, Add(Mul(right, kFan[i][0] * s.coneRadius), Mul(orbitUp, kFan[i][1] * s.coneRadius)));
		WriteVec(ray + kRayStart, g_pivot);
		WriteVec(ray + kRayEnd, e);
		WriteU32(ray + kRayEntity, 0);
		WriteU32(ray + kRayHit, 0);
	}
	const u32 flags[9] = {
		1,                                                           // buildings
		(kind == CamKind::OnFoot && s.footCollideVehicles) ? 1u : 0u,  // vehicles - never our own
		0,                                                           // peds
		s.collideObjects ? 1u : 0u,                                  // objects
		0, 1, 1, 0, 0,                                               // as the game's own camera clip
	};
	for (int i = 0; i < 9; i++) {
		WriteU32(g_block + kOffFlags + (u32)i * 4, flags[i]);
	}
	WriteU32(g_block + kOffCount, (u32)count);
	g_rayCount = count;
	g_prepared = true;
	return 0;
}

int Hook_vcs_camera_finish() {
	if (!g_prepared || !g_block || ReadU32(g_block + kOffMagic).value_or(0) != kMagic) {
		g_prepared = false;
		return 0;
	}
	g_prepared = false;
	const VCSChaseCamSettings &s = ChaseCamSettings();

	const u32 sourcePtr = currentMIPS->r[MIPS_REG_FP];
	const u32 frontPtr = currentMIPS->r[MIPS_REG_S7];
	const u32 upPtr = currentMIPS->r[MIPS_REG_S6];
	const std::optional<CamVec> postSource = ReadVec(sourcePtr);
	const std::optional<CamVec> postFront = ReadVec(frontPtr);
	const std::optional<CamVec> postUp = ReadVec(upPtr);
	if (!postSource || !postFront || !postUp) {
		return 0;
	}

	if (g_blendingOut) {
		g_blendOutT += s.blendOut > 0.0f ? g_dt / s.blendOut : 1.0f;
		if (g_blendOutT >= 1.0f) {
			g_blendingOut = false;
			return 0;
		}
		const float b = SmoothStep(g_blendOutT);
		const CamVec source = Lerp(g_outSource, *postSource, b);
		const CamVec front = Normalize(Lerp(g_outFront, *postFront, b), *postFront);
		const CamVec up = Orthonormal(Lerp(g_outUp, *postUp, b), front, *postUp);
		WriteVec(sourcePtr, source);
		WriteVec(frontPtr, front);
		WriteVec(upPtr, up);
		return 0;
	}

	// --- Collision: the nearest hit along the view, measured along the view ---
	float allowed = g_wantDistance;
	int hits = 0;
	const CamVec back = Mul(g_orbit, -1.0f);
	for (int i = 0; i < g_rayCount; i++) {
		const u32 ray = g_block + kOffRays + (u32)i * kRayStride;
		if (ReadU32(ray + kRayHit).value_or(0) == 0) {
			continue;
		}
		const std::optional<CamVec> point = ReadVec(ray + kRayCol);
		if (!point) {
			continue;
		}
		hits++;
		allowed = std::min(allowed, Dot(Sub(*point, g_pivot), back) - s.collisionMargin);
	}
	allowed = std::max(allowed, s.minDistance);
	g_hits = hits;
	g_allowed = allowed;

	// A hit pulls in at once, or the near plane goes through the wall for however long an ease takes.
	// Anything else eases, in either direction: out past an obstruction that has cleared, and in when
	// the wanted distance itself shrinks - climbing off a bike is not a collision.
	if (hits > 0 && allowed < g_distance) {
		g_distance = allowed;
	} else {
		g_distance += (allowed - g_distance) * EaseFactor(s.easeOutRate, g_dt);
	}

	CamVec source = KeepOutOfSubject(g_kind, Sub(g_pivot, Mul(g_orbit, g_distance)));
	CamVec front = g_front;
	CamVec up = g_up;

	if (s.keepShake) {
		source = Add(source, Sub(*postSource, g_preSource));
		front = Normalize(Add(front, Sub(*postFront, g_preFront)), g_front);
		up = Orthonormal(Add(up, Sub(*postUp, g_preUp)), front, g_up);
	}
	if (g_blend < 1.0f) {
		g_blend = std::min(1.0f, g_blend + (s.blendIn > 0.0f ? g_dt / s.blendIn : 1.0f));
		const float b = SmoothStep(g_blend);
		source = Lerp(*postSource, source, b);
		front = Normalize(Lerp(*postFront, front, b), front);
		up = Orthonormal(Lerp(*postUp, up, b), front, up);
	}

	// The near clip plane: a fraction of the camera's clearance from the player or the car, and never
	// more than the game already chose - the game's own value is in the camera by now, the prologue's
	// 0.9 or whatever mode 18 lowered it to. The program calls the setter once this hook returns.
	g_clearance = SubjectClearance(g_kind, source);
	g_near = 0.0f;
	if (s.nearClip) {
		const u32 rwCamera = ReadU32(kVCSSceneCameraPtr).value_or(0);
		const float gameNear = rwCamera ? ReadFloat(rwCamera + kVCSRwCameraNearOffset).value_or(0.0f) : 0.0f;
		if (gameNear > 0.0f && gameNear < 10.0f) {
			const float want = Clamp(g_clearance * s.nearClipFactor, std::max(s.nearClipMin, 0.01f), gameNear);
			g_near = want;
			if (want < gameNear - 0.001f) {
				WriteFloat(g_block + kOffNear, want);
				WriteU32(g_block + kOffNearSet, 1);
			}
		}
	}

	WriteVec(sourcePtr, source);
	WriteVec(frontPtr, front);
	WriteVec(upPtr, up);
	g_outSource = source;
	g_outFront = front;
	g_outUp = up;

	// On foot the game's own angles follow the view: they are where the aim camera starts from, and
	// where the walk direction comes from. Never in a vehicle - that is the integrator this exists to
	// stay away from.
	if (g_kind == CamKind::OnFoot) {
		float gameYaw = std::fmod(g_yaw + kPi, kTwoPi);
		if (gameYaw < 0.0f) {
			gameYaw += kTwoPi;
		}
		WriteAddrFloat(VCSAddr::CameraYaw, gameYaw);
		WriteAddrFloat(VCSAddr::CameraPitch, Clamp(g_pitch, -kGamePitchLimit, kGamePitchLimit));
	}

	if (g_trace) {
		g_traceTimer += g_dt;
		if (g_traceTimer >= 1.0f) {
			g_traceTimer = 0.0f;
			NOTICE_LOG(Log::HLE,
				"VCS cam: %s mode %d yaw %.3f pitch %.3f game %.2f want %.2f allowed %.2f now %.2f hits %d/%d "
				"pivot %.2f %.2f %.2f source %.2f %.2f %.2f speed %.1f clear %.2f near %.2f%s",
				KindName(g_kind), g_mode, g_yaw, g_pitch, g_gameDistance, g_wantDistance, g_allowed, g_distance,
				g_hits, g_rayCount, g_pivot.x, g_pivot.y, g_pivot.z, source.x, source.y, source.z, g_speed,
				g_clearance, g_near, g_glancing ? " glancing" : "");
		}
	}
	return 0;
}

void ChaseCamGetStats(VCSChaseCamStats *out) {
	out->status = g_status;
	out->installed = g_hooked;
	out->owning = g_owned;
	out->kind = KindName(g_owned ? g_lastKind : CamKind::None);
	out->mode = g_mode;
	out->frames = g_frames;
	out->ownedFrames = g_ownedFrames;
	out->yaw = g_yaw;
	out->pitch = g_pitch;
	out->gameDistance = g_gameDistance;
	out->wantDistance = g_wantDistance;
	out->allowedDistance = g_allowed;
	out->distance = g_distance;
	out->clearance = g_clearance;
	out->nearClip = g_near;
	out->rays = g_rayCount;
	out->hits = g_hits;
	out->pivot[0] = g_pivot.x;
	out->pivot[1] = g_pivot.y;
	out->pivot[2] = g_pivot.z;
	out->speed = g_speed;
	out->sinceLook = g_sinceLook;
	out->glancing = g_glancing;
	out->blendingOut = g_blendingOut;
	out->blendIn = g_blend;
	out->block = g_block;
}

}  // namespace VCS
