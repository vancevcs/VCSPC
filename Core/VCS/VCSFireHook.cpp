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

#include <cmath>

#include "Core/VCS/VCSFireHook.h"

#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

// MSVC does not define M_PI without _USE_MATH_DEFINES, and relying on that macro from a
// header this file does not control is how a cross-platform build breaks on one compiler.
static constexpr float kPi = 3.14159265358979323846f;

static bool g_installed = false;
static u64 g_seen = 0;
static u64 g_redirected = 0;

VCSFireHookSettings &FireHookSettings() {
	static VCSFireHookSettings s;
	return s;
}

bool FireHookInstalled() {
	return g_installed;
}

void FireHookStats(u64 *seen, u64 *redirected) {
	if (seen) *seen = g_seen;
	if (redirected) *redirected = g_redirected;
}

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

static bool ReadVec3(u32 addr, Vec3 *out) {
	auto x = ReadFloat(addr);
	auto y = ReadFloat(addr + 4);
	auto z = ReadFloat(addr + 8);
	if (!x || !y || !z)
		return false;
	out->x = *x;
	out->y = *y;
	out->z = *z;
	return true;
}

static bool WriteVec3(u32 addr, const Vec3 &v) {
	return WriteFloat(addr, v.x) && WriteFloat(addr + 4, v.y) && WriteFloat(addr + 8, v.z);
}

static float Dist(const Vec3 &a, const Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Where the player ped is standing. The entity's transform sits at the head of the struct with
// translation at +0x30 - the layout every address in this fork's table was found through.
static bool PlayerPosition(Vec3 *out) {
	auto ped = ReadAddrU32(VCSAddr::PlayerBase);
	if (!ped || !*ped)
		return false;
	return ReadVec3(*ped + 0x30, out);
}

// The camera's forward direction, rebuilt from the angles rather than read as a basis.
//
// CCam almost certainly stores Front/Up directly - two mutually orthogonal unit vectors sit at
// CCam[0]+0x010 and +0x060 - but their headings did not reconcile with CameraYaw when measured,
// so they are NOT used here. Deriving from yaw and pitch uses only values this fork has verified
// by writing to them and watching the view move. Swap to the stored vectors if they are ever
// confirmed; the only thing lost by deriving is the camera's own position, which this hook does
// not need (see AimedTarget).
static bool CameraForward(Vec3 *out) {
	auto camYaw = ReadAddrFloat(VCSAddr::CameraYaw);
	auto camPitch = ReadAddrFloat(VCSAddr::CameraPitch);
	if (!camYaw || !camPitch)
		return false;

	// CameraYaw is the camera matrix yaw + PI/2, and pitch is stored negated - more negative is
	// looking UP. Both conventions come from docs/VCS_ADDRESSES.md and both were established by
	// writing values and watching the view, so they are the trustworthy ones.
	const float yaw = *camYaw - kPi / 2.0f;
	const float pitch = -*camPitch;

	const float cp = cosf(pitch);
	out->x = cp * cosf(yaw);
	out->y = cp * sinf(yaw);
	out->z = sinf(pitch);
	return true;
}

// Rotate a shot about Z by some angle, preserving its length. Only used by the debug path.
static Vec3 Deflected(const Vec3 &source, const Vec3 &target, float degrees) {
	const float a = degrees * kPi / 180.0f;
	const float ca = cosf(a), sa = sinf(a);
	const float dx = target.x - source.x, dy = target.y - source.y, dz = target.z - source.z;
	return Vec3{source.x + dx * ca - dy * sa, source.y + dx * sa + dy * ca, source.z + dz};
}

// The re3 model, adapted to what VCS gives us.
//
// re3 builds the ray from the CAMERA's position and slides its origin forward to the muzzle, so
// the ray stays exactly collinear with the crosshair pixel. We keep the game's own source - it
// is already the muzzle, and it saves needing the camera's position, which is the one field of
// the basis that cannot be derived from angles. The cost is parallax: at very close range what
// the crosshair covers and what the bullet hits diverge slightly. Worth revisiting only if that
// is visible in play.
static Vec3 AimedTarget(const Vec3 &source, const Vec3 &target, const Vec3 &forward) {
	const float range = Dist(source, target);
	return Vec3{source.x + forward.x * range,
	            source.y + forward.y * range,
	            source.z + forward.z * range};
}

int Hook_vcs_weapon_raycast() {
	const VCSFireHookSettings &s = FireHookSettings();
	if (!s.enabled || !IsActive())
		return 0;

	g_seen++;

	// a0 and a1 are point1 and point2 - the wrapper forwards them untouched, so they are the
	// same pointers CWorld::ProcessLineOfSight is about to read.
	const u32 srcPtr = PARAM(0);
	const u32 dstPtr = PARAM(1);

	Vec3 source, target;
	if (!ReadVec3(srcPtr, &source) || !ReadVec3(dstPtr, &target))
		return 0;

	// Guard, in two independent halves, because getting this wrong redirects other people's
	// bullets through the player's camera. The aim key says the player intends to aim; the
	// distance says this particular ray is the player's. Neither alone is enough - NPCs shoot
	// through this same wrapper, and they do it while the player is aiming too.
	if (!IsHostKeyDown(kVCSAimKey))
		return 0;

	Vec3 player;
	if (!PlayerPosition(&player) || Dist(source, player) > s.playerRadius)
		return 0;

	Vec3 aimed;
	if (s.debugDeflectDegrees != 0.0f) {
		aimed = Deflected(source, target, s.debugDeflectDegrees);
	} else {
		Vec3 forward;
		if (!CameraForward(&forward))
			return 0;
		aimed = AimedTarget(source, target, forward);
	}

	if (WriteVec3(dstPtr, aimed))
		g_redirected++;

	// HOOKENTER: the original instruction runs after us, so the raycast proceeds - now with our
	// target. Returning 0 costs no emulated cycles.
	return 0;
}

void InstallFireHook() {
	if (g_installed || !IsActive())
		return;

	const int index = GetReplacementFuncIndexByName("vcs_weapon_raycast");
	if (index < 0) {
		WARN_LOG(Log::HLE, "VCS: no replacement entry named vcs_weapon_raycast");
		return;
	}

	// Installed by ADDRESS rather than by the usual function-hash match. The PSP has no ASLR and
	// this is a single known build (ULUS10160), so the address is stable; and the hash path would
	// need the analyser to have found and named this function first, which it has not.
	if (WriteReplaceInstructionAt(kVCSWeaponRaycastCall, index)) {
		g_installed = true;
		INFO_LOG(Log::HLE, "VCS: fire hook installed at %08x", kVCSWeaponRaycastCall);
	}
}

void RemoveFireHook() {
	if (!g_installed)
		return;
	RestoreReplacedInstruction(kVCSWeaponRaycastCall);
	g_installed = false;
	g_seen = 0;
	g_redirected = 0;
}

}  // namespace VCS
