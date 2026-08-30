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
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSState.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSVault.h"
#include "Core/VCS/VCSWorld.h"

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
	// The slot decides whether free aim engages at all (melee never does), so show what the aim
	// layer concluded next to it rather than making anyone remember where the cutoff is.
	row("weaponIndex");   TextOptionalU32(state.weaponIndex);
	if (state.weaponIndex) {
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", VCS::WeaponSlotIsMelee(*state.weaponIndex) ? "melee" : "gun");
	}
	row("weaponType");    TextOptionalU32(state.weaponType);
	row("cameraYaw");     TextOptionalFloat(state.cameraYaw);
	row("cameraPitch");   TextOptionalFloat(state.cameraPitch);
	row("health");        TextOptionalFloat(state.health);
	row("playerBase");    TextOptionalU32(state.playerBase);
	row("playerVehicle"); TextOptionalU32(state.playerVehicle);
	// Set for about 1.65s before playerVehicle appears; pitch is held at the vehicle ceiling for
	// exactly that window, which is what stops the on-foot pitch seeding the spring runaway.
	row("entering veh");  TextOptionalU32(state.enteringVehicle);
	row("vehicleModel");  TextOptionalU32(state.vehicleModel);
	// The class is what actually picks the bindings, so show it rather than making anyone map
	// the model id back by hand while sitting in the thing.
	row("vehicleClass");  ImGui::TextUnformatted(VCS::VehicleClassName(state.vehicleClass));

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

	// Free aim at the fire site - the one aiming path that does not fight the game's own aim
	// integrator, because it rewrites the shot's target vector just before the raycast.
	if (ImGui::CollapsingHeader("Free aim (fire-site hook)", ImGuiTreeNodeFlags_DefaultOpen)) {
		VCS::VCSFireHookSettings &f = VCS::FireHookSettings();
		ImGui::Text("hook: %s", VCS::FireHookInstalled() ? "installed" : "NOT installed");

		u64 seen = 0, redirected = 0;
		VCS::FireHookStats(&seen, &redirected);
		ImGui::Text("raycasts seen: %llu   redirected: %llu",
			(unsigned long long)seen, (unsigned long long)redirected);
		// The failure worth spotting at a glance: the hook runs but the guard rejects every
		// shot, which from the outside looks identical to the hook not working at all.
		if (seen > 0 && redirected == 0) {
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
				"guard rejecting every shot - aim key held? player radius too small?");
		}

		ImGui::Checkbox("Enable fire-site free aim", &f.enabled);
		ImGui::SliderFloat("Crosshair X", &f.crosshairX, 0.40f, 0.60f, "%.4f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Where the crosshair sits ACROSS the screen, 0.5 being centre. re3's m_f3rdPersonCHairMultX, and it means the same thing: the ray is unprojected through that pixel. Expected to want 0.5 now that the camera's own Front is used - the 0.51 the old build wanted was correcting a 3.3 degree error in the direction, not describing the crosshair.");
		}
		ImGui::SliderFloat("Crosshair Y", &f.crosshairY, 0.40f, 0.60f, "%.4f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Same, vertically. Lower value aims higher up the screen.");
		}
		ImGui::SliderFloat("Yaw trim (deg)", &f.aimYawOffsetDeg, -15.0f, 15.0f, "%.2f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Residual rotation on top of the solved ray. EXPECTED TO WANT ZERO now that the camera's own Front is read rather than reconstructed - if it needs several degrees again, that is evidence the basis is not being used, not a value to keep.");
		}
		ImGui::Checkbox("Camera-origin ray (removes parallax)", &f.useCameraOrigin);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Fire from the camera and slide the origin forward to the muzzle, as re3 does, instead of firing from the gun along a borrowed direction. Off means accepting parallax that changes with the camera's angle to the player AND with range. The camera position is CCam+0x20, measured rather than ranked.");
		}
		ImGui::Checkbox("Weapon range from CWeaponInfo", &f.useWeaponRange);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Fire the ray at the weapon's own range instead of reusing the length of whatever target the game had already computed. That length is the weapon range on the plain path, the distance to a locked-on entity on another, and the distance to the free-aim dummy on a third - so a redirected ray kept stopping short by a different amount every shot.");
		}
		ImGui::SliderFloat("Fallback / minimum range", &f.fallbackRange, 10.0f, 200.0f, "%.0f");
		ImGui::SliderFloat("Player radius", &f.playerRadius, 0.5f, 10.0f, "%.1f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("How close to the player a ray must start to count as the player's shot. The wrapper carries no shooter argument, so this stands in for re3's shooter == FindPlayerPed check. NPCs fire through the same code.");
		}

		ImGui::Separator();
		// The last shot, in numbers. The point is that a wrong answer can be READ here instead of
		// inferred from where the rounds went - which is how the direction stayed 4.4 degrees out
		// through several rounds of tuning the wrong knob.
		// LIVE, read every UI frame. This block used to come from the last redirected shot, which
		// made it useless for the question it exists to answer: these numbers only refreshed when
		// you FIRED, so watching them while moving the mouse showed a frozen snapshot from whenever
		// the last round went off. A diagnostic that updates on the wrong event is worse than none,
		// because it invites conclusions from stale data.
		{
			const std::optional<float> fx = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamFrontOffset);
			const std::optional<float> fy = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamFrontOffset + 4);
			const std::optional<float> fz = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamFrontOffset + 8);
			const std::optional<float> fov = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamFOVOffset);
			const std::optional<float> sx = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamSourceOffset);
			const std::optional<float> sy = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamSourceOffset + 4);
			const std::optional<float> sz = VCS::ReadFloat(VCS::kVCSCam0 + VCS::kVCSCamSourceOffset + 8);
			const std::optional<u16> camMode = VCS::ReadAddrAsU32(VCS::VCSAddr::CamMode)
				? std::optional<u16>((u16)*VCS::ReadAddrAsU32(VCS::VCSAddr::CamMode)) : std::nullopt;
			if (fx && fy && fz) {
				ImGui::Text("LIVE Front  %+.4f %+.4f %+.4f   FOV %.1f   CamMode %d",
					*fx, *fy, *fz, fov ? *fov : 0.0f, camMode ? (int)*camMode : -1);
				if (sx && sy && sz) {
					ImGui::Text("LIVE Source %.2f %.2f %.2f", *sx, *sy, *sz);
				}
				const std::optional<float> cy = VCS::ReadAddrFloat(VCS::VCSAddr::CameraYaw);
				const std::optional<float> cp = VCS::ReadAddrFloat(VCS::VCSAddr::CameraPitch);
				if (cy && cp) {
					const float kPi = 3.14159265358979f;
					const float frontYaw = atan2f(*fy, *fx);
					const float fzc = *fz > 1.0f ? 1.0f : (*fz < -1.0f ? -1.0f : *fz);
					const float frontPitch = asinf(fzc);
					float dYawDeg = (frontYaw - (*cy - kPi)) * 180.0f / kPi;
					while (dYawDeg > 180.0f) dYawDeg -= 360.0f;
					while (dYawDeg < -180.0f) dYawDeg += 360.0f;
					// Move the mouse on ONE axis and watch the matching row. Front is what the shot
					// and the gun follow; CameraYaw/CameraPitch is what the mouse writes. If a row
					// does not move when its axis does, the write is landing and not reaching the
					// aim - which is a different problem from the write being rejected.
					ImGui::Text("LIVE Front yaw   %+.4f   CameraYaw-PI %+.4f   off %+.2f deg",
						frontYaw, *cy - kPi, dYawDeg);
					ImGui::Text("LIVE Front pitch %+.4f   CameraPitch  %+.4f   off %+.2f deg",
						frontPitch, *cp, (frontPitch - *cp) * 180.0f / kPi);
				}
			} else {
				ImGui::TextDisabled("LIVE camera basis unreadable");
			}
		}

		ImGui::Separator();
		const VCS::VCSFireHookTrace &t = VCS::FireHookLastTrace();
		if (!t.valid) {
			ImGui::TextDisabled("(last shot: none redirected yet - hold aim and fire)");
		} else if (!t.haveBasis) {
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "camera basis unreadable - fell back to the angle route");
		} else {
			ImGui::TextDisabled("last redirected shot:");
			ImGui::Text("Ray    %+.4f %+.4f %+.4f  from %.2f %.2f %.2f",
				t.dir[0], t.dir[1], t.dir[2], t.origin[0], t.origin[1], t.origin[2]);
			ImGui::Text("Range  %.1f used, %.1f from the game's own target%s",
				t.range, t.gameRange, t.weaponType >= 0 ? "" : "  (weapon record unreadable)");
			if (t.weaponType >= 0) {
				ImGui::SameLine();
				ImGui::Text("  weapon %d", t.weaponType);
			}
			// The single number that says whether the old build's mistake has come back: how far
			// the camera's own forward sits from the direction CameraYaw/CameraPitch imply. It is
			// NOT expected to be zero - that gap is the crosshair offset and it moves with the
			// camera. It is here because the old route silently aimed along the second one.
		}

		ImGui::Separator();
		// Both axes, live against desired. Circle the mouse and watch: if pitch sticks at a limit
		// while yaw keeps running, the two diverge here and the circle comes out flat-topped. If
		// both track and the shape is still wrong, the cause is downstream of this and no amount of
		// tuning these will touch it.
		{
			float dYaw = 0.0f, lYaw = 0.0f, dPitch = 0.0f, lPitch = 0.0f;
			VCS::AimAxisStats(&dYaw, &lYaw, &dPitch, &lPitch);
			ImGui::Text("yaw   desired %+.4f  live %+.4f  diff %+.4f", dYaw, lYaw, dYaw - lYaw);
			ImGui::Text("pitch desired %+.4f  live %+.4f  diff %+.4f", dPitch, lPitch, dPitch - lPitch);
			ImGui::Checkbox("Lead only while the aim is stalled", &s.aimLeadOnStallOnly);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The deadband is stiction - what it takes to get the aim MOVING - not an offset to hold. Measured: Front moved one deadband while the camera angle moved two, so Front converges onto the lever rather than stopping short of it. Holding the lead therefore parks the aim a deadband past target and the correction swings back by twice that. Off restores always-lead.");
			}
			ImGui::SliderFloat("Aim lead blend (rad)", &s.aimLeadBlend, 0.0f, 0.15f, "%.3f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Band over which the deadband lead fades to zero as the aim arrives. Without it the lead flips sign the instant the aim passes its target and the camera angle moves two deadbands in one frame - the measured 19 degree snap. Larger is smoother through the crossing; 0 restores the hard flip.");
			}
			ImGui::Checkbox("Retract the lead when a stroke ends", &s.aimLeadRetract);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Drops the deadband lead once, on the first still frame after aiming. Without it the hold window expires with the camera angle parked a deadband past where you stopped, the game adopts that as its own, and the view snaps. If aiming misbehaves at the END of a movement, turn this off first - it is the only thing that touches that moment.");
			}
			ImGui::SliderFloat("Aim yaw deadband (rad, SUPERSEDED, keep 0)", &s.aimYawDeadband, 0.0f, 0.4f, "%.3f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The closed-loop yaw lead: desired = liveYaw + error + bias, error measured against Front's yaw. The lead itself was right - yaw's deadband is measured at 0.166, the same as pitch - but this route flips its bias at the zero crossing and moves the lever 2D in one frame, which is the 0.340 rad snap. Superseded by Aim yaw kick, which carries the same number open loop. Keep 0.");
			}
			ImGui::SliderFloat("Aim yaw kick (rad)", &s.aimYawKick, 0.0f, 0.4f, "%.3f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The lead that gets yaw MOVING, applied in the direction of the stroke without reading Front. Measured 2026-08-21 at 9.56 deg (0.166 rad), symmetric both ways and releasing the moment the view breaks loose - which is stiction, not the constant between the two spaces. Open loop, so there is no error to cross zero and the snap the deadband above produced cannot occur. 0 restores the plain accumulator.");
			}
			ImGui::SliderFloat("Yaw kick reversal threshold (rad)", &s.aimYawKickHysteresis, 0.0f, 0.2f, "%.3f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("KEEP 0. How far the mouse must travel AGAINST the current lead before it moves to the other side - and it does not prevent the reversal cost, it only delays it. During the spend the lever moves but the aim does not, because an aim at rest has no lead in front of it, so you push and nothing happens and then it all arrives at once. That is the dead zone after a pause: watched in the debugger, 0.04 of intent in and 0.34 out, which is this setting and 2*aimYawKick read back exactly. At 0 the flip lands on the first frame of the reversal, which is a frame you are already moving on.");
			}
			ImGui::SliderInt("Yaw kick re-arm (ticks at rest)", &s.aimYawKickRearmTicks, 0, 30, "%d ticks");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("KEEP 0 - this was not the fault. How long the game's aim must sit still (Front not moving, mouse not moving) before the next stroke pays for the kick again, on the theory that a parked sign outlives the lead behind it. It does, but the dead zone people were feeling is the reversal threshold above, and setting that to 0 fixes it with this off. Two builds went into this and both were reported worse - the first counted still MOUSE ticks and re-armed inside slow sweeps, stacking a second kick on the one already working. Kept switchable so it is not rebuilt from the argument a third time.");
			}
			ImGui::Checkbox("Retract the yaw kick when a stroke ends", &s.aimYawKickRetract);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("OFF, and it CAUSES a snap - it shipped on for one build and was reported at once. Moving the lever back by a full deadband is exactly the step that breaks the aim loose again, because the deadband gates the ONSET of movement rather than being a gap Front rests inside. Leaving the lead parked past the aim costs nothing, which is why pitch has always done the same. On is for reproducing the snap, not for fixing anything.");
			}
			{
				float kickSign = 0.0f, kickAgainst = 0.0f;
				int kickRest = 0;
				VCS::YawKickState(&kickSign, &kickAgainst, &kickRest);
				// The two numbers that say whether the kick is behaving. Sign should sit at +1 or -1
				// for the whole of a one-way stroke and return to 0 when you stop; if it oscillates
				// while you hold a steady sweep, the reversal threshold is too low and that is the
				// chatter to catch before it is felt.
				// The rest count is what to watch when tuning the re-arm. It should climb only while
				// you have genuinely stopped and the view has settled; reaching the threshold during
				// a sweep you think is continuous is the slow-stroke case the tooltip warns about,
				// and shows up as a kick arriving partway through.
				ImGui::TextDisabled("   yaw kick: side %+.0f   travelled against it %.4f / %.3f   at rest %d / %d",
					kickSign, kickAgainst, s.aimYawKickHysteresis, kickRest, s.aimYawKickRearmTicks);
			}
			ImGui::SliderFloat("Aim pitch deadband (rad)", &s.aimPitchDeadband, 0.0f, 0.4f, "%.3f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The lead CameraPitch must hold over the aim's real pitch before the game moves it - measured at 0.164 rad (9.4 deg). Applied as an instant offset when a stroke starts, so pitch begins immediately and then tracks 1:1. Too small and the delay returns; too large and it overshoots slightly before settling. 0 drives CameraPitch directly.");
			}
			ImGui::SliderFloat("Aim pitch gain", &s.aimPitchGain, 0.25f, 4.0f, "%.2f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Plain vertical sensitivity multiplier while aiming, on top of the deadband solve. Leave at 1.00 unless vertical should genuinely be faster or slower than horizontal - it is no longer doing the deadband's job.");
			}
			ImGui::SliderFloat("Aim pitch leash (DISPROVEN, keep 0)", &s.aimPitchLeash, 0.0f, 0.5f, "%.3f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Any non-zero value makes vertical aiming impossible, which falsified the premise it was built on: Front does not chase CameraPitch, it has to be LED. A leash forbidding exactly that forbids pitching. Kept visible so the idea is not reinvented.");
			}
			// The asymmetry to look for: yaw's diff should sit near zero while pitch's does not.
			// That is the game correcting Alpha every frame and leaving Beta alone, which makes
			// vertical inherently laggier than horizontal.
			if (std::fabs(dPitch - lPitch) > 0.02f && std::fabs(dYaw - lYaw) < 0.01f) {
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
					"pitch is being fought and yaw is not - the axes are not symmetric here");
			}
		}

		ImGui::Separator();
		// Issue two of the pair: the bullet followed the crosshair and the man did not.
		// The gun and the body are separate mechanisms, and conflating them cost a wrong diagnosis -
		// the body was tracking correctly while the gun stayed pinned to a world point, which from
		// the outside looked like the body turning the wrong way. Two controls, listed apart.
		// The one line that says WHY, so an intermittent detach does not have to be reproduced
		// once per candidate. Anything starting SUPPRESSED means the aim path is off and names
		// the gate that turned it off.
		{
			const char *why = VCS::AimPathStatus(VCS::GetCurrentContext());
			// PARTIAL earns the warning colour too - it means the game took the aim back and
			// only the gun is still keeping up.
			const bool bad = strncmp(why, "SUPPRESSED", 10) == 0 ||
			                 strncmp(why, "PARTIAL", 7) == 0;
			ImGui::Text("aim path:");
			ImGui::SameLine();
			ImGui::TextColored(bad ? kBadColor : kGoodColor, "%s", why);
		}

		ImGui::Checkbox("Point the gun at the crosshair", &s.pedAimGun);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The arm IK aims at a world POSITION, so once the stick stops being fed the gun holds its bearing while the body turns under it. This moves that position onto the crosshair every frame, down the same ray the bullet takes. Only ever moves a DUMMY entity - a real ped in that slot means lock-on, and writing its position would teleport an NPC.");
		}
		if (s.pedAimGun) {
			ImGui::SliderFloat("Aim point distance", &s.pedAimGunDistance, 5.0f, 150.0f, "%.0f");
			u32 target = 0;
			int type = -1;
			u64 gunWrites = 0;
			VCS::PedGunStats(&target, &type, &gunWrites);
			if (!target) {
				ImGui::TextDisabled("point-gun-at: none (nothing to move - aim at something first)");
			} else {
				// Blocked types are the ones the world owns: moving a ped or a vehicle teleports
				// it. Anything else is a placeholder and gets moved. Measured value in free aim is
				// 4, an object.
				static const char *kTypeNames[8] = {
					"nothing", "building", "vehicle", "ped", "object", "dummy", "?", "?"
				};
				const bool blocked = type == VCS::kVCSEntityTypePed ||
					type == VCS::kVCSEntityTypeVehicle ||
					type == VCS::kVCSEntityTypeBuilding || type == 0;
				ImGui::TextColored(blocked ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : kGoodColor,
					"point-gun-at: %08x  type %d (%s)%s   moves %llu",
					target, type, (type >= 0 && type < 8) ? kTypeNames[type] : "?",
					blocked ? " - world-owned, NOT moving it" : "",
					(unsigned long long)gunWrites);
				// Where it actually sits. A placeholder parked at the aim point sits a sensible
				// aiming distance away and tracks the crosshair once this is running; a real world
				// object does not, and that difference is the thing to watch if anything in the
				// scenery ever moves that should not.
				const std::optional<float> tx = VCS::ReadFloat(target + VCS::kVCSEntityPositionOffset);
				const std::optional<float> ty = VCS::ReadFloat(target + VCS::kVCSEntityPositionOffset + 4);
				const std::optional<float> tz = VCS::ReadFloat(target + VCS::kVCSEntityPositionOffset + 8);
				const std::optional<u32> playerPed = VCS::ReadAddrU32(VCS::VCSAddr::PlayerBase);
				if (tx && ty && tz) {
					float dist = -1.0f;
					if (playerPed && *playerPed) {
						const std::optional<float> px = VCS::ReadFloat(*playerPed + VCS::kVCSEntityPositionOffset);
						const std::optional<float> py = VCS::ReadFloat(*playerPed + VCS::kVCSEntityPositionOffset + 4);
						const std::optional<float> pz = VCS::ReadFloat(*playerPed + VCS::kVCSEntityPositionOffset + 8);
						if (px && py && pz) {
							const float dx = *tx - *px, dy = *ty - *py, dz = *tz - *pz;
							dist = sqrtf(dx * dx + dy * dy + dz * dz);
						}
					}
					ImGui::Text("   at %.2f %.2f %.2f   %.1f m from the player", *tx, *ty, *tz, dist);
				}
			}
		}
		ImGui::Checkbox("Turn the character with the aim (redundant)", &s.pedFollowAim);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("OFF because the game already does it: mode 45's Process writes m_fRotationCur and m_fRotationDest every frame as atan2(Front.y, Front.x) - PI/2, which is the same formula. Two writers at different rates is what made the crosshair unable to move horizontally until the body had finished turning. Turn it on only to test whether the game's own write is gated on something.");
		}
		if (s.pedFollowAim) {
			ImGui::Checkbox("Snap heading (off = use the game's turn rate)", &s.pedSnapHeading);
			ImGui::Checkbox("Invert heading (calibration)", &s.pedHeadingInvert);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("ON by default because play says so, against four static checks that all said otherwise. Unexplained - see the note on pedHeadingInvert. Use the two readings below to settle it properly rather than leaving it as a knob.");
			}
			ImGui::SliderFloat("Heading offset (deg)", &s.pedHeadingOffsetDeg, -180.0f, 180.0f, "%.1f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Residual rotation on the ped heading, after the invert.");
			}
			bool driving = false;
			float desired = 0.0f;
			u64 writes = 0;
			VCS::PedAimStats(&driving, &desired, &writes);
			const std::optional<float> live = VCS::ReadAddrFloat(VCS::VCSAddr::PedHeading);
			if (live) {
				ImGui::Text("ped heading: live %.4f  desired %.4f  writes %llu  %s",
					*live, desired, (unsigned long long)writes, driving ? "DRIVING" : "idle");
			} else {
				ImGui::Text("ped heading: live unset  desired %.4f  writes %llu  %s",
					desired, (unsigned long long)writes, driving ? "DRIVING" : "idle");
			}
		}

		// THE measurement that settles the heading convention, and the reason it is here rather
		// than in a comment: both numbers are the same quantity in the same convention, so if the
		// mapping is `camYaw + PI/2` their difference is CONSTANT as you turn, and if it is
		// `-camYaw + c` the difference moves at twice the rate you do. One glance while turning
		// answers it; four single-sample static checks did not.
		//
		// Take it with "Turn the character with the aim" OFF and simply walking around on foot,
		// so nothing here is writing the heading and the game's own relationship is on show.
		{
			const std::optional<float> camYaw = VCS::ReadAddrFloat(VCS::VCSAddr::CameraYaw);
			const std::optional<u32> playerBase = VCS::ReadAddrU32(VCS::VCSAddr::PlayerBase);
			const std::optional<float> pedAsCamYaw =
				playerBase ? VCS::ReadEntityHeading(*playerBase) : std::nullopt;
			if (camYaw && pedAsCamYaw) {
				const float kPi = 3.14159265358979f;
				float diff = *camYaw - *pedAsCamYaw;
				while (diff > kPi) diff -= 2.0f * kPi;
				while (diff < -kPi) diff += 2.0f * kPi;
				ImGui::Text("convention check: camYaw %.4f   ped-as-camYaw %.4f   diff %+.4f (%+.1f deg)",
					*camYaw, *pedAsCamYaw, diff, diff * 180.0f / kPi);
				ImGui::TextDisabled("turn on foot with the feature OFF: steady diff = camYaw + PI/2, sweeping diff = inverted");
			} else {
				ImGui::TextDisabled("convention check: needs CameraYaw and a live player ped");
			}
		}
		ImGui::Separator();
	}

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
	ImGui::SliderFloat("Vertical gain", &s.verticalGain, 0.25f, 4.0f, "%.2fx");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Vertical sensitivity as a multiple of horizontal, for ordinary look.\n"
			"1.90 is GTA III and Vice City's own ratio: they run vertical at 0.012\n"
			"rad/count against horizontal's 0.00625, behind a single slider.\n"
			"Aiming ignores this and uses Aim pitch gain instead.");
	}
	ImGui::Checkbox("Scale with FOV", &s.scaleByFOV);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Scales the look by FOV/70, so a count covers a constant distance on SCREEN\n"
			"rather than a constant angle. Zooming a scope then slows the mouse instead of\n"
			"multiplying it. re3/reVC do this as FOV/80; 70 is used here because that is what\n"
			"VCS actually runs at, so the sensitivity above keeps its tuned meaning.\n"
			"Direct angle writes only - the reticle path already carries the game's own FOV term.");
	}
	if (s.scaleByFOV) {
		ImGui::SameLine();
		// The scale comes from the camera rather than being recomputed here, so this cannot
		// disagree with what the mouse is actually getting - clamps and fallbacks included.
		const std::optional<float> fov = VCS::ReadAddrFloat(VCS::VCSAddr::CamFOV);
		if (fov) {
			ImGui::TextDisabled("(FOV %.1f -> %.2fx)", *fov, VCS::FOVLookScale());
		} else {
			ImGui::TextDisabled("(FOV unreadable -> %.2fx)", VCS::FOVLookScale());
		}
	}
	ImGui::Checkbox("Invert X", &s.invertX);
	ImGui::SameLine();
	ImGui::Checkbox("Invert Y", &s.invertY);
	ImGui::Checkbox("Return the view to the game", &s.returnLook);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Off never hands the camera back: the view stays exactly where you leave it,\n"
			"indefinitely, and everything below stops applying. On foot that means no drift\n"
			"back behind you; in a vehicle it means no swing back behind the car either.");
	}
	if (!s.returnLook) {
		ImGui::TextDisabled("  the view stays where you leave it - use Q/E in a car to recentre");
	}
	ImGui::BeginDisabled(!s.returnLook);
	ImGui::SliderInt("Hold frames", &s.lookHoldFrames, 5, 300, "%d ticks");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Ticks of FULL authority after the last mouse movement, at ~60Hz.\n"
			"The camera stays exactly where you left it for this long.");
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(%.2f s)", s.lookHoldFrames / 60.0f);
	ImGui::Checkbox("Keep the view until you move", &s.holdUntilMoving);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Standing still, never hand the camera back - it stays where you left it.\n"
			"The handback runs on the first frame you move, which is when the game's own\n"
			"recentring is running and heading for the same place the walk aims at.");
	}
	ImGui::EndDisabled();

	// Deliberately OUTSIDE the disabled block. With the automatic return off these are the only
	// things that still decide anything - the glance is the one remaining way to ask for the
	// default view, and the two rows below shape the walk it runs.
	ImGui::Checkbox("Glance keys recentre the view", &s.recenterOnGlance);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"In a car, Q or E - or both - walk the view back behind the car and then\n"
			"hand the camera over. This is how you ask for the default view when the\n"
			"automatic return is off, and it also lets the game's own glance work: a\n"
			"glance is the game moving its camera, which a held view would paint over.");
	}
	ImGui::SliderInt("Handback frames", &s.lookReleaseFrames, 0, 300, "%d ticks");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"How long the camera takes to walk back, whether the return ran on its own\n"
			"or a glance asked for it. 0 drops it in one frame, which is the snap this\n"
			"exists to remove. The walk steps once per GAME logic frame, so 45 ticks is\n"
			"about 22 steps.");
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(%.2f s)", s.lookReleaseFrames / 60.0f);
	ImGui::Checkbox("Return behind the player", &s.returnBehindPlayer);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"On foot, walk the camera back to behind the character instead of to\n"
			"whatever the game's own camera is holding. Off leaves the view wherever\n"
			"you stopped looking, which is what the game itself does.\n"
			"Vehicles ignore this: that camera already returns behind the car by itself.");
	}
	if (s.returnBehindPlayer) {
		ImGui::Checkbox("  learn the offset", &s.learnFollowOffset);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Measures CameraYaw - PedHeading while you WALK IN A STRAIGHT LINE and the\n"
				"camera is at rest, which is the only moment the game is holding the camera\n"
				"where it wants it. Off uses the derived quarter turn, which lands 15-35 deg\n"
				"out. Watch the readout below: 'live' should settle near 'using'.");
		}
		ImGui::SliderFloat("  trim (deg)", &s.returnBehindTrimDeg, -45.0f, 45.0f, "%.1f");
		float used = 0.0f, live = 0.0f, speed = 0.0f;
		u64 samples = 0;
		VCS::FollowOffsetState(&used, &live, &samples, &speed);
		ImGui::Text("  offset: using %.1f deg   live %.1f deg   %llu samples   speed %.3f",
			used * 57.2957795f, live * 57.2957795f, (unsigned long long)samples, speed);
		if (samples == 0) {
			ImGui::SameLine();
			ImGui::TextDisabled("(walk in a straight line to measure)");
		}
	}
	ImGui::Checkbox("Vertical look in vehicles", &s.pitchInVehicle);
	if (s.pitchInVehicle) {
		ImGui::SliderFloat("  look-up band (rad)", &s.pitchVehicleDown, 0.02f, 1.55f, "%.3f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("How far UP vehicle pitch may look, from the angle you entered at.\n"
			                  "1.37 rad reaches about -85 deg; 0.15 is only about -15 deg.\n"
			                  "Vehicle pitch is spring-controlled and the fight grows with\n"
			                  "displacement: ~0.02 rad/frame at 0.15, ~0.26 at 0.5-0.75. Raise it\n"
			                  "until the camera shudders or slams to the roof, then back off.");
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(%.1f deg)", s.pitchVehicleDown * 57.2958f);
	}
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

	ImGui::Checkbox("Camera aim for sniper / RPG", &s.aimScopedCamera);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Scoped weapons aim by moving the camera directly - the same smooth path as mouse "
			"look, and for a scope it is not just nicer but correct: down the sights, the camera "
			"direction IS the firing direction.\n\n"
			"Which is exactly why the same write fails for every other weapon. In third person "
			"the crosshair is drawn from the camera while the shot comes from the ped, so "
			"steering the camera alone splits them - it looks right and misses.\n\n"
			"Uses the Sensitivity slider at the top, not Aim sensitivity.");
	}

	ImGui::Checkbox("Mouse aim in the passenger seat", &s.driveByMouseAim);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"The passenger drive-by aims with the NUB, and it is a third aim mechanism: no aim "
			"control is held, the context is InVehicle rather than Aiming, and both aim flags "
			"read 0 throughout. So every other gate here concludes 'not aiming', the mouse goes "
			"to the camera as it does while driving, and the gun is left to A and D.\n\n"
			"On, the mouse takes the nub instead, through the same response model free aim uses - "
			"which already had this camera's numbers, since mode 11 was measured with the rest.\n\n"
			"Recognised by the weapon camera and the active camera both being mode 11. Watch the "
			"aim path readout: it says 'drive-by' while this is live.");
	}
	if (s.driveByMouseAim) {
		ImGui::SliderFloat("Drive-by sensitivity", &s.driveBySensitivity,
			0.002f, 0.30f, "%.3f stick/count", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Stick deflection per mouse count in the passenger seat. Proportional, not "
				"modelled - measured across 551 samples of a real drive-by, BOTH camera aim "
				"increments read exactly 0.000000 the whole time, so the mechanism the response "
				"model inverts is simply not running in this mode.\n\n"
				"That is what made the axes uneven: the model's rate goes as the SQUARE of the "
				"game's axis scale, which is 2.5 on X against 0.5 on Y, so it asked for five "
				"times less deflection sideways than vertically.");
		}
		ImGui::Checkbox("Mouse aim a mounted cannon", &s.cannonMouseAim);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"The fire truck's water cannon, which the game asks you to aim with the analog "
				"stick. A and D already reach its yaw - they are the steering row, and steering "
				"is the stick's X - but nothing drives the stick's Y in a vehicle, because W and "
				"S are the pedals. So the cannon could turn but never rise.\n\n"
				"This takes the Y axis ONLY, and never X, so steering cannot break. Gated on the "
				"vehicle model rather than on any 'is it spraying' flag - IsFreeAiming looked "
				"like one and measured out as a ~15 second timer that runs while you drive.");
		}
		if (s.cannonMouseAim) {
			ImGui::SliderFloat("Cannon elevation sensitivity", &s.cannonSensitivity,
				0.002f, 0.15f, "%.3f stick/count", ImGuiSliderFlags_Logarithmic);
		}
		ImGui::Checkbox("Match drive-by aim axes", &s.driveByMatchAxes);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Divide the game's own 2.5 / 0.5 axis scale back out, so a sideways sweep and a "
				"vertical one cover the same distance per mouse count. One `03E9 2.5 0.5` in the "
				"retail script sets that up - wide horizontally, damped vertically, which suits a "
				"thumbstick and not a mouse.\n\n"
				"Off gives the game's own balance. This assumes the game's consumer is linear in "
				"the scaled axis, which is the one part here that is reasoned rather than "
				"measured - so it is a switch rather than a constant.");
		}
	}
	ImGui::Checkbox("Camera aim for ALL weapons (DISPROVEN - see tooltip)", &s.mouseLookInFreeAim);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Leave this OFF.\n\n"
			"Writes CameraYaw/CameraPitch directly, the path that makes mouse look smooth. It "
			"does move the crosshair, and it is smooth - and the shot goes somewhere else.\n\n"
			"The crosshair is DRAWN from the camera; the shot comes from the ped's own aim "
			"state. The stick normally drives both, which is what keeps them agreeing. Steering "
			"only the camera desynchronises them - worse than doing nothing, because it looks "
			"right and misses.\n\n"
			"Kept visible so the result is not re-discovered the hard way.");
	}
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
				"IMPORTANT: the crosshair and the player read the same analog axis, so the "
				"movement has to come from somewhere else - it is the latched run the entry "
				"sequence establishes before free aim engages. See MoveGateBranch.");
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
		ImGui::SliderFloat("Mouse acceleration", &s.aimAccel, 0.0f, 0.05f, "%.3f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Extra gain for fast mouse movement. 0 is off and the response stays purely "
				"linear.\n\n"
				"This adds back a nonlinearity the model exists to remove, and that is deliberate: "
				"the game's own curve is a signed square nobody chose, which crushes slow movement "
				"and cannot be tuned because it is a shape. This one is chosen and bounded, and it "
				"scales the wanted ROTATION rather than the stick, so it never eats the low end.\n\n"
				"At the default a slow 5-count frame gets 1.06x and a 40-count sweep gets 1.5x.");
		}
		ImGui::SliderFloat("Acceleration cap", &s.aimAccelMax, 1.0f, 5.0f, "%.1fx");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Ceiling on the multiplier. The aim channel saturates - past that, extra gain only "
				"fills the carry, which arrives late and reads as the aim running away rather than "
				"as speed.");
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
		if (s.aimSensitivity > 0.0015f) {
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
	ImGui::Checkbox("Free aim on aim key (L toggles lock-on)", &s.autoFreeAim);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Pulses the game's Free Aim button (d-pad down) after the aim key goes down, so "
			"aiming starts in free aim instead of lock-on.\n\n"
			"L toggles lock-on mode, which skips the pulse, stops the mouse taking the analog "
			"stick, and stops it turning the camera - so WASD moves and the game frames the "
			"target. That is what melee needs, because melee's lock-on does not register in "
			"IsAiming and free aim would otherwise be assumed.");
	}
	// The state has to be visible. The previous version of this was a HELD key that never once
	// arrived, and nothing on screen would have shown that - it looked correctly bound everywhere
	// anyone would have checked.
	ImGui::Text("Lock-on mode (L):");
	ImGui::SameLine();
	const bool lockOn = VCS::LockOnModeActive();
	ImGui::TextColored(lockOn ? kGoodColor : kUnsetColor, "%s", lockOn ? "ON" : "off");
	ImGui::SameLine();
	ImGui::TextDisabled(lockOn ? "(game aims, WASD moves, mouse idle)" : "(mouse aims)");
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
		ImGui::Text("Accel gain: %.2fx", VCS::AimAccelGain());
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
	const int releaseFrames = VCS::CameraReleaseFrames();
	ImGui::Text("Hold frames left: %d   handback steps left: %d   last context: %s",
		holdFrames, releaseFrames, ctxName);

	// What the game did with the camera on the frames after the last handback let go, as offsets
	// from the player's heading. One frame of movement is a stored value being restored; a slide
	// over several is a spring; no movement means the snap is not in the release at all.
	const float *trace = nullptr;
	int traceCount = 0;
	float traceWritten = 0.0f;
	VCS::ReleaseTrace(&trace, &traceCount, &traceWritten);
	const char *parked = VCS::CameraParkedReason();
	if (parked) {
		ImGui::TextColored(kGoodColor, "Holding the view: %s", parked);
	}
	if (traceCount > 0 && trace) {
		ImGui::Text("After release (deg from heading), handed over %.1f:", traceWritten * 57.2957795f);
		char row[160];
		int used = 0;
		for (int i = 0; i < traceCount && i < 12 && used < (int)sizeof(row) - 1; i++) {
			used += snprintf(row + used, sizeof(row) - used, "%.1f ", trace[i] * 57.2957795f);
		}
		ImGui::Text("  %s", row);
		const float moved = trace[traceCount - 1] - traceWritten;
		ImGui::Text("  moved %.1f deg over %d frames", moved * 57.2957795f, traceCount);
	}
	if (releaseFrames > 0 && yaw) {
		// The gap the walk is actually closing, the short way round. It shrinking to nothing by the
		// last step is the whole fix - and WHICH target it is closing on matters just as much, so
		// both are printed: a walk aimed at the game's own value has nothing to do when the game
		// left our value alone, which is exactly the case behind-the-player exists for.
		bool behindPlayer = false;
		const float target = VCS::CameraHandbackTargetYaw(&behindPlayer);
		float gap = target - *yaw;
		gap = std::fmod(gap + 3.14159265f, 6.28318531f);
		if (gap < 0.0f) gap += 6.28318531f;
		gap -= 3.14159265f;
		ImGui::Text("Handing back to %s: %.1f deg  (%.1f deg to go)",
			behindPlayer ? "behind the player" : "the game's own yaw",
			target * 57.2957795f, gap * 57.2957795f);
	}
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

// Vaulting, and the geometry query underneath it.
//
// The numbers here are the whole point of the tab. A ledge either registers or it doesn't, and
// standing in front of a wall watching the four heights is the only way to tell which of the two
// dozen reasons applies - the answer never arrived, the footing was never found, the wall is
// outside the band, or the far side has nothing to stand on.
// The game's own pause menu, and the two numbers the map and load rows are waiting on.
//
// This tab exists to be READ, once. Open the game's menu (the MAP row does it, or Start on a pad
// with this fork's scheme off), tab across to the map and to the page that holds LOAD GAME, and
// write down what `Page` says at each. Those are `mapPage` and `loadPage`, and until they are set
// the two menu rows open the game's menu and stop there.
//
// The live `Page` row is also the only way to tell the two failure modes apart: a page that never
// moves while tabbing means the tab control is wrong, and a page that moves but never arrives
// means the target number is.
void ImVCSWindow::DrawFrontEnd() {
	VCS::VCSFrontEndSettings &s = VCS::FrontEndSettings();

	const bool active = VCS::GameMenuActive();
	ImGui::Text("Menu active : %s", active ? "YES" : "no");
	ImGui::Text("Page        : %d", VCS::GameMenuPage());
	ImGui::Text("Bridge      : %s%s", VCS::FrontEndStatus(),
		VCS::FrontEndDriving() ? "  (driving the pad)" : "");
	ImGui::Text("Mask        : 0x%08x", VCS::FrontEndButtonMask());
	// Zero while the menu is up means the tree walk matched nothing, which is what a build whose
	// widget names differ would look like - as opposed to the setting simply being off.
	ImGui::Text("Chrome hidden: %d widget(s)", VCS::HiddenChromeCount());
	{
		const std::optional<u32> wpOn = VCS::ReadAddrAsU32(VCS::VCSAddr::WaypointActive);
		const std::optional<float> wx = VCS::ReadAddrFloat(VCS::VCSAddr::WaypointX);
		const std::optional<float> wy = VCS::ReadAddrFloat(VCS::VCSAddr::WaypointY);
		if (wpOn && *wpOn && wx && wy) {
			ImGui::Text("Waypoint    : %.1f, %.1f", *wx, *wy);
		} else {
			ImGui::Text("Waypoint    : none");
		}
	}
	ImGui::Text("Wheel notches: %d   (zoom mask 0x%08x)",
		VCS::WheelNotchesSeen(), VCS::MapZoomButtonMask());
	ImGui::Text("Page has items: %s", VCS::MenuPageHasItems() ? "yes (up/down select)"
		: "no (up/down locked)");
	ImGui::Text("Focus       : %s", VCS::MenuOnTabStrip() ? "tab strip" : "inside the page");

	ImGui::Separator();
	ImGui::TextWrapped("Tabs are a grid: map(0) brief(1) game(2) stats(3) controls(4) on the top "
		"row, audio(5) display(6) multiplayer(7) below. D-pad right cycles within a row, up/down "
		"switches rows. Page reads -1 while the menu is closed.");
	ImGui::Separator();

	ImGui::InputInt("Map page", &s.mapPage);
	ImGui::InputInt("Brief page", &s.briefPage);
	ImGui::InputInt("Game page", &s.gamePage);
	ImGui::InputInt("Stats page", &s.statsPage);
	ImGui::Checkbox("Hide the game's menu chrome", &s.hideMenuChrome);
	ImGui::Checkbox("Fill the backdrop past the screen", &s.fillMenuBackdrop);
	ImGui::InputInt("Backdrop height", &s.backdropHeight);
	ImGui::Checkbox("Lock tab switching (arrows)", &s.lockMenuTabs);
	ImGui::Checkbox("Hide pages while walking", &s.hidePagesWhileWalking);
	ImGui::Checkbox("Jump straight to the page (FREEZES - see header)", &s.jumpDirectlyToPage);
	ImGui::Checkbox("Enter the page on arrival (taps Cross)", &s.enterPageOnArrival);
	ImGui::InputInt("Enter frames", &s.enterFrames);
	ImGui::Checkbox("Drag the map with the mouse", &s.mapDrag);
	ImGui::InputFloat("Map drag speed", &s.mapDragSpeed);
	ImGui::Checkbox("Zoom the map with the wheel", &s.mapZoomWithWheel);
	ImGui::InputInt("Zoom press frames", &s.zoomInFrames);
	ImGui::Checkbox("Place a marker (Space, or a click)", &s.mapWaypoint);
	ImGui::InputFloat("Click slop", &s.waypointClickSlop);
	ImGui::Checkbox("Extra Cross on the first map open", &s.extraCrossOnFirstMap);
	ImGui::InputInt("Hold frames", &s.holdFrames);
	ImGui::InputInt("Gap frames", &s.gapFrames);
	ImGui::InputInt("Open timeout frames", &s.openTimeoutFrames);
	ImGui::InputInt("Max tab presses", &s.maxTabPresses);

	ImGui::Separator();
	if (ImGui::Button("Open menu")) {
		VCS::RequestGameMenu(VCS::FrontEndTarget::Menu);
	}
	ImGui::SameLine();
	if (ImGui::Button("Go to map")) {
		VCS::RequestGameMenu(VCS::FrontEndTarget::Map);
	}
	ImGui::SameLine();
	if (ImGui::Button("Go to game")) {
		VCS::RequestGameMenu(VCS::FrontEndTarget::Game);
	}
	ImGui::SameLine();
	// No menu row asks for this - saving is done at a safe house's save icon. The button stays
	// because SaveMenuRequest is a real, documented capability and this is the only thing that
	// exercises it.
	if (ImGui::Button("Save menu")) {
		VCS::RequestSaveMenu();
	}
}

void ImVCSWindow::DrawVault() {
	VCS::VCSVaultSettings &s = VCS::VaultSettings();
	const VCS::VCSVaultDebug &d = VCS::VaultDebugState();

	ImGui::Checkbox("Vaulting", &s.enabled);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Pull up onto a ledge, or hop over a fence, with the jump key when there is something "
			"in front of the player to get over.\n\n"
			"The motion is the game's own climb-out, animation and all. A written position is the "
			"fallback for the cases the climb cannot take.");
	}

	// The query first, because everything below it is meaningless if this is not running.
	u64 requests = 0, dispatches = 0, answers = 0, abandoned = 0;
	u32 block = 0;
	const char *host = nullptr;
	VCS::WorldQueryStats(&requests, &dispatches, &answers, &abandoned, &block, &host);
	ImGui::Text("World query:");
	ImGui::SameLine();
	if (!VCS::WorldQueryReady()) {
		ImGui::TextColored(kBadColor, "not installed - %s", VCS::WorldQueryStatus());
	} else {
		ImGui::TextColored(kGoodColor, "%08x", block);
		ImGui::SameLine();
		// Answers trailing requests by more than the one in flight is the failure this whole path
		// is most likely to have: the call is being enqueued and never reaching the game.
		const bool healthy = requests - answers <= 1 && abandoned == 0;
		ImGui::TextColored(healthy ? kUnsetColor : kBadColor,
			"%llu asked / %llu sent / %llu answered%s",
			(unsigned long long)requests, (unsigned long long)dispatches,
			(unsigned long long)answers, abandoned ? " (some abandoned)" : "");
		// Which syscall carried it. Asked climbing while sent stays at zero means no host fired -
		// either the game does not call these, or the main thread was never identified.
		ImGui::TextDisabled("carried by %s", host ? host : "nothing yet");
	}

	ImGui::Separator();

	ImGui::Text("Footing:");
	ImGui::SameLine();
	if (d.haveFooting) {
		ImGui::TextColored(kGoodColor, "%.2f", d.footingZ);
		ImGui::SameLine();
		// The gap between the two is the ped origin's height above the surface. Nothing depends on
		// it - every height here is measured against the footing precisely so it doesn't - but it
		// is worth being able to see, because it is the constant a naive version would have
		// guessed at.
		ImGui::TextDisabled("(ped Z %.2f, origin sits %.2f above)", d.playerZ,
			d.playerZ - d.footingZ);
	} else {
		ImGui::TextColored(kBadColor, "none");
	}

	// The wall lines near to far, then the landing. Numbered rather than named near/mid/far: there
	// are six of them now - see the note in VCSVault.h on why a fence needs that many - and there
	// are not six words for it.
	for (int i = 0; i <= VCS::kVaultWallSamples; i++) {
		const bool landing = i == VCS::kVaultWallSamples;
		if (landing) {
			ImGui::Text("%-8s", "landing");
		} else {
			ImGui::Text("wall %-3d", i + 1);
		}
		ImGui::SameLine();
		if (!d.found[i]) {
			ImGui::TextColored(kUnsetColor, "nothing below");
			continue;
		}
		const bool inBand = d.heights[i] >= s.minHeight && d.heights[i] <= s.maxHeight;
		ImGui::TextColored(!landing && inBand ? kGoodColor : kUnsetColor, "%+.2f", d.heights[i]);
	}

	ImGui::Separator();

	ImGui::Text("Ledge:");
	ImGui::SameLine();
	if (d.armed) {
		// Which move it is matters as much as the fact that one armed: "over" and "onto" fail in
		// completely different ways, and only one of the two can be animated by the game.
		ImGui::TextColored(kGoodColor, "armed - %s %+.2f at %.2f ahead",
			d.kind == VCS::VaultKind::Over ? "over" : "onto", d.targetHeight, d.targetDistance);
	} else {
		ImGui::TextColored(kUnsetColor, "%s", d.reject ? d.reject : "no answer yet");
	}

	ImGui::Text("Phase:");
	ImGui::SameLine();
	switch (d.phase) {
	case VCS::VaultPhase::AskingGame:
		ImGui::TextColored(kGoodColor, "asking the game (%d)", d.phaseTicks);
		break;
	case VCS::VaultPhase::Climbing:
		ImGui::TextColored(kGoodColor, "the game is climbing (%d)", d.phaseTicks);
		break;
	case VCS::VaultPhase::Rising:
		ImGui::TextColored(kGoodColor, "rising (%d)", d.phaseTicks);
		break;
	case VCS::VaultPhase::Stepping:
		ImGui::TextColored(kGoodColor, "stepping (%d)", d.phaseTicks);
		break;
	default:
		ImGui::TextColored(kUnsetColor, "idle");
		break;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%llu vaulted", (unsigned long long)d.vaults);

	// Where the animation went, broken out rather than left as "vaulted against animated". That
	// pair could not tell the two failures apart - a vault that never asked the game (a hop over a
	// fence, or preferNative off) and one it asked and was refused both read as an unanimated
	// vault, and they need opposite fixes. Asked-but-never-animated says the band is arming walls
	// the game's own search won't take; never-asked says nothing is even reaching it.
	const unsigned long long vaults = (unsigned long long)d.vaults;
	const unsigned long long own = (unsigned long long)d.nativeClimbs;
	const unsigned long long ran = (unsigned long long)d.nativeForced;
	const unsigned long long stillborn = (unsigned long long)d.nativeStillborn;
	// A forced climb that engaged is every bit as animated as one the game chose - the animation is
	// the game's own either way, only the decision differs. One that never engaged is not, and is
	// counted below with the other fallbacks rather than here.
	const unsigned long long forced = ran > stillborn ? ran - stillborn : 0;
	const unsigned long long animated = own + forced;
	const unsigned long long declined = (unsigned long long)d.nativeDeclines;
	const unsigned long long silent = (unsigned long long)d.nativeSilent;
	const unsigned long long never =
		d.vaults > d.nativeAttempts ? (unsigned long long)(d.vaults - d.nativeAttempts) : 0;

	ImGui::Text("Animation:");
	ImGui::SameLine();
	ImGui::TextColored(vaults > 0 && animated == vaults ? kGoodColor
		: (animated > 0 ? kUnsetColor : kBadColor), "%llu of %llu vaults", animated, vaults);
	ImGui::SameLine();
	ImGui::TextDisabled("(%llu the game's own, %llu forced past a decline)", own, forced);

	// Where the rest went. Every one of these fell back to the written motion, and each wants a
	// different fix, which is why they are four numbers rather than one.
	if (animated < vaults) {
		const unsigned long long refused = declined > ran ? declined - ran : 0;
		ImGui::Text("Fell back:");
		ImGui::SameLine();
		ImGui::TextDisabled("%llu refused, %llu never asked, %llu unanswered, %llu never engaged",
			refused, never, silent, stillborn);
	}

	// The climb-out is the game's SWIMMING one, and it splashes partway through. This counts the
	// ones that were talked out of it - see "THE SPLASH" in Core/VCS/VCSVault.cpp. Zero while
	// vaults are animating means the hook never installed, which is a different thing entirely
	// from a hook with nothing to do.
	ImGui::Text("Splashes silenced:");
	ImGui::SameLine();
	ImGui::TextColored(d.splashesSilenced > 0 ? kGoodColor : kUnsetColor, "%llu",
		(unsigned long long)d.splashesSilenced);

	// And why the LAST one went the way it did. Sticky, unlike the reject line above - that one is
	// rewritten by the next probe a frame after the vault ends, which used to put this answer out
	// of reach at exactly the moment it was wanted.
	ImGui::Text("Last vault:");
	ImGui::SameLine();
	ImGui::TextColored(kUnsetColor, "%s", d.lastNative ? d.lastNative : "none yet");

	ImGui::Separator();

	ImGui::SliderFloat("Min height", &s.minHeight, 0.20f, 5.00f, "%.2f");
	ImGui::SliderFloat("Max height", &s.maxHeight, 0.50f, 5.00f, "%.2f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"The pull-up band, measured from the surface the player is standing on rather than "
			"from the ped's own position - which sits somewhere around the hips, at an offset "
			"nobody has measured.");
	}
	ImGui::SliderFloat("Probe ceiling", &s.probeCeiling, 1.00f, 8.00f, "%.2f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"How high above the footing the probe lines start. Also the hard ceiling on what can "
			"be climbed: the query only reports surfaces BELOW its start point, so a wall taller "
			"than this does not register at all - keep it above Max height, or the band silently "
			"stops at this number instead.");
	}
	ImGui::SliderFloat("Reach near", &s.reachNear, 0.10f, 1.50f, "%.2f");
	ImGui::SliderFloat("Reach far", &s.reachFar, 0.30f, 2.50f, "%.2f");
	ImGui::SliderFloat("Landing depth", &s.landingDepth, 0.20f, 2.00f, "%.2f");
	ImGui::Checkbox("Require somewhere to land", &s.requireLanding);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Refuse ledges with nothing behind them, and parapets with a real drop on the far "
			"side. Turning this off allows narrow ledges and the landings that go with them - and "
			"everything it lets through is treated as a pull-up onto the top.");
	}
	ImGui::SliderFloat("Landing tolerance", &s.landingTolerance, 0.05f, 1.00f, "%.2f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"How close the far side has to be to the ledge top to count as the same surface - "
			"that is, as a pull-up ONTO it rather than a hop OVER it.");
	}
	ImGui::SliderFloat("Landing drop", &s.landingDrop, 0.00f, 4.00f, "%.2f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"How far below the player's own footing the far side may be and still be hopped down "
			"onto. This is what separates a fence from a parapet: behind a fence is the ground you "
			"were already on, behind a roof edge is the street. Zero refuses every hop over and "
			"leaves only pull-ups.");
	}

	ImGui::SliderInt("Rise ticks", &s.riseTicks, 4, 60);
	ImGui::SliderInt("Step ticks", &s.stepTicks, 2, 40);
	ImGui::SliderFloat("Rise forward share", &s.riseForwardFraction, 0.0f, 0.8f, "%.2f");
	ImGui::SliderFloat("Clearance", &s.clearance, 0.0f, 0.50f, "%.2f");

	ImGui::Checkbox("Prefer the game's own climb", &s.preferNative);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Ask the game to climb, rather than moving the player by hand - which is what gets the "
			"animation, because it is the game's own swimming climb-out.\n\n"
			"It runs the game's own ledge search first, so it can decline a wall this probe was "
			"happy with; the written motion is the fallback when it does.");
	}

	ImGui::Checkbox("Force the climb when the game declines", &s.forceNativeClimb);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"CanClimb only fills a struct - found, entity, target - and StartClimb never asks "
			"where that struct came from. This fills it in when the game's own search says no, "
			"and calls the climb anyway. The animation is still entirely the game's; only the "
			"decision is taken away from it.\n\n"
			"Without this the game refuses roughly four walls in five and they slide up with no "
			"animation. Turn it off to see the game's own judgement on its own.\n\n"
			"With it on, fences are asked for too - we choose the target, so it points at the "
			"far side instead of the rail.");
	}
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
		if (ImGui::BeginTabItem("Front End")) {
			DrawFrontEnd();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Vault")) {
			DrawVault();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
}
