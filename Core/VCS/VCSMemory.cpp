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

#include "Core/MemMap.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

// Memory::IsValidAddress and friends are only meaningful once the memory map exists. Before a
// game boots, base is null and the unchecked readers would dereference it, so check first.
// This matters more than it looks: the debugger window can be open across a game shutdown.
static bool MemoryReady() {
	return Memory::base != nullptr;
}

std::optional<u8> ReadU8(u32 address) {
	if (!MemoryReady() || !Memory::IsValidRange(address, sizeof(u8))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_U8(address);
}

std::optional<u16> ReadU16(u32 address) {
	if (!MemoryReady() || !Memory::IsValid2AlignedAddress(address) || !Memory::IsValidRange(address, sizeof(u16))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_U16(address);
}

std::optional<u32> ReadU32(u32 address) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(u32))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_U32(address);
}

std::optional<s32> ReadS32(u32 address) {
	const std::optional<u32> value = ReadU32(address);
	if (!value) {
		return std::nullopt;
	}
	// Go through memcpy rather than a cast, to keep this well-defined for negative values.
	s32 result;
	memcpy(&result, &*value, sizeof(result));
	return result;
}

std::optional<float> ReadFloat(u32 address) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(float))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_Float(address);
}

std::optional<u32> ReadPointer(u32 address) {
	const std::optional<u32> ptr = ReadU32(address);
	if (!ptr || *ptr == 0) {
		return std::nullopt;
	}
	// A pointer that doesn't point anywhere sane is as useless as no pointer at all, and letting
	// it through would just push the failure one level up.
	if (!Memory::IsValidAddress(*ptr)) {
		return std::nullopt;
	}
	return ptr;
}

std::optional<u32> ResolveAddr(VCSAddr id) {
	const VCSAddrEntry &entry = LookupAddr(id);

	if (entry.base == kNoBase) {
		if (entry.address == kUnsetAddress) {
			return std::nullopt;
		}
		return entry.address;
	}

	// Based entry: chase the pointer stored at the base and add our offset. Only one level of
	// indirection is supported - a base that is itself based would need a loop, and nothing in
	// this game has called for it. Guard rather than silently reading the wrong thing.
	const VCSAddrEntry &baseEntry = LookupAddr(entry.base);
	if (baseEntry.base != kNoBase || baseEntry.address == kUnsetAddress) {
		return std::nullopt;
	}

	const std::optional<u32> basePtr = ReadPointer(baseEntry.address);
	if (!basePtr) {
		return std::nullopt;
	}
	return *basePtr + entry.address;
}

std::optional<u32> ReadAddrU32(VCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? ReadU32(*addr) : std::nullopt;
}

std::optional<s32> ReadAddrS32(VCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? ReadS32(*addr) : std::nullopt;
}

std::optional<float> ReadAddrFloat(VCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? ReadFloat(*addr) : std::nullopt;
}

std::optional<u32> ReadAddrAsU32(VCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	if (!addr) {
		return std::nullopt;
	}

	switch (LookupAddr(id).type) {
	case VCSAddrType::U8:
	{
		const std::optional<u8> value = ReadU8(*addr);
		return value ? std::optional<u32>(*value) : std::nullopt;
	}
	case VCSAddrType::U16:
	{
		const std::optional<u16> value = ReadU16(*addr);
		return value ? std::optional<u32>(*value) : std::nullopt;
	}
	case VCSAddrType::U32:
	case VCSAddrType::S32:
		return ReadU32(*addr);
	case VCSAddrType::Float:
		// Reinterpreting a float as an integer flag is never what the caller meant.
		return std::nullopt;
	default:
		return std::nullopt;
	}
}

std::optional<float> ReadEntityHeading(u32 entityPtr) {
	if (entityPtr == 0 || !Memory::IsValidRange(entityPtr, 0x40)) {
		return std::nullopt;
	}

	const std::optional<float> m00 = ReadFloat(entityPtr + 0x00);
	const std::optional<float> m01 = ReadFloat(entityPtr + 0x04);
	if (!m00 || !m01) {
		return std::nullopt;
	}

	// Reject anything that isn't a sane rotation row - a stale or wrong pointer would otherwise
	// produce a plausible-looking angle out of garbage.
	if (!(*m00 >= -1.05f && *m00 <= 1.05f) || !(*m01 >= -1.05f && *m01 <= 1.05f)) {
		return std::nullopt;
	}

	static const float kTwoPi = 6.28318530718f;
	static const float kHalfPi = 1.57079632679f;

	float yaw = std::atan2(*m01, *m00) - kHalfPi;
	yaw = std::fmod(yaw, kTwoPi);
	if (yaw < 0.0f) {
		yaw += kTwoPi;
	}
	return yaw;
}

bool WriteFloat(u32 address, float value) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(float))) {
		return false;
	}
	// Same unchecked writer the rest of PPSSPP uses; the bounds check above is what makes it safe.
	Memory::WriteUnchecked_Float(value, address);
	return true;
}

bool WriteAddrFloat(VCSAddr id, float value) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? WriteFloat(*addr, value) : false;
}

bool WriteU16(u32 address, u16 value) {
	// Only 2-byte alignment is required here, so IsValid4AlignedRange would wrongly reject the
	// odd-numbered pad fields. IsValidRange is the right check for a halfword.
	if (!MemoryReady() || !Memory::IsValidRange(address, sizeof(u16))) {
		return false;
	}
	Memory::WriteUnchecked_U16(value, address);
	return true;
}

bool WriteU8(u32 address, u8 value) {
	if (!MemoryReady() || !Memory::IsValidRange(address, sizeof(u8))) {
		return false;
	}
	Memory::WriteUnchecked_U8(value, address);
	return true;
}

bool WriteAddrU8(VCSAddr id, u8 value) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? WriteU8(*addr, value) : false;
}

bool WriteAddrU16(VCSAddr id, u16 value) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? WriteU16(*addr, value) : false;
}

std::optional<bool> ReadAddrBool(VCSAddr id) {
	const std::optional<u32> value = ReadAddrAsU32(id);
	if (!value) {
		return std::nullopt;
	}
	return *value != 0;
}

}  // namespace VCS
