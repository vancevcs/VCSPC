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
#include <cstdlib>
#include <cstring>

#include "ppsspp_config.h"

#include "UI/ImDebugger/ImVCS.h"
#include "UI/ImDebugger/ImDebugger.h"

#include "Core/HLE/sceCtrl.h"
#include "Core/System.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSState.h"

static const ImVec4 kUnsetColor = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
static const ImVec4 kGoodColor = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
static const ImVec4 kBadColor = ImVec4(0.90f, 0.45f, 0.45f, 1.0f);

// Renders "unset" in grey rather than a misleading zero. Used everywhere a value might not be
// available, which right now is everywhere.
static void TextUnset() {
	ImGui::TextColored(kUnsetColor, "unset");
}

static void TextOptionalBool(const std::optional<bool> &value) {
	if (!value) {
		TextUnset();
		return;
	}
	ImGui::TextColored(*value ? kGoodColor : kUnsetColor, "%s", *value ? "true" : "false");
}

static void TextOptionalU32(const std::optional<u32> &value) {
	if (!value) {
		TextUnset();
		return;
	}
	ImGui::Text("%u  (0x%08x)", *value, *value);
}

static void TextOptionalFloat(const std::optional<float> &value) {
	if (!value) {
		TextUnset();
		return;
	}
	ImGui::Text("%.4f", *value);
}

static const char *AddrTypeName(VCS::VCSAddrType type) {
	switch (type) {
	case VCS::VCSAddrType::U8: return "u8";
	case VCS::VCSAddrType::U16: return "u16";
	case VCS::VCSAddrType::U32: return "u32";
	case VCS::VCSAddrType::S32: return "s32";
	case VCS::VCSAddrType::Float: return "float";
	default: return "?";
	}
}

void ImVCSWindow::DrawStatus() {
	if (!PSP_IsInited()) {
		ImGui::TextColored(kUnsetColor, "No game running.");
		return;
	}

	if (!VCS::IsActive()) {
		ImGui::TextColored(kBadColor, "Inactive.");
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"The VCS layer activates only when the VCSInputOverhaul compat flag is set\n"
				"and the disc ID is ULUS10160. Current disc ID: %s",
				VCS::GetDiscID().empty() ? "(none)" : VCS::GetDiscID().c_str());
		}
		ImGui::Text("Disc ID: %s", VCS::GetDiscID().empty() ? "(none)" : VCS::GetDiscID().c_str());
		return;
	}

	ImGui::TextColored(kGoodColor, "Active");
	ImGui::SameLine();
	ImGui::Text("- %s", VCS::GetDiscID().c_str());

	// A frozen tick counter means the vblank hook isn't running, which is a completely different
	// problem from an address being wrong. Worth being able to tell at a glance.
	ImGui::Text("Ticks: %llu", (unsigned long long)VCS::GetTickCount());

	int known = 0;
	for (size_t i = 0; i < ARRAY_SIZE(VCS::kVCSAddresses); i++) {
		if (VCS::IsAddrSet(VCS::kVCSAddresses[i].id)) {
			known++;
		}
	}
	ImGui::Text("Addresses known: %d / %d", known, (int)VCS::VCSAddr::Count);

	ImGui::Text("Input context: %s", VCS::VCSInputContextName(VCS::GetCurrentContext()));
	if (VCS::GetCurrentContext() == VCS::VCSInputContext::Unknown) {
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Context is Unknown until PlayerVehicle or PlayerOnFoot is filled in.\n"
				"While Unknown, the input mapping is not applied at all.");
		}
	}
}

void ImVCSWindow::DrawDecodedState() {
	const VCS::VCSState &state = VCS::GetState();

	if (!ImGui::BeginTable("vcsstate", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		return;
	}
	ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 130.0f);
	ImGui::TableSetupColumn("Value");
	ImGui::TableHeadersRow();

	auto row = [](const char *label) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(label);
		ImGui::TableNextColumn();
	};

	row("onFoot");        TextOptionalBool(state.onFoot);
	row("inVehicle");     TextOptionalBool(state.inVehicle);
	row("isAiming");      TextOptionalBool(state.isAiming);
	row("isFreeAiming");  TextOptionalBool(state.isFreeAiming);
	row("weaponIndex");   TextOptionalU32(state.weaponIndex);
	row("cameraYaw");     TextOptionalFloat(state.cameraYaw);
	row("cameraPitch");   TextOptionalFloat(state.cameraPitch);
	row("health");        TextOptionalFloat(state.health);
	row("playerBase");    TextOptionalU32(state.playerBase);
	row("playerVehicle"); TextOptionalU32(state.playerVehicle);

	ImGui::EndTable();
}

void ImVCSWindow::DrawAddressTable() {
	ImGui::TextWrapped(
		"Addresses live in Core/VCS/VCSAddresses.h. Find one with the scratchpad below, then "
		"edit the table and rebuild - they are constexpr on purpose, so there is exactly one "
		"place to change and nothing to keep in sync at runtime.");
	ImGui::Spacing();

	if (!ImGui::BeginTable("vcsaddrs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		return;
	}
	ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 110.0f);
	ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 45.0f);
	ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90.0f);
	ImGui::TableSetupColumn("Live value");
	ImGui::TableHeadersRow();

	for (size_t i = 0; i < ARRAY_SIZE(VCS::kVCSAddresses); i++) {
		const VCS::VCSAddrEntry &entry = VCS::kVCSAddresses[i];

		ImGui::TableNextRow();
		ImGui::PushID((int)i);

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(entry.name);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", entry.note);
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(AddrTypeName(entry.type));

		ImGui::TableNextColumn();
		if (!VCS::IsAddrSet(entry.id)) {
			TextUnset();
		} else if (entry.base != VCS::kNoBase) {
			// Show where it lives right now, not the raw offset - the offset alone is useless
			// when you want to paste an address into the scratchpad.
			const std::optional<u32> resolved = VCS::ResolveAddr(entry.id);
			if (resolved) {
				ImGui::Text("0x%08x", *resolved);
			} else {
				ImGui::TextDisabled("(base null)");
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s + 0x%x", VCS::LookupAddr(entry.base).name, entry.address);
			}
		} else {
			ImGui::Text("0x%08x", entry.address);
		}

		ImGui::TableNextColumn();
		if (!VCS::IsAddrSet(entry.id)) {
			ImGui::TextDisabled("-");
		} else if (entry.type == VCS::VCSAddrType::Float) {
			TextOptionalFloat(VCS::ReadAddrFloat(entry.id));
		} else {
			TextOptionalU32(VCS::ReadAddrAsU32(entry.id));
		}

		ImGui::PopID();
	}

	ImGui::EndTable();
}

void ImVCSWindow::DrawScratchpad() {
	ImGui::TextWrapped(
		"Type a candidate address in hex. Every row is re-read each frame and shown as all "
		"four interpretations at once, so you can tell a float from an int by which column "
		"looks sane. Out-of-bounds and unaligned addresses read as '-', never a crash.");
	ImGui::Spacing();

	if (!ImGui::BeginTable("vcsscratch", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		return;
	}
	ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("Hold", ImGuiTableColumnFlags_WidthFixed, 45.0f);
	ImGui::TableSetupColumn("u32", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("s32", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("float", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("u8 / hex");
	ImGui::TableHeadersRow();

	for (int i = 0; i < kScratchRows; i++) {
		ScratchRow &rowData = scratch_[i];

		ImGui::TableNextRow();
		ImGui::PushID(i);

		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText("##addr", rowData.addressText, sizeof(rowData.addressText),
			ImGuiInputTextFlags_CharsHexadecimal);

		ImGui::TableNextColumn();
		ImGui::Checkbox("##hold", &rowData.freeze);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Stop refreshing this row, so a fast-changing value can be read.");
		}

		// Parse whatever the user typed. Empty or nonsense simply yields no value - we never
		// want a half-typed address to produce a diagnostic.
		u32 address = 0;
		bool haveAddress = false;
		if (rowData.addressText[0] != '\0') {
			char *end = nullptr;
			const unsigned long parsed = strtoul(rowData.addressText, &end, 16);
			if (end != rowData.addressText) {
				address = (u32)parsed;
				haveAddress = true;
			}
		}

		std::optional<u32> raw;
		std::optional<float> asFloat;
		if (haveAddress) {
			if (rowData.freeze && rowData.hasFrozen) {
				raw = rowData.frozenRaw;
			} else {
				raw = VCS::ReadU32(address);
				if (raw) {
					rowData.frozenRaw = *raw;
					rowData.hasFrozen = true;
				}
			}
			asFloat = rowData.freeze && rowData.hasFrozen ? std::optional<float>() : VCS::ReadFloat(address);
		} else {
			rowData.hasFrozen = false;
		}

		ImGui::TableNextColumn();
		if (raw) {
			ImGui::Text("%u", *raw);
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		if (raw) {
			s32 signedValue;
			memcpy(&signedValue, &*raw, sizeof(signedValue));
			ImGui::Text("%d", signedValue);
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		if (asFloat) {
			ImGui::Text("%.4f", *asFloat);
		} else if (raw) {
			// Frozen rows still have the raw bits, so reinterpret those rather than showing
			// nothing at all.
			float frozenFloat;
			memcpy(&frozenFloat, &*raw, sizeof(frozenFloat));
			ImGui::Text("%.4f", frozenFloat);
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		if (rowData.freeze && rowData.hasFrozen) {
			// Hold means hold: derive the byte from the frozen word rather than reading live,
			// so every column in the row describes the same instant.
			ImGui::Text("%u / 0x%08x", rowData.frozenRaw & 0xFF, rowData.frozenRaw);
		} else if (haveAddress) {
			const std::optional<u8> byteValue = VCS::ReadU8(address);
			if (byteValue) {
				ImGui::Text("%u / 0x%08x", *byteValue, raw.value_or(0));
			} else {
				ImGui::TextDisabled("-");
			}
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::PopID();
	}

	ImGui::EndTable();

	if (ImGui::Button("Clear all rows")) {
		for (int i = 0; i < kScratchRows; i++) {
			scratch_[i] = ScratchRow();
		}
	}
}

void ImVCSWindow::DrawInputTester() {
	ImGui::TextWrapped(
		"Real keyboard and mouse input is wired up and feeds this table. Nothing is applied "
		"while the context is Unknown, though, which is the case until the address table can "
		"distinguish on-foot from in-vehicle - so pressing these keys in game does nothing yet. "
		"Hold a row's button to simulate that key and check the mapping reaches sceCtrl.");
	ImGui::Spacing();

	const VCS::VCSInputContext context = VCS::GetCurrentContext();
	ImGui::Text("Current context: %s", VCS::VCSInputContextName(context));
	ImGui::Text("Applied button mask: 0x%08x", VCS::ComputeButtonMask(context));
	float ax = 0.0f, ay = 0.0f;
	VCS::GetAppliedAnalog(&ax, &ay);
	// Which source is driving the stick is the fastest way to see whether the aiming model is
	// engaged: hold the aim key and this must flip to "reticle/mouse" before the crosshair can
	// possibly move.
	ImGui::Text("Analog stick (%s):", VCS::AnalogIsReticle() ? "reticle/mouse" : "WASD");
	ImGui::SameLine();
	ImGui::TextColored((ax != 0.0f || ay != 0.0f) ? kGoodColor : kUnsetColor,
		"x=%+.2f  y=%+.2f", ax, ay);

	// Cumulative, so a screenshot can answer "did the mouse ever drive the reticle" - the live
	// values above are zero whenever the mouse is still, which is always true in a screenshot.
	u64 rFrames = 0, rNonZero = 0;
	VCS::ReticleStats(&rFrames, &rNonZero);
	ImGui::Text("  reticle ticks: %llu   with movement: ", (unsigned long long)rFrames);
	ImGui::SameLine();
	ImGui::TextColored(rNonZero > 0 ? kGoodColor : kBadColor, "%llu", (unsigned long long)rNonZero);

	// The plugin channel. Nothing in VCS reads this stick, so movement here proves only that we
	// are sending - whether anything receives it is the CLEO plugin's business.
	float rx = 0.0f, ry = 0.0f;
	VCS::GetAppliedAimStick(&rx, &ry);
	ImGui::Text("Right stick (plugin aim):");
	ImGui::SameLine();
	if (VCS::PluginAimActive(context)) {
		ImGui::TextColored((rx != 0.0f || ry != 0.0f) ? kGoodColor : kUnsetColor,
			"x=%+.2f  y=%+.2f", rx, ry);
	} else {
		ImGui::TextColored(kUnsetColor, "inactive");
	}

	// Independent of any mapping: proves real host input is reaching this layer at all. If this
	// stays 0 while you hold a key, the problem is the NativeKey hook, not the mapping table.
	const size_t held = VCS::HeldHostKeyCount();
	ImGui::Text("Host keys held:");
	ImGui::SameLine();
	ImGui::TextColored(held > 0 ? kGoodColor : kUnsetColor, "%d", (int)held);

	ImGui::Spacing();

	if (!ImGui::BeginTable("vcsmappings", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		return;
	}
	ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 60.0f);
	ImGui::TableSetupColumn("Held", ImGuiTableColumnFlags_WidthFixed, 40.0f);
	ImGui::TableSetupColumn("PSP", ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn("Action");
	ImGui::TableHeadersRow();

	for (size_t i = 0; i < VCS::kVCSKeyMappingCount; i++) {
		const VCS::VCSKeyMapping &mapping = VCS::kVCSKeyMappings[i];

		ImGui::TableNextRow();
		ImGui::PushID((int)i);

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(VCS::VCSInputContextName(mapping.context));

		ImGui::TableNextColumn();
		// Hold the button to simulate the key being down. Only one row can be simulated at a
		// time, and we track which one ourselves rather than reading back the held state - now
		// that real input feeds the same set, reading it back would make us fight the physical
		// key when the user actually presses it.
		char label[32];
		snprintf(label, sizeof(label), "%d##key", (int)mapping.key);
		ImGui::Button(label);
		const bool nowHeld = ImGui::IsItemActive();
		const bool wasSimulated = simulatedKeyIndex_ == (int)i;
		if (nowHeld && !wasSimulated) {
			VCS::SetHostKeyDown(mapping.key, true);
			simulatedKeyIndex_ = (int)i;
		} else if (!nowHeld && wasSimulated) {
			VCS::SetHostKeyDown(mapping.key, false);
			simulatedKeyIndex_ = -1;
		}
		ImGui::TableNextColumn();
		if (VCS::IsHostKeyDown(mapping.key)) {
			ImGui::TextColored(kGoodColor, "down");
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		ImGui::Text("0x%04x", mapping.psp);

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(mapping.description);

		ImGui::PopID();
	}

	ImGui::EndTable();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextWrapped(
		"PSP button tester. Hold one and watch the game to find out what it actually does. "
		"Guessing these has been the main source of wrong bindings - aim sat on L trigger doing "
		"nothing until it was checked this way. Works regardless of the mapping table.");

	struct PspButton { const char *name; u32 mask; };
	static const PspButton kButtons[] = {
		{ "Cross",    CTRL_CROSS },    { "Circle",   CTRL_CIRCLE },
		{ "Square",   CTRL_SQUARE },   { "Triangle", CTRL_TRIANGLE },
		{ "L",        CTRL_LTRIGGER }, { "R",        CTRL_RTRIGGER },
		{ "Up",       CTRL_UP },       { "Down",     CTRL_DOWN },
		{ "Left",     CTRL_LEFT },     { "Right",    CTRL_RIGHT },
		{ "Select",   CTRL_SELECT },   { "Start",    CTRL_START },
	};

	u32 forced = 0;
	for (size_t i = 0; i < ARRAY_SIZE(kButtons); i++) {
		if (i % 4 != 0) {
			ImGui::SameLine();
		}
		ImGui::Button(kButtons[i].name, ImVec2(90.0f, 0.0f));
		if (ImGui::IsItemActive()) {
			forced |= kButtons[i].mask;
		}
	}
	VCS::SetForcedButtons(forced);
	if (forced != 0) {
		ImGui::TextColored(kGoodColor, "forcing 0x%08x", forced);
	} else {
		ImGui::TextDisabled("(hold a button above)");
	}
}

void ImVCSWindow::DrawCamera() {
	VCS::VCSCameraSettings &s = VCS::CameraSettings();

	ImGui::TextWrapped(
		"Mouse look writes CameraYaw directly instead of going through sceCtrl - the PSP has no "
		"input that could express an exact rotation. It only runs while the context is OnFoot or "
		"InVehicle, so menus are left alone - and so is Aiming, where the mouse is the reticle "
		"instead (see below).");
	ImGui::Spacing();

	// This is the master switch for the mouse, not just for camera rotation - it gates the point
	// where deltas are claimed, which is upstream of both look and aim. Labelled accordingly, so
	// nobody turns it off expecting aiming to survive.
	ImGui::Checkbox("Enable mouse (look + aim)", &s.enabled);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Off hands the mouse back to PPSSPP's own mouse-to-analog handling entirely, "
			"disabling both mouse look and mouse aiming.");
	}
	ImGui::SliderFloat("Sensitivity", &s.sensitivity, 0.0005f, 0.02f, "%.4f rad/count");
	ImGui::Checkbox("Invert X", &s.invertX);
	ImGui::SameLine();
	ImGui::Checkbox("Invert Y", &s.invertY);
	ImGui::Checkbox("Vertical look in vehicles", &s.pitchInVehicle);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Off by default. Driving pitch against the vehicle follow-camera leaves it "
			"settled steeply downward after releasing. Yaw in vehicles is unaffected.");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::TextWrapped(
		"Aiming: in FREE AIM the mouse leaves the camera and drives the analog stick, because the "
		"game has no stored aim direction to write - the weapon camera integrates the look axis "
		"into its own angle every frame, and the gun follows that. Watch 'Analog stick' on the "
		"Input tab: it reads 'reticle/mouse' only when free aim is actually engaged.");
	ImGui::Spacing();

	ImGui::Checkbox("Aim response model (invert the game's curve)", &s.aimResponseModel);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"This is what makes free aim feel like a mouse instead of a thumbstick.\n\n"
			"The game turns the stick axis into rotation with a signed SQUARE, then low-passes "
			"the result, then saturates it. Squaring is the big one: it is a shape, not a scale, "
			"so no sensitivity value can undo it - move the mouse 10 counts in one frame and the "
			"aim travels 100x as far as 1 count in each of ten frames.\n\n"
			"On, the deflection written each frame is whatever the model says will move the aim "
			"by the distance the mouse actually moved, including pushing BACK against the "
			"smoother when the mouse stops so the aim does not coast.\n\n"
			"Off gives the old proportional mapping, for comparison.");
	}
	if (s.aimResponseModel) {
		ImGui::SliderFloat("Aim sensitivity", &s.aimSensitivity, 0.00005f, 0.008f,
			"%.5f rad/count", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Radians of aim rotation per count of mouse movement - the same units as the "
				"Sensitivity slider above, but wants to be far SMALLER, and that is not taste.\n\n"
				"The game's aim rate tops out near 0.053 rad per frame. Ask for more than the "
				"channel can deliver and the model's solve sits at full deflection, which "
				"flattens the response again exactly where it should be linear. At the default "
				"it saturates only on a hard flick.\n\n"
				"Raise it if aiming feels sluggish, but watch the 'Undelivered (carry)' readout "
				"below - once that stops returning to zero, you have gone past what the channel "
				"can do and are trading linearity back away. Aim range boost is what actually "
				"buys headroom, and it needs the second stick.");
		}
		ImGui::Checkbox("Move while free-aiming (PATCHES GAME CODE)", &s.moveInFreeAim);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Patches one instruction so aiming and moving stop being mutually exclusive.\n\n"
				"The player control function processes aiming and then branches straight over the "
				"call that applies movement - that single branch at 0x0894b864 is the whole "
				"reason free aim freezes you. This turns it into a nop so the aim path falls "
				"through into the movement path and both run.\n\n"
				"This is the only thing here that writes game CODE rather than data, and a wrong "
				"address crashes rather than misbehaves. It refuses to patch unless the "
				"instruction reads exactly as expected.\n\n"
				"IMPORTANT: on its own this makes you walk wherever you sweep the crosshair, "
				"because movement reads the same analog axis the aim uses. Turn on 'Drive the "
				"game's second stick' as well - that moves the aim onto the d-pad and frees the "
				"nub for WASD.");
		}
		if (s.moveInFreeAim && !s.usePadStick) {
			ImGui::TextColored(kBadColor,
				"  Needs 'Drive the game's second stick' too, or you walk where you aim.");
		}
		ImGui::Checkbox("Plain mapping for sniper / RPG", &s.aimScopedLinear);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Sniper and RPG (weapon modes 7 and 8) never had lock-on and do not aim through "
				"the camera's squared response - their reticle is driven directly, and linearly."
				"\n\n"
				"The model exists to invert a square. Point it at something already linear and the "
				"sqrt cancels nothing, it just makes small inputs disproportionately large - the "
				"slightest movement sends the scope off on its own. These two aimed correctly "
				"before the model existed, so this puts them back on that mapping.");
		}
		if (s.aimScopedLinear) {
			ImGui::SliderFloat("Sniper/RPG sensitivity", &s.aimScopedSensitivity,
				0.002f, 0.30f, "%.3f stick/count", ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Stick deflection per mouse count for those two weapons. Not radians - this "
					"path pushes the stick in proportion rather than modelling the game, so it "
					"uses the old units.\n\n"
					"At the default, about 22 counts in one frame reaches full deflection. Past "
					"the point where ordinary movement pins the stick, more sensitivity buys "
					"nothing but snapping - the game's own rate limit is the ceiling.");
			}
		}
		ImGui::SliderFloat("Stop strength", &s.aimCancel, 0.0f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"How hard to stop the aim once a stroke has ended.\n\n"
				"The game keeps a smoothed increment that would coast the aim onward after you "
				"stop moving. The model cancels it by briefly pushing the stick BACKWARDS. At "
				"1.00 the cancellation is exact - the momentum is read out of the game, not "
				"predicted.\n\n"
				"Lower it only if the aim travels backwards after a stroke has genuinely ended. "
				"If movement feels interrupted mid-stroke, that is the Stop delay below, not "
				"this.");
		}
		ImGui::SliderInt("Stop delay", &s.aimStopDelay, 0, 6, "%d frames");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"How many consecutive still frames count as 'the stroke ended' before the stop is "
				"allowed to fire. At 30fps each is 33ms.\n\n"
				"This exists because a single frame with no mouse movement does NOT mean you "
				"stopped - slow careful aiming produces zero-count frames mid-stroke, and braking "
				"on those is what made the aim stutter. Waiting separates a pause from a stop.\n\n"
				"0 restores the old behaviour, which stutters. 2 is about right. Raise it if "
				"movement still feels interrupted; lower it for a snappier stop.");
		}
		ImGui::SliderFloat("Aim range boost", &s.aimRangeBoost, 1.0f, 8.0f, "%.1fx");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Needed for FAST aim movements to stay linear. The game's aim rate tops out near "
				"0.05 rad per frame, well under mouse look, so without this anything quicker than "
				"a slow drag gets flattened - which still reads as a thumbstick even with the "
				"model on.\n\n"
				"The d-pad fields are int16 and the accessor only halves and sign-extends them - "
				"it never clamps - so writing past what a real d-pad could produce gives a larger "
				"axis and, through the square, a proportionally higher rate.\n\n"
				"3x covers everything up to a deliberate flick. 1x is hardware-faithful range.\n\n"
				"Only affects the second-stick (d-pad) channel below. The nub is clamped to full "
				"deflection by sceCtrl before the game sees it.");
		}
		if (s.aimRangeBoost > 1.0f && !s.usePadStick) {
			// Worth shouting about: this slider is the obvious thing to reach for when aiming
			// feels rate-limited, and on the nub it does nothing at all - so someone can spend a
			// while dragging it to the stop and concluding the model doesn't work.
			ImGui::TextColored(kBadColor,
				"  Doing nothing - the nub is clamped by sceCtrl. Needs 'Drive the game's "
				"second stick' below.");
		}
		if (s.aimSensitivity > 0.0015f && !s.usePadStick) {
			ImGui::TextColored(kUnsetColor,
				"  On the nub, much above 0.001 saturates on ordinary movement - watch the carry.");
		}
	} else {
		ImGui::SliderFloat("Aim sensitivity", &s.aimSensitivity, 0.005f, 0.30f, "%.3f stick/count");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Legacy units: stick deflection per count of mouse movement in one frame.");
		}
	}
	ImGui::Checkbox("Free aim on aim key (G toggles lock-on)", &s.autoFreeAim);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Pulses the game's Free Aim button (d-pad down) after the aim key goes down, so "
			"aiming starts in free aim instead of lock-on.\n\n"
			"G toggles lock-on mode, which skips the pulse AND stops the mouse taking the analog "
			"stick - so WASD moves and the mouse looks. That is what melee needs, because melee's "
			"lock-on does not register in IsAiming and free aim would otherwise be assumed.");
	}
	// The state has to be visible. The previous version of this was a HELD key that never once
	// arrived, and nothing on screen would have shown that - it looked correctly bound everywhere
	// anyone would have checked.
	ImGui::Text("Lock-on mode (G):");
	ImGui::SameLine();
	const bool lockOn = VCS::LockOnModeActive();
	ImGui::TextColored(lockOn ? kGoodColor : kUnsetColor, "%s", lockOn ? "ON" : "off");
	ImGui::SameLine();
	ImGui::TextDisabled(lockOn ? "(mouse looks, WASD moves)" : "(mouse aims)");
	if (s.autoFreeAim) {
		ImGui::SliderInt("Free aim delay", &s.aimFreeAimDelay, 0, 16, "%d ticks");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"How long to wait after the aim trigger before pressing Free Aim.\n\n"
				"This is what decides whether you SEE lock-on. The game starts hunting for a "
				"target the moment the trigger registers, so every tick here is a tick in which "
				"it can find someone and snap to them. Ticks are 60Hz against a 30fps game.\n\n"
				"0 presses Free Aim on the same tick as the trigger - the least lock-on the game "
				"can be given. Raise it only if free aim stops engaging reliably.");
		}
	}
	ImGui::Checkbox("Invert aim Y", &s.aimInvertY);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Separate from the camera's Invert Y on purpose. The camera defaults to inverted, "
			"the reticle does not - pushing the mouse away raises the crosshair.");
	}

	ImGui::Checkbox("Aim via right stick (CLEO plugin)", &s.aimViaRightStick);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Sends the aim delta to the PSP's right analog stick instead of the left, for the "
			"CLEO plugin in memstick/PSP/PLUGINS/cleo. Nothing in VCS itself reads that stick, "
			"so with no plugin installed this makes aiming do nothing at all. Also disables the "
			"left-stick reticle and stops the camera being driven while aim is held.");
	}
	if (s.aimViaRightStick) {
		ImGui::SameLine();
		ImGui::TextColored(kGoodColor, "(plugin mode)");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::TextWrapped(
		"Second stick: VCS wants two analog sticks and the PSP has one, so it builds the second "
		"from the d-pad - camera X is (DPadRight - DPadLeft) / 2. Those fields are int16 but a "
		"real d-pad only puts 0 or 255 in them, so a player gets three positions. Writing values "
		"in between makes it a true analog axis, and it is the game's OWN camera and aim input, "
		"so it should drive both. Both this and the nub feed the same accessor and get the same "
		"response, so the aim model above applies to whichever one is live - CameraInputMode "
		"decides that, and turning this on is what sets it.");
	if (!VCS::PadStickAvailable()) {
		ImGui::TextColored(kBadColor, "Pad d-pad addresses unset - cannot drive the second stick.");
	} else {
		ImGui::Checkbox("Drive the game's second stick", &s.usePadStick);
		if (!s.aimResponseModel) {
			ImGui::SliderFloat("Stick sensitivity", &s.padStickSensitivity, 0.002f, 0.10f, "%.3f stick/count");
		}
		float px = 0.0f, py = 0.0f;
		VCS::GetPadStick(&px, &py);
		ImGui::Text("Written:");
		ImGui::SameLine();
		ImGui::TextColored((px != 0.0f || py != 0.0f) ? kGoodColor : kUnsetColor,
			"x=%+.2f  y=%+.2f", px, py);
		ImGui::SameLine();
		ImGui::TextDisabled("(live - reads 0 whenever the mouse is still)");

		// The numbers that survive long enough to appear in a screenshot.
		u64 frames = 0, nonZero = 0;
		float peak = 0.0f;
		bool modeSet = false;
		VCS::PadStickStats(&frames, &nonZero, &peak, &modeSet);
		ImGui::Text("Mode flag in game:");
		ImGui::SameLine();
		ImGui::TextColored(modeSet ? kGoodColor : kBadColor, modeSet ? "set" : "NOT set");
		ImGui::Text("Ticks driving: %llu   with movement: ", (unsigned long long)frames);
		ImGui::SameLine();
		ImGui::TextColored(nonZero > 0 ? kGoodColor : kBadColor, "%llu", (unsigned long long)nonZero);
		ImGui::SameLine();
		ImGui::Text("  peak %.2f", peak);
		if (frames > 0 && nonZero == 0) {
			ImGui::TextColored(kBadColor,
				"No mouse movement is reaching this. Move the cursor OVER THE GAME - the "
				"debugger window takes the mouse before we ever see it.");
		}
	}

	if (s.aimResponseModel) {
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::TextUnformatted("Aim model");

		const VCS::VCSAimAxis *ax = nullptr;
		const VCS::VCSAimAxis *ay = nullptr;
		float timeStep = 0.0f;
		VCS::AimModelState(&ax, &ay, &timeStep);

		// The timestep is the one game value the model reads, and everything it computes scales
		// with it - so if this is wrong or stale, nothing else here means anything.
		const std::optional<float> liveStep = VCS::ReadAddrFloat(VCS::VCSAddr::TimeStep);
		ImGui::Text("Timestep:");
		ImGui::SameLine();
		if (liveStep && *liveStep > 0.01f) {
			ImGui::TextColored(kGoodColor, "%.3f", *liveStep);
		} else {
			ImGui::TextColored(kUnsetColor, "not in gameplay - using %.3f", timeStep);
		}

		// We tick from the vblank hook at ~60Hz and this game runs its logic at 30fps, so the model
		// only steps on ticks where the frame counter moved. The ratio is the check that the
		// counter is really the counter: it should sit near 1:1 on this game.
		u64 gameFrames = 0, skipped = 0;
		VCS::AimFrameStats(&gameFrames, &skipped);
		ImGui::Text("Game frames: %llu   ticks skipped: %llu",
			(unsigned long long)gameFrames, (unsigned long long)skipped);
		if (!VCS::IsAddrSet(VCS::VCSAddr::FrameCounter)) {
			ImGui::TextColored(kBadColor,
				"  FrameCounter unset - stepping every tick, which is twice per game frame");
		} else if (gameFrames > 200 && skipped < gameFrames / 4) {
			ImGui::TextColored(kUnsetColor,
				"  expected roughly 1:1 - a low skip count means the game is not running at 30fps");
		}

		// The ceiling the model is solving against. It must match what the live channel can really
		// deliver: if the model solves past what gets written, its mirror records rotation that
		// never happened and the next frame's correction shows up as the aim snapping backwards.
		ImGui::Text("Deflection asked for:  x=%+.3f  y=%+.3f   (channel limit %.1f)",
			ax->lastAxis, ay->lastAxis, VCS::AimChannelLimit());

		// Carry is the honest measure of whether the channel can keep up. Persistently large means
		// the mouse is outrunning the game's maximum aim rate, which is what Aim range boost is
		// for - so say that rather than leaving a number to interpret.
		// Which response the game is applying. The model reads this rather than assuming, because
		// the two families use different smoothing factors (0.9 vs 0.8) and modelling the wrong one
		// makes the stop overshoot - which is the aim snapping backwards.
		const std::optional<u32> camMode = VCS::ReadAddrU32(VCS::VCSAddr::CamMode);
		const std::optional<u32> wepMode = VCS::ReadAddrU32(VCS::VCSAddr::WeaponCamMode);
		const std::optional<float> camFov = VCS::ReadAddrFloat(VCS::VCSAddr::CamFOV);
		ImGui::Text("Camera mode:");
		ImGui::SameLine();
		if (camMode) {
			const bool famB = (*camMode == 11 || *camMode == 28);
			ImGui::TextColored(kGoodColor, "%u", *camMode);
			ImGui::SameLine();
			ImGui::TextDisabled("(%s, alpha %.1f)  weapon mode %u  FOV %.0f",
				famB ? "0x0899c6a0" : "0x089a341c", famB ? 0.8f : 0.9f,
				wepMode ? *wepMode : 0, camFov ? *camFov : 0.0f);
		} else {
			TextUnset();
		}

		// The two things that explain a bad aim entry, side by side.
		//
		// IsAiming going to 1 as you press aim IS the lock-on - if it flickers up before free aim
		// engages, lower the Free aim delay above.
		//
		// CameraInputMode is the trap worth watching: it selects whether the game reads the nub or
		// the D-PAD as its aim axis, and the game sets it itself as modes change. If it reads 1
		// while the Free Aim pulse is pressing d-pad down, that press lands as a full-deflection
		// aim axis instead of a button - which would swing the view hard the moment you aim.
		const std::optional<u32> lockedOn = VCS::ReadAddrU32(VCS::VCSAddr::IsAiming);
		const std::optional<u32> inputMode = VCS::ReadAddrU32(VCS::VCSAddr::CameraInputMode);
		ImGui::Text("Locked on:");
		ImGui::SameLine();
		ImGui::TextColored(lockedOn.value_or(0) ? kBadColor : kGoodColor,
			"%s", lockedOn.value_or(0) ? "YES" : "no");
		ImGui::SameLine();
		ImGui::Text("   CameraInputMode:");
		ImGui::SameLine();
		if (inputMode) {
			ImGui::TextColored(*inputMode ? kBadColor : kGoodColor,
				"%u %s", *inputMode, *inputMode ? "(d-pad IS the aim axis - see below)" : "(nub)");
		} else {
			TextUnset();
		}

		// Read straight out of the game, so this is the momentum the stop is cancelling - not a
		// prediction of it. If it never returns to zero after you stop moving, the aim is coasting.
		const std::optional<float> incX = VCS::ReadAddrFloat(VCS::VCSAddr::CamAimIncX);
		const std::optional<float> incY = VCS::ReadAddrFloat(VCS::VCSAddr::CamAimIncY);
		ImGui::Text("Game's increment:      x=%+.5f  y=%+.5f rad  (read, not predicted)",
			incX.value_or(0.0f), incY.value_or(0.0f));

		// Whether the model currently thinks a stroke is in progress or over. This is the thing to
		// watch if movement feels interrupted: if it reaches the delay while you are still moving,
		// the stop is firing mid-stroke and the delay wants raising.
		const int still = VCS::AimStillFrames();
		const bool stopped = still >= s.aimStopDelay;
		ImGui::Text("Stroke:");
		ImGui::SameLine();
		ImGui::TextColored(stopped ? kUnsetColor : kGoodColor,
			"%s", stopped ? "ended - stop active" : "moving - stop held off");
		ImGui::SameLine();
		ImGui::TextDisabled("(%d still frames, delay %d)", still > 99 ? 99 : still, s.aimStopDelay);

		ImGui::Text("Undelivered (carry):   x=%+.4f  y=%+.4f rad", ax->carry, ay->carry);
		const float carryMag = (ax->carry < 0.0f ? -ax->carry : ax->carry);
		if (carryMag > 0.02f) {
			ImGui::TextColored(kUnsetColor,
				"  hitting the game's aim rate limit - raise Aim range boost");
		}
	}

	ImGui::Spacing();
	ImGui::Separator();

	if (!VCS::IsAddrSet(VCS::VCSAddr::CameraYaw)) {
		ImGui::TextColored(kBadColor, "CameraYaw is unset - mouse look cannot run.");
		return;
	}

	const std::optional<float> yaw = VCS::ReadAddrFloat(VCS::VCSAddr::CameraYaw);
	const std::optional<float> pitch = VCS::ReadAddrFloat(VCS::VCSAddr::CameraPitch);
	ImGui::Text("CameraPitch:");
	ImGui::SameLine();
	if (pitch) { ImGui::Text("%.4f rad  (%.1f deg)", *pitch, *pitch * 57.2957795f); } else { TextUnset(); }

	ImGui::Text("CameraYaw:");
	ImGui::SameLine();
	if (yaw) {
		ImGui::Text("%.4f rad  (%.1f deg)", *yaw, *yaw * 57.2957795f);
	} else {
		TextUnset();
	}

	float dYaw = 0.0f, dPitch = 0.0f, aPitch = 0.0f;
	int holdFrames = 0;
	const char *ctxName = "";
	VCS::CameraHoldState(&dYaw, &dPitch, &aPitch, &holdFrames, &ctxName);
	ImGui::Text("Asserting: yaw %.4f  pitch %.4f   (anchored pitch %.4f)", dYaw, dPitch, aPitch);
	ImGui::Text("Hold frames left: %d   last context: %s", holdFrames, ctxName);
	if (pitch && holdFrames > 0) {
		const float drift = dPitch - *pitch;
		ImGui::Text("Pitch we assert minus game pitch:");
		ImGui::SameLine();
		ImGui::TextColored((drift > 0.15f || drift < -0.15f) ? kBadColor : kUnsetColor,
			"%+.4f", drift);
	}

	ImGui::Text("Driving the camera:");
	ImGui::SameLine();
	const bool driving = VCS::CameraIsDriving();
	ImGui::TextColored(driving ? kGoodColor : kUnsetColor, "%s", driving ? "yes" : "no");

	ImGui::Text("Pending mouse dx: %.2f", VCS::CameraPendingDeltaX());

	u64 seen = 0, claimed = 0, rejected = 0;
	float maxAbsDx = 0.0f, sumAbsDx = 0.0f;
	VCS::CameraDiagnostics(&seen, &claimed, &rejected, &maxAbsDx, &sumAbsDx);
	ImGui::Text("Mouse deltas seen: %llu   claimed: %llu   rejected (context): %llu",
		(unsigned long long)seen, (unsigned long long)claimed, (unsigned long long)rejected);
	ImGui::Text("Largest single dx: %.2f   total movement: %.1f", maxAbsDx, sumAbsDx);
	u64 writes = 0, fails = 0;
	float lastWritten = 0.0f;
	VCS::CameraWriteStats(&writes, &fails, &lastWritten);
	ImGui::Text("Yaw writes: %llu   failed: %llu   last written: %.4f",
		(unsigned long long)writes, (unsigned long long)fails, lastWritten);
	if (maxAbsDx == 0.0f && seen > 0) {
		ImGui::TextColored(kBadColor,
			"Events arrive but every delta is zero - the real mouse movement is not reaching here.");
	}
	if (seen == 0) {
		ImGui::TextColored(kBadColor,
			"No mouse events are reaching this layer at all - the problem is upstream of VCS.");
	}

	ImGui::Spacing();
	ImGui::TextDisabled(
		"Pitch limits follow the anchor by +/-0.7 rad, because the game's pitch baseline "
		"differs per camera: about -0.05 on foot, about -1.55 in a vehicle.");
}

void ImVCSWindow::Draw(ImConfig &cfg) {
	ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("VCS", &cfg.vcsOpen)) {
		ImGui::End();
		return;
	}

	DrawStatus();
	ImGui::Separator();

	if (ImGui::BeginTabBar("vcstabs")) {
		if (ImGui::BeginTabItem("State")) {
			DrawDecodedState();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Addresses")) {
			DrawAddressTable();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Scratchpad")) {
			DrawScratchpad();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Input")) {
			DrawInputTester();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Camera")) {
			DrawCamera();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
}
