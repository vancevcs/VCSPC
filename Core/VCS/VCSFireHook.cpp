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
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

// MSVC does not define M_PI without _USE_MATH_DEFINES, and relying on that macro from a
// header this file does not control is how a cross-platform build breaks on one compiler.
static constexpr float kPi = 3.14159265358979323846f;

// The PSP's framebuffer is 480x272. The crosshair offset is a screen-space quantity, so the
// horizontal half of it has to be scaled by the aspect ratio - exactly as re3 does with
// SCREEN_ASPECT_RATIO.
static constexpr float kScreenAspect = 480.0f / 272.0f;

static bool g_installed = false;
static bool g_doneInstalled = false;
static u64 g_seen = 0;
static u64 g_redirected = 0;
static VCSFireHookTrace g_trace;

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

const VCSFireHookTrace &FireHookLastTrace() {
	return g_trace;
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

static void StoreVec3(float *dst, const Vec3 &v) {
	dst[0] = v.x;
	dst[1] = v.y;
	dst[2] = v.z;
}

static float Dist(const Vec3 &a, const Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static Vec3 Cross(const Vec3 &a, const Vec3 &b) {
	return Vec3{a.y * b.z - a.z * b.y,
	            a.z * b.x - a.x * b.z,
	            a.x * b.y - a.y * b.x};
}

// Returns false rather than producing a garbage direction, because a zero-length direction here
// would fire the shot straight at the muzzle.
static bool Normalise(Vec3 *v) {
	const float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
	if (!(len > 1e-6f))
		return false;
	v->x /= len;
	v->y /= len;
	v->z /= len;
	return true;
}

// Where the player ped is standing. The entity's transform sits at the head of the struct with
// translation at +0x30 - the layout every address in this fork's table was found through.
static bool PlayerPosition(Vec3 *out) {
	auto ped = ReadAddrU32(VCSAddr::PlayerBase);
	if (!ped || !*ped)
		return false;
	return ReadVec3(*ped + 0x30, out);
}

// re3's CCamera::Find3rdPersonCamTargetVector, with VCS's stored basis in place of TheCamera's.
//
// The whole formula is: take the camera's forward, push it sideways and upwards by the crosshair's
// offset from screen centre scaled by tan(FOV/2), normalise, and that is the world ray the pixel
// under the crosshair covers. The x term additionally carries the aspect ratio because FOV is
// vertical.
//
// Nothing here is reconstructed from an angle, which is the point - see the long note in the
// header for why the reconstruction was 4.42 degrees out and why that error moved.
static bool CrosshairRay(Vec3 front, Vec3 up, float *fovOut, Vec3 *dir) {
	// A stale or half-written basis is a real possibility during a camera transition, and the
	// cheapest way to reject one is that it is no longer a unit vector.
	if (!Normalise(&front) || !Normalise(&up))
		return false;

	const VCSFireHookSettings &s = FireHookSettings();
	auto fovRead = ReadFloat(kVCSCam0 + kVCSCamFOVOffset);
	const float fov = (fovRead && *fovRead > 1.0f && *fovRead < 179.0f) ? *fovRead : 70.0f;
	const float tanHalf = tanf(fov * 0.5f * kPi / 180.0f);

	// Screen offsets in the same units re3 uses: -1 at the left/bottom edge, +1 at the right/top.
	const float ox = (s.crosshairX - 0.5f) * 2.0f;
	const float oy = (0.5f - s.crosshairY) * 2.0f;

	// front x up is screen-right in this coordinate system: with front (0,1,0) and up (0,0,1) it
	// gives (1,0,0), i.e. +X to the right of a camera facing +Y. Verified against the live basis,
	// which reads back exactly orthonormal (dot(front, up) == 0.000000).
	const Vec3 right = Cross(front, up);

	Vec3 d{front.x + right.x * ox * tanHalf * kScreenAspect + up.x * oy * tanHalf,
	       front.y + right.y * ox * tanHalf * kScreenAspect + up.y * oy * tanHalf,
	       front.z + right.z * ox * tanHalf * kScreenAspect + up.z * oy * tanHalf};
	if (!Normalise(&d))
		return false;

	// The trim is expected to be 0. It survives so that "it wants zero" can be a measurement.
	if (s.aimYawOffsetDeg != 0.0f) {
		const float a = s.aimYawOffsetDeg * kPi / 180.0f;
		const float ca = cosf(a), sa = sinf(a);
		d = Vec3{d.x * ca - d.y * sa, d.x * sa + d.y * ca, d.z};
	}

	*fovOut = fov;
	*dir = d;
	return true;
}

static bool CameraRay(Vec3 *source, Vec3 *dir, VCSFireHookTrace *trace) {
	Vec3 front, up, camPos;
	if (!ReadVec3(kVCSCam0 + kVCSCamFrontOffset, &front) ||
	    !ReadVec3(kVCSCam0 + kVCSCamUpOffset, &up) ||
	    !ReadVec3(kVCSCam0 + kVCSCamSourceOffset, &camPos)) {
		return false;
	}
	float fov = 0.0f;
	Vec3 d;
	if (!CrosshairRay(front, up, &fov, &d))
		return false;

	*source = camPos;
	*dir = d;
	if (trace) {
		Normalise(&front);
		Normalise(&up);
		trace->haveBasis = true;
		StoreVec3(trace->front, front);
		StoreVec3(trace->up, up);
		StoreVec3(trace->camSource, camPos);
		trace->fov = fov;
	}
	return true;
}

bool SolveAimRay(float origin[3], float dir[3]) {
	Vec3 source, d;
	if (!CameraRay(&source, &d, nullptr))
		return false;
	StoreVec3(origin, source);
	StoreVec3(dir, d);
	return true;
}

bool SolveAimRayAlong(const float frontIn[3], float origin[3], float dir[3]) {
	Vec3 camPos;
	if (!ReadVec3(kVCSCam0 + kVCSCamSourceOffset, &camPos))
		return false;
	Vec3 front{frontIn[0], frontIn[1], frontIn[2]};
	if (!Normalise(&front))
		return false;
	// Up for a camera with no roll: world Z with Front's share taken out. It only matters when
	// crosshairX/Y are off centre, and building it here keeps this ray free of anything the game
	// derived from the Front it exists to replace. Straight up or down leaves nothing to normalise,
	// and CrosshairRay refuses it - the caller falls back to the live ray for that frame.
	const Vec3 up{-front.z * front.x, -front.z * front.y, 1.0f - front.z * front.z};
	float fov = 0.0f;
	Vec3 d;
	if (!CrosshairRay(front, up, &fov, &d))
		return false;
	StoreVec3(origin, camPos);
	StoreVec3(dir, d);
	return true;
}

// How far the shot should travel, out of the weapon's own CWeaponInfo.
//
// Returns false when any link in the chain fails, and the caller falls back rather than guessing -
// the same rule the rest of the address table follows.
static bool WeaponRange(float *out, int *weaponType) {
	auto ped = ReadAddrU32(VCSAddr::PlayerBase);
	if (!ped || !*ped)
		return false;
	auto slot = ReadAddrAsU32(VCSAddr::WeaponIndex);
	// The game reads the slot with `lb`, so a -1 for "no weapon" arrives here as 255. Range-check
	// before it becomes an array index.
	if (!slot || *slot > 12)
		return false;

	const u32 record = *ped + kVCSWeaponRecordsOffset + *slot * kVCSWeaponRecordStride;
	auto type = ReadU32(record + kVCSWeaponRecordTypeOffset);
	// The live table runs out at about 40; past that the entries decode as noise. Anything beyond
	// that is a bad read, not an exotic weapon.
	if (!type || *type > 40)
		return false;
	if (weaponType)
		*weaponType = (int)*type;

	auto select = ReadU8(kVCSWeaponInfoTableSelect);
	auto tablePtr = ReadU32(kVCSWeaponInfoTablePtr);
	if (!select || !tablePtr || !*tablePtr)
		return false;
	auto base = ReadU32(*tablePtr + (*select ? 0 : 4));
	if (!base || !*base)
		return false;

	auto range = ReadFloat(*base + *type * kVCSWeaponInfoStride + kVCSWeaponInfoRangeOffset);
	if (!range || !(*range > 1.0f) || !(*range < 1000.0f))
		return false;
	*out = *range;
	return true;
}

// THE SHOT'S SOURCE IS BORROWED, NOT MOVED - and the muzzle flash is why.
//
// Writing the camera-ray origin into the game's own point1 is correct for the RAYCAST and wrong for
// everything the caller does afterwards. CWeapon::FireInstantHit draws the gunflash from that same
// vector once the raycast returns, so the flash was being drawn on the camera axis a few metres
// ahead of the lens - i.e. dead centre of the screen, at point-blank range. Reported in play as
// losing all visibility the moment you fire.
//
// Turning useCameraOrigin off does fix the flash, and costs the accuracy it exists for: the ray then
// runs from the gun PARALLEL to the camera ray, so it never converges on the crosshair at any
// distance rather than at all of them.
//
// So the origin is restored the instant the raycast has read it - see kVCSWeaponRaycastDone. The
// bullet gets the exact camera ray; the flash gets the muzzle. Nothing else in the frame can observe
// the borrowed value, because the wrapper does nothing between the two points but return.
static bool g_restorePending = false;
static u32 g_restoreAddr = 0;
static Vec3 g_restoreSource;

int Hook_vcs_weapon_raycast_done() {
	if (!g_restorePending) {
		return 0;
	}
	g_restorePending = false;
	WriteVec3(g_restoreAddr, g_restoreSource);
	return 0;
}

int Hook_vcs_weapon_raycast() {
	const VCSFireHookSettings &s = FireHookSettings();
	if (!s.enabled || !IsActive())
		return 0;

	g_seen++;

	// a0 and a1 are point1 and point2 - the wrapper forwards them untouched, so they are the
	// same pointers CWorld::ProcessLineOfSight is about to read. Confirmed by disassembly: the
	// wrapper at 0x08a41d28 only shuffles t0-t5 and the stack, and its delay slot stores t4, so
	// nothing an argument register holds is set up after the point we run.
	const u32 srcPtr = PARAM(0);
	const u32 dstPtr = PARAM(1);

	Vec3 source, target;
	if (!ReadVec3(srcPtr, &source) || !ReadVec3(dstPtr, &target))
		return 0;

	// Guard, in two independent halves, because getting this wrong redirects other people's
	// bullets through the player's camera. The first half says the player is aiming in a mode
	// this fork steers; the distance says this particular ray is the player's. Neither alone is
	// enough - NPCs shoot through this same wrapper, and they do it while the player is aiming too.
	//
	// It asks CameraDrivenAimHeld rather than whether the aim control is down, and the difference
	// only appeared with the pad. This redirects the shot along the CAMERA ray, which is right
	// when the camera is the aim and wrong under lock-on: there the game has picked a target the
	// camera need not be pointing at, and redirecting would send the bullet past it. The mouse
	// always free aims, so for the mouse the two are the same question and this path is unchanged.
	if (!CameraDrivenAimHeld())
		return 0;

	Vec3 player;
	if (!PlayerPosition(&player) || Dist(source, player) > s.playerRadius)
		return 0;

	Vec3 aimed;
	VCSFireHookTrace trace;
	trace.gameRange = Dist(source, target);
	StoreVec3(trace.gunSource, source);

	// How far to fire. The game's own target length is the last resort and not the default,
	// because it means three different things on three different code paths - see the header.
	float range = s.fallbackRange;
	if (s.useWeaponRange) {
		float w = 0.0f;
		if (WeaponRange(&w, &trace.weaponType))
			range = w;
		else if (trace.gameRange > range)
			range = trace.gameRange;
	} else if (trace.gameRange > range) {
		range = trace.gameRange;
	}

	// The camera ray is the only way the shot is built now. The angle-derived fallback that used
	// to stand behind it was the pre-CameraRay construction, kept while the two were being
	// compared; no ray at all is the honest answer to CameraRay failing, and letting the game
	// resolve the shot its own way is a better one than firing along a guess.
	Vec3 camPos;
	Vec3 dir;
	if (!CameraRay(&camPos, &dir, &trace))
		return 0;

	Vec3 origin = source;
	if (s.useCameraOrigin) {
		// re3's Find3rdPersonCamTargetVector, exactly: start at the camera and slide the origin
		// along the ray to the point nearest the muzzle. The ray stays collinear with the pixel
		// the crosshair covers, which is the whole point - but it no longer starts behind the
		// player, so it cannot hit geometry between the camera and the gun.
		const float t = (source.x - camPos.x) * dir.x
		              + (source.y - camPos.y) * dir.y
		              + (source.z - camPos.z) * dir.z;
		origin = Vec3{camPos.x + dir.x * t,
		              camPos.y + dir.y * t,
		              camPos.z + dir.z * t};
		// The origin moved, so the game's own source has to move with it or the bullet would
		// travel a different line from the one we solved.
		//
		// Borrowed for the duration of the raycast only. Arm the restore BEFORE the write, so a
		// failed write cannot leave the game holding our origin with nothing scheduled to take
		// it back.
		g_restoreAddr = srcPtr;
		g_restoreSource = source;
		g_restorePending = true;
		WriteVec3(srcPtr, origin);
	}

	aimed = Vec3{origin.x + dir.x * range,
	             origin.y + dir.y * range,
	             origin.z + dir.z * range};
	trace.range = range;
	StoreVec3(trace.origin, origin);
	StoreVec3(trace.dir, dir);

	if (WriteVec3(dstPtr, aimed)) {
		g_redirected++;
		trace.valid = true;
		g_trace = trace;
	}

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

	// Verify the game's own instruction is there before touching it.
	//
	// This is what makes installing after module load safe. PPSSPP's normal replacement path runs
	// from MIPSAnalyst while the module is being scanned; ours cannot, because VCS::Init runs at
	// __KernelInit - BEFORE the EBOOT is loaded - so anything written then is overwritten by the
	// module loader. That is exactly what happened on the first attempt: the install reported
	// success and the hook never fired once. Installing from Tick fixes the timing, and this check
	// makes sure we only ever patch the instruction we measured.
	auto op = ReadU32(kVCSWeaponRaycastCall);
	if (!op || *op != kVCSWeaponRaycastOp) {
		return;  // Not loaded yet, or not this build. Try again next tick.
	}

	// Installed by ADDRESS rather than by the usual function-hash match. The PSP has no ASLR and
	// this is a single known build (ULUS10160), so the address is stable; and the hash path would
	// need the analyser to have found and named this function first, which it has not.
	if (WriteReplaceInstructionAt(kVCSWeaponRaycastCall, index)) {
		// Writing code after the JIT may already have compiled the block containing it means the
		// cached block has to go, or the game keeps running the original instruction. The normal
		// replacement path never needs this because it runs before anything is compiled.
		currentMIPS->InvalidateICache(kVCSWeaponRaycastCall, 4);
		g_installed = true;
		INFO_LOG(Log::HLE, "VCS: fire hook installed at %08x", kVCSWeaponRaycastCall);
	}

	// The restore point, on the same terms: verified opcode, installed by address, and OPTIONAL.
	//
	// Optional because the two are not equal partners. Without the first hook there is no free aim
	// at all; without this one there is free aim with a muzzle flash in the wrong place. So a
	// failure here warns and leaves the shot redirect running, rather than taking the feature down
	// with it - but useCameraOrigin has to stand down, since the borrowed origin would then never
	// be given back and every shot would draw its flash on the camera axis.
	const int doneIndex = GetReplacementFuncIndexByName("vcs_weapon_raycast_done");
	auto doneOp = ReadU32(kVCSWeaponRaycastDone);
	if (doneIndex >= 0 && doneOp && *doneOp == kVCSWeaponRaycastDoneOp) {
		if (WriteReplaceInstructionAt(kVCSWeaponRaycastDone, doneIndex)) {
			currentMIPS->InvalidateICache(kVCSWeaponRaycastDone, 4);
			g_doneInstalled = true;
			INFO_LOG(Log::HLE, "VCS: fire hook restore installed at %08x", kVCSWeaponRaycastDone);
		}
	}
	if (!g_doneInstalled && FireHookSettings().useCameraOrigin) {
		WARN_LOG(Log::HLE, "VCS: no raycast restore point at %08x - camera-origin ray off, "
			"or the muzzle flash would draw at the camera", kVCSWeaponRaycastDone);
		FireHookSettings().useCameraOrigin = false;
	}
}

void RemoveFireHook() {
	if (!g_installed)
		return;
	RestoreReplacedInstruction(kVCSWeaponRaycastCall);
	currentMIPS->InvalidateICache(kVCSWeaponRaycastCall, 4);
	g_installed = false;
	if (g_doneInstalled) {
		RestoreReplacedInstruction(kVCSWeaponRaycastDone);
		currentMIPS->InvalidateICache(kVCSWeaponRaycastDone, 4);
		g_doneInstalled = false;
	}
	// Anything still owed is owed to a game that is going away. Drop it rather than letting the
	// next install find a restore armed against a stale address.
	g_restorePending = false;
	g_seen = 0;
	g_redirected = 0;
	g_trace = VCSFireHookTrace();
}

}  // namespace VCS
