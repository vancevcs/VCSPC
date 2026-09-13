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

#include <algorithm>
#include <cstring>
#include <vector>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSCheats.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSRadar.h"
#include "Core/VCS/VCSRoute.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSSettings.h"
#include "Core/VCS/VCSState.h"
#include "Core/VCS/VCSVault.h"
#include "Core/VCS/VCSWorld.h"

namespace VCS {

// GTA: Vice City Stories, USA. The PAL (ULES00502/ULES00503) and JP (ULJM05297) releases have
// different builds and therefore different addresses, so they deliberately aren't accepted here
// even though the compat flag could be set for them. Supporting them means a second address
// table, not just another disc ID.
static const char *kVCSDiscIDUSA = "ULUS10160";

static bool g_active = false;
static std::string g_discID;
static u64 g_tickCount = 0;

// Defined with the disc patch itself, further down - Init only has to forget last boot's answer.
static void ResetDiscPatches();

// --- Boot phase ---
//
// Measured: FrameCounter is 0 for the whole of the logos and credits, and becomes nonzero - a
// fixed 30602, deterministically - the instant the world starts. That edge is the seam we hang
// the startup menu on.
//
// This is NOT the reverted "logic stopped means menu" idea from 03c1f3cfe0. That watched the
// counter stall *continuously* during play, where loading screens and cutscenes make it wrong.
// This reads the first 0 -> nonzero transition after boot, once. It is monotonic, so the
// ambiguity that sank the other one cannot arise.
static BootPhase g_bootPhase = BootPhase::Intro;
static int g_introSkipFrames = 0;

// Long enough for the game to notice the press across its 30Hz logic rate.
static constexpr int kIntroSkipHoldFrames = 8;

BootPhase GetBootPhase() {
	return g_bootPhase;
}

void NotifyMenuDismissed() {
	g_bootPhase = BootPhase::Playing;
}

void RequestIntroSkip() {
	if (g_bootPhase == BootPhase::Intro) {
		g_introSkipFrames = kIntroSkipHoldFrames;
	}
}

static void UpdateBootPhase() {
	if (g_bootPhase != BootPhase::Intro) {
		return;
	}

	// Cross is what skips the credits - measured, and Start on its own does not do it.
	if (g_introSkipFrames > 0) {
		g_introSkipFrames--;
		__CtrlUpdateButtons(CTRL_CROSS, 0);
	}

	const std::optional<u32> frames = ReadAddrU32(VCSAddr::FrameCounter);
	if (frames && *frames != 0) {
		g_bootPhase = BootPhase::AtMenu;
		g_introSkipFrames = 0;
	}
}

// Push one of the game's own settings across, if the game does not already agree.
//
// Written only on a disagreement, rather than every tick. The game changes these from its own
// front end and nowhere else, so a matching value means there is genuinely nothing to do - and
// re-asserting a value the game already holds is the shape of mistake the camera pitch runaway
// taught. It is still self-healing: a new game, or a load that restores the preferences out of a
// save, is simply a disagreement on the next tick.
//
// Reads first and writes only on a successful read, so before the object exists - during a load,
// or on a build where the address is wrong - this does nothing at all rather than writing into
// whatever happens to be there.
static void ApplyPref(VCSAddr id, int want) {
	const std::optional<u32> current = ReadAddrAsU32(id);
	if (!current || (int)*current == want) {
		return;
	}

	const std::optional<u32> address = ResolveAddr(id);
	if (!address) {
		return;
	}

	if (LookupAddr(id).type == VCSAddrType::U8) {
		WriteU8(*address, (u8)want);
	} else {
		WriteU32(*address, (u32)want);
	}
}

// The game's own settings the menu owns: two switches off its Display page and two volumes off
// its Audio page.
static void ApplyGamePrefs() {
	const VCSGameSettings &settings = GameSettings();

	ApplyPref(VCSAddr::ShowSubtitles, settings.subtitles ? 1 : 0);
	ApplyPref(VCSAddr::HudMode, settings.hud ? 1 : 0);

	// Two writes each, and both are what the game's own slider does. The live level is what you
	// hear; the preference is what its Audio page reads back and what goes into a save. Setting
	// only the first is a volume that forgets itself, and only the second is a row that moves and
	// changes nothing - see the note over these entries in VCSAddresses.h.
	//
	// And the live SFX level is 0 while the bridge is walking the game's own front end, which is
	// the one place the two layers being separate pays for itself. Picking MAP, BRIEF or STATS
	// makes this port press Start and then tab across the game's menu, and the game blips at
	// every one of those presses - button sounds for buttons the player never touched, behind a
	// curtain that is there precisely so they do not watch it happen. Muting the CHANNEL rather
	// than the emulator keeps the radio playing through it, which is what the player is actually
	// listening to.
	//
	// The preference is left alone throughout, so nothing about this reaches the game's Audio
	// page or a save - and the level comes back on its own the tick after the walk ends, because
	// ApplyPref is a disagreement check rather than a one-shot.
	const int sfx = FrontEndDriving() ? 0 : settings.sfxVolume;
	ApplyPref(VCSAddr::SfxVolume, sfx);
	ApplyPref(VCSAddr::SfxVolumePref, settings.sfxVolume);
	ApplyPref(VCSAddr::RadioVolume, settings.radioVolume);
	ApplyPref(VCSAddr::RadioVolumePref, settings.radioVolume);
}

void PatchLoadedModule() {
	if (!IsActive()) {
		return;
	}
	const float wanted = GameSettings().worldMemory;
	if (!(wanted > 1.0f) || !Memory::IsValid4AlignedAddress(kVCSMainPoolReserve)) {
		return;
	}
	// Nothing has run yet, so this is the raw instruction rather than anything the JIT has
	// had an opinion about - which is the whole reason the patch happens here.
	if (Memory::ReadUnchecked_U32(kVCSMainPoolReserve) != kVCSMainPoolReserveOp) {
		WARN_LOG(Log::System, "VCS: %08x does not hold the pool reserve on this build - "
			"leaving the world memory alone", kVCSMainPoolReserve);
		return;
	}

	// Only ever hand the streaming heap memory a real PSP-1000 never had. That is what makes
	// this safe without measuring anything: MainMemoryManager keeps exactly the bytes it had
	// at retail, and the extra partition - which it would otherwise absorb, since it sizes
	// itself as everything-minus-the-reserve - goes to the world instead.
	const u32 extra = Memory::g_MemorySize > Memory::RAM_NORMAL_SIZE
		? Memory::g_MemorySize - Memory::RAM_NORMAL_SIZE : 0;
	const u32 ceiling = kVCSMainPoolReserveStock + extra;
	u64 want = (u64)((double)kVCSMainPoolReserveStock * wanted);
	if (want > ceiling) {
		want = ceiling;
	}
	// A lui immediate, so the reserve is a multiple of 64K by construction.
	const u32 reserve = (u32)want & 0xFFFF0000u;
	if (reserve <= kVCSMainPoolReserveStock) {
		return;
	}
	Memory::WriteUnchecked_U32(0x3C040000u | (reserve >> 16), kVCSMainPoolReserve);
	INFO_LOG(Log::System, "VCS: holding %.1f MB back for the world (retail is %.1f MB), "
		"so the streaming heap gets about %.1f MB instead of 4.8",
		reserve / 1048576.0f, kVCSMainPoolReserveStock / 1048576.0f,
		(reserve - 0x67000) / 1048576.0f);
}

void Init() {
	g_active = false;
	g_discID.clear();
	g_tickCount = 0;
	g_bootPhase = BootPhase::Intro;
	g_introSkipFrames = 0;

	// Per boot: a different image, or a replacement file that appeared or was deleted between
	// runs, has to be looked at again rather than remembered.
	ResetDiscPatches();

	ResetHostKeys();
	ClearSharedState();
	CameraReset();
	CheatReset();
	FrontEndReset();
	MapCursorReset();
	RouteReset();
	RadarReset();

	g_discID = g_paramSFO.GetDiscID();

	// Two independent gates. The compat flag alone isn't enough, because a user can put anything
	// in PSP/System/compat.ini and we'd rather do nothing than read a different game's memory.
	if (!PSP_CoreParameter().compat.flags().VCSInputOverhaul) {
		return;
	}

	if (g_discID != kVCSDiscIDUSA) {
		WARN_LOG(Log::System, "VCSInputOverhaul is set for disc ID %s, but only %s is supported. Staying inactive.",
			g_discID.c_str(), kVCSDiscIDUSA);
		return;
	}

	g_active = true;

	// Only now, once we know this is actually VCS. The settings apply to nothing otherwise, and
	// reading the file for every game booted would be work done for no one.
	LoadSettings();

	int known = 0;
	for (size_t i = 0; i < ARRAY_SIZE(kVCSAddresses); i++) {
		if (IsAddrSet(kVCSAddresses[i].id)) {
			known++;
		}
	}

	INFO_LOG(Log::System, "VCS input overhaul active for %s (%d/%d addresses known)",
		g_discID.c_str(), known, (int)VCSAddr::Count);
}

void Shutdown() {
	// Release any buttons we were holding before the ctrl module goes away.
	ResetHostKeys();
	ClearSharedState();
	CameraReset();
	// A combination half typed into a game that is going away is not worth finishing, and the
	// queue must not survive into the next boot.
	CheatReset();
	FrontEndReset();
	// Put the game's own instruction back before anything else tears down.
	RemoveFireHook();
	RemoveClimbSplashHook();
	VaultReset();
	RemoveWorldQuery();

	g_active = false;
	g_discID.clear();
	g_tickCount = 0;
}

void Tick() {
	if (!g_active) {
		return;
	}

	g_tickCount++;

	// What stutter IS, measured where the player feels it: host time between two vblanks. Normal is
	// 16.7 ms and a 30 fps game frame is two of them; past 100 ms it is logged, with the game's own
	// frame counter so a run can line it up against anything else in the log. The pause menu and
	// loading screens trip it too, and are easy to tell apart by where they fall.
	{
		static double s_lastTickTime = 0.0;
		const double now = time_now_d();
		if (s_lastTickTime > 0.0 && now - s_lastTickTime > 0.1) {
			const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
			NOTICE_LOG(Log::System, "VCS: %.0f ms between vblanks at game frame %u",
				(now - s_lastTickTime) * 1000.0, frame ? *frame : 0);
		}
		s_lastTickTime = now;
	}

	// Cheap, and the front end needs it before anything else this tick.
	UpdateBootPhase();

	// The game's own settings the menu owns. Nothing else this tick depends on them, so the
	// position is only about keeping it away from the input ordering below, which does.
	ApplyGamePrefs();

	// Free aim at the fire site. Installed HERE rather than in Init, because Init runs at
	// __KernelInit - before the EBOOT is loaded - so anything written there is overwritten by the
	// module loader. It self-guards on already-installed and on finding the expected instruction,
	// so retrying every tick until the code exists costs a single compare.
	InstallFireHook();

	// The world query, installed on the same terms and for the same reason - it writes a small
	// program into PSP memory, which cannot happen before there is a game to write it next to.
	//
	// Gated on vaulting being ON, unlike the fire hook, because unlike the fire hook it TAKES
	// something: 512 bytes out of the game's own user memory partition. That is almost certainly
	// harmless - the game sizes its pools from a fixed budget rather than from what is left - but
	// "almost certainly harmless" is not a reason to do it to someone who has not asked for the
	// feature. Nothing gives the block back until shutdown; a call could still be in flight.
	if (VaultSettings().enabled) {
		InstallWorldQuery();
		// And the hook that keeps the climb-out's water splash off dry land. It takes nothing from
		// the game and writes one instruction, but it is only ever about vaulting - so it goes on
		// the same switch.
		InstallClimbSplashHook();
	}

	// Decode first, then map - the context depends on what we just read.
	UpdateSharedState();
	const VCSInputContext context = ResolveContext(GetState());

	// Collect any answer the game left us, then let vaulting ask its next question and drive
	// whatever climb is running.
	//
	// BEFORE ApplyMapping, and that ordering is the whole trigger: a vault starts on the tick the
	// jump key goes down, and the mapping has to already know that so it can send the game nothing
	// instead of a jump. Reversed, every vault would begin with a hop.
	WorldQueryTick();
	VaultTick(context);

	// Advance any cheat combination the menu queued. BEFORE ApplyMapping for the same reason
	// VaultTick is: it decides on this tick whether it owns the pad, and the mapping has to
	// already know that so it can send the game the sequencer's press instead of the player's
	// keys. Reversed, the first press of every combination would go out with a held W beside it.
	CheatTick();

	// And the bridge into the game's own front end, on the same terms and in the same place: it
	// decides on this tick whether it owns the pad, and ApplyMapping has to already know.
	FrontEndTick();

	// The map's own cursor: put the game's full-screen cross away while the map is up, and work
	// out where ours goes. Emu thread, because it patches an instruction and reads the widget.
	MapCursorTick();

	// The GPS line. RadarTick is the one that matters: it reads the radar's origin, facing and
	// range, projects the route through the game's own transform and leaves screen-space segments
	// for the UI thread to draw. It writes no PSP memory.
	RadarTick();

	ApplyMapping(context);

	// The pad's right stick becomes look movement here, and it has to be BEFORE the three
	// consumers below rather than beside them: it FILLS the accumulator they drain. Run after
	// them and the stick would be one frame behind in every context, which on a camera reads as
	// lag rather than as nothing happening.
	ApplyPadLook(context);

	// These MUST stay in this order. Both want this frame's mouse delta and exactly one of them
	// gets it, decided by context and settings:
	//
	//   ApplyAnalog   takes it when the LEFT stick is the reticle (free aim)
	//   CameraTick    takes whatever it did not claim, and turns it into a rotation
	//
	// Reordering these lines would silently break aiming - the camera would consume the movement
	// and the crosshair would never move.
	//
	// Opens the aim model's frame. ApplyAnalog asks it what deflection to write and it must only
	// step once, so this has to come first.
	AimModelBeginFrame();

	ApplyAnalog(context);
	// Before CameraTick, so free aim gets the delta ahead of the camera - same one-consumer rule
	// as the line above.
	AimTick(context);

	// Mouse look last, so it sees the context we just resolved. This writes memory rather than
	// pressing buttons, which is why it isn't part of VCSInput.
	CameraTick(context);

	// After CameraTick, because it steers by the camera yaw and wants this frame's value rather
	// than last frame's. Writes the ped's velocity, so it has to run every frame or be wiped.
	FreeAimMoveTick(context);

	// Also after CameraTick, and for the same reason: it turns the character to the camera's yaw
	// and wants the value CameraTick just wrote, not the one from before it ran.
	PedAimTick(context);
}

bool IsActive() {
	return g_active;
}

VCSGameSettings &GameSettings() {
	static VCSGameSettings settings;
	return settings;
}

bool IsGameBuild() {
#ifdef _DEBUG
	return false;
#else
	return true;
#endif
}

bool PresentAsGame() {
#ifdef _DEBUG
	// The Debug build is the workshop. It keeps the menu bar, the ImGui debugger and the
	// overlays, because that is what every measurement and every address hunt is done with.
	return false;
#else
	// And the Release build is the GAME, unconditionally.
	//
	// This used to fall back to "is a VCS disc remembered", asked before anything booted, and
	// memoised for the session. On a fresh copy nothing is remembered, so the answer was NO for
	// the whole of a first run - and a first run is exactly when somebody who has just unzipped
	// this meets it. They got the PPSSPP logo screen, a title bar reading PPSSPP and its version,
	// and an emulator's chrome around a game they downloaded to play. Reported, bluntly and
	// fairly, as "no ppsspp art, signs".
	//
	// The fallback was answering a question this build does not have: whether it is being used as
	// an emulator this time. It is not, ever - the exe is named after one game, the compat flag
	// and the disc ID still gate every behaviour, and a wrong disc simply leaves the VCS layer
	// dormant while this presents as the game it says it is on the tin.
	return true;
#endif
}


// --- The disc patch ---
//
// What may be substituted, and nothing else. A blanket "any disc file the memory stick also has"
// rule is not available anyway - there are no filenames here, only offsets - but the narrowness is
// the point regardless: a wrong range writes into the middle of some other file, and the failure
// would arrive as corruption somewhere unrelated.
//
// The offset is a property of THIS disc image, found by searching it for the file's own bytes
// (Tools/vcsgxtkeys.py does the same to patch an ISO). It is not a memory address, so it does not
// belong in VCSAddresses.h, but it is measured in the same spirit and gets the same treatment: the
// disc is checked for the header that should be there before anything is overwritten.
struct VCSDiscPatch {
	const char *file;    // under memstick/PSP/VCS
	u64 offset;          // byte offset on the disc
	size_t length;       // the original's length; the replacement is padded to exactly this
	const char *magic;   // what the disc must have at `offset`, or this patch stays off
	size_t magicLen;
};

static const VCSDiscPatch kVCSDiscPatches[] = {
	// The game's text. Tutorial messages name their controls through it - see Tools/vcsgxtkeys.py.
	{ "ENGLISH.GXT", 0x04330000ull, 572710, "TABL", 4 },
};

struct VCSDiscPatchState {
	std::vector<u8> data;  // padded to length; empty means there is no replacement to apply
	bool tried = false;
	bool refused = false;  // the disc did not hold what we expected at that offset
};

static VCSDiscPatchState g_discPatches[ARRAY_SIZE(kVCSDiscPatches)];

static void ResetDiscPatches() {
	for (VCSDiscPatchState &state : g_discPatches) {
		state = VCSDiscPatchState();
	}
}

static void LoadDiscPatch(const VCSDiscPatch &patch, VCSDiscPatchState &state) {
	state.tried = true;

	const Path path = GetSysDirectory(DIRECTORY_PSP) / "VCS" / patch.file;
	std::string contents;
	if (!File::ReadBinaryFileToString(path, &contents)) {
		return;  // the normal case: nobody has generated one
	}
	if (contents.size() > patch.length) {
		ERROR_LOG(Log::System, "VCS disc patch %s is %d bytes, larger than the %d it replaces - ignored",
			patch.file, (int)contents.size(), (int)patch.length);
		return;
	}

	// Padded rather than short, so a read landing past the end of the replacement gets zeroes
	// instead of whatever the disc had there - half of one file and half of another is the one
	// outcome worth ruling out by construction.
	state.data.assign(patch.length, 0);
	memcpy(state.data.data(), contents.data(), contents.size());
	INFO_LOG(Log::System, "VCS disc patch: %s (%d bytes, padded to %d) will stand in at 0x%llx",
		patch.file, (int)contents.size(), (int)patch.length, (unsigned long long)patch.offset);
}

void PatchDiscRead(u64 positionOnIso, u8 *data, size_t bytes) {
	// The cheap gate first, and it is the one that guarantees no effect on any other game.
	if (!g_active || bytes == 0) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(kVCSDiscPatches); i++) {
		const VCSDiscPatch &patch = kVCSDiscPatches[i];
		const u64 patchEnd = patch.offset + patch.length;
		if (positionOnIso >= patchEnd || positionOnIso + bytes <= patch.offset) {
			continue;  // no overlap, which is almost every read
		}

		VCSDiscPatchState &state = g_discPatches[i];
		if (state.refused) {
			continue;
		}
		if (!state.tried) {
			LoadDiscPatch(patch, state);
		}
		if (state.data.empty()) {
			continue;
		}

		// Check the disc before writing over it, on the first read that shows us the header. The
		// same discipline the fire hook follows: if what is there is not what the offset was
		// measured against, this is a different image and the right amount to do is nothing.
		if (positionOnIso <= patch.offset && positionOnIso + bytes >= patch.offset + patch.magicLen) {
			if (memcmp(data + (patch.offset - positionOnIso), patch.magic, patch.magicLen) != 0) {
				state.refused = true;
				ERROR_LOG(Log::System, "VCS disc patch %s: 0x%llx does not hold '%s' on this disc - not patching",
					patch.file, (unsigned long long)patch.offset, patch.magic);
				continue;
			}
		}

		const u64 from = std::max(positionOnIso, patch.offset);
		const u64 to = std::min(positionOnIso + bytes, patchEnd);
		memcpy(data + (from - positionOnIso), state.data.data() + (from - patch.offset), (size_t)(to - from));
	}
}

const std::string &GetDiscID() {
	return g_discID;
}

u64 GetTickCount() {
	return g_tickCount;
}

}  // namespace VCS
