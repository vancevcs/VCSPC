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

#include <memory>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/UI/UIScreen.h"
#include "Common/UI/View.h"
#include "UI/BaseScreens.h"

namespace VCS {
struct Option;
struct VCSListingRow;
// Legal to forward-declare: a scoped enum has a defined underlying type even undefined.
enum class OptionPage;
enum class VCSKeyList;
enum class CheatGroup;
enum class FrontEndTarget;
}

// The pause menu for the VCS front end, in the shape GTA uses: a left-hand column of large
// options, the page name in the top right, the selected row's explanation along the bottom.
//
// It follows reVC's *structure* and none of its drawing. reVC had to write its own hit-testing,
// hover state, keyboard repeat and layout because the game had no UI toolkit; PPSSPP has one,
// with mouse, keyboard and pad navigation already working and already themed. So a row here is a
// UI::ClickableItem subclass that paints itself the way the GTA menu looks - and inherits focus,
// clicks and navigation for free.
//
// What IS taken from reVC is how pages are described: a table of rows, each row either a jump to
// another page or a binding to one setting, with back-navigation expressed as the page's parent
// rather than as code. See Core/VCS/VCSSettings.h for the settings half of that.

enum class VCSMenuPage {
	Root,
	Settings,
	Controls,
	Mouse,
	Controller,
	Aiming,
	Audio,
	Graphics,
	// What this port adds to the game, each row a switch that hands one of them back.
	Gameplay,

	// The read-only controls listing: the device switch and a menu of the four situations, then
	// a page of bindings for each. Nothing on those pages can be edited - they are a reference
	// card, which is what the game's own Controls screen is once you take the rebinding out.
	Bindings,
	KeysOnFoot,
	KeysVehicle,
	KeysAircraft,
	KeysMelee,

	// The cheat menu. One page per group rather than one long list, because there are 36 of them
	// and nothing on this menu scrolls - the same constraint that sizes the bindings cards.
	Cheats,
	CheatsPlayer,
	CheatsVehicles,
	CheatsPedestrians,
	CheatsWorld,
};

// The same screen serves two jobs, because they differ only in what the root page offers and in
// what Back means. Sharing it keeps one page table, one look and one set of option rows.
enum class VCSMenuMode {
	Pause,     // over a running game: RESUME at the top, Back closes and resumes
	MainMenu,  // at startup, nothing loaded: LOAD GAME at the top, Back on the root does nothing
	Startup,   // over the credits seam: START GAME at the top, and it just resumes
};

// One row. Either an action ("RESUME GAME") or a setting bound to a VCS::Option, in which case
// the value sits in the right-hand column and left/right edits it.
class VCSMenuItem : public UI::ClickableItem {
public:
	VCSMenuItem(std::string_view label, UI::LayoutParams *layoutParams = nullptr);
	VCSMenuItem(const VCS::Option *option, UI::LayoutParams *layoutParams = nullptr);

	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;
	bool Key(const KeyInput &input) override;
	bool Touch(const TouchInput &input) override;
	std::string DescribeText() const override;

	const VCS::Option *option() const { return option_; }
	std::string_view help() const;

	// The hint line for a row that has no Option behind it. A settings row gets its help from the
	// table; an action row - a cheat, a page jump - has nowhere else to carry one.
	void SetHelp(std::string_view help) { help_ = help; }

protected:
	// A toggle row flips its value on click. Float rows are edited through the bar and the
	// arrow keys instead, so this leaves them alone.
	void ClickInternal() override;

private:
	// direction is -1 or +1; a bool flips either way, a float moves one step of its range.
	void Adjust(int direction);
	// Focus this row on a press, in the one way that survives the press. See the definition.
	void ClaimFocus();
	// Where along the row's value bar x falls, 0..1. Only meaningful for float options.
	float ValueFractionAt(float x) const;

	// The box the pointer actually has to be inside, which is NOT bounds_. Rows are laid out at
	// the full screen width so that the two-column settings layout can align on the screen's
	// centre line; using that for hit-testing made every row a full-width band, so anything at
	// the same height counted as a hit no matter how far from the text it was. Measured during
	// Draw, which is the only place with a UIContext to measure text with, and falls back to the
	// row until the first frame has been drawn.
	Bounds HitBounds() const;
	mutable Bounds hitBounds_;
	mutable bool hitBoundsValid_ = false;

	std::string label_;
	std::string help_;
	const VCS::Option *option_ = nullptr;
	bool draggingValue_ = false;
};

// One line of the controls listing: the action on the left, then one column per device - what it
// is on the keyboard, on an Xbox pad and on a PlayStation one - and a bar across the row when it
// is selected.
//
// A ClickableItem purely for the highlight. There is nothing to click - bindings are not
// editable here - but a row that does not light up under the mouse or the arrow keys reads as
// dead, and the game's own Controls screen highlights the same way.
class VCSBindingRow : public UI::ClickableItem {
public:
	explicit VCSBindingRow(const VCS::VCSListingRow &row,
		UI::LayoutParams *layoutParams = nullptr);

	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;
	bool Touch(const TouchInput &input) override;
	std::string DescribeText() const override;

private:
	std::string name_;
	// One cell per device column, already joined into the string that gets drawn. A vector
	// rather than an array sized by the device count, so this header does not have to pull in
	// the input layer for one constant - the listing it is built from decides how many there
	// are, and an empty cell is an action that device cannot do.
	std::vector<std::string> cells_;
};

class VCSMenuScreen : public UIBaseDialogScreen {
public:
	VCSMenuScreen(const Path &gamePath, bool bootPending, VCSMenuMode mode = VCSMenuMode::Pause);
	~VCSMenuScreen();

	const char *tag() const override { return "VCSMenu"; }

	bool key(const KeyInput &key) override;

	// The title art is a GPU object, and screens are deleted long after the graphics device is
	// destroyed - NativeShutdownGraphics calls deviceLost() and tears Vulkan down, and
	// NativeShutdown deletes the screen manager much later. So this, not the destructor, is
	// where the textures have to go.
	void deviceLost() override;
	void deviceRestored(Draw::DrawContext *draw) override;

protected:
	void CreateViews() override;
	void update() override;
	void DrawBackground(UIContext &dc) override;
	ViewLayoutMode LayoutMode() const override {
		return ViewLayoutMode::ApplyInsets;
	}

private:
	void GoToPage(VCSMenuPage page);
	// The page one Back press lands on. Root's parent is "close the menu", handled by the caller.
	static VCSMenuPage ParentPage(VCSMenuPage page);
	const char *PageTitle(VCSMenuPage page) const;

	void AddOptionRows(UI::ViewGroup *parent, VCSMenuPage page);
	void AddBindingRows(UI::ViewGroup *parent, VCSMenuPage page);
	// One row per cheat in this page's group. Clicking one queues it and closes the menu, because
	// nothing can be typed into a paused game - see Core/VCS/VCSCheats.h.
	void AddCheatRows(UI::ViewGroup *parent, VCSMenuPage page);
	// True for the five pages that list cheats, false for the index page above them.
	static bool IsCheatPage(VCSMenuPage page);
	static VCS::CheatGroup ToCheatGroup(VCSMenuPage page);
	// True for the four pages that list bindings rather than offering anything to change.
	static bool IsKeyListPage(VCSMenuPage page);
	// Whether the controller caveat belongs on this page. True on the cards and on the page that
	// leads to them, false everywhere else - it is a statement about what the listing describes,
	// so it has no business on a settings page.
	bool ShowingControllerNote() const;
	static VCS::VCSKeyList ToKeyList(VCSMenuPage page);
	// The frame the listing sits in. Sized to the rows actually on the page, so BACK lands just
	// below it whether the page has eight rows or eighteen.
	Bounds ListPanel() const;
	// How tall each row on a listing page is, which depends on how many there are - the card has
	// to fit the window, since nothing here scrolls.
	float ListRowHeight(int rowCount) const;
	// A row that just walks to another page. The commonest thing on this menu by far.
	void AddPageRow(UI::ViewGroup *parent, const char *label, VCSMenuPage target);
	// A row that asks for the GAME's own front end - its map, its save list. Like a cheat row it
	// queues and closes, because nothing reaches a paused game. See Core/VCS/VCSFrontEnd.h.
	void AddGameMenuRow(UI::ViewGroup *parent, const char *label, VCS::FrontEndTarget target,
		const char *help);
	void AddBackRow(UI::ViewGroup *parent);
	// True for the leaf pages that are a list of settings rather than a list of pages.
	static bool IsOptionPage(VCSMenuPage page);
	static VCS::OptionPage ToOptionPage(VCSMenuPage page);

	void OnResume(UI::EventParams &e);
	void OnLoadGame(UI::EventParams &e);
	void OnExitGame(UI::EventParams &e);
	void OnQuitApp(UI::EventParams &e);
	void OnGameSettings(UI::EventParams &e);
	void OnRestoreDefaults(UI::EventParams &e);

	// Carried because the pause-screen call sites pass it and PPSSPP's own pause menu needs it to
	// grey out save-state controls mid-boot. Nothing here reads it yet - this menu has no
	// save-state rows - but it is what those rows would have to be gated on.
	bool bootPending_;
	VCSMenuMode mode_;

	VCSMenuPage page_ = VCSMenuPage::Root;

	// How many binding rows the current page drew, and how tall each one came out, so
	// DrawBackground can size the panel behind them. Zero rows on every page that is not a
	// listing.
	int listRowCount_ = 0;
	float listRowHeight_ = 0.0f;

	// Rebuilt by CreateViews. Used only to find the focused row for the helper line, so these
	// are borrowed pointers into the view tree, never owned.
	std::vector<VCSMenuItem *> rows_;

	// The page-title textures. A member and not a file-scope static, because a static's
	// destructor runs at program exit - after Vulkan has gone - and releasing a texture there
	// is at best too late and at worst a use-after-free. Opaque so this header does not have to
	// know about Draw::Texture.
	std::unique_ptr<struct VCSMenuArt> art_;
};

// Returns the VCS pause menu for VCS, and PPSSPP's ordinary one for everything else. The three
// places EmuScreen opens a pause screen go through here, which is the whole integration.
UIScreen *CreatePauseScreen(const Path &gamePath, bool bootPending);

// The screen the app opens on: the VCS main menu once a VCS disc has been booted at least once,
// and PPSSPP's ordinary game browser until then - there is nothing to put behind LOAD GAME
// before we have been told which disc that is.
UIScreen *CreateStartScreen();

// The loading screen over the boot's auto-load, drawn straight onto EmuScreen rather than pushed
// as a screen of its own: a screen would pause the emulator, and the whole point of this one is
// that the game carries on working behind it.
//
// Here rather than in EmuScreen because this file owns the menu's artwork and its font - the
// curtain is the menu's backdrop with one word on it, and a second copy of the art loader is the
// thing worth avoiding. `alpha` fades it out at the end; 1.0 is fully opaque.
void DrawVCSBootCurtain(UIContext &dc, float alpha, const char *word);

// Let go of the curtain's texture. Called when the graphics device goes, beside the other VCS
// overlay textures EmuScreen owns.
void ReleaseVCSBootCurtainArt();
