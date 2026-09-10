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
#include <cstdio>

#include "Common/Data/Format/IniFile.h"
#include "Common/File/Path.h"
#include "Common/System/Display.h"
#include "Common/System/Request.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSRadar.h"
#include "Core/VCS/VCSSettings.h"
#include "Core/VCS/VCSVault.h"
#include "GPU/Common/VCSShadow.h"

namespace VCS {

// The Shadows row is one number; the shadow pass wants two answers. Deriving both in one
// place is what stops "off" and "people and vehicles" ever being live at the same time.
static void ApplyShadowSetting() {
	const int mode = GameSettings().shadows;
	VCSShadow::GetSettings().entityCastersOnly = mode == kVCSShadowsEntities;
	VCSShadow::SetEnabled(mode != kVCSShadowsOff);
}

// Labels for the Choice options. These mirror PPSSPP's own settings screen so the two never
// disagree about what a given index means.
static const char *const kResolutionLabels[] = {
	"Auto", "1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x",
};
static const char *const kAnisoLabels[] = { "Off", "2x", "4x", "8x", "16x" };

// Not "all" and "entities". A player knows what a person and a car are, and has never heard of an
// entity - and "everything" says the thing the other choice is measured against.
static const char *const kShadowLabels[] = { "Off", "People and vehicles", "Everything" };

// "3x (1440x816)". The multiplier is what the setting means; the pixel count is what it does, and
// a resolution is the one setting a player already has a number for. Computed rather than written
// into the labels above because Auto's half of it is the window's, and the window can be resized
// while this page is open.
static std::string ResolutionText(const Option &opt) {
	const int index = std::clamp(*opt.intValue, 0, (int)ARRAY_SIZE(kResolutionLabels) - 1);

	// Auto is NOT "render at the window size". It rounds the window UP to a whole multiple of the
	// PSP's own 480 and renders at that, which is why it can only ever read as one of the same
	// sizes the numbered rows offer. Same arithmetic as PresentationCommon's
	// CalculateRenderResolution, and it has to stay the same arithmetic or the parenthesis is a
	// lie. Its two other cases - a portrait internal rotation, and an upscaling post shader - are
	// reached through settings this menu does not offer.
	const int scale = index != 0 ? index : std::max((g_display.pixel_xres + 479) / 480, 1);

	char buffer[48];
	snprintf(buffer, sizeof(buffer), "%s (%dx%d)", kResolutionLabels[index], 480 * scale, 272 * scale);
	return std::string(buffer);
}

static Path SettingsPath() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "vcs.ini";
}

const std::vector<Option> &Options() {
	// Function-local static, not a file-scope table, because the rows point into
	// CameraSettings() and FireHookSettings() - themselves function-local statics. A file-scope
	// table would be racing their construction.
	static const std::vector<Option> options = [] {
		VCSCameraSettings &cam = CameraSettings();
		VCSPadSettings &pad = PadSettings();

		std::vector<Option> opts;

		auto addBool = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, bool *value, bool external = false,
				bool defaultValue = false, void (*onChange)() = nullptr,
				std::string (*valueText)(const Option &) = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Bool;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.boolValue = value;
			opt.external = external;
			opt.onChange = onChange;
			// A flag that does not read as ON and OFF. Texture quality is the case it was added
			// for: it is one bool, and what it means to a player is HIGH and LOW.
			opt.valueText = valueText;
			// For a setting we own, the compiled-in value IS the default and capturing it here is
			// right. For one PPSSPP owns, the live value at table-build time is whatever the user
			// last saved, so capturing it would make "restore defaults" mean "restore what I had
			// at startup" - the same trap addInt and addChoice take an explicit default to avoid.
			opt.defaultBool = external ? defaultValue : *value;
			opts.push_back(opt);
		};

		// Ties the row just added to another setting, so it greys out when that one is off.
		// Applied afterwards rather than as a parameter on all four adders, since one row in the
		// table wants it.
		auto enabledBy = [&opts](bool *flag) {
			opts.back().enabledBy = flag;
		};

		auto addFloat = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, float *value, float minValue, float maxValue,
				const char *format = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Float;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.floatValue = value;
			opt.minValue = minValue;
			opt.maxValue = maxValue;
			opt.format = format;
			opt.defaultFloat = *value;
			opts.push_back(opt);
		};

		auto addInt = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, int *value, int minValue, int maxValue, int step,
				int defaultValue, bool external = false, void (*onChange)() = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Int;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.intValue = value;
			opt.minInt = minValue;
			opt.maxInt = maxValue;
			opt.stepInt = step;
			opt.external = external;
			opt.onChange = onChange;
			// Explicit for the same reason the Choice adder takes one - see below.
			opt.defaultInt = defaultValue;
			opts.push_back(opt);
		};

		auto addChoice = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, int *value, const char *const *choices, int numChoices,
				int defaultIndex, bool external = false, void (*onChange)() = nullptr,
				std::string (*valueText)(const Option &) = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Choice;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.intValue = value;
			opt.choices = choices;
			opt.numChoices = numChoices;
			opt.external = external;
			opt.onChange = onChange;
			opt.valueText = valueText;
			// Explicit rather than captured from the live value: for an external setting the
			// live value at table-build time is whatever the user last saved, so capturing it
			// would make "restore defaults" mean "restore what I had at startup".
			opt.defaultInt = defaultIndex;
			opts.push_back(opt);
		};

		// WHAT IS NOT ON THESE TWO PAGES, AND WHY
		//
		// Mouse and Aiming carried 23 rows between them, and most were not preferences. Two kinds
		// were cut, and the test for each is the same: would a player have an OPINION about it?
		//
		//   Calibration to match GTA on PC.  Vertical look speed is 1.9x because that is what III
		//   and Vice City use; mouse acceleration and its cap are the aim model's own tuning,
		//   measured rather than chosen. Numbers like these are how the fork hits the feel it is
		//   aiming for, and a player moving them is drifting away from it, not expressing a taste.
		//
		//   Corrections that should simply be on.  Scaling sensitivity with zoom, matching the
		//   drive-by axes, camera aim for scoped weapons, the mouse-native response curve - each
		//   undoes something the game does for a thumbstick. Off is not an alternative style, it
		//   is the bug back.
		//
		// What survives is what a player really does have a view on: how fast it moves, which way
		// up it is, and the two switches that change what the mouse DOES - free aim, and whether
		// you can walk while aiming.
		//
		// Every cut row keeps its compiled default, and each of those defaults is already the
		// PC-like value - checked one at a time, not assumed. The settings still exist, vcs.ini
		// still holds them, and the debugger's Camera tab still binds every one; they are off the
		// PLAYER's page, not out of the fork.
		// --- Mouse ---

		addBool(OptionPage::Mouse, "MouseEnabled", "Mouse control",
			"Mouse look on foot and in vehicles. Needs Use Mouse Control on, in Settings - Controls.",
			&cam.enabled);
		addFloat(OptionPage::Mouse, "LookSensitivity", "Look sensitivity",
			"How far the camera turns per unit of mouse movement.",
			&cam.sensitivity, 0.0005f, 0.02f);
		addBool(OptionPage::Mouse, "InvertLookY", "Invert look vertically",
			"", &cam.invertY);

		// The master switch first, then the two halves of what happens after you stop moving the
		// mouse. Ticks are the emulator's ~60Hz, which is the unit the camera code counts in; the
		// help text gives seconds because that is the unit the player is actually judging.
		addBool(OptionPage::Mouse, "ReturnLook", "Return the view to the game",
			"Off keeps the view exactly where you leave it and never gives the camera back - "
			"no drift back behind you on foot, and no swing back behind the car when driving.",
			&cam.returnLook);
		// The five rows that used to sit here - hold time, return time, return-behind-you, its trim,
		// and keep-until-moving - are gone from the menu on purpose. They are the tuning FOR the
		// switch above, and a page that offers five ways to shape a behaviour the player has most
		// likely turned off is five rows of noise around the one row that matters.
		//
		// The settings themselves are untouched: VCSCameraSettings still owns them, vcs.ini still
		// persists whatever is in them, and the debugger's Camera tab still binds every one. This
		// removes them from the PLAYER's page, not from the fork.

		// --- Controller ---
		//
		// Four rows, and the first is the important one: the scheme this fork lays out for a pad
		// is an opinion, and the handheld's own layout is the other one. Off hands the pad back
		// to PPSSPP's mapper and every button goes back to meaning what it means on a PSP.
		//
		// The sensitivity is in the stick's own units rather than the mouse's, and the two rows
		// are separate on purpose: a mouse count and a stick deflection are different kinds of
		// number, and one slider driving both would be a slider that is wrong for whichever
		// device the player is not holding.
		addBool(OptionPage::Controller, "PadEnabled", "Controller scheme",
			"Off gives a pad the PSP's own layout, through PPSSPP's control mapper.",
			&pad.enabled);
		addFloat(OptionPage::Controller, "PadLookSpeed", "Look sensitivity",
			"How fast the right stick turns the camera.",
			&pad.lookSpeed, 4.0f, 40.0f);
		addBool(OptionPage::Controller, "PadInvertLookY", "Invert look vertically",
			"Push the right stick forward to look down.",
			&pad.invertLookY);
		addFloat(OptionPage::Controller, "PadDeadzone", "Stick deadzone",
			"How far a stick must move before it counts. Raise it if the camera drifts at rest.",
			&pad.deadzone, 0.02f, 0.45f);

		// --- Aiming ---

		addFloat(OptionPage::Aiming, "AimSensitivity", "Aim sensitivity",
			"Aiming is a separate channel from looking, and runs a good deal slower.",
			&cam.aimSensitivity, 0.00005f, 0.008f);
		addBool(OptionPage::Aiming, "InvertAimY", "Invert aim vertically",
			"", &cam.aimInvertY);
		addBool(OptionPage::Aiming, "FreeAim", "Free aim",
			"Shoot where the mouse points, instead of the game locking on to a target for you.",
			&cam.autoFreeAim);
		addBool(OptionPage::Aiming, "MoveWhileAiming", "Move while aiming",
			"Walk with WASD with a weapon raised. Patches game code while it is on.",
			&cam.moveInFreeAim);
		addFloat(OptionPage::Aiming, "ScopedSensitivity", "Sniper and RPG sensitivity",
			"Only applies to the two scoped weapons.",
			&cam.aimScopedSensitivity, 0.005f, 0.2f);

		// --- Audio ---
		//
		// Three rows now. The master volume is the emulator's, applied after everything; the two
		// below it are the GAME's own mixer, off the Audio page of its front end, and they are
		// here for the same reason subtitles and the HUD are on the Gameplay page - the fork's
		// menu offers no way into that page, so without a row they were unreachable.
		//
		// This page carried a note for a long while saying SFX and radio "need addresses we do
		// not have, and a row wired to a variable nothing reads would move and change nothing".
		// That was the right call to make at the time and the second half of it turned out to be
		// exactly the trap: the first pair of addresses found for these DID move and change
		// nothing, because they are the preferences and not the levels. See VCSAddresses.h.
		//
		// g_Config.iGameVolume runs 0..VOLUMEHI_FULL, and the step is what keeps a row of ten
		// blocks honest: one press moves one block. It was a Choice into an eleven-label list
		// first, which read correctly and was wrong - the index and the value are not the same
		// number, so the first press dropped the volume from 100 to 10.
		// The master switch, above the volume it governs - because with it off the volume row
		// below does nothing at all, and there was no way to discover that from inside this menu.
		// Somebody muted the emulator in PPSSPP's own settings, came back a fortnight later, and
		// found an in-game volume slider that moved and changed nothing.
		//
		// The default is written out rather than asked for, which breaks this file's own rule
		// about external settings, and the reason is worth stating: Config has GetDefaultValueInt
		// and no bool equivalent, and adding one means editing Core/Config.cpp - the file this
		// fork keeps its hands off so that rebases stay clean. Upstream's default is `true`
		// (ConfigSetting("Enable", ..., true) in Core/Config.cpp), and a sound-enable flag
		// defaulting to anything else is not a change anyone is going to make.
		addBool(OptionPage::Audio, nullptr, "Sound",
			"Master switch. Off silences everything, whatever the volume below says.",
			&g_Config.bEnableSound, true, true);

		addInt(OptionPage::Audio, nullptr, "Master volume",
			"Overall volume, applied by the emulator.",
			&g_Config.iGameVolume, VOLUME_OFF, VOLUMEHI_FULL, VOLUMEHI_FULL / 10,
			Config::GetDefaultValueInt(&g_Config.iGameVolume), true);
		enabledBy(&g_Config.bEnableSound);

		// The game's own two, and the step is what makes them read as sliders rather than as
		// numbers: the strip draws ten blocks, so one press has to be a tenth of the range.
		// 13 rather than 12 because 12 leaves the tenth press short of the top - ten steps of 13
		// overshoot 127 and the write path clamps, which is the same thing the game's own slider
		// does when you press right at 112.
		//
		// Not the game's own ladder of 16, deliberately. That one is eight notches against a
		// ten-block strip, so a press would sometimes move a block and sometimes two. Both
		// ladders produce values the game accepts; this one matches the control the player is
		// actually looking at.
		//
		// Greyed under the same master switch as the row above, and for the same reason: with the
		// emulator muted these move and nothing can be heard to change.
		static constexpr int kVolumeStep = 13;

		addInt(OptionPage::Audio, "SfxVolume", "SFX volume",
			"Engines, weapons, footsteps and the people talking. Not the radio.",
			&GameSettings().sfxVolume, 0, kVCSVolumeMax, kVolumeStep, kVCSVolumeMax);
		enabledBy(&g_Config.bEnableSound);

		addInt(OptionPage::Audio, "RadioVolume", "Radio volume",
			"The car radio, and the music the game plays over it. Its own menu calls this MUSIC.",
			&GameSettings().radioVolume, 0, kVCSVolumeMax, kVolumeStep, kVCSVolumeMax);
		enabledBy(&g_Config.bEnableSound);

		// --- Graphics ---

		addChoice(OptionPage::Graphics, nullptr, "Resolution",
			"Internal rendering resolution. Higher is sharper and costs more.",
			&g_Config.iInternalResolution, kResolutionLabels, ARRAY_SIZE(kResolutionLabels),
			Config::GetDefaultValueInt(&g_Config.iInternalResolution), true, []() {
				System_PostUIMessage(UIMessage::GPU_RENDER_RESIZED);
			}, &ResolutionText);
		addChoice(OptionPage::Graphics, nullptr, "Anisotropic filtering",
			"Sharpens textures viewed at a shallow angle, like road surfaces ahead of you.",
			&g_Config.iAnisotropyLevel, kAnisoLabels, ARRAY_SIZE(kAnisoLabels),
			Config::GetDefaultValueInt(&g_Config.iAnisotropyLevel), true);

		// The HD pack, under the name a player would look for it by. It is PPSSPP's texture
		// replacement underneath - the same flag its own developer tools expose - and this is the
		// one place in this port where that switch is a GAME setting rather than a developer one:
		// with a pack installed it is the difference between the PSP's textures and this port's,
		// which is most of what "PC version" means from three feet away.
		//
		// HIGH and LOW rather than ON and OFF, because nobody chooses "texture replacement off";
		// they choose the lower setting when the higher one costs too much.
		//
		// The change takes effect on the next frame rather than needing a restart:
		// GPU_CONFIG_CHANGED makes the GPU clear its texture cache and re-ask the replacer what
		// it has, which is exactly what happens when the same flag is flipped in PPSSPP's own
		// settings.
		addBool(OptionPage::Graphics, nullptr, "Texture quality",
			"HIGH uses the installed HD texture pack. LOW draws the PSP's own textures.",
			&g_Config.bReplaceTextures, true, true, []() {
				System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
			}, [](const Option &opt) {
				return std::string(*opt.boolValue ? "HIGH" : "LOW");
			});

		// FULLSCREEN, and it is here because this build took away every other way to reach it.
		//
		// PPSSPP offers it on its View menu, and the game build hides the menu bar - correctly, it
		// carried a Debug menu - so a packaged copy had NO route to fullscreen at all. Not even a
		// key: VIRTKEY_TOGGLE_FULLSCREEN has no default binding in Core/KeyMapDefaults.cpp, which
		// is fine when a menu offers it and a hole when nothing does.
		//
		// Reported from a packaged build, and it is the general shape of the mistake worth keeping:
		// removing a surface removes everything that was only reachable through it. Take an
		// emulator's chrome away and every setting behind it has to be given a home, or it is gone.
		addBool(OptionPage::Graphics, nullptr, "Fullscreen",
			"Fill the screen. The window's size and position come back when this is turned off.",
			&g_Config.bFullScreen, true, true, []() {
				// The same call PPSSPP's own checkbox makes, rather than a message of our own.
				System_ApplyFullscreenState();
			});

		// Ours rather than g_Config.iShowStatusFlags, which is a bitfield the option table has no
		// type for - and which the Debug build still uses. See VCSGameSettings for why the two are
		// not one setting with two homes.
		// Dynamic sun shadows. The sun comes from the directional light the game already hands
		// the hardware, so this follows the time of day by itself and there is nothing to set
		// but whether it runs at all - everything a player would otherwise tune (cascade size,
		// bias, tint) is in the debugger's Shadows tab, which is where a knob needing a
		// paragraph of measurement belongs.
		// One row, three positions, because it is one decision. What the pass costs is mostly
		// the geometry it draws a second time, and the city is nearly all of that geometry - so
		// the middle position is the frame-rate setting as well as a real preference. Shadows
		// under the player, the traffic and the crowd are the ones a player watches, and the
		// ones the PSP game itself fakes with a blob under every object.
		addChoice(OptionPage::Graphics, "Shadows", "Shadows",
			"Real shadows from the sun, which the PSP game has none of. Each step costs frames; "
			"the middle one keeps the shadows you actually watch.",
			&GameSettings().shadows, kShadowLabels,
			ARRAY_SIZE(kShadowLabels), kVCSShadowsEntities, false, []() {
				ApplyShadowSetting();
			});

		// The one setting here that changes what the GAME can hold rather than how the
		// emulator draws it. It only takes effect on the next boot, because the pools are
		// built about a second into one and never again - the help line says so, because a
		// setting that appears to do nothing is worse than one that is not offered.
		addFloat(OptionPage::Graphics, "WorldMemory", "World memory",
			"How much of the city stays loaded at once. Higher means you see further and "
			"more of it casts shadows. Takes effect after a restart.",
			&GameSettings().worldMemory, 1.0f, kVCSWorldMemoryMax, "%.1fx");

		addBool(OptionPage::Graphics, "ShowFps", "Show FPS",
			"Draw the frame rate in the corner of the screen.",
			&GameSettings().showFps);

		// --- Gameplay ---
		//
		// What every row here has in common is that it is about the GAME rather than about the
		// device you drive it with - which is what the other four pages are, each named after one.
		//
		// Two kinds sit here, and the order is deliberate. The first three turn off something this
		// port ADDED and hand that behaviour back to the game as Rockstar shipped it. The last two
		// are the opposite: they are the game's OWN settings, off the Display page of its front
		// end, which this fork's menu deliberately offers no way into - so without a row here they
		// were simply unreachable, the same loss the map and the save list took when Start was
		// claimed. Reaching for the game's existing setting rather than inventing a mechanism is
		// the same rule the vault and the cheat menu follow.
		addBool(OptionPage::Gameplay, "Vaulting", "Vaulting",
			"Pull up onto ledges and hop over fences with the jump button. The PSP game cannot "
			"climb at all.",
			&VaultSettings().enabled);
		addBool(OptionPage::Gameplay, "AutoSave", "Auto-save",
			"Save after every story mission, over the auto-save slot. Starting the game always "
			"loads the most recent save, whichever it is.",
			&FrontEndSettings().autoSaveOnMissionPassed);
		addBool(OptionPage::Gameplay, "GPS", "GPS route",
			"Draw a route to your marker on the radar, along the roads the game itself uses.",
			&RadarSettings().drawRoute);

		// The game's own two, bound to the mirrors VCSGame keeps rather than to PSP memory - the
		// menu edits these on the UI thread, and PSP memory is the emu thread's alone.
		//
		// "HUD" rather than "Remove HUD", so that OFF means the same thing here as it does in
		// every other row on the page. The help line has to say what it leaves behind, because
		// the radar staying up looks like the setting half worked; it is what the retail row
		// does, and the radar has a setting of its own that this fork does not offer.
		addBool(OptionPage::Gameplay, "Subtitles", "Subtitles",
			"Dialogue as text on screen, during cutscenes and phone calls.",
			&GameSettings().subtitles);
		addBool(OptionPage::Gameplay, "Hud", "HUD",
			"Health, armour, money, the weapon and the clock. The radar is separate and stays.",
			&GameSettings().hud);

		return opts;
	}();

	return options;
}

static std::string g_gamePath;

std::string GamePath() {
	return g_gamePath;
}

void SetGamePath(std::string_view path) {
	if (g_gamePath == path) {
		return;
	}
	g_gamePath = std::string(path);
	SaveSettings();
}

bool WantsExtraWorldMemory() {
	// Asked before anything else here exists. The partition has to be sized while the ISO is
	// being loaded, which is long before Init builds the option table, so this reads the one
	// key straight out of the file rather than going through it.
	//
	// It is asked at all because the answer costs savestate compatibility: a state written
	// under one partition size cannot be loaded under another, and every state this fork's
	// users already have was written on the retail 32MB. So the larger partition is taken
	// only when somebody has actually asked for more world.
	IniFile ini;
	if (!ini.Load(SettingsPath())) {
		return false;
	}
	const Section *section = ini.GetSection("Settings");
	if (!section) {
		return false;
	}
	float value = 1.0f;
	if (!section->Get("WorldMemory", &value)) {
		return false;
	}
	return value > 1.0f;
}

void LoadSettings() {
	// Touch the table before the file, so the defaults it captures are the compiled-in ones.
	const std::vector<Option> &options = Options();

	IniFile ini;
	// Ignore the return value - the file not existing yet is the normal first run, and every
	// option below then falls back to its own default.
	ini.Load(SettingsPath());
	const Section *section = ini.GetOrCreateSection("Settings");

	if (!section->Get("GamePath", &g_gamePath)) {
		g_gamePath.clear();
	}

	for (const Option &opt : options) {
		if (opt.external) {
			continue;  // g_Config owns it and has already loaded it.
		}
		switch (opt.type) {
		case OptionType::Choice:
			// Clamped to the label list rather than to a min/max pair: an index past the end
			// would read off it, and a hand-edited file is exactly how that happens.
			if (!section->Get(opt.iniKey, opt.intValue)) {
				*opt.intValue = opt.defaultInt;
			} else {
				*opt.intValue = std::clamp(*opt.intValue, 0, opt.numChoices - 1);
			}
			break;
		case OptionType::Bool:
			if (!section->Get(opt.iniKey, opt.boolValue)) {
				*opt.boolValue = opt.defaultBool;
			}
			break;
		case OptionType::Int:
			if (!section->Get(opt.iniKey, opt.intValue)) {
				*opt.intValue = opt.defaultInt;
			} else {
				*opt.intValue = std::clamp(*opt.intValue, opt.minInt, opt.maxInt);
			}
			break;
		case OptionType::Float:
			if (!section->Get(opt.iniKey, opt.floatValue)) {
				*opt.floatValue = opt.defaultFloat;
			} else {
				// A hand-edited file is the expected way to get an out-of-range value here, and
				// several of these feed the aim solver, where a wild number is not a wild
				// setting but a division that stops making sense.
				*opt.floatValue = std::clamp(*opt.floatValue, opt.minValue, opt.maxValue);
			}
			break;
		}
	}

	// The renderer keeps its own copy of this one, because it is read once per draw call and a
	// switched-off feature has to cost a bool test rather than a call into Core. Pushed here as
	// well as from the row's onChange, so the value in the file is live from the first frame
	// rather than from the first time somebody opens the page.
	ApplyShadowSetting();
}

void SaveSettings() {
	IniFile ini;
	ini.Load(SettingsPath());  // Round-trips anything we don't own, e.g. a hand-added section.
	Section *section = ini.GetOrCreateSection("Settings");

	section->Set("GamePath", g_gamePath);

	for (const Option &opt : Options()) {
		if (opt.external) {
			continue;  // g_Config saves it to its own file.
		}
		switch (opt.type) {
		case OptionType::Choice:
			section->Set(opt.iniKey, *opt.intValue);
			break;
		case OptionType::Bool:
			section->Set(opt.iniKey, *opt.boolValue);
			break;
		case OptionType::Int:
			section->Set(opt.iniKey, *opt.intValue);
			break;
		case OptionType::Float:
			section->Set(opt.iniKey, *opt.floatValue);
			break;
		}
	}

	ini.Save(SettingsPath());
}

void ResetPage(OptionPage page) {
	for (const Option &opt : Options()) {
		if (opt.page != page) {
			continue;
		}
		switch (opt.type) {
		case OptionType::Bool:
			*opt.boolValue = opt.defaultBool;
			break;
		case OptionType::Float:
			*opt.floatValue = opt.defaultFloat;
			break;
		case OptionType::Int:
		case OptionType::Choice:
			*opt.intValue = opt.defaultInt;
			break;
		}
		// Fired for every type, not just the numeric ones. Restoring a page's defaults moves
		// the same values the controls move, so it owes the same notifications -- a reset that
		// turned texture quality back on without telling the GPU would leave the page reading
		// HIGH over the PSP's own textures.
		if (opt.onChange) {
			opt.onChange();
		}
	}
}

float GetNormalized(const Option &opt) {
	if (opt.type == OptionType::Int) {
		if (opt.maxInt <= opt.minInt) {
			return 0.0f;
		}
		const float t = (float)(*opt.intValue - opt.minInt) / (float)(opt.maxInt - opt.minInt);
		return std::clamp(t, 0.0f, 1.0f);
	}
	if (opt.type != OptionType::Float || opt.maxValue <= opt.minValue) {
		return 0.0f;
	}
	const float t = (*opt.floatValue - opt.minValue) / (opt.maxValue - opt.minValue);
	return std::clamp(t, 0.0f, 1.0f);
}

void SetNormalized(const Option &opt, float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	if (opt.type == OptionType::Int) {
		// Snapped to the step, so that dragging the strip can only land on values the arrow keys
		// could also reach - an off-step value would show a half-lit block and then jump when
		// next nudged.
		const int span = opt.maxInt - opt.minInt;
		const int step = opt.stepInt > 0 ? opt.stepInt : 1;
		const int offset = ((int)(t * (float)span + 0.5f) + step / 2) / step * step;
		SetInt(opt, opt.minInt + offset);
		return;
	}
	if (opt.type != OptionType::Float) {
		return;
	}
	*opt.floatValue = opt.minValue + t * (opt.maxValue - opt.minValue);
}

void SetInt(const Option &opt, int value) {
	if (opt.type != OptionType::Int && opt.type != OptionType::Choice) {
		return;
	}
	const int low = opt.type == OptionType::Int ? opt.minInt : 0;
	const int high = opt.type == OptionType::Int ? opt.maxInt : opt.numChoices - 1;
	value = std::clamp(value, low, high);
	if (value == *opt.intValue) {
		return;
	}
	*opt.intValue = value;
	// Only on a real change: dragging the strip calls this every frame, and resizing the
	// framebuffer once per frame for a value that did not move is not free.
	if (opt.onChange) {
		opt.onChange();
	}
}

void SetBool(const Option &opt, bool value) {
	if (opt.type != OptionType::Bool) {
		return;
	}
	if (value == *opt.boolValue) {
		return;
	}
	*opt.boolValue = value;
	// Only on a real change, for the same reason SetInt is careful about it: onChange is where
	// the expensive consequences live, and a click that lands on the value already showing is
	// not a change.
	if (opt.onChange) {
		opt.onChange();
	}
}

std::string ValueText(const Option &opt) {
	if (opt.valueText) {
		return opt.valueText(opt);
	}
	switch (opt.type) {
	case OptionType::Bool:
		return *opt.boolValue ? "ON" : "OFF";
	case OptionType::Choice:
	{
		const int index = std::clamp(*opt.intValue, 0, opt.numChoices - 1);
		return std::string(opt.choices[index]);
	}
	case OptionType::Int:
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), opt.format ? opt.format : "%d", *opt.intValue);
		return std::string(buffer);
	}
	case OptionType::Float:
	{
		char buffer[32];
		if (opt.format) {
			snprintf(buffer, sizeof(buffer), opt.format, *opt.floatValue);
		} else {
			snprintf(buffer, sizeof(buffer), "%d", (int)(GetNormalized(opt) * 100.0f + 0.5f));
		}
		return std::string(buffer);
	}
	default:
		return std::string();
	}
}

}  // namespace VCS
