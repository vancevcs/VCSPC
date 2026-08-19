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
#include "Common/System/Request.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSSettings.h"

namespace VCS {

// Labels for the Choice options. These mirror PPSSPP's own settings screen so the two never
// disagree about what a given index means.
static const char *const kResolutionLabels[] = {
	"Auto", "1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x",
};
static const char *const kAnisoLabels[] = { "Off", "2x", "4x", "8x", "16x" };

static Path SettingsPath() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "vcs.ini";
}

const std::vector<Option> &Options() {
	// Function-local static, not a file-scope table, because the rows point into
	// CameraSettings() and FireHookSettings() - themselves function-local statics. A file-scope
	// table would be racing their construction.
	static const std::vector<Option> options = [] {
		VCSCameraSettings &cam = CameraSettings();

		std::vector<Option> opts;

		auto addBool = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, bool *value) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Bool;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.boolValue = value;
			opt.defaultBool = *value;
			opts.push_back(opt);
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
				int defaultIndex, bool external = false, void (*onChange)() = nullptr) {
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
			// Explicit rather than captured from the live value: for an external setting the
			// live value at table-build time is whatever the user last saved, so capturing it
			// would make "restore defaults" mean "restore what I had at startup".
			opt.defaultInt = defaultIndex;
			opts.push_back(opt);
		};

		// --- Mouse ---

		addBool(OptionPage::Mouse, "MouseEnabled", "Mouse control",
			"Mouse look on foot and in vehicles. Needs Use Mouse Control on, in Settings - Controls.",
			&cam.enabled);
		addFloat(OptionPage::Mouse, "LookSensitivity", "Look sensitivity",
			"How far the camera turns per unit of mouse movement.",
			&cam.sensitivity, 0.0005f, 0.02f);
		addBool(OptionPage::Mouse, "InvertLookX", "Invert look horizontally",
			"", &cam.invertX);
		addBool(OptionPage::Mouse, "InvertLookY", "Invert look vertically",
			"", &cam.invertY);
		addBool(OptionPage::Mouse, "PitchInVehicle", "Vertical look in vehicles",
			"Look up and down while driving, not only left and right.",
			&cam.pitchInVehicle);

		// --- Aiming ---

		addFloat(OptionPage::Aiming, "AimSensitivity", "Aim sensitivity",
			"Aiming is a separate channel from looking, and runs a good deal slower.",
			&cam.aimSensitivity, 0.00005f, 0.008f);
		addBool(OptionPage::Aiming, "InvertAimY", "Invert aim vertically",
			"", &cam.aimInvertY);
		addFloat(OptionPage::Aiming, "MouseAcceleration", "Mouse acceleration",
			"Extra gain on fast movements. Zero keeps the response perfectly linear.",
			&cam.aimAccel, 0.0f, 0.05f);
		addFloat(OptionPage::Aiming, "AccelerationCap", "Acceleration cap",
			"Ceiling on that gain. Past the aim channel's own limit, more only arrives late.",
			&cam.aimAccelMax, 1.0f, 5.0f, "%.1fx");
		addBool(OptionPage::Aiming, "FreeAim", "Free aim",
			"Shoot where the mouse points, instead of the game locking on to a target for you.",
			&cam.autoFreeAim);
		addBool(OptionPage::Aiming, "MoveWhileAiming", "Move while aiming",
			"Walk with WASD with a weapon raised. Patches game code while it is on.",
			&cam.moveInFreeAim);
		addBool(OptionPage::Aiming, "ScopedCameraAim", "Camera aim for sniper and RPG",
			"Scoped weapons aim by turning the camera, which is what looking down sights is.",
			&cam.aimScopedCamera);
		addFloat(OptionPage::Aiming, "ScopedSensitivity", "Sniper and RPG sensitivity",
			"Only applies to the two scoped weapons.",
			&cam.aimScopedSensitivity, 0.005f, 0.2f);
		addBool(OptionPage::Aiming, "AimResponseModel", "Mouse-native aim response",
			"Undoes the game's stick response curve. Off is the old thumbstick feel.",
			&cam.aimResponseModel);

		// --- Audio ---
		//
		// One row, because master volume is the only one of the three that has anything behind
		// it. SFX and radio belong to the game's own mixer and need addresses we do not have,
		// and a row wired to a variable nothing reads would move and change nothing - worse
		// than a page that is visibly still short.
		//
		// g_Config.iGameVolume runs 0..VOLUMEHI_FULL, and the step is what keeps a row of ten
		// blocks honest: one press moves one block. It was a Choice into an eleven-label list
		// first, which read correctly and was wrong - the index and the value are not the same
		// number, so the first press dropped the volume from 100 to 10.
		addInt(OptionPage::Audio, nullptr, "Master volume",
			"Overall volume, applied by the emulator.",
			&g_Config.iGameVolume, VOLUME_OFF, VOLUMEHI_FULL, VOLUMEHI_FULL / 10,
			Config::GetDefaultValueInt(&g_Config.iGameVolume), true);

		// --- Graphics ---

		addChoice(OptionPage::Graphics, nullptr, "Resolution",
			"Internal rendering resolution. Higher is sharper and costs more.",
			&g_Config.iInternalResolution, kResolutionLabels, ARRAY_SIZE(kResolutionLabels),
			Config::GetDefaultValueInt(&g_Config.iInternalResolution), true, []() {
				System_PostUIMessage(UIMessage::GPU_RENDER_RESIZED);
			});
		addChoice(OptionPage::Graphics, nullptr, "Anisotropic filtering",
			"Sharpens textures viewed at a shallow angle, like road surfaces ahead of you.",
			&g_Config.iAnisotropyLevel, kAnisoLabels, ARRAY_SIZE(kAnisoLabels),
			Config::GetDefaultValueInt(&g_Config.iAnisotropyLevel), true);

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
			break;  // No non-external Choice options yet.
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
			if (opt.onChange) {
				opt.onChange();
			}
			break;
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

std::string ValueText(const Option &opt) {
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
