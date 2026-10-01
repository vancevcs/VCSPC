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

#include <string>

#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
// For kVCSVolumeMax, which is the range the two volume settings below are expressed in.
#include "Core/VCS/VCSAddresses.h"

// Lifecycle and per-frame driver for the GTA: Vice City Stories input overhaul.
//
// This is the only entry point the rest of PPSSPP knows about. Three call sites, all of them
// no-ops for every other game:
//
//   __KernelInit()      -> VCS::Init()      (Core/HLE/sceKernel.cpp)
//   __KernelShutdown()  -> VCS::Shutdown()  (Core/HLE/sceKernel.cpp)
//   hleEnterVblank()    -> VCS::Tick()      (Core/HLE/sceDisplay.cpp)
//
// Everything is gated on the VCSInputOverhaul compat flag AND on the disc ID, so with the flag
// off - which is every game except ULUS10160 - Tick returns on its first line and no other VCS
// code is ever reached.

namespace VCS {

// Called once per game boot, from __KernelInit. Decides whether this is VCS and whether the
// compat flag is on; if either is false, everything else in this module stays dormant.
void Init();

// Patches the loaded module BEFORE it starts. The pool sizes are decided about a second into
// boot, long before the first vblank and long before the debugger is reachable, so a per-tick
// installer of the kind everything else here uses is far too late.
void PatchLoadedModule();

// Called from __KernelShutdown. Safe to call when Init decided not to activate.
void Shutdown();

// Called once per vblank from hleEnterVblank, on the emu thread. Decodes game state and applies
// the input mapping. Returns immediately when inactive.
// Where the game is in its boot sequence, which the front end needs because VCS has no menu of
// its own - it goes logos, credits, straight into the story. See "The boot sequence, measured"
// in CLAUDE.md for the measurements behind this.
enum class BootPhase {
	Intro,    // logos and credits are playing; FrameCounter reads 0
	AtMenu,   // the world has just started - the moment to put our menu up
	Playing,  // the menu has been dismissed and the player is in the game
};

BootPhase GetBootPhase();

// Called once the startup menu has been dismissed, so it is not shown again this run.
void NotifyMenuDismissed();

// Skip the credits. Presses the game's own skip button rather than trying to fast-forward it,
// which is why the seam still takes about four seconds to arrive afterwards.
void RequestIntroSkip();

void Tick();

// True when this is VCS and the compat flag is on. Used by the debugger window to decide
// whether to show live data or an explanation of why there isn't any.
bool IsActive();

// True when this build should present as the GAME rather than as an emulator: no PPSSPP logo,
// no menu bar, no ImGui debugger, no speed counter. Release builds only, and only for VCS -
// the Debug build keeps every tool, and no other game is ever affected, per the fork's
// zero-behaviour-change rule.
//
// Safe to call before a game boots, which is what the logo-screen decision needs.
bool PresentAsGame();

// Whether this BINARY is the game's rather than the workshop's: Release in this fork, whatever
// disc it has or has not been pointed at yet.
//
// A different question from PresentAsGame, and the difference only shows on the first run. That
// one asks "is this session the game", which needs a disc, and is right for the things it gates -
// the speed counter, the pause key, the logo screen. The MENU BAR is not one of them: it carries
// Debug, Emulation and Game settings, and a build somebody downloaded to play one game should not
// put those on the only screen they have to use, purely because it has not been told where their
// disc is yet. Measured on a packaged build: the first screen offered a Debug menu.
bool IsGameBuild();

// The folder the player put the game in, which is where a disc image and the memory stick are
// looked for. On Windows that is the exe's own folder, as it always was. On macOS the executable
// sits inside its .app bundle, and the folder the player can see is the one holding the bundle.
// On Android there is no exe folder the player can see, so NativeInit names one with
// SetGameFolder before anything asks - GTAVCS at the root of storage. On iOS it is the app's own
// Documents folder, which is what Finder's file sharing and the Files app show.
const Path &GameFolder();
void SetGameFolder(const Path &path);

// Presentation settings this module owns, as opposed to the ones that belong to a mechanic.
//
// showFps deliberately does NOT edit g_Config.iShowStatusFlags, and that is not a duplicate
// home for one setting: PPSSPP's own settings screen is unreachable in the game build, so the
// two govern different builds. This row is the counter in Release; iShowStatusFlags is still
// the counter in Debug, set from PPSSPP's Graphics page as it always was.
//
// Off by default. The point of the game build is that it looks like a game, and a player who
// wants the number now has a row to turn it on with.
//
// The two below are a different kind of row and worth telling apart from showFps: they do not
// belong to this fork at all. Subtitles and the HUD are settings the PSP game already has, on the
// Display page of its own front end - which this fork's menu does not offer a way into, so
// without these rows they became unreachable, the same loss the map and the save list took.
//
// They are mirrored into plain bools here rather than edited in place because the menu runs on
// the UI thread and PSP memory may only be touched from the emu thread. ApplyDisplayPrefs pushes
// them across once a tick; see the note there for why that is a write-when-different rather than
// a write-every-frame.
// The three positions of the Shadows row, in the order they appear.
inline constexpr int kVCSShadowsOff = 0;
inline constexpr int kVCSShadowsEntities = 1;
inline constexpr int kVCSShadowsProps = 2;
inline constexpr int kVCSShadowsEverything = 3;

// Wet surfaces: the sea, and rain on the roads. Same shape as the shadow row above and for the
// same reason - each step costs frames and adds something, so it is one decision rather than two
// switches that can contradict each other.
inline constexpr int kVCSWaterOff = 0;
inline constexpr int kVCSWaterSea = 1;
inline constexpr int kVCSWaterSeaAndRoads = 2;

// Where both rows start, and what "restore defaults" puts back - one constant each, so the struct
// below and the option row cannot disagree. They used to: the struct started shadows at HIGH and
// the row restored MEDIUM.
//
// A phone starts at the cheap end. Both passes capture and transform the scene on the CPU every
// frame, and a phone's CPU is what a PSP emulator runs short of first. Measure there, then raise.
#if PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(IOS)
inline constexpr int kVCSShadowsDefault = kVCSShadowsOff;
inline constexpr int kVCSWaterDefault = kVCSWaterSea;
// A phone's screen is always wider than the PSP's, so the fix has something to do from the first
// boot. Off on a desktop, where the window is usually 16:9 and the player chose its shape.
inline constexpr int kVCSWidescreenDefault = 1;  // WidescreenMode::Crop
#else
inline constexpr int kVCSShadowsDefault = kVCSShadowsProps;
inline constexpr int kVCSWaterDefault = kVCSWaterSeaAndRoads;
inline constexpr int kVCSWidescreenDefault = 0;
#endif

struct VCSGameSettings {
	bool showFps = false;

	// Dynamic sun shadows: off, people and vehicles, or everything. Ours rather than the game's
	// - see GPU/Common/VCSShadow.cpp - and the only setting on this struct that reaches the
	// renderer rather than PSP memory, which is why it is pushed by an onChange in the option
	// table instead of by ApplyGamePrefs.
	//
	// One setting rather than a switch and a separate mode, because "off" and "only the
	// things that move" are points on one scale: each step costs frames and adds shadows,
	// and a player choosing between them is making a single decision. It was a bool plus
	// a choice, which meant a mode row that greyed out and a saved mode that went on
	// existing while the feature was off.
	int shadows = kVCSShadowsDefault;

	// Better water, and rain on the roads. Ours rather than the game's - see
	// GPU/Common/VCSWater.cpp - so like `shadows` it reaches the renderer through an onChange in
	// the option table rather than through ApplyGamePrefs.
	//
	// The road half of it does nothing at all in dry weather, which is most of the time. That is
	// not a reason to split it into its own row: a player who turns water up wants the weather to
	// look right when it arrives, and a row that appears to do nothing when you move it is worse
	// than one whose effect waits for the rain.
	int water = kVCSWaterDefault;

	// The radar in the top-left corner instead of the bottom-left, where the phone ports of the
	// trilogy have always put it - and where, on a phone, the left thumb is not sitting on it.
	//
	// A code patch rather than a write, because the rect is four compiled-in constants; see
	// kVCSRadarTopNarrow. It is applied once before the game runs, so changing it takes effect on
	// the next boot, exactly like World memory above.
#if PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(IOS)
	bool radarTopLeft = true;
#else
	bool radarTopLeft = false;
#endif

	// How many times the retail 4.75MB of resident world to keep. This is the whole of how
	// far VCS can see - four distance mechanisms were patched and measured and none of them
	// decides anything, so what is drawn is what fits. Needs the larger partition, which
	// InitMemorySizeForGame hands this disc. 1 leaves the game's own code untouched.
	float worldMemory = 1.0f;

	bool subtitles = true;

	// The health, armour, money, weapon and clock panel. NOT the radar, which the game keeps on a
	// separate RADAR MODE setting - turning this off leaves the radar drawn, exactly as the
	// retail row does.
	bool hud = true;

	// The game's own two volumes, 0..kVCSVolumeMax. `radioVolume` is what its own Audio page
	// calls MUSIC VOLUME - in this game the music IS the radio, and the row is named after what
	// a player hears rather than after the mixer channel.
	//
	// Full by default, which is this fork's choice rather than the game's: nothing here can ask
	// the game what it shipped with, and a menu that owns a setting has to have an answer for
	// "restore defaults". The cost is one-off and worth stating - the first boot after these rows
	// appeared overrides whatever the player had set in the game's own Audio page, and every boot
	// after that uses what they set here.
	int sfxVolume = kVCSVolumeMax;
	int radioVolume = kVCSVolumeMax;

	// A wider view on a screen wider than the PSP's - see the Widescreen section at the bottom of
	// this file for what the three positions do. On a phone, where the screen always IS wider,
	// the default is the one that keeps the HUD's shape.
	int widescreen = kVCSWidescreenDefault;
};

VCSGameSettings &GameSettings();

// Substitute a file on the UMD as it is read, by BYTE RANGE rather than by name.
//
// Called from ISOFileSystem::ReadFile with the absolute position on the disc that was just read
// into `data`. Overwrites any part of it that falls inside a patched file. Returns immediately
// for every other game, and for VCS whenever the read is nowhere near one.
//
// **By range because there is no name to match.** VCS opens `disc0:/sce_lbn0x0_size0x65170000` -
// the whole UMD as one stream - and seeks to its own files by sector, so nothing on the disc is
// ever requested by filename. A redirect keyed on the path was built first and never fired once;
// this is the same substitution one layer down, where the game's own addressing lives.
//
// What it buys over patching the ISO: the retail disc is untouched and stays the boot path, the
// replacement is a file on the memory stick that can be deleted to undo it, and there is no
// 1.6 GB copy. What it does NOT change is the size limit - the game seeks by sector numbers baked
// into its own code, so a replacement has to fit the original's span and is padded to it.
void PatchDiscRead(u64 positionOnIso, u8 *data, size_t bytes);

// The disc ID we booted with, for display. Empty when no game is running.
const std::string &GetDiscID();

// Number of ticks since Init, so the debugger can show that the hook is actually firing. This
// is genuinely useful while bringing the module up - a frozen counter means the vblank hook
// isn't wired.
u64 GetTickCount();

// --- Widescreen ----------------------------------------------------------------------------------
//
// THE PROBLEM. The PSP's screen is 480x272, which is 16:9.06. A modern phone is 19.5:9 or wider.
// PPSSPP can do one of two things with that and neither is right: keep the aspect and leave black
// bars down both sides, or stretch the frame to fill and make everything 20% wide.
//
// THE FIX, which is what every GTA widescreen mod does. The frame is stretched to fill, and the
// 3D is rendered PRE-SQUASHED by the same factor so that it comes out correct - which means the
// projection now covers a wider field of view, so about 20% more of the world is visible. Nothing
// is distorted and nothing is cropped; you simply see more, which is what the extra glass is for.
//
// WHY IT IS DONE HERE RATHER THAN IN THE GAME. A real widescreen mod patches the game's own FOV,
// and that would be the better answer for everything downstream - the game would know. It needs
// an address hunt this fork has not done, and the renderer can reach the same place without one:
// PPSSPP's through-mode (2D) draws never touch `u_proj`, so the 3D projection can be widened on
// its own with the HUD untouched. The two places that ALSO read `gstate.projMatrix` - the shadow
// and water passes - are this fork's own, and apply the same widening to stay aligned.
//
// TWO WAYS TO FILL THE SCREEN, and the row offers both because they trade against each other.
//
// HOR+ keeps the vertical field of view and widens the horizontal one, so about 20% more of the
// world is visible. That is the generous reading of "widescreen" and it has a cost this fork can
// see and cannot fix from here: the game culls and streams against ITS OWN idea of the view, so
// the strip of world either side that it never expected to show is a strip where objects appear
// and vanish as you turn. Reported from play, in those words.
//
// VERT- keeps the horizontal field of view and narrows the vertical one instead - the frame is
// filled by cropping the top and bottom rather than by revealing anything new. Nothing enters the
// frustum that was not already in it, so there is nothing to pop, whatever the culling mechanism
// turns out to be. That argument holds without knowing what the culling mechanism is, and it is
// still why this is the default.
//
// THE MECHANISM IS NOW KNOWN AND WIDENING IT DID NOT WORK. VCS culls to the RenderWare view
// window, that can be widened while the projection stays put, and a build that did it dropped
// objects the player was looking straight at. Reverted. So the strip either side that WIDER
// reveals is still a strip the game never expected to show, and CROP is still the honest default.
// See "VCS culls to the camera, and widening it is worse than the pop-in" in CLAUDE.md.
//
// The HUD is squashed in both, because the frame is stretched horizontally in both.

enum class WidescreenMode {
	Off = 0,
	Crop = 1,   // VERT-: fill the screen by cropping top and bottom. Nothing new, nothing pops.
	Wider = 2,  // HOR+: fill it by showing ~20% more world, and accept the pop-in at the edges.
};

// How far the frame is squashed horizontally before it is stretched back out: the PSP's aspect
// over the device's, so 0.836 on a 19.5:9 phone and 1.0 whenever this is off.
//
// A plain variable rather than a call into Core because it is read once per DRAW CALL, which is
// the same reason VCSShadow::g_active is one. Never above 1: a screen narrower than the PSP's
// would need the opposite correction, and pillarboxing it is already right.
extern float g_widescreenSquash;
// Whether the 2D half is squashed with it - `Full` rather than `ThreeD`. Read per draw too.
extern bool g_widescreenSquashesHud;

// Whether the draw being set up right now had its 2D vertices squashed.
//
// Set by the software transform, which is the only place that knows - the decision is per draw,
// because anything covering the screen is exempt - and read moments later when the scissor is
// converted, which happens after it in the same Flush.
//
// IT EXISTS BECAUSE THE FIRST BUILD MOVED THE VERTICES AND NOT THE CLIP. The radar's map is
// drawn clipped to its own circle; squashing the content while the scissor stayed put slid the
// map sideways inside the ring, which is exactly how it was reported - "the minimap content is
// displaced by one half".
extern bool g_widescreenSquashedDraw;

inline bool WidescreenActive() { return g_widescreenSquash < 0.999f; }

// Correct a PSP projection matrix for the device's aspect, whichever way the row asks for. Both
// the shader upload and the two passes that capture the same matrix go through this, so none of
// them can end up describing a different view from the others.
//
// gstate's 16 floats become a column-major mat4 and the shader does `u_proj * viewPos`, so the x
// row of the product is elements 0, 4, 8 and 12 and the y row is 1, 5, 9 and 13 - not the first
// four and the second four, which is the mistake this comment exists to stop somebody making.
void WidenProjection(float *matrix16);

// Move a horizontal screen span into the band the HUD is squashed into. A no-op unless the HUD
// half is on. Everything this fork draws ON the game's HUD - the route line, the map cursor, the
// radar and weapon tap zones - has to ask, or it lands beside what it is meant to sit on.
void ApplyHudSquash(float *x, float *width);

// Recompute from the setting and the current display. Called when either might have changed.
void RefreshWidescreen();

}  // namespace VCS
