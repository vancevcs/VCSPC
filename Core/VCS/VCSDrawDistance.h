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

#pragma once

#include "Common/CommonTypes.h"

// Pushing the world out further than a 2006 handheld was asked to draw it.
//
// PORTED, not derived. PSPRecomp's VCS profile (profiles/vcs/host/vcs_draw_distance_patch.cpp,
// MIT) found every address this uses, against this same disc. What is NOT ported is its method:
// that project is a static recompiler, so it swaps whole recompiled functions out of a dispatch
// table and jumps to a continuation address when it is done. Nothing here can do that - PPSSPP
// runs the real MIPS, and a replacement always returns to $ra. So each of their five hooks was
// disassembled first and then re-expressed as the smallest thing that reaches the same value:
//
//   their hook                        here
//   --------------------------------  --------------------------------------------------------
//   replace CDraw::SetFarClipZ        replace it too - it is a two-instruction leaf, which is
//                                     the one case where PPSSPP's own replacement path is an
//                                     exact fit
//   replace 4 instrs at the LOD site  HOOKENTER on the single `swc1` in the middle of them,
//                                     scaling $f12 on its way into the store
//   replace 2 instrs, vehicle range   rewrite one `lui` immediate
//   replace 15 instrs, ped ranges     rewrite three `lui` immediates
//   hook the IDE init epilogue        not ported - their own comment records that it never runs,
//                                     because control reaches it as a local label
//
// Three of the five stop being code patches at all, because the constants they were reproducing
// are `lui` immediates. That is worth stating plainly: the reason this is a small file is that
// the sites were read before they were patched, not that anything was skipped.
//
// Threading: everything here is emu-thread only, like the rest of Core/VCS.

namespace VCS {

// What the player sets. Multipliers on the original PSP distances; 1.0 is stock.
//
// The ceilings are the ones PSPRecomp measured, and they are not arbitrary. World tolerates the
// most because it costs streaming and fill rate; vehicles and peds cost emulated CPU per entity
// and the mission scripts assume the original ranges, which is why their own note advises staying
// at or under 2.0 there unless a given mission has actually been tested.
struct VCSDrawDistanceSettings {
	bool enabled = false;
	float world = 1.5f;     // 1..8  - buildings, props, the far clip
	float vehicles = 1.5f;  // 1..4  - how far off-screen traffic survives
	float npcs = 1.5f;      // 1..4  - ped population ranges
};

VCSDrawDistanceSettings &DrawDistanceSettings();

// Put the patches in. Idempotent, cheap once done, and safe to call before the game module has
// loaded - it simply does nothing and can be retried. Called every tick from VCSGame, for the
// reason InstallFireHook is: VCS::Init runs before the EBOOT exists.
void InstallDrawDistance();

// Take them back out and restore every byte and every model-info value that was touched. Called
// from VCS::Shutdown, and also whenever the settings change, since re-applying from a patched
// baseline is how a multiplier ends up squared.
void RemoveDrawDistance();

// Once per frame. Applies setting changes, and walks the model-info table when it is ready -
// the table does not exist yet at the first far-clip call, so this keeps trying and then keeps
// an eye on it, because a streamed-in reload can put the original values back.
void DrawDistanceTick();

// Whether the code patches are live. The model-info table is separate - see the stats below.
bool DrawDistanceInstalled();

// One line saying why it is not, when it is not. Every branch that declines to install sets
// this, because "not installed" alone says nothing about which of several reasons applied.
const char *DrawDistanceStatus();

// For the debugger window. `models` is how many model-info entries carry scaled values right
// now, `farClip` the value the game last asked for BEFORE scaling, and `gp` the captured
// register - zero until the far-clip replacement has run once, which is also the thing that
// gates the table walk.
void DrawDistanceStats(u32 *models, float *farClipIn, float *farClipOut, u32 *gp);

// CDraw::SetFarClipZ(float), replaced. Registered in ReplaceTables as
// "vcs_draw_distance_far_clip" and installed by address, exactly like the fire hook.
int Replace_vcs_draw_distance_far_clip();

// The entity LOD store, hooked. "vcs_draw_distance_entity_lod", REPFLAG_HOOKENTER.
int Hook_vcs_draw_distance_entity_lod();

}  // namespace VCS
