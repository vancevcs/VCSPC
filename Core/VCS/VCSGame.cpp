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
#include <atomic>
#include <cstring>
#include <vector>

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(MAC)
#include <dlfcn.h>
#include <limits.h>

// The four CoreFoundation calls GameFolder needs, declared by hand: the real header brings MacTypes
// with it, and names like Point and Rect with that, which this file's neighbours use for their own.
extern "C" {
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFAllocator *CFAllocatorRef;
CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef allocator, const unsigned char *buffer, long bufLen, unsigned char isDirectory);
unsigned char CFURLGetFileSystemRepresentation(CFURLRef url, unsigned char resolveAgainstBase, unsigned char *buffer, long maxBufLen);
void CFRelease(const void *cf);
}
#endif

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/System/Display.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSChaseCam.h"
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

// Whether this boot's loading curtain has gone up yet - see kVCSLegalScreenOffset.
static bool g_bootCurtainRaised = false;

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

static void ApplyCustomTracks(bool want);

// The game's own settings the menu owns: three rows off its Display page, and two volumes and the
// custom soundtracks off its Audio page.
static void ApplyGamePrefs() {
	const VCSGameSettings &settings = GameSettings();

	ApplyPref(VCSAddr::ShowSubtitles, settings.subtitles ? 1 : 0);
	ApplyPref(VCSAddr::HudMode, settings.hud ? 1 : 0);
	// One write, unlike the volumes: the preference is what the renderer reads. See the entry.
	ApplyPref(VCSAddr::Brightness, settings.brightness);

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

	ApplyCustomTracks(settings.customTracks);
}

// Custom soundtracks, driven through the game's own setter - see kVCSSetCustomTracks. Like
// ApplyPref, only on a disagreement; unlike it, through a queued call that lands a frame or two
// later, so a request waits that long before it may be repeated rather than piling up behind
// itself. A game that says it has nothing to play (2) is left alone: there is nothing to turn on.
static std::atomic<int> g_customTracksState{-1};
static int g_customTracksWait = 0;

int CustomTracksState() {
	return g_customTracksState.load(std::memory_order_relaxed);
}

Path CustomTracksFolder() {
	return GetSysDirectory(DIRECTORY_SAVEDATA) / (std::string(kVCSDiscIDUSA) + "CUSTOMTRACKS");
}

static void ApplyCustomTracks(bool want) {
	const std::optional<u32> state = ReadAddrAsU32(VCSAddr::CustomTracksPref);
	const std::optional<u32> begin = ReadAddrAsU32(VCSAddr::CustomTracksBegin);
	const std::optional<u32> end = ReadAddrAsU32(VCSAddr::CustomTracksEnd);
	if (!state || !begin || !end) {
		g_customTracksState.store(-1, std::memory_order_relaxed);
		return;
	}
	// Nothing found is NONE to the menu whatever the preference says - a save made with tracks
	// present still says ON after they are gone, and the game's own row would say UNAVAILABLE.
	const bool haveTracks = *end > *begin;
	g_customTracksState.store(haveTracks ? (int)*state : (int)kVCSCustomTracksNone,
		std::memory_order_relaxed);
	if (g_customTracksWait > 0) {
		g_customTracksWait--;
		return;
	}
	// Only ON when there is something to play, which the game's own menu checks before it will
	// offer ON and its setter does not. And a save left ON with nothing to play is turned off,
	// which is what the game's own restore does in the same situation.
	if (!haveTracks) {
		want = false;
	}
	if (*state == kVCSCustomTracksNone || (*state == kVCSCustomTracksOn) == want) {
		return;
	}
	const std::optional<u32> prefs = ReadAddrU32(VCSAddr::DisplayPrefs);
	// Through Read_Instruction, not a raw read: the setter has run by now, so the JIT has a block
	// over it and the word in memory is the block's marker rather than the game's instruction.
	if (!prefs || *prefs == 0 || FrontEndDriving() || GameMenuActive() ||
			Memory::Read_Instruction(kVCSSetCustomTracks, true).encoding != kVCSSetCustomTracksOp) {
		return;
	}
	if (EnqueueGameCall(kVCSSetCustomTracks, *prefs, want ? kVCSCustomTracksOn : kVCSCustomTracksOff)) {
		g_customTracksWait = 30;
		// And the playing bit, once, here - the second half of what the game's own restore does.
		// Not re-asserted afterwards: the game clears it itself when a radio is retuned away from
		// the custom station, and a bit put back every tick would undo that.
		const std::optional<u32> flags = ResolveAddr(VCSAddr::CustomTracksFlags);
		const std::optional<u8> old = flags ? ReadU8(*flags) : std::nullopt;
		if (old) {
			WriteU8(*flags, (u8)((*old & ~1u) | (want ? 1u : 0u)));
		}
	}
}

// The frame limiter row - see kVCSFrameLimit. At 60 three things go on together and at 30 they all
// come off: the cap, the millisecond carry and a faster emulated CPU. A fourth, dropping frames
// rather than speed, is read by sceDisplay through ForceAutoFrameSkip.
//
// Patched LIVE, from the pause menu, unlike the radar - so the JIT's block markers are handled the
// way the move gate handles them: invalidate first, which puts the game's own word back over any
// marker, then read the raw word, write, and invalidate again. Checked every tick through
// Read_Instruction, which sees through a marker, because a savestate replaces PSP memory wholesale
// and what is installed is whatever the words say, not what this file remembers.
static bool g_frameRate60 = false;
// The clock the game ran at before 60 raised it, so 30 can give it back. 0 while not raised.
static int g_gameClockHz = 0;

// One word into the state asked for. False if it holds neither the game's op nor ours - another
// build, or code not loaded yet - and then it is left alone.
static bool SetCodeWord(u32 address, u32 stock, u32 patched, bool want) {
	if (!Memory::IsValid4AlignedAddress(address)) {
		return false;
	}
	const u32 target = want ? patched : stock;
	// The cheap answer first: invalidating every tick would throw away a hot block 60 times a second.
	if (Memory::Read_Instruction(address, true).encoding == target) {
		return true;
	}
	currentMIPS->InvalidateICache(address, 4);
	const u32 raw = Memory::ReadUnchecked_U32(address);
	if (raw != stock && raw != patched) {
		return false;
	}
	if (raw != target) {
		Memory::WriteUnchecked_U32(target, address);
		currentMIPS->InvalidateICache(address, 4);
	}
	return true;
}

static void ApplyFrameRate() {
	if (!currentMIPS) {
		return;
	}
	const bool want = GameSettings().frameRate == kVCSFrameRate60;

	// Before the EBOOT is loaded this reads as neither op and nothing is written; it simply takes
	// on the first tick the code is there.
	const bool capped = SetCodeWord(kVCSFrameLimit, kVCSFrameLimitOp, kVCSFrameLimit60Op, want);

	// The carry: all four words or none, so every one is checked before any is written.
	bool carryKnown = true;
	bool carryChanges = false;
	for (const VCSCodeWord &word : kVCSTimerCarry) {
		const u32 current = Memory::IsValid4AlignedAddress(word.address)
			? Memory::Read_Instruction(word.address, true).encoding : 0;
		if (current != word.stock && current != word.patched) {
			carryKnown = false;
		}
		if (current != (want ? word.patched : word.stock)) {
			carryChanges = true;
		}
	}
	if (carryKnown && carryChanges) {
		for (const VCSCodeWord &word : kVCSTimerCarry) {
			SetCodeWord(word.address, word.stock, word.patched, want);
		}
		// Whatever the 16 ms path last left in its remainder would otherwise land on one frame.
		if (want && Memory::IsValid4AlignedAddress(kVCSTimerRemainder)) {
			Memory::WriteUnchecked_U32(0, kVCSTimerRemainder);
		}
	}

	const bool on = want && capped;

	// The clock, unless the player has locked one of their own - PPSSPP's setting, which scePower
	// already honours over the game - or the compat database says this game must not be clocked.
	const bool mayClock = g_Config.iLockedCPUSpeed == 0 &&
		!PSP_CoreParameter().compat.flags().RequireDefaultCPUClock;
	const int clock = CoreTiming::GetClockFrequencyHz();
	if (on && mayClock) {
		if (clock != kVCSFrameRate60ClockHz) {
			// Taken again whenever it is not ours, so a game that sets its own clock meanwhile is
			// followed rather than overruled with a stale number when 30 comes back.
			g_gameClockHz = clock;
			CoreTiming::SetClockFrequencyHz(kVCSFrameRate60ClockHz);
		}
	} else if (g_gameClockHz != 0) {
		if (clock == kVCSFrameRate60ClockHz) {
			CoreTiming::SetClockFrequencyHz(g_gameClockHz);
		}
		g_gameClockHz = 0;
	}

	if (on != g_frameRate60) {
		NOTICE_LOG(Log::System, "VCS: frame limiter at %d (millisecond carry %s, CPU %d MHz)",
			on ? 60 : 30, carryKnown ? (want ? "on" : "off") : "not found",
			CoreTiming::GetClockFrequencyHz() / 1000000);
	}
	g_frameRate60 = on;
}

bool ForceAutoFrameSkip() {
	return g_active && g_frameRate60;
}

// The radar's corner, patched before the game runs for the reason the pool reserve is: these are
// immediates inside functions the game calls every frame, and by the time a frame has been drawn
// the JIT has a block over them - see the trap in AGENTS.md.
static void PatchRadarCorner() {
	if (!GameSettings().radarTopLeft) {
		return;
	}
	struct Site { u32 address; u32 stock; };
	const Site sites[] = {
		{ kVCSRadarTopWide, kVCSRadarTopWideOp },
		{ kVCSRadarTopNarrow, kVCSRadarTopNarrowOp },
	};
	for (const Site &site : sites) {
		if (!Memory::IsValid4AlignedAddress(site.address)) {
			continue;
		}
		// Checked against what the instruction should BE rather than against a flag of our own,
		// so a build that does not hold this constant is left alone instead of corrupted.
		if (Memory::ReadUnchecked_U32(site.address) != site.stock) {
			WARN_LOG(Log::System, "VCS: %08x does not hold the radar's top edge on this build - "
				"leaving the radar where the game put it", site.address);
			return;
		}
	}
	for (const Site &site : sites) {
		Memory::WriteUnchecked_U32(kVCSRadarTopMoved, site.address);
	}
	// The route line this fork draws over the radar, and the touch zone that opens the menu when
	// the radar is tapped, both work from these - so they move when the radar does rather than
	// carrying a second copy of the same decision.
	VCSRadarSettings &radar = RadarSettings();
	radar.centreX = kVCSRadarLeft + kVCSRadarSize * 0.5f;
	radar.centreY = kVCSRadarTopMovedY + kVCSRadarSize * 0.5f;
	radar.radius = kVCSRadarSize * 0.5f;
	INFO_LOG(Log::System, "VCS: the radar moves to the top-left corner, centred on (%.1f, %.1f)",
		radar.centreX, radar.centreY);
}

// The help box, out of the corner the radar has just moved into - see kVCSHelpBoxLeft. The same
// setting as the radar and patched at the same moment for the same reason; on its own it would be
// a box moved for no reason.
static void PatchHelpBox() {
	if (!GameSettings().radarTopLeft) {
		return;
	}
	struct Site { u32 address; u32 stock; u32 moved; };
	const Site sites[] = {
		{ kVCSHelpBoxLeft, kVCSHelpBoxLeftOp, kVCSHelpBoxLeftMoved },
		{ kVCSHelpBoxRight, kVCSHelpBoxRightOp, kVCSHelpBoxRightMoved },
	};
	// Both or neither: one edge moved without the other is a box twice as wide, or none at all.
	for (const Site &site : sites) {
		if (!Memory::IsValid4AlignedAddress(site.address) ||
				Memory::ReadUnchecked_U32(site.address) != site.stock) {
			WARN_LOG(Log::System, "VCS: %08x does not hold the help box's layout on this build - "
				"leaving it where the game put it", site.address);
			return;
		}
	}
	for (const Site &site : sites) {
		Memory::WriteUnchecked_U32(site.moved, site.address);
	}
}

// The language, as data rather than code: the flag that tells the game's lazy initialiser it has
// already run, and the index beside it. Every reader then takes ours instead of zeroing it - see
// kVCSGameLanguage. English is the game's own answer, so it is left entirely alone.
//
// Checked first, like the code patches: both words must still be the zeroes the loader leaves,
// which is what they are on this disc before its first frame. Anything else is a build whose
// layout differs, and that is left as it is rather than written into.
static void PatchLanguage() {
	const int language = GameSettings().language;
	if (language <= 0 || language >= kVCSLanguageCount) {
		return;
	}
	if (!Memory::IsValid4AlignedAddress(kVCSGameLanguage) ||
			Memory::ReadUnchecked_U32(kVCSGameLanguage) != 0 ||
			Memory::ReadUnchecked_U32(kVCSGameLanguageInit) != 0) {
		WARN_LOG(Log::System, "VCS: the language index is not where it should be on this build - "
			"leaving the game in English");
		return;
	}
	Memory::WriteUnchecked_U32(1, kVCSGameLanguageInit);
	Memory::WriteUnchecked_U32((u32)language, kVCSGameLanguage);
}

void PatchLoadedModule() {
	if (!IsActive()) {
		return;
	}
	PatchRadarCorner();
	PatchHelpBox();
	PatchLanguage();
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
	g_bootCurtainRaised = false;
	g_frameRate60 = false;
	g_gameClockHz = 0;

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

	// The custom soundtrack folder, so a player looking for where their music goes finds it - the
	// PSP release had the Rockstar tool make it, and nothing else ever will. Harmless to the save
	// list: every save lives in a slot folder of its own name, and the game reads this one only for
	// .gta files.
	g_customTracksState.store(-1, std::memory_order_relaxed);
	g_customTracksWait = 0;
	File::CreateFullPath(CustomTracksFolder());

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
	RemoveChaseCam();
	VaultReset();
	RemoveWorldQuery();

	// The renderer reads these on every draw of every game, and RefreshWidescreen - the only thing
	// that writes them - runs from Tick, which is gated on g_active. So without this they keep VCS's
	// last values into whatever is booted next in the same session: another game's projection
	// squashed or cropped. Zero behaviour change for every other game is the first rule in this
	// file, and a global that outlives the game that set it is the easiest way to break it.
	g_widescreenSquash = 1.0f;
	g_widescreenSquashesHud = false;
	g_widescreenSquashedDraw = false;
	g_widescreenEdgeFill = false;
	// Read by sceDisplay for every game, so it must not outlive this one. The clock needs nothing:
	// CoreTiming is set up again for whatever boots next.
	g_frameRate60 = false;
	g_gameClockHz = 0;

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
	ApplyFrameRate();

	// Free aim at the fire site. Installed HERE rather than in Init, because Init runs at
	// __KernelInit - before the EBOOT is loaded - so anything written there is overwritten by the
	// module loader. It self-guards on already-installed and on finding the expected instruction,
	// so retrying every tick until the code exists costs a single compare.
	InstallFireHook();

	// The chase camera's two hooks into the game's camera update, on the same terms: installed once
	// the code exists, re-checked every tick because a savestate can take them away, and taken back
	// out when the setting is switched off.
	InstallChaseCam();

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
	PublishDriveByCrosshair(context);

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

	// The widescreen factor, which depends on the SCREEN rather than on the game - so a rotation
	// or a resized window changes it with nothing to tell us. A few float operations once a frame
	// is cheaper than finding every place the display can change shape.
	RefreshWidescreen();

	// What the touch overlay is allowed to know, published last so it describes the frame that
	// just ran. It reads this and never PSP memory, which keeps the emu-thread rule intact
	// through a feature whose whole output is drawn by the UI.
	PublishTouchState(context);
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

#if PPSSPP_PLATFORM(MAC)
// Where the bundle really is, when macOS is running it from somewhere else. An app that is still
// quarantined and has not been moved since it was unzipped - every download without a Developer ID
// signature - runs from a randomised read-only copy under /private/var/folders, and the folder
// beside THAT holds nothing. Security.framework can name the original; the two calls are looked
// up at run time because they are not in the public headers, and without them the path stands.
static std::string UntranslocatedPath(const std::string &path) {
	void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY | RTLD_LOCAL);
	if (!security) {
		return path;
	}
	using IsTranslocatedFn = unsigned char (*)(CFURLRef, bool *, void *);
	using OriginalPathFn = CFURLRef (*)(CFURLRef, void *);
	const auto isTranslocated = (IsTranslocatedFn)dlsym(security, "SecTranslocateIsTranslocatedURL");
	const auto originalPath = (OriginalPathFn)dlsym(security, "SecTranslocateCreateOriginalPathForURL");

	std::string result = path;
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const unsigned char *)path.c_str(), (long)path.size(), true);
	bool translocated = false;
	if (url && isTranslocated && originalPath && isTranslocated(url, &translocated, nullptr) && translocated) {
		if (CFURLRef original = originalPath(url, nullptr)) {
			char buffer[PATH_MAX];
			if (CFURLGetFileSystemRepresentation(original, true, (unsigned char *)buffer, sizeof(buffer))) {
				result = buffer;
			}
			CFRelease(original);
		}
	}
	if (url) {
		CFRelease(url);
	}
	dlclose(security);
	return result;
}
#endif

static Path g_gameFolder;

void SetGameFolder(const Path &path) {
	g_gameFolder = path;
}

const Path &GameFolder() {
	Path &folder = g_gameFolder;
	if (!folder.empty()) {
		return folder;
	}
	folder = File::GetExeDirectory();
#if PPSSPP_PLATFORM(MAC)
	// .../Name.app/Contents/MacOS, up to the bundle and then out of it. A bare executable - a
	// command-line build run in place - is left where it is.
	std::string exe = folder.ToString();
	while (!exe.empty() && exe.back() == '/') {
		exe.pop_back();
	}
	const std::string inBundle = ".app/Contents/MacOS";
	if (exe.size() > inBundle.size() && exe.compare(exe.size() - inBundle.size(), inBundle.size(), inBundle) == 0) {
		const std::string bundle = UntranslocatedPath(exe.substr(0, exe.size() - strlen("/Contents/MacOS")));
		const size_t slash = bundle.rfind('/');
		if (slash != std::string::npos && slash > 0) {
			folder = Path(bundle.substr(0, slash));
		}
	}
#endif
	return folder;
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

// THE BOOT'S LOADING SCREENS, hidden behind this fork's own. After the credits the game shows a
// title-and-legal card, then the firmware's "Loading from Memory Stick" over MEMCARD.XTX while it
// autoloads, then its own loading screen while the world streams in - three screens that are the
// PSP's machinery rather than the game, and then the seam, where the fork's own auto-load raises
// its curtain anyway. So the curtain goes up at the END OF THE MOVIE instead, and the seam takes it
// over.
//
// The end of the movie, measured by logging every disc read of a boot by file: LOGO.PMF, TITLES.PMF
// (the credits), then SCEALE.XTX - the title card - the moment the movie ends or is skipped, and
// nowhere else in the boot; MEMCARD.XTX follows a second later, the LOADSC screens two after that.
// The game reads its disc by sector, so this is a position, checked against the file's header.
static constexpr u64 kVCSLegalScreenOffset = 26960ull * 2048ull;
static const u8 kVCSLegalScreenHeader[16] = {
	0x78, 0x65, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbc, 0x04, 0x04, 0x00, 0xa0, 0x04, 0x04, 0x00,
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
	// The end of the credits movie, which is when the boot's loading curtain goes up - see
	// kVCSLegalScreenOffset. Checked against the file's own header rather than trusted by position,
	// for the reason every disc patch here checks: a curtain raised over the movie by a different
	// disc layout would be worse than none.
	if (g_bootPhase == BootPhase::Intro && !g_bootCurtainRaised &&
			positionOnIso <= kVCSLegalScreenOffset &&
			positionOnIso + bytes >= kVCSLegalScreenOffset + sizeof(kVCSLegalScreenHeader) &&
			memcmp(data + (kVCSLegalScreenOffset - positionOnIso), kVCSLegalScreenHeader,
				sizeof(kVCSLegalScreenHeader)) == 0) {
		g_bootCurtainRaised = true;
		RaiseCurtain(CurtainKind::Loading);
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

// --- Widescreen ----------------------------------------------------------------------------------
//
// See the section at the bottom of VCSGame.h for what this is and why it is done in the renderer
// rather than in the game.

float g_widescreenSquash = 1.0f;
bool g_widescreenSquashesHud = false;
bool g_widescreenSquashedDraw = false;
bool g_widescreenEdgeFill = false;

void WidenProjection(float *matrix16) {
	if (!WidescreenActive()) {
		return;
	}
	if (GameSettings().widescreen == (int)WidescreenMode::Wider) {
		// HOR+: scale the x row down, which brings more of the world inside the clip box.
		matrix16[0] *= g_widescreenSquash;
		matrix16[4] *= g_widescreenSquash;
		matrix16[8] *= g_widescreenSquash;
		matrix16[12] *= g_widescreenSquash;
		return;
	}
	// VERT-: scale the y row UP instead, which pushes the top and bottom of the old view outside
	// the clip box. Same resulting aspect and the same stretched frame; the difference is that
	// nothing enters the view that the game was not already drawing, so nothing can pop.
	const float crop = 1.0f / g_widescreenSquash;
	matrix16[1] *= crop;
	matrix16[5] *= crop;
	matrix16[9] *= crop;
	matrix16[13] *= crop;
}

void ApplyHudSquash(float *x, float *width) {
	if (!g_widescreenSquashesHud || !WidescreenActive()) {
		return;
	}
	const float centre = *x + *width * 0.5f;
	*width *= g_widescreenSquash;
	*x = centre - *width * 0.5f;
}

void RefreshWidescreen() {
	g_widescreenSquash = 1.0f;
	g_widescreenSquashesHud = false;

	const int mode = GameSettings().widescreen;
	const float screenW = (float)g_display.pixel_xres;
	const float screenH = (float)g_display.pixel_yres;
	if (g_active && mode != (int)WidescreenMode::Off && screenW >= 1.0f && screenH >= 1.0f) {
		// 480x272 rather than the framebuffer the game happens to draw into: this is about the
		// shape of the PSP's SCREEN, which is what the frame is stretched from.
		const float squash = (480.0f / 272.0f) / (screenW / screenH);
		// A screen NARROWER than the PSP's would need the opposite correction - letterboxing
		// rather than pillarboxing - and PPSSPP already does the right thing there by keeping the
		// aspect. Refusing is better than squeezing the view into a shape nobody asked for.
		if (squash < 0.999f) {
			g_widescreenSquash = squash;
			// Both modes stretch the frame horizontally, so both want the HUD squashed to keep
			// its shape - the choice between them is about the WORLD, not about the panel.
			g_widescreenSquashesHud = true;
		}
	}

	// The frame has to be STRETCHED to fill for any of this to look right - the 3D is rendered
	// pre-squashed, so a pillarboxed presentation would show a narrow, squeezed picture.
	//
	// Set here, in the function that runs every tick, rather than only when the row is moved.
	// The row's own onChange fires before a game has booted, where g_active is false and there is
	// nothing to decide yet; without this the first boot after switching it on would widen the
	// view and never stretch the frame - which is worse than either half alone.
	//
	// Only while a VCS game is running, so this fork's binary pointed at some other game never
	// rewrites the player's own display setting. And only on a disagreement, which is the same
	// discipline ApplyPref follows and for the same reason.
	if (g_active) {
		DisplayLayoutConfig &display =
			g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
		const bool want = WidescreenActive();
		if (display.bDisplayStretch != want) {
			display.bDisplayStretch = want;
		}
	}
}

const std::string &GetDiscID() {
	return g_discID;
}

u64 GetTickCount() {
	return g_tickCount;
}

}  // namespace VCS
