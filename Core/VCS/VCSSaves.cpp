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

#include <cstdio>
#include <ctime>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/StringUtils.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/System.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSSaves.h"

namespace VCS {

// `ULUS10160S92F3`. The game builds these from a template of its own; this rebuilds the same
// string rather than matching a prefix, so a directory belonging to some other slot scheme cannot
// be mistaken for slot 3.
static std::string SlotDirName(const std::string &discID, int index) {
	char name[32];
	snprintf(name, sizeof(name), "%sS92F%d", discID.c_str(), index);
	return std::string(name);
}

// The four lines the game writes into SAVEDATA_DETAIL, on one line with the date after them.
//
// A menu row has one line to explain itself and the game wrote four, so they are joined rather
// than truncated: every one of them is something a player is choosing between saves on - which
// island, which safe house, which day, how much money, how far in.
static std::string Flatten(const std::string &detail, uint64_t modified) {
	std::string out;
	for (size_t i = 0; i < detail.size(); i++) {
		const char c = detail[i];
		if (c == '\n' || c == '\r') {
			// One separator per run of newlines, and never a trailing one.
			if (!out.empty() && out.back() != ' ') {
				out += "  ";
			}
			continue;
		}
		out += c;
	}
	// The game ends three of its four lines with a full stop, which reads as clutter once they
	// are on one line. The dots between the pieces are doing that job now.
	std::string tidy;
	for (size_t i = 0; i < out.size(); i++) {
		if (out[i] == '.' && i + 1 < out.size() && out[i + 1] == ' ') {
			tidy += " ";
			i++;
			while (i + 1 < out.size() && out[i + 1] == ' ') {
				i++;
			}
			if (!tidy.empty()) {
				tidy += "\xc2\xb7 ";  // a middle dot, which is what a list of facts wants
			}
			continue;
		}
		tidy += out[i];
	}
	if (modified) {
		const time_t t = (time_t)modified;
		struct tm local{};
#ifdef _WIN32
		localtime_s(&local, &t);
#else
		localtime_r(&t, &local);
#endif
		char when[32];
		if (strftime(when, sizeof(when), "%d %b %H:%M", &local) > 0) {
			if (!tidy.empty()) {
				tidy += "  \xc2\xb7  ";
			}
			tidy += when;
		}
	}
	return tidy;
}

std::vector<SaveSlot> EnumerateSaves() {
	std::vector<SaveSlot> slots;
	slots.reserve(kVCSSaveSlots);

	const std::string discID = GetDiscID();
	const Path root = GetSysDirectory(PSPDirectories::DIRECTORY_SAVEDATA);

	for (int i = 0; i < kVCSSaveSlots; i++) {
		SaveSlot slot;
		slot.index = i;
		if (discID.empty()) {
			slots.push_back(slot);
			continue;
		}
		slot.directory = SlotDirName(discID, i);
		const Path dir = root / slot.directory;
		const Path sfoPath = dir / "PARAM.SFO";

		File::FileInfo info;
		if (!File::GetFileInfo(sfoPath, &info) || !info.exists) {
			slots.push_back(slot);
			continue;
		}
		slot.modified = (int64_t)info.mtime;

		std::string raw;
		if (!File::ReadBinaryFileToString(sfoPath, &raw) || raw.empty()) {
			// The directory is there and its PARAM.SFO is not readable. Present, because
			// something occupies the slot, and unnamed, because nothing here can say what.
			slot.present = true;
			slot.title = "SAVE";
			slot.summary = Flatten("", (uint64_t)slot.modified);
			slots.push_back(slot);
			continue;
		}

		ParamSFOData sfo;
		if (sfo.ReadSFO((const u8 *)raw.data(), raw.size())) {
			slot.present = true;
			slot.title = sfo.GetValueString("SAVEDATA_TITLE");
			slot.detail = sfo.GetValueString("SAVEDATA_DETAIL");
			if (slot.title.empty()) {
				slot.title = "SAVE";
			}
			slot.summary = Flatten(slot.detail, (uint64_t)slot.modified);
		}
		slots.push_back(slot);
	}
	return slots;
}

int NewestSaveSlot() {
	int best = -1;
	int64_t newest = 0;
	for (const SaveSlot &slot : EnumerateSaves()) {
		if (slot.present && slot.modified >= newest) {
			newest = slot.modified;
			best = slot.index;
		}
	}
	return best;
}

bool DeleteSave(int index) {
	if (index < 0 || index >= kVCSSaveSlots) {
		return false;
	}
	const std::string discID = GetDiscID();
	if (discID.empty()) {
		return false;
	}
	const Path dir = GetSysDirectory(PSPDirectories::DIRECTORY_SAVEDATA) / SlotDirName(discID, index);
	if (!File::Exists(dir)) {
		return false;
	}
	return File::DeleteDirRecursively(dir);
}

}  // namespace VCS
