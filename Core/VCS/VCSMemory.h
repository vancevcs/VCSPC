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

#include <optional>

#include "Common/CommonTypes.h"
#include "Core/VCS/VCSAddresses.h"

// Bounds-checked typed reads of PSP memory for the VCS layer.
//
// The whole point of this file is that a wrong address is a normal, expected condition while
// hunting for addresses - not a crash. Every read validates against PPSSPP's own memory map
// (Memory::IsValidRange and friends) and returns std::nullopt on failure rather than faulting,
// asserting or logging. Reading a garbage address a few thousand times a second while you scrub
// through candidates in the debugger window must stay completely silent.
//
// Threading: PSP memory is only coherent on the emu thread. Every function here must be called
// from there. In practice that means from VCSGame::Tick (driven by hleEnterVblank) or from the
// ImGui debugger, which PPSSPP guarantees runs on the same thread as the CPU - see the
// "Debugger threading model" section of AGENTS.md.

namespace VCS {

// --- Raw reads at an explicit address ---
// These are what the debugger scratchpad uses, since there the address comes from the user
// rather than from the table.

std::optional<u8> ReadU8(u32 address);
std::optional<u16> ReadU16(u32 address);
std::optional<u32> ReadU32(u32 address);
std::optional<s32> ReadS32(u32 address);
std::optional<float> ReadFloat(u32 address);

// Follows a pointer stored at `address` and validates the result, so callers can chase struct
// pointers without hand-rolling the "is the pointee also sane" check. Returns nullopt if either
// the pointer itself or the value it points at is out of bounds, or if the pointer is null.
std::optional<u32> ReadPointer(u32 address);

// --- Table-driven reads ---
// Return nullopt when the entry is still unset, without touching memory. That is the case that
// keeps everything working with a completely empty table.

// Where an entry actually lives right now. For an absolute entry that is just its address; for
// a based entry it chases the base pointer and adds the offset, so the result changes as the
// game reallocates. nullopt when unset, or when the base pointer isn't currently valid (e.g.
// during a load, before the player ped exists). Exposed mainly so the debugger can show it.
std::optional<u32> ResolveAddr(VCSAddr id);

std::optional<u32> ReadAddrU32(VCSAddr id);
std::optional<s32> ReadAddrS32(VCSAddr id);
std::optional<float> ReadAddrFloat(VCSAddr id);

// Reads an entry using whatever type the table says it is, widened to u32 for flag-ish checks.
// Float entries are not meaningful here and return nullopt.
std::optional<u32> ReadAddrAsU32(VCSAddr id);

// Convenience for the very common "is this flag nonzero" question. Returns nullopt when unset
// or unreadable, so callers can tell "false" apart from "don't know".
std::optional<bool> ReadAddrBool(VCSAddr id);

// Heading of an entity, taken from the transform matrix every entity starts with, and returned
// in the SAME convention as VCSAddr::CameraYaw - i.e. the camera yaw you would need to look the
// way this entity faces. Verified against a vehicle: `atan2(m01, m00) - PI/2` matches the live
// camera yaw to 0.0003 rad while driving normally.
//
// nullopt if the pointer isn't a plausible entity. Used for car-relative camera glances, which
// must key off the vehicle rather than the current camera or repeated glances compound.
std::optional<float> ReadEntityHeading(u32 entityPtr);

// --- Writes ---
// The camera layer needs to push values back into the game. Same contract as the reads: an
// out-of-bounds or unset target is a silent no-op, never a fault. Returns whether the write
// actually happened, so callers can tell "wrote it" from "nowhere to write".
//
// Emu thread only - writing PSP memory from anywhere else races the CPU.

bool WriteFloat(u32 address, float value);
bool WriteAddrFloat(VCSAddr id, float value);

// Halfword writes, for the pad's synthesised second stick. Those fields are 2-byte aligned but
// not 4-byte aligned, so they need their own accessor rather than the float/u32 ones.
bool WriteU16(u32 address, u16 value);
bool WriteAddrU16(VCSAddr id, u16 value);

bool WriteU8(u32 address, u8 value);
bool WriteAddrU8(VCSAddr id, u8 value);

// Word writes. Added for the world-query block in VCSWorld, which has to place a small program
// and its arguments in PSP memory - the one thing this fork writes that is not a game variable.
bool WriteU32(u32 address, u32 value);

}  // namespace VCS
