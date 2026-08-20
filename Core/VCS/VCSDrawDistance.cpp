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
#include <cstring>
#include <unordered_map>

#include "Core/VCS/VCSDrawDistance.h"

#include "Common/Log.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

VCSDrawDistanceSettings &DrawDistanceSettings() {
	static VCSDrawDistanceSettings s;
	return s;
}

namespace {

// What is actually applied right now, as opposed to what the player has set. The two diverge for
// exactly one frame whenever a slider moves, and DrawDistanceTick closes the gap by taking
// everything back out and putting it in again - see the note there on why re-applying in place is
// not an option.
VCSDrawDistanceSettings g_applied;
bool g_installed = false;

// Whether the four range constants currently hold scaled words rather than the game's own. Kept
// separately from g_installed because it decides whether the shipped-word verification still
// applies: a constant we have already rewritten is not the shipped word any more, and checking it
// against one would fail forever.
bool g_constantsApplied = false;

const char *g_status = "not installed yet";

// $gp, captured the first time the far-clip replacement runs.
//
// Deliberately not read at vblank. $gp belongs to whichever thread is current, and the tick runs
// on the emu thread at a moment that has nothing to do with the game's own thread; the far-clip
// replacement, by contrast, IS the game's thread, mid-call, with the register live. One capture
// from a place that cannot be wrong beats a read from a place that usually is not.
u32 g_gp = 0;

// The last values the game asked for and we handed on, purely so the debugger can show that the
// replacement is running and by how much it is moving the number.
float g_lastFarClipIn = 0.0f;
float g_lastFarClipOut = 0.0f;

// One model-info entry as the game shipped it. Keyed by the info pointer.
//
// The hash and type come along so a slot that has been recycled for a different model can be
// told from one that is still the same model - without that, a streamed-in reload would have its
// fresh values treated as "already scaled" and left alone at 1.0.
struct ModelBaseline {
	u32 hash = 0;
	u8 type = 0;
	float d1 = 0.0f;
	float d2 = 0.0f;
	float d3 = 0.0f;
};

std::unordered_map<u32, ModelBaseline> g_modelBaselines;
u32 g_modelsPatched = 0;
u64 g_ticks = 0;
bool g_tableDone = false;

float BitsToFloat(u32 bits) {
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

u32 FloatToBits(float f) {
	u32 bits;
	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

// Zero and negative are sentinels in model-info, and scaling a sentinel turns it into a number.
bool SaneDistance(float value) {
	return std::isfinite(value) && value > 0.0f && value < 100000.0f;
}

bool NearlyEqual(float a, float b) {
	const float scale = std::max({1.0f, std::fabs(a), std::fabs(b)});
	return std::fabs(a - b) <= scale * 1.0e-4f;
}

float ScaledOrOriginal(float original, float multiplier) {
	return SaneDistance(original) ? original * multiplier : original;
}

// --- Code patching --------------------------------------------------------------------------

// Read what the game's code REALLY says at an address.
//
// Not a plain memory read. Once a block has been compiled, the word at its first instruction is a
// 0x68xxxxxx marker rather than the game's own opcode, and a replacement leaves one there too;
// Read_Instruction resolves both back to the original. Every verification below goes through this,
// which is what lets these patches be installed at any time rather than only before first
// execution.
u32 LiveOpcode(u32 address) {
	if (!Memory::IsValid4AlignedAddress(address))
		return 0;
	return Memory::Read_Instruction(address, true).encoding;
}

// Write one instruction word, with the cache handled on both sides.
//
// Invalidating BEFORE the write is the part that is easy to get wrong. If the address happens to
// be a block start, the live word is a marker owned by the block cache, and writing under it means
// the cache and memory disagree about what is there - so the block is destroyed first, which puts
// the game's own word back, and only then is ours written over plain memory. The invalidate
// afterwards is for any block that merely CONTAINS the address without starting at it.
bool WriteInstruction(u32 address, u32 word) {
	if (!Memory::IsValid4AlignedAddress(address))
		return false;
	currentMIPS->InvalidateICache(address, 4);
	Memory::WriteUnchecked_U32(word, address);
	currentMIPS->InvalidateICache(address, 4);
	return true;
}

// A `lui` that loads the top half of a float, rewritten to load the top half of a scaled one.
//
// The whole reason the range constants are cheap: `lui $a0, 0x4270` is 60.0f with its low 16 bits
// implicitly zero, so scaling it is a matter of producing the nearest float that also has zero
// low bits. That quantises to the top 7 mantissa bits - half a unit at these magnitudes, which is
// nothing for a despawn radius and buys not having to replace any code.
u32 ScaledLui(u32 originalWord, float multiplier) {
	const float original = BitsToFloat((originalWord & 0xFFFFu) << 16);
	if (!SaneDistance(original))
		return originalWord;

	const u32 bits = FloatToBits(original * multiplier);
	// Round to nearest rather than truncating, and refuse to carry into the exponent's top bit.
	const u32 rounded = (bits > 0xFFFF7FFFu) ? bits : bits + 0x8000u;
	return (originalWord & 0xFFFF0000u) | (rounded >> 16);
}

struct ConstantPatch {
	u32 address;
	u32 expected;
	const float *multiplier;
};

// The four range constants, and which slider owns each. The 120.0f at 0x089cb398 is not here on
// purpose - see the note in VCSAddresses.h.
const ConstantPatch *ConstantPatches(int *count) {
	static const VCSDrawDistanceSettings &s = DrawDistanceSettings();
	static const ConstantPatch patches[] = {
		{ kVCSVehicleRangeConst, kVCSVehicleRangeConstOp, &s.vehicles },
		{ kVCSPedRangeConstA, kVCSPedRangeConstAOp, &s.npcs },
		{ kVCSPedRangeConstB, kVCSPedRangeConstBOp, &s.npcs },
		{ kVCSPedRangeConstC, kVCSPedRangeConstCOp, &s.npcs },
	};
	*count = (int)ARRAY_SIZE(patches);
	return patches;
}

// --- The model-info table -------------------------------------------------------------------

// Scale every map object's draw distances, keeping what they started as.
//
// Returns true once it has actually written something, so the caller can stop retrying. At the
// first far-clip calls the globals are still zero and this correctly does nothing.
bool PatchModelTable() {
	const VCSDrawDistanceSettings &s = DrawDistanceSettings();
	if (!g_gp || s.world <= 1.0f)
		return false;

	auto count = ReadU32(g_gp + kVCSGpModelCount);
	auto table = ReadU32(g_gp + kVCSGpModelTable);
	if (!count || !table)
		return false;

	// A wrong $gp, or globals the game has not filled in yet, must never turn an optional
	// visual tweak into a crash. Everything past here is bounds-checked as well.
	if (*count == 0 || *count > 32768 || *table == 0 ||
	    !Memory::IsValidRange(*table, *count * 4)) {
		g_status = "model table not ready";
		return false;
	}

	u32 patched = 0;
	for (u32 i = 0; i < *count; i++) {
		auto info = ReadU32(*table + i * 4);
		if (!info || !*info || !Memory::IsValidRange(*info, 0x38))
			continue;

		auto type = ReadU8(*info + kVCSModelTypeOffset);
		if (!type || (*type != kVCSModelTypeObject && *type != kVCSModelTypeTimedObject))
			continue;

		auto hash = ReadU32(*info + kVCSModelHashOffset);
		auto d1 = ReadFloat(*info + kVCSModelDrawDist1);
		auto d2 = ReadFloat(*info + kVCSModelDrawDist2);
		auto d3 = ReadFloat(*info + kVCSModelDrawDist3);
		if (!hash || !d1 || !d2 || !d3)
			continue;

		auto found = g_modelBaselines.find(*info);
		if (found == g_modelBaselines.end() || found->second.hash != *hash ||
		    found->second.type != *type) {
			found = g_modelBaselines.emplace(*info,
				ModelBaseline{*hash, *type, *d1, *d2, *d3}).first;
		} else {
			// Same slot, same model, but the values are neither what we wrote nor what they
			// started as - the game has reloaded it. Take the new values as the baseline, or
			// the next pass would multiply an already multiplied number.
			const ModelBaseline &old = found->second;
			const bool stillPatched =
				NearlyEqual(*d1, ScaledOrOriginal(old.d1, g_applied.world)) &&
				NearlyEqual(*d2, ScaledOrOriginal(old.d2, g_applied.world)) &&
				NearlyEqual(*d3, ScaledOrOriginal(old.d3, g_applied.world));
			const bool stillOriginal =
				NearlyEqual(*d1, old.d1) && NearlyEqual(*d2, old.d2) && NearlyEqual(*d3, old.d3);
			if (!stillPatched && !stillOriginal)
				found->second = ModelBaseline{*hash, *type, *d1, *d2, *d3};
		}

		const ModelBaseline &base = found->second;
		WriteFloat(*info + kVCSModelDrawDist1, ScaledOrOriginal(base.d1, s.world));
		WriteFloat(*info + kVCSModelDrawDist2, ScaledOrOriginal(base.d2, s.world));
		WriteFloat(*info + kVCSModelDrawDist3, ScaledOrOriginal(base.d3, s.world));
		patched++;
	}

	g_modelsPatched = patched;
	return patched != 0;
}

// Put every model-info entry back to what it shipped with, and forget them.
void RestoreModelTable() {
	for (const auto &entry : g_modelBaselines) {
		const u32 info = entry.first;
		const ModelBaseline &base = entry.second;
		if (!Memory::IsValidRange(info, 0x38))
			continue;
		// Only restore a slot that still holds the model we recorded. If the game has recycled
		// it, its current values are somebody else's and must be left alone.
		auto hash = ReadU32(info + kVCSModelHashOffset);
		auto type = ReadU8(info + kVCSModelTypeOffset);
		if (!hash || !type || *hash != base.hash || *type != base.type)
			continue;
		WriteFloat(info + kVCSModelDrawDist1, base.d1);
		WriteFloat(info + kVCSModelDrawDist2, base.d2);
		WriteFloat(info + kVCSModelDrawDist3, base.d3);
	}
	g_modelBaselines.clear();
	g_modelsPatched = 0;
	g_tableDone = false;
}

}  // namespace

// --- The replaced and hooked code -------------------------------------------------------------

// float CDraw::SetFarClipZ(float)
//
// The whole original is `swc1 $f12, 0x1e74($gp)` in a `jr $ra` delay slot, so this replacement has
// to do exactly that one store and nothing else. PPSSPP returns to $ra for us.
//
// It stores unconditionally, including while the feature is off. A replacement that declined to
// do the function's job would leave the game's far clip at whatever it happened to be, and the
// install/remove path is not instantaneous - there is always a frame where this runs with the
// setting already false.
int Replace_vcs_draw_distance_far_clip() {
	const float in = PARAMF(0);
	const VCSDrawDistanceSettings &s = DrawDistanceSettings();

	g_gp = currentMIPS->r[MIPS_REG_GP];
	g_lastFarClipIn = in;

	float out = in;
	if (s.enabled && s.world > 1.0f && std::isfinite(in))
		out = in * s.world;
	g_lastFarClipOut = out;

	WriteFloat(g_gp + kVCSGpFarClipZ, out);

	// Two instructions' worth. Getting this wrong only skews timing, but there is no reason to.
	return 2;
}

// `swc1 $f12, 0x7a0($s0)` - the entity's LOD distance being set from its base.
//
// HOOKENTER, so the store still happens; all this does is scale what is about to be stored. That
// is the one difference from the patch this was ported from, which replaced the surrounding block
// and in doing so also skipped a helper call and scaled the BASE at +0x7a8 as well. Scaling only
// the derived value is both smaller and safer: the base is what everything else recomputes from,
// and multiplying it in place is how a number grows every time the code runs.
int Hook_vcs_draw_distance_entity_lod() {
	const VCSDrawDistanceSettings &s = DrawDistanceSettings();
	const float multiplier = std::max(s.vehicles, s.npcs);
	if (s.enabled && multiplier > 1.0f) {
		const float value = currentMIPS->f[12];
		if (SaneDistance(value))
			currentMIPS->f[12] = value * multiplier;
	}
	return 0;
}

// --- Install / remove -------------------------------------------------------------------------

namespace {

// Is OUR replacement the thing currently at this address?
//
// Read_Instruction rather than a raw read, because once the JIT has compiled a block starting
// here the raw word is a RUNBLOCK marker with the replacement hidden behind it. Answering this
// question off the raw word is what made a live, working hook look like a failed install.
bool ReplacementIsOurs(u32 address, int index) {
	if (index < 0 || !Memory::IsValid4AlignedAddress(address))
		return false;
	const u32 op = Memory::Read_Instruction(address, false).encoding;
	return MIPS_IS_REPLACEMENT(op) && (int)(op & MIPS_EMUHACK_VALUE_MASK) == index;
}

// Put one replacement in, and report whether it is in - which is NOT the same as whether it was
// written now.
//
// WriteReplaceInstructionAt returns false for two completely different situations: the write
// failed, and the identical replacement was already there. The second is success. Treating the
// return value as the answer meant every re-install after the first reported "refused" while the
// hook went on working perfectly, which is exactly the state the debugger showed.
bool EnsureReplacement(u32 address, int index) {
	if (index < 0)
		return false;
	if (ReplacementIsOurs(address, index))
		return true;
	WriteReplaceInstructionAt(address, index);
	currentMIPS->InvalidateICache(address, 4);
	return ReplacementIsOurs(address, index);
}

// Take one back out.
//
// The invalidate has to come FIRST. RestoreReplacedInstruction reads the raw word and does
// nothing unless it sees a replacement marker there - so if the JIT has compiled a block starting
// at this address, the raw word is a RUNBLOCK marker, the restore silently declines, and the hook
// stays live while every flag here says it is gone. Destroying the block first puts the
// replacement marker back where the restore can see it.
void RemoveReplacement(u32 address) {
	if (!Memory::IsValid4AlignedAddress(address))
		return;
	currentMIPS->InvalidateICache(address, 4);
	RestoreReplacedInstruction(address);
	currentMIPS->InvalidateICache(address, 4);
}

// Write the four range constants from their ORIGINAL words times the current multipliers.
//
// Always from the originals, never from whatever is there now - the originals are compile-time
// constants precisely so that re-applying is idempotent. Scaling what is already scaled is how a
// multiplier quietly becomes its own square.
void ApplyConstants(const VCSDrawDistanceSettings &s) {
	int patchCount = 0;
	const ConstantPatch *patches = ConstantPatches(&patchCount);
	for (int i = 0; i < patchCount; i++) {
		const float multiplier = s.enabled ? *patches[i].multiplier : 1.0f;
		const u32 word = (multiplier > 1.0f)
			? ScaledLui(patches[i].expected, multiplier)
			: patches[i].expected;
		WriteInstruction(patches[i].address, word);
	}
	g_constantsApplied = s.enabled;
}

}  // namespace

void InstallDrawDistance() {
	const VCSDrawDistanceSettings &s = DrawDistanceSettings();
	if (g_installed || !IsActive())
		return;
	if (!s.enabled) {
		g_status = "off";
		return;
	}

	// Nothing is touched until the code we measured is actually there. Before the module has
	// loaded, or on any build that is not ULUS10160, these reads simply do not match and the
	// install is retried next tick - the same contract InstallFireHook works to.
	//
	// The far-clip check reads the SECOND instruction: the first is a function entry, so it is a
	// JIT block start, and once we have replaced it the word there is ours rather than the game's.
	if (LiveOpcode(kVCSSetFarClipZ + 4) != kVCSSetFarClipZOp2) {
		g_status = "waiting for the module";
		return;
	}

	const int farClipIndex = GetReplacementFuncIndexByName("vcs_draw_distance_far_clip");
	const int lodIndex = GetReplacementFuncIndexByName("vcs_draw_distance_entity_lod");
	if (farClipIndex < 0 || lodIndex < 0) {
		g_status = "no replacement table entry";
		return;
	}

	// The LOD store is checked only while it is still the game's own instruction. Once ours is
	// there, the compare would fail forever and the install would never settle.
	if (!ReplacementIsOurs(kVCSEntityLodStore, lodIndex) &&
	    LiveOpcode(kVCSEntityLodStore) != kVCSEntityLodStoreOp) {
		g_status = "entity LOD store not where expected";
		return;
	}

	int patchCount = 0;
	const ConstantPatch *patches = ConstantPatches(&patchCount);
	for (int i = 0; i < patchCount; i++) {
		// Same reasoning: a constant we have already rewritten is not the shipped word any more.
		if (!g_constantsApplied && LiveOpcode(patches[i].address) != patches[i].expected) {
			g_status = "a range constant is not where expected";
			return;
		}
	}

	if (!EnsureReplacement(kVCSSetFarClipZ, farClipIndex)) {
		g_status = "far clip replacement would not take";
		return;
	}
	if (!EnsureReplacement(kVCSEntityLodStore, lodIndex)) {
		g_status = "entity LOD hook would not take";
		return;
	}

	ApplyConstants(s);

	g_applied = s;
	g_installed = true;
	g_tableDone = false;
	g_status = "installed";
	INFO_LOG(Log::HLE, "VCS: draw distance installed (world %.2f vehicles %.2f npcs %.2f)",
		s.world, s.vehicles, s.npcs);
}

void RemoveDrawDistance() {
	RestoreModelTable();

	if (g_constantsApplied) {
		VCSDrawDistanceSettings off;
		off.enabled = false;
		ApplyConstants(off);
	}

	RemoveReplacement(kVCSEntityLodStore);
	RemoveReplacement(kVCSSetFarClipZ);

	// The far clip itself is left at whatever the game last set. It is rewritten every frame by
	// the function we just gave back, so it corrects itself on the next one.
	g_installed = false;
	g_modelsPatched = 0;
	g_status = "off";
}

void DrawDistanceTick() {
	const VCSDrawDistanceSettings &s = DrawDistanceSettings();
	g_ticks++;

	if (!g_installed) {
		InstallDrawDistance();
		return;
	}

	// Turned off. This is the only setting change that takes the hooks back out.
	if (!s.enabled) {
		RemoveDrawDistance();
		return;
	}

	// A multiplier moved. The hooks are deliberately NOT reinstalled for this: both of them read
	// the live settings every time they run, so they are already correct the moment the slider
	// moves. Only the two things baked at write time need redoing - the constants, rewritten from
	// their originals, and the model table, rewritten from its recorded baselines.
	if (g_applied.world != s.world || g_applied.vehicles != s.vehicles ||
	    g_applied.npcs != s.npcs) {
		ApplyConstants(s);
		if (g_applied.world != s.world) {
			// Only the world multiplier reaches model-info, and re-scaling from the baselines is
			// cheap enough to just do rather than track.
			g_applied = s;
			PatchModelTable();
		}
		g_applied = s;
	}

	// The model-info table does not exist at the first far-clip call, so this keeps trying; once
	// it has taken, it is re-checked every few hundred frames, because a streamed-in reload puts
	// the original values back and nothing tells us when that happened.
	if (!g_tableDone) {
		if (PatchModelTable())
			g_tableDone = true;
	} else if ((g_ticks % 600) == 0) {
		PatchModelTable();
	}
}

bool DrawDistanceInstalled() {
	return g_installed;
}

const char *DrawDistanceStatus() {
	return g_status;
}

void DrawDistanceStats(u32 *models, float *farClipIn, float *farClipOut, u32 *gp) {
	if (models) *models = g_modelsPatched;
	if (farClipIn) *farClipIn = g_lastFarClipIn;
	if (farClipOut) *farClipOut = g_lastFarClipOut;
	if (gp) *gp = g_gp;
}

}  // namespace VCS
