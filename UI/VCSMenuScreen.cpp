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
#include <map>
#include <string>

#include "Common/Data/Color/RGBAUtil.h"
#include "Common/Log.h"
#include "Common/Data/Text/I18n.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/Render/Text/Font.h"
#include "Common/System/Display.h"
#include "Common/UI/Context.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/Root.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSSettings.h"
#include "UI/GameSettingsScreen.h"
#include "Common/File/FileUtil.h"
#include "Common/System/Request.h"
#include "UI/EmuScreen.h"
#include "UI/MainScreen.h"
#include "UI/PauseScreen.h"
#include "UI/VCSMenuScreen.h"

// The front end's palette. Unlike the pause menu of VC on PC - which is pink on pink with a
// slanted highlight behind the selected row - this one carries the selection in the text colour
// alone: everything is cyan, the selected row is cream, and there is no highlight shape at all.
static const uint32_t kItemColor = COLOR(0x3FDFDB);          // cyan, every unselected row
static const uint32_t kItemSelectedColor = COLOR(0xF7E9A6);  // cream, the selected row
static const uint32_t kSliderEmptyColor = COLOR(0x1B5982);   // unfilled slider blocks
static const uint32_t kHintColor = COLOR(0xE8E8E8);
static const uint32_t kBarColor = 0xD0000000;

// The background, flat. It was a generated backdrop - a gradient with palm fronds over it - and
// this is that image's own base colour, the midpoint of its BG_TOP/BG_BOTTOM gradient, so the
// menu sits on the same purple it always did with nothing drawn on top of it.
// Tools/vcsmenuart.py can still produce the patterned one; nothing loads it any more.
static const uint32_t kBackgroundColor = COLOR(0x3E0B4E);

// Layout, in PPSSPP's dp units. Rows are full screen width so that "centred" means centred on
// the screen, which is what this layout is built around.
static constexpr float kRowHeight = 76.0f;
static constexpr float kTitleLeft = 58.0f;
static constexpr float kTitleTop = 24.0f;
static constexpr float kTitleHeight = 104.0f;
static constexpr float kBottomBarHeight = 38.0f;

// Menu text is set at a real font size rather than by scaling a theme font up: the text drawer
// rasterises at the style's size and a scale multiplier only stretches those pixels, which at
// the size this layout wants is the difference between crisp and mushy.
static const FontStyle kItemFont(FontFamily::Display, 38, FontStyleFlags::Default);

// Gap from the screen centre to the start of the value column. Labels are right-aligned to the
// centre, values left-aligned this far past it, which is what gives settings pages their
// two-column look while plain action rows stay simply centred.
static constexpr float kValueGap = 100.0f;

static constexpr int kSliderBlocks = 10;

// The controls listing is a table rather than a stack of big centred labels, so it has its own
// metrics and its own face. Pricedown is a display font - it sets four words across a screen
// beautifully and is unreadable at twenty rows - so the listing uses the UI sans, which is what
// the game does too: its Controls screen is set in a plain condensed face, not in the logo one.
static const FontStyle kListFont(FontFamily::SansSerif, 25, FontStyleFlags::Default);
static constexpr float kListRowHeight = 40.0f;
static constexpr float kListTop = 150.0f;      // clear of the title art, which is fixed dp
static constexpr float kListPadding = 22.0f;
// The band above the rows, naming the columns. Its own height rather than the panel's padding,
// because the panel has to grow by exactly this much to make room for it.
static constexpr float kListHeaderHeight = 46.0f;

// Column positions as fractions of the screen width, because that is what this layout aligns
// to - the name column on the left, then up to three key columns evenly spaced.
static constexpr float kListPanelInsetFrac = 0.023f;
static constexpr float kListNameFrac = 0.047f;
static constexpr float kListKeyFrac = 0.315f;
static constexpr float kListKeyStepFrac = 0.088f;
static constexpr size_t kListMaxKeys = 3;

static const uint32_t kListPanelColor = 0x38C8B48D;      // translucent, lifts the list off the art
static const uint32_t kListNameColor = COLOR(0x9BA6BE);  // muted, the way the original greys labels
static const uint32_t kListKeyColor = COLOR(0xF0F2F8);
static const uint32_t kListRowColor = 0x66E8CBAE;        // the bar across the selected row
static const uint32_t kListHeaderColor = COLOR(0xC9B48D);
static const uint32_t kListRuleColor = 0x40FFFFFF;       // hairline under the column names

// Smaller than the rows it labels, so the header reads as a caption rather than as another
// entry in the table.
static const FontStyle kListHeaderFont(FontFamily::SansSerif, 20, FontStyleFlags::Default);

// The one thing a controller player is owed in writing, and the reason it is a fixed line rather
// than a row's help text: it is true of the whole card, not of whichever row has focus.
//
// It is also not a hedge. The pad bindings listed are the game's own and work exactly as they do
// on a PSP; what a pad cannot reach is everything this fork built on top of them - mouse look,
// free aim, the lock-on toggle, the vault. "Less polished" is the difference between the game's
// scheme and the scheme this port was made for.
static const char *const kControllerNote =
	"Controls on a controller are less polished than on mouse and keyboard.";

// Centre of the screen, in the same dp space as a view's bounds. Deliberately not
// bounds_.centerX(): a row's own bounds are only the screen's width if the layout gave it every
// pixel, and when that assumption quietly failed the whole menu sat off-centre. The screen's
// midpoint is what this design is actually built around, so ask for it directly.
static float ScreenCenterX() {
	return g_display.dp_xres * 0.5f;
}

// Textures live for the life of the screen. They are loaded on the first draw because that is
// the first time a Draw::DrawContext is in hand.
struct VCSMenuArt {
	~VCSMenuArt() {
		Release();
	}

	void Release() {
		for (auto &pair : titles) {
			if (pair.second) {
				pair.second->Release();
			}
		}
		titles.clear();
	}

	static Draw::Texture *Load(UIContext &dc, const char *path) {
		size_t size = 0;
		uint8_t *data = g_VFS.ReadFile(path, &size);
		if (!data) {
			return nullptr;
		}
		Draw::Texture *tex = CreateTextureFromFileData(dc.GetDrawContext(), data, size,
			ImageFileType::DETECT, false, path);
		delete[] data;
		return tex;
	}

	// Anything in assets/vcs, loaded once and kept until the device goes.
	//
	// A miss is cached as nullptr on purpose: without that, a missing file would be re-read from
	// the VFS on every single frame. Which is also why there is no separate "did we try" flag -
	// the map entry IS the flag, and Release() clearing the map clears both together. An earlier
	// version of this screen kept them apart and the backdrop appeared exactly once per run.
	Draw::Texture *Image(UIContext &dc, const char *path, const char *key) {
		// Between deviceLost and deviceRestored there is no device to make a texture on, and
		// this function's whole job is to make one whenever the map does not have it. Release()
		// empties that map, so without this the first draw after a device loss would go
		// straight back to CreateTextureFromFileData on a dead device.
		if (deviceLost) {
			return nullptr;
		}
		auto iter = titles.find(key);
		if (iter != titles.end()) {
			return iter->second;
		}
		Draw::Texture *tex = Load(dc, path);
		titles[key] = tex;
		return tex;
	}

	// Page titles are art, not text - see Tools/vcsmenuart.py for why.
	Draw::Texture *Title(UIContext &dc, const char *key) {
		char path[128];
		snprintf(path, sizeof(path), "vcs/title_%s.png", key);
		return Image(dc, path, key);
	}

	Draw::Texture *Background(UIContext &dc) {
		return Image(dc, "vcs/background.png", "#background");
	}

	std::map<std::string, Draw::Texture *> titles;
	bool deviceLost = false;
};

static void DrawTexture(UIContext &dc, Draw::Texture *tex, const Bounds &bounds, uint32_t color,
                        float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f) {
	dc.Flush();
	dc.Begin();
	dc.GetDrawContext()->BindTexture(0, tex);
	dc.Draw()->DrawTexRect(bounds, u0, v0, u1, v1, color);
	dc.Flush();
	dc.RebindTexture();
}

// Fill `bounds` with the image, keeping its proportions and cropping the overflow - what CSS calls
// "cover". Stretching instead would be one line shorter and would distort the art on any window
// that is not the image's own aspect, which on a resizable desktop window is most of them.
static void DrawCover(UIContext &dc, Draw::Texture *tex, const Bounds &bounds) {
	const float texW = (float)tex->Width();
	const float texH = (float)tex->Height();
	if (texW <= 0.0f || texH <= 0.0f || bounds.h <= 0.0f) {
		return;
	}
	const float want = bounds.w / bounds.h;
	const float have = texW / texH;
	float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	if (have > want) {
		// Image is wider than the window: keep a centred slice of its width.
		const float span = want / have;
		u0 = (1.0f - span) * 0.5f;
		u1 = u0 + span;
	} else {
		const float span = have / want;
		v0 = (1.0f - span) * 0.5f;
		v1 = v0 + span;
	}
	DrawTexture(dc, tex, bounds, 0xFFFFFFFF, u0, v0, u1, v1);
}

// A value shown as a row of blocks rather than a number, which is how this front end renders
// anything that is really "a position within a range".
static bool ShowsBlocks(const VCS::Option &opt) {
	return (opt.type == VCS::OptionType::Float || opt.type == VCS::OptionType::Int)
		&& !opt.format;
}

VCSMenuItem::VCSMenuItem(std::string_view label, UI::LayoutParams *layoutParams)
	: UI::ClickableItem(layoutParams), label_(label) {}

VCSMenuItem::VCSMenuItem(const VCS::Option *option, UI::LayoutParams *layoutParams)
	: UI::ClickableItem(layoutParams), label_(option->label), option_(option) {}

std::string_view VCSMenuItem::help() const {
	return option_ ? option_->help : "";
}

void VCSMenuItem::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 100.0f;  // The row fills the screen; the text inside it is placed from the centre.
	h = kRowHeight;
}

// Where the blocks sit: immediately past the value column's left edge.
static Bounds BlockStrip(const Bounds &row) {
	const float blockH = row.h * 0.34f;
	const float blockW = blockH;
	const float gap = blockW * 0.28f;
	const float width = kSliderBlocks * blockW + (kSliderBlocks - 1) * gap;
	return Bounds(ScreenCenterX() + kValueGap, row.centerY() - blockH * 0.5f, width, blockH);
}

float VCSMenuItem::ValueFractionAt(float x) const {
	const Bounds strip = BlockStrip(bounds_);
	if (strip.w <= 0.0f) {
		return 0.0f;
	}
	return std::clamp((x - strip.x) / strip.w, 0.0f, 1.0f);
}

void VCSMenuItem::Adjust(int direction) {
	if (!option_) {
		return;
	}
	switch (option_->type) {
	case VCS::OptionType::Bool:
		*option_->boolValue = !*option_->boolValue;
		break;
	case VCS::OptionType::Float:
		// One block per press for the block-rendered values, so the readout and the control
		// agree: a press should visibly move exactly one square.
		VCS::SetNormalized(*option_, VCS::GetNormalized(*option_)
			+ (1.0f / kSliderBlocks) * (float)direction);
		break;
	case VCS::OptionType::Int:
		// One step per press, which for a value drawn as blocks is also one block.
		VCS::SetInt(*option_, *option_->intValue
			+ (option_->stepInt > 0 ? option_->stepInt : 1) * direction);
		break;
	case VCS::OptionType::Choice:
		// SetInt clamps to the choice list and tells the GPU to resize if this was the
		// resolution; most settings have nothing to do on a change.
		VCS::SetInt(*option_, *option_->intValue + direction);
		break;
	}
}

bool VCSMenuItem::Key(const KeyInput &input) {
	if (option_ && IsEnabled() && HasFocus() && (input.flags & KeyInputFlags::DOWN)) {
		switch (input.keyCode) {
		case NKCODE_DPAD_LEFT:
			Adjust(-1);
			return true;
		case NKCODE_DPAD_RIGHT:
			Adjust(1);
			return true;
		default:
			break;
		}
	}
	return UI::ClickableItem::Key(input);
}

Bounds VCSMenuItem::HitBounds() const {
	return hitBoundsValid_ ? hitBounds_ : bounds_;
}

// Take focus on a press, and take it *forced*, which is what makes clicking work at all.
//
// Clickable::Touch claims focus unforced, and gets away with it because it also claims only
// while IsFocusMovementEnabled(): the first mouse press turns focus movement off, and from then
// on there is no focused view for TouchEvent's EnableFocusMovement(false) to unfocus. These rows
// take focus on hover, so there is always one - it would be unfocused on every press, and
// Clickable::FocusChanged clears down_ and dragging_ with it, so the release found nothing to
// click and the whole menu ignored the mouse. `force` suppresses that call, and is there for
// this exact case: TextEdit uses it to keep the focus it takes when you click into a field.
void VCSMenuItem::ClaimFocus() {
	UI::SetFocusedView(this, UI::FocusFlags::CAUSE_FORCED, true);
}

bool VCSMenuItem::Touch(const TouchInput &input) {
	const Bounds hit = HitBounds();
	const bool contains = hit.Contains(input.x, input.y);

	// Hovering moves the selection. This is the one piece of reVC's input handling that really
	// does have to be reimplemented: PPSSPP's UI moves focus on click and on keyboard or pad
	// navigation, but has no notion of hover, and a menu where the mouse can't highlight a row
	// reads as broken on a PC. reVC hit-tests the pointer against every row for the same reason.
	//
	// Only on a buttonless move, so it can't fire in the middle of dragging a value.
	if ((input.flags & TouchInputFlags::MOVE) && input.buttons == 0 && contains && !HasFocus()) {
		UI::SetFocusedView(this, UI::FocusFlags::CAUSE_OTHER);
	}

	// Dragging along the block strip sets the value directly. Checked for enablement here as
	// well as in the press handling below, because this branch comes first and would otherwise
	// let the pointer drag a value the keyboard cannot reach.
	if (option_ && IsEnabled() && ShowsBlocks(*option_)) {
		const Bounds strip = BlockStrip(bounds_).Expand(0.0f, 14.0f);
		if ((input.flags & TouchInputFlags::DOWN) && strip.Contains(input.x, input.y)) {
			draggingValue_ = true;
			ClaimFocus();
		}
		if (draggingValue_) {
			if (input.flags & (TouchInputFlags::DOWN | TouchInputFlags::MOVE)) {
				VCS::SetNormalized(*option_, ValueFractionAt(input.x));
			}
			if (input.flags & TouchInputFlags::UP) {
				draggingValue_ = false;
			}
			return true;
		}
	}

	// The press/release handling below is Clickable::Touch, reproduced against `hit` instead of
	// `bounds_`. Delegating to the base is not an option: every one of its containment tests
	// reads bounds_ directly, which for these rows is the full width of the screen.
	if (!IsEnabled()) {
		down_ = false;
		dragging_ = false;
		return contains;
	}

	// Ignore buttons other than the left one.
	if ((input.flags & TouchInputFlags::MOUSE) && (input.buttons & 1) == 0) {
		return contains;
	}

	if (input.flags & TouchInputFlags::DOWN) {
		// Before the flags, not after: SetFocusedView sends LOST_FOCUS to the outgoing view
		// even when that view is this one, and Clickable::FocusChanged clears down_ on it.
		if (contains) {
			ClaimFocus();
		}
		down_ = contains;
		dragging_ = contains;
	} else if (input.flags & TouchInputFlags::MOVE) {
		if (dragging_) {
			down_ = contains;
		}
	}
	if (input.flags & TouchInputFlags::UP) {
		if ((input.flags & TouchInputFlags::CANCEL) == 0 && dragging_ && contains) {
			ClickInternal();
		}
		down_ = false;
		dragging_ = false;
	}
	return contains;
}

void VCSMenuItem::ClickInternal() {
	// Clicking a toggle flips it. Block-rendered values are edited through the strip and the
	// arrow keys, so a click elsewhere on one deliberately does nothing.
	if (option_ && IsEnabled() && option_->type == VCS::OptionType::Bool) {
		*option_->boolValue = !*option_->boolValue;
	}
	UI::ClickableItem::ClickInternal();
}

std::string VCSMenuItem::DescribeText() const {
	if (option_) {
		return label_ + ": " + VCS::ValueText(*option_);
	}
	return label_;
}

void VCSMenuItem::Draw(UIContext &dc) {
	const bool selected = HasFocus() || down_;
	const uint32_t color = !IsEnabled() ? colorAlpha(kItemColor, 0.35f)
	                                    : (selected ? kItemSelectedColor : kItemColor);

	dc.SetFontStyle(kItemFont);

	// A little slack around the glyphs, so the pointer does not have to be dead on a letter.
	const float slack = 14.0f;
	float labelW = 0.0f, labelH = 0.0f;
	dc.MeasureText(kItemFont, 1.0f, 1.0f, label_, &labelW, &labelH);

	if (!option_) {
		// An action row - RESUME GAME, BACK - is centred on the screen with nothing beside it.
		dc.DrawTextShadow(label_, ScreenCenterX(), bounds_.centerY(), color,
			ALIGN_VCENTER | ALIGN_HCENTER);
		hitBounds_ = Bounds(ScreenCenterX() - labelW * 0.5f - slack, bounds_.y,
			labelW + slack * 2.0f, bounds_.h);
		hitBoundsValid_ = true;
		return;
	}

	// A setting row is two columns: the label right-aligned into the centre, the value starting
	// a fixed gap past it, so every value on the page lines up regardless of label length.
	dc.DrawTextShadow(label_ + ":", ScreenCenterX(), bounds_.centerY(), color,
		ALIGN_VCENTER | ALIGN_RIGHT);

	if (ShowsBlocks(*option_)) {
		const Bounds strip = BlockStrip(bounds_);
		const float blockH = strip.h;
		const float gap = blockH * 0.28f;
		const int filled = (int)(VCS::GetNormalized(*option_) * kSliderBlocks + 0.5f);

		dc.Flush();
		dc.BeginNoTex();
		for (int i = 0; i < kSliderBlocks; i++) {
			const float x = strip.x + i * (blockH + gap);
			dc.Draw()->Rect(x, strip.y, blockH, blockH, i < filled ? color : kSliderEmptyColor);
		}
		dc.Flush();
		dc.Begin();
	} else {
		dc.DrawTextShadow(VCS::ValueText(*option_), ScreenCenterX() + kValueGap,
			bounds_.centerY(), color, ALIGN_VCENTER | ALIGN_LEFT);
	}

	// Label sits to the left of the centre line, value to the right of it; the row is the span
	// between them, not the width of the screen.
	float valueW = 0.0f, valueH = 0.0f;
	if (ShowsBlocks(*option_)) {
		valueW = BlockStrip(bounds_).w;
	} else {
		dc.MeasureText(kItemFont, 1.0f, 1.0f, VCS::ValueText(*option_), &valueW, &valueH);
	}
	const float left = ScreenCenterX() - labelW - slack;
	const float right = ScreenCenterX() + kValueGap + valueW + slack;
	hitBounds_ = Bounds(left, bounds_.y, right - left, bounds_.h);
	hitBoundsValid_ = true;
}

VCSBindingRow::VCSBindingRow(std::string_view name, const std::vector<std::string> &keys,
	UI::LayoutParams *layoutParams)
	: UI::ClickableItem(layoutParams), name_(name), keys_(keys) {}

void VCSBindingRow::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 100.0f;  // The row fills the screen; the columns inside it are placed from its width.
	h = kListRowHeight;
}

bool VCSBindingRow::Touch(const TouchInput &input) {
	const bool contains = bounds_.Contains(input.x, input.y);

	// Hover moves the selection, as everywhere else in this menu. Hit-testing bounds_ directly
	// is right here and wrong for VCSMenuItem: the highlight really is the full width of the
	// panel, so the band the pointer has to be in really is the whole row.
	if ((input.flags & TouchInputFlags::MOVE) && input.buttons == 0 && contains && !HasFocus()) {
		UI::SetFocusedView(this, UI::FocusFlags::CAUSE_OTHER);
	}
	// Forced, so the press does not immediately unfocus the row it just selected - see
	// VCSMenuItem::ClaimFocus for the mechanism. Nothing else happens on a press: there is
	// nothing to activate.
	if ((input.flags & TouchInputFlags::DOWN) && contains) {
		UI::SetFocusedView(this, UI::FocusFlags::CAUSE_FORCED, true);
	}
	return contains;
}

std::string VCSBindingRow::DescribeText() const {
	std::string text = name_;
	for (size_t i = 0; i < keys_.size() && i < kListMaxKeys; i++) {
		text += i == 0 ? ": " : ", ";
		text += keys_[i];
	}
	return text;
}

void VCSBindingRow::Draw(UIContext &dc) {
	if (HasFocus()) {
		// Across the whole panel, not just the text - which is what makes a table read as a
		// table rather than as a list of buttons.
		const float inset = g_display.dp_xres * kListPanelInsetFrac;
		dc.FillRect(UI::Drawable(kListRowColor),
			Bounds(inset, bounds_.y, g_display.dp_xres - inset * 2.0f, bounds_.h));
	}

	dc.SetFontStyle(kListFont);
	dc.DrawTextShadow(name_, g_display.dp_xres * kListNameFrac, bounds_.centerY(),
		kListNameColor, ALIGN_VCENTER | ALIGN_LEFT);

	for (size_t i = 0; i < keys_.size() && i < kListMaxKeys; i++) {
		const float x = g_display.dp_xres * (kListKeyFrac + kListKeyStepFrac * (float)i);
		dc.DrawTextShadow(keys_[i], x, bounds_.centerY(), kListKeyColor,
			ALIGN_VCENTER | ALIGN_LEFT);
	}
}

VCSMenuScreen::VCSMenuScreen(const Path &gamePath, bool bootPending, VCSMenuMode mode)
	: UIBaseDialogScreen(gamePath), bootPending_(bootPending), mode_(mode),
	  art_(new VCSMenuArt()) {}

void VCSMenuScreen::deviceLost() {
	// Before Vulkan goes. Release() clears the map as well as the textures, so the destructor
	// below finds nothing left to free.
	art_->Release();
	art_->deviceLost = true;
	UIBaseDialogScreen::deviceLost();
}

void VCSMenuScreen::deviceRestored(Draw::DrawContext *draw) {
	// There is somewhere to put a texture again; the next draw reloads them lazily.
	art_->deviceLost = false;
	UIBaseDialogScreen::deviceRestored(draw);
}

VCSMenuScreen::~VCSMenuScreen() {
	if (mode_ == VCSMenuMode::Startup) {
		// However it was dismissed - START GAME, Escape, anything - the boot menu is a
		// once-per-run thing and the game carries on from here.
		VCS::NotifyMenuDismissed();
	}
	// Every way out of this menu comes through here, which is why the save lives here rather
	// than on the resume row.
	VCS::SaveSettings();
}

VCSMenuPage VCSMenuScreen::ParentPage(VCSMenuPage page) {
	// The page table's back-link, and the only place the shape of the menu is written down.
	switch (page) {
	case VCSMenuPage::Settings: return VCSMenuPage::Root;
	case VCSMenuPage::Controls: return VCSMenuPage::Settings;
	case VCSMenuPage::Audio: return VCSMenuPage::Settings;
	case VCSMenuPage::Graphics: return VCSMenuPage::Settings;
	case VCSMenuPage::Mouse: return VCSMenuPage::Controls;
	case VCSMenuPage::Controller: return VCSMenuPage::Controls;
	case VCSMenuPage::Aiming: return VCSMenuPage::Controls;
	case VCSMenuPage::Bindings: return VCSMenuPage::Controls;
	case VCSMenuPage::KeysOnFoot: return VCSMenuPage::Bindings;
	case VCSMenuPage::KeysVehicle: return VCSMenuPage::Bindings;
	case VCSMenuPage::KeysAircraft: return VCSMenuPage::Bindings;
	case VCSMenuPage::KeysMelee: return VCSMenuPage::Bindings;
	default: return VCSMenuPage::Root;
	}
}

bool VCSMenuScreen::IsOptionPage(VCSMenuPage page) {
	switch (page) {
	case VCSMenuPage::Mouse:
	case VCSMenuPage::Controller:
	case VCSMenuPage::Aiming:
	case VCSMenuPage::Audio:
	case VCSMenuPage::Graphics:
		return true;
	default:
		return false;
	}
}

bool VCSMenuScreen::IsKeyListPage(VCSMenuPage page) {
	switch (page) {
	case VCSMenuPage::KeysOnFoot:
	case VCSMenuPage::KeysVehicle:
	case VCSMenuPage::KeysAircraft:
	case VCSMenuPage::KeysMelee:
		return true;
	default:
		return false;
	}
}

VCS::VCSKeyList VCSMenuScreen::ToKeyList(VCSMenuPage page) {
	switch (page) {
	case VCSMenuPage::KeysVehicle: return VCS::VCSKeyList::InVehicle;
	case VCSMenuPage::KeysAircraft: return VCS::VCSKeyList::Aircraft;
	case VCSMenuPage::KeysMelee: return VCS::VCSKeyList::Melee;
	default: return VCS::VCSKeyList::OnFoot;
	}
}

VCS::OptionPage VCSMenuScreen::ToOptionPage(VCSMenuPage page) {
	switch (page) {
	case VCSMenuPage::Controller: return VCS::OptionPage::Controller;
	case VCSMenuPage::Aiming: return VCS::OptionPage::Aiming;
	case VCSMenuPage::Audio: return VCS::OptionPage::Audio;
	case VCSMenuPage::Graphics: return VCS::OptionPage::Graphics;
	default: return VCS::OptionPage::Mouse;
	}
}

const char *VCSMenuScreen::PageTitle(VCSMenuPage page) const {
	// These name the art files in assets/vcs/, so they have to match Tools/vcsmenuart.py.
	switch (page) {
	case VCSMenuPage::Settings: return "settings";
	case VCSMenuPage::Controls: return "controls";
	case VCSMenuPage::Mouse: return "mouse";
	// Shares its art with the bindings page on its controller setting, which is right: both
	// pages are about the pad, and both headings say the same word.
	case VCSMenuPage::Controller: return "controller";
	case VCSMenuPage::Aiming: return "aiming";
	case VCSMenuPage::Audio: return "audio";
	case VCSMenuPage::Graphics: return "graphics";
	// The one heading that is not fixed: the page is the same page either way, and what it is a
	// listing OF is the whole of what the switch changes. Both files are generated by
	// Tools/vcsmenuart.py, so both have to be in its TITLES.
	case VCSMenuPage::Bindings:
		return VCS::ListDevice() == VCS::VCSListDevice::Controller ? "controller" : "keyboard";
	case VCSMenuPage::KeysOnFoot: return "onfoot";
	case VCSMenuPage::KeysVehicle: return "invehicle";
	case VCSMenuPage::KeysAircraft: return "aircraft";
	case VCSMenuPage::KeysMelee: return "melee";
	default:
		return mode_ == VCSMenuMode::Pause ? "paused" : "mainmenu";
	}
}

void VCSMenuScreen::GoToPage(VCSMenuPage page) {
	page_ = page;
	RecreateViews();
}

void VCSMenuScreen::ToggleListDevice() {
	const VCS::Option *device = VCS::ListDeviceOption();
	if (!device) {
		return;
	}
	// Through SetInt rather than by assigning the variable, so this and the row on the bindings
	// page cannot disagree about clamping or about what a change means.
	VCS::SetInt(*device, *device->intValue == 0 ? 1 : 0);
	RecreateViews();
}

bool VCSMenuScreen::ShowingControllerNote() const {
	if (VCS::ListDevice() != VCS::VCSListDevice::Controller) {
		return false;
	}
	return page_ == VCSMenuPage::Bindings || IsKeyListPage(page_);
}

bool VCSMenuScreen::key(const KeyInput &key) {
	// Left and right flip the card between the two devices, so they can be compared without
	// walking back up to the switch that owns them - which is the difference between a switch
	// and a detour. Only on a listing page: everywhere else those keys belong to the focused
	// row, and taking them would leave every slider on the settings pages unadjustable.
	if ((key.flags & KeyInputFlags::DOWN) && IsKeyListPage(page_)
		&& (key.keyCode == NKCODE_DPAD_LEFT || key.keyCode == NKCODE_DPAD_RIGHT)) {
		ToggleListDevice();
		return true;
	}

	// Back on a sub-page means "up one level", not "close the menu" - the page table's parent
	// link, which is how reVC expresses this too.
	const bool back = key.keyCode == NKCODE_ESCAPE || key.keyCode == NKCODE_BACK;
	if ((key.flags & KeyInputFlags::DOWN) && back) {
		if (page_ != VCSMenuPage::Root) {
			GoToPage(ParentPage(page_));
			return true;
		}
		// The main menu is the bottom of the stack. Letting the dialog base finish it would pop
		// the last screen and leave the app with nothing to draw.
		if (mode_ == VCSMenuMode::MainMenu) {
			return true;
		}
	}
	return UIBaseDialogScreen::key(key);
}

void VCSMenuScreen::AddPageRow(UI::ViewGroup *parent, const char *label, VCSMenuPage target) {
	using namespace UI;
	VCSMenuItem *row = parent->Add(new VCSMenuItem(label,
		new LinearLayoutParams(FILL_PARENT, kRowHeight)));
	row->OnClick.Add([this, target](UI::EventParams &e) {
		GoToPage(target);
	});
	rows_.push_back(row);
}

void VCSMenuScreen::AddBackRow(UI::ViewGroup *parent) {
	using namespace UI;
	VCSMenuItem *row = parent->Add(new VCSMenuItem("BACK",
		new LinearLayoutParams(FILL_PARENT, kRowHeight)));
	// page_ is read at click time, not captured, so one lambda serves every page.
	row->OnClick.Add([this](UI::EventParams &e) {
		GoToPage(ParentPage(page_));
	});
	rows_.push_back(row);
}

void VCSMenuScreen::AddOptionRows(UI::ViewGroup *parent, VCSMenuPage page) {
	using namespace UI;

	const VCS::OptionPage optionPage = ToOptionPage(page);

	for (const VCS::Option &option : VCS::Options()) {
		if (option.page != optionPage) {
			continue;
		}
		VCSMenuItem *row = parent->Add(new VCSMenuItem(&option,
			new LinearLayoutParams(FILL_PARENT, kRowHeight)));
		if (option.enabledBy) {
			// A pointer rather than a value, so the row follows the switch the moment it is
			// flipped - the two are on the same page and the greying has to happen under the
			// cursor, not on the next visit.
			row->SetEnabledPtr(option.enabledBy);
		}
		rows_.push_back(row);
	}

	// A blank row before the two actions, the way the original separates them from the settings.
	parent->Add(new Spacer(kRowHeight * 0.5f));

	VCSMenuItem *defaults = parent->Add(new VCSMenuItem("RESTORE DEFAULTS",
		new LinearLayoutParams(FILL_PARENT, kRowHeight)));
	defaults->OnClick.Handle(this, &VCSMenuScreen::OnRestoreDefaults);
	rows_.push_back(defaults);

	AddBackRow(parent);
}

void VCSMenuScreen::AddBindingRows(UI::ViewGroup *parent, VCSMenuPage page) {
	using namespace UI;

	const std::vector<VCS::VCSListingRow> listing =
		VCS::KeyListing(ToKeyList(page), VCS::ListDevice());
	for (const VCS::VCSListingRow &row : listing) {
		parent->Add(new VCSBindingRow(row.name, row.keys,
			new LinearLayoutParams(FILL_PARENT, kListRowHeight)));
	}
	listRowCount_ = (int)listing.size();

	// Clear of the panel, which is sized to the rows above.
	parent->Add(new Spacer(kListPadding * 2.0f));
	AddBackRow(parent);
}

Bounds VCSMenuScreen::ListPanel() const {
	const float inset = g_display.dp_xres * kListPanelInsetFrac;
	const float height = kListHeaderHeight + (float)listRowCount_ * kListRowHeight + kListPadding;
	return Bounds(inset, kListTop, g_display.dp_xres - inset * 2.0f, height);
}

void VCSMenuScreen::CreateViews() {
	using namespace UI;

	rows_.clear();
	listRowCount_ = 0;

	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));

	// As a fraction of the screen rather than a fixed offset, so the block stays put when the
	// window is resized and so the longer settings pages start higher without a second constant
	// that has to be kept in step with the row count.
	// The listing starts inside its panel; everything else is a block of large centred rows,
	// higher up on the longer pages so they still fit.
	const float top = IsKeyListPage(page_)
		? kListTop + kListHeaderHeight
		: g_display.dp_yres * (page_ == VCSMenuPage::Root ? 0.30f : 0.17f);
	LinearLayout *list = new LinearLayout(ORIENT_VERTICAL,
		new AnchorLayoutParams(FILL_PARENT, WRAP_CONTENT, 0.0f, top, 0.0f, NONE));
	list->SetSpacing(0.0f);
	root_->Add(list);

	if (page_ == VCSMenuPage::Root) {
		const bool mainMenu = mode_ == VCSMenuMode::MainMenu;
		// Startup sits over a game that is already running, so its top row resumes like the
		// pause menu's does - the game is mid-way into starting and just needs to be let go.
		const char *firstLabel = mainMenu ? "LOAD GAME"
			: (mode_ == VCSMenuMode::Startup ? "START GAME" : "RESUME GAME");
		VCSMenuItem *first = list->Add(new VCSMenuItem(firstLabel,
			new LinearLayoutParams(FILL_PARENT, kRowHeight)));
		if (mainMenu) {
			first->OnClick.Handle(this, &VCSMenuScreen::OnLoadGame);
		} else {
			first->OnClick.Handle(this, &VCSMenuScreen::OnResume);
		}
		rows_.push_back(first);

		AddPageRow(list, "SETTINGS", VCSMenuPage::Settings);

		VCSMenuItem *quit = list->Add(new VCSMenuItem("QUIT GAME",
			new LinearLayoutParams(FILL_PARENT, kRowHeight)));
		if (mainMenu) {
			quit->OnClick.Handle(this, &VCSMenuScreen::OnQuitApp);
		} else {
			quit->OnClick.Handle(this, &VCSMenuScreen::OnExitGame);
		}
		rows_.push_back(quit);
	} else if (page_ == VCSMenuPage::Settings) {
		AddPageRow(list, "CONTROLS", VCSMenuPage::Controls);
		AddPageRow(list, "AUDIO", VCSMenuPage::Audio);
		AddPageRow(list, "GRAPHICS", VCSMenuPage::Graphics);
		AddBackRow(list);
	} else if (page_ == VCSMenuPage::Controls) {
		AddPageRow(list, "MOUSE", VCSMenuPage::Mouse);
		AddPageRow(list, "CONTROLLER", VCSMenuPage::Controller);
		AddPageRow(list, "AIMING", VCSMenuPage::Aiming);

		// Read-only, and that is the whole design: the bindings are context-aware, so one key
		// is several things depending on what you are doing, and a rebinding screen that cannot
		// express that would be lying about what it changed. PPSSPP's own mapper is deliberately
		// not offered here either, for the same reason - it binds keys to PSP buttons, one step
		// below the layer that decides what a PSP button means.
		//
		// Named for what it lists rather than for a device, because it lists two.
		AddPageRow(list, "BINDINGS", VCSMenuPage::Bindings);

		AddBackRow(list);
	} else if (page_ == VCSMenuPage::Bindings) {
		// The switch above the four situations it applies to. An ordinary settings row on a page
		// that is otherwise all page rows, which is the honest shape: it changes a value, and
		// they go somewhere. Nothing on this page has to be rebuilt when it moves - the heading
		// and the caveat are both drawn from the live value every frame - so it edits the option
		// directly, the way every other settings row does.
		if (const VCS::Option *device = VCS::ListDeviceOption()) {
			rows_.push_back(list->Add(new VCSMenuItem(device,
				new LinearLayoutParams(FILL_PARENT, kRowHeight))));
		}
		AddPageRow(list, "ON FOOT", VCSMenuPage::KeysOnFoot);
		AddPageRow(list, "IN VEHICLE", VCSMenuPage::KeysVehicle);
		AddPageRow(list, "AIRCRAFT", VCSMenuPage::KeysAircraft);
		AddPageRow(list, "MELEE COMBAT", VCSMenuPage::KeysMelee);
		AddBackRow(list);
	} else if (IsKeyListPage(page_)) {
		AddBindingRows(list, page_);
	} else if (IsOptionPage(page_)) {
		AddOptionRows(list, page_);
	}

	if (!rows_.empty()) {
		UI::SetFocusedView(rows_[0], UI::FocusFlags::CAUSE_OTHER);
	}
}

void VCSMenuScreen::update() {
	// Tell the rest of the emulator the game is paused. PPSSPP's own pause screen does this every
	// frame and it is not optional: without it the UI state stays UISTATE_INGAME, and the most
	// visible consequence is that the mouse pointer never comes back - CorrectCursor() in
	// Windows/MainWindow.cpp only un-hides it once the state is something other than INGAME, and
	// with mouse control on it also keeps the pointer clipped to the window. The framebuffer
	// manager and the WebSocket game broadcaster read the same state.
	UpdateUIState(UISTATE_PAUSEMENU);
	UIBaseDialogScreen::update();
}

void VCSMenuScreen::DrawBackground(UIContext &dc) {
	const Bounds &bounds = dc.GetBounds();

	// Opaque, not a dim over the game. GTA's front end covers the screen, and the paused world
	// showing through would fight the text.
	//
	// The flat fill goes down first and stays: it is what shows if assets/vcs is not deployed, and
	// it is the artwork's own base colour, so a missing file reads as a plain backdrop rather than
	// as a broken menu. Missing art is never fatal here - same rule the page titles follow.
	dc.FillRect(UI::Drawable(kBackgroundColor), bounds);
	if (Draw::Texture *backdrop = art_->Background(dc)) {
		DrawCover(dc, backdrop, bounds);
	}

	// Page title, top left, in the brush script the game sets its headings in.
	Draw::Texture *title = art_->Title(dc, PageTitle(page_));
	if (title) {
		const float h = kTitleHeight;
		const float w = h * (float)title->Width() / (float)title->Height();
		DrawTexture(dc, title, Bounds(kTitleLeft, kTitleTop, w, h), 0xFFFFFFFF);
	}

	// The frame behind the listing, sized to its rows so it stops where they do - and the
	// column names in the band along its top, which is otherwise dead space above the first row.
	if (IsKeyListPage(page_)) {
		const Bounds panel = ListPanel();
		dc.FillRect(UI::Drawable(kListPanelColor), panel);

		dc.SetFontStyle(kListHeaderFont);
		const float headerY = panel.y + kListHeaderHeight * 0.5f;
		dc.DrawText("ACTION", g_display.dp_xres * kListNameFrac, headerY, kListHeaderColor,
			ALIGN_VCENTER | ALIGN_LEFT);
		// The column names the device rather than saying "BINDING", which makes it the one place
		// a card states what it is a card of. The buttons alone do not say it - CIRCLE and LMB
		// are obvious, D-PAD LEFT and Q less so - and a player who flipped the switch by
		// accident has nothing else to read.
		dc.DrawText(VCS::ListDevice() == VCS::VCSListDevice::Controller ? "CONTROLLER" : "KEYBOARD",
			g_display.dp_xres * kListKeyFrac, headerY, kListHeaderColor,
			ALIGN_VCENTER | ALIGN_LEFT);

		// A hairline rather than a full rule: enough to separate the caption from the data
		// without drawing a second frame inside the first.
		dc.FillRect(UI::Drawable(kListRuleColor),
			Bounds(panel.x, panel.y + kListHeaderHeight - 2.0f, panel.w, 1.0f));
	}

	// The bar along the bottom: what the selected row means on the left, how to work it on the
	// right. The original puts a build stamp on the left instead, but the explanation earns the
	// space better, and it is the one thing that lets an option be named in two words.
	const Bounds bar(bounds.x, bounds.y2() - kBottomBarHeight, bounds.w, kBottomBarHeight);
	dc.FillRect(UI::Drawable(kBarColor), bar);

	const VCSMenuItem *focused = nullptr;
	for (VCSMenuItem *row : rows_) {
		if (row->HasFocus()) {
			focused = row;
			break;
		}
	}

	dc.SetFontStyle(dc.GetTheme().uiFontSmall);

	// The caveat outranks the focused row's own help, and deliberately: it is true of everything
	// on the page rather than of the row under the cursor, so a player is owed it whether or not
	// they happen to be sitting on the switch that turned it on.
	std::string_view note;
	if (ShowingControllerNote()) {
		note = kControllerNote;
	} else if (focused) {
		note = focused->help();
	}
	if (!note.empty()) {
		dc.DrawText(note, bar.x + kTitleLeft, bar.centerY(),
			colorAlpha(kHintColor, 0.85f), ALIGN_VCENTER | ALIGN_LEFT);
	}

	const char *hint = "ENTER / LMB - SELECT     ESC - BACK";
	if (IsKeyListPage(page_)) {
		hint = "LEFT / RIGHT - DEVICE     ESC - BACK";
	} else if (focused && focused->option()) {
		hint = focused->option()->type == VCS::OptionType::Bool
			? "ENTER / LMB - TOGGLE     ESC - BACK"
			: "LEFT / RIGHT - ADJUST     ESC - BACK";
	}
	dc.DrawText(hint, bar.x2() - kTitleLeft, bar.centerY(), kHintColor,
		ALIGN_VCENTER | ALIGN_RIGHT);

	dc.Flush();
}

void VCSMenuScreen::OnResume(UI::EventParams &e) {
	// DR_CANCEL is EmuScreen's "continue" - see its dialogFinished.
	TriggerFinish(DR_CANCEL);
}

void VCSMenuScreen::OnExitGame(UI::EventParams &e) {
	auto di = GetI18NCategory(I18NCat::DIALOG);
	screenManager()->push(new UI::MessagePopupScreen(di->T("Are you sure you want to exit?"),
		"", di->T("Exit"), di->T("Cancel"), [this](bool result) {
			if (result) {
				TriggerFinish(DR_OK);  // DR_OK is "back to the game list".
			}
		}));
}

void VCSMenuScreen::OnLoadGame(UI::EventParams &e) {
	const std::string path = VCS::GamePath();
	if (path.empty()) {
		// Shouldn't happen - CreateStartScreen only builds a main menu when there is a path -
		// but falling back to the browser beats a dead row.
		screenManager()->switchScreen(new MainScreen());
		return;
	}
	screenManager()->switchScreen(new EmuScreen(Path(path)));
}

void VCSMenuScreen::OnQuitApp(UI::EventParams &e) {
	auto di = GetI18NCategory(I18NCat::DIALOG);
	screenManager()->push(new UI::MessagePopupScreen(di->T("Are you sure you want to exit?"),
		"", di->T("Exit"), di->T("Cancel"), [](bool result) {
			if (result) {
				System_ExitApp();
			}
		}));
}

void VCSMenuScreen::OnGameSettings(UI::EventParams &e) {
	screenManager()->push(new GameSettingsScreen(gamePath_));
}

void VCSMenuScreen::OnRestoreDefaults(UI::EventParams &e) {
	VCS::ResetPage(ToOptionPage(page_));
}

UIScreen *CreatePauseScreen(const Path &gamePath, bool bootPending) {
	if (VCS::IsActive()) {
		return new VCSMenuScreen(gamePath, bootPending);
	}
	return new GamePauseScreen(gamePath, bootPending);
}

UIScreen *CreateStartScreen() {
	// Nothing has booted yet, so VCS::Init() has not run and the settings have not been read.
	// This is the only caller that needs them before a game exists.
	VCS::LoadSettings();

	const std::string path = VCS::GamePath();
	if (!path.empty() && File::Exists(Path(path))) {
		// Straight onto the disc, with no menu in front of it. VCS opens with logos and a
		// credits sequence and then walks itself into the story; the menu belongs at the far
		// end of that, which is where EmuScreen raises it. Putting one here as well would mean
		// asking the player to start the game twice.
		return new EmuScreen(Path(path));
	}
	return new MainScreen();
}
