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

// --- Which of the eight this port wrote by itself ---------------------------------------------
//
// The auto-save does not write the file: it opens the game's own save menu and the GAME writes
// it, which is the whole reason the saves it makes are trustworthy. The cost is that they are
// also indistinguishable - same routine, same PARAM.SFO, same fields - so "this one was
// automatic" is knowledge that exists only in this process and has to be written down.
//
// A line per slot in a file of our own, beside vcs.ini. Deliberately NOT a file inside the save
// directory: that directory belongs to the game, and the one thing worse than a missing label is
// a stray file in a savedata folder the firmware enumerates.
//
// THE TIMESTAMP IS WHAT KEEPS IT HONEST, and it is not defensive habit. The player can overwrite
// any slot from a save pickup at a safe house, which never comes through this port at all - so a
// bare "slot 0 is automatic" would go on claiming a save the player made by hand, forever. The
// mtime recorded is the one the file had when the auto-save finished; a slot whose file has moved
// on since is simply no longer the save this ledger is about, and the mark drops on its own.
static Path AutoSaveLedgerPath() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "vcs_autosaves.txt";
}

// Zero for a slot with no record. Missing, unreadable and malformed all land here as "none of
// them", because a label is not worth a failure path: the list still draws, without the mark.
static void ReadAutoSaveLedger(int64_t (&when)[kVCSSaveSlots]) {
	for (int i = 0; i < kVCSSaveSlots; i++) {
		when[i] = 0;
	}
	std::string raw;
	if (!File::ReadTextFileToString(AutoSaveLedgerPath(), &raw) || raw.empty()) {
		return;
	}
	size_t pos = 0;
	while (pos < raw.size()) {
		size_t end = raw.find('\n', pos);
		if (end == std::string::npos) {
			end = raw.size();
		}
		int index = -1;
		long long stamp = 0;
		if (sscanf(raw.substr(pos, end - pos).c_str(), "%d %lld", &index, &stamp) == 2) {
			if (index >= 0 && index < kVCSSaveSlots && stamp > 0) {
				when[index] = (int64_t)stamp;
			}
		}
		pos = end + 1;
	}
}

static void WriteAutoSaveLedger(const int64_t (&when)[kVCSSaveSlots]) {
	std::string out;
	for (int i = 0; i < kVCSSaveSlots; i++) {
		if (when[i] > 0) {
			char line[64];
			snprintf(line, sizeof(line), "%d %lld\n", i, (long long)when[i]);
			out += line;
		}
	}
	if (out.empty()) {
		// Nothing left to remember. Removed rather than left as an empty file, so the ordinary
		// state of a memory stick with no auto-saves on it is no file at all.
		File::Delete(AutoSaveLedgerPath(), true);
		return;
	}
	File::WriteStringToFile(true, out, AutoSaveLedgerPath());
}

void NoteAutoSave(int index) {
	if (index < 0 || index >= kVCSSaveSlots) {
		return;
	}
	const std::string discID = GetDiscID();
	if (discID.empty()) {
		return;
	}
	// The file's own timestamp, read back rather than taken from the clock: what has to match
	// later is what the filesystem says about this file, and a clock reading taken a moment
	// earlier is a different number.
	File::FileInfo info;
	const Path sfo = GetSysDirectory(PSPDirectories::DIRECTORY_SAVEDATA)
		/ SlotDirName(discID, index) / "PARAM.SFO";
	if (!File::GetFileInfo(sfo, &info) || !info.exists) {
		return;  // no file to mark, which means the save did not land after all
	}

	int64_t when[kVCSSaveSlots];
	ReadAutoSaveLedger(when);
	when[index] = (int64_t)info.mtime;
	WriteAutoSaveLedger(when);
}

std::vector<SaveSlot> EnumerateSaves() {
	std::vector<SaveSlot> slots;
	slots.reserve(kVCSSaveSlots);

	const std::string discID = GetDiscID();
	const Path root = GetSysDirectory(PSPDirectories::DIRECTORY_SAVEDATA);

	int64_t autoWhen[kVCSSaveSlots];
	ReadAutoSaveLedger(autoWhen);

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
		// The mark only survives while the file is still the one that was marked. See the note
		// over AutoSaveLedgerPath for the save pickup this is guarding against.
		slot.autoSave = autoWhen[i] != 0 && autoWhen[i] == slot.modified;

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
	if (!File::DeleteDirRecursively(dir)) {
		return false;
	}
	// The ledger entry goes with it. Not strictly needed - the timestamp check already refuses to
	// mark a file that is not the one recorded - but a record of a save that no longer exists is
	// a record that will read as true again the moment the slot happens to be written at the same
	// second, and it costs one line to not leave that lying around.
	int64_t when[kVCSSaveSlots];
	ReadAutoSaveLedger(when);
	if (when[index] != 0) {
		when[index] = 0;
		WriteAutoSaveLedger(when);
	}
	return true;
}

}  // namespace VCS
