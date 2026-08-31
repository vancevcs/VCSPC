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
#include <optional>
#include <vector>

#include "Common/System/OSD.h"
#include "Core/HLE/sceCtrl.h"
#include "Common/Input/KeyCodes.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include <mutex>
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

VCSFrontEndSettings &FrontEndSettings() {
	static VCSFrontEndSettings settings;
	return settings;
}

bool MenuOnTabStrip() {
	// Defaults to "on the strip" when unreadable, because that is the answer that makes the enter
	// press conditional rather than automatic - and an automatic Cross is the one that activates
	// LOAD GAME by accident.
	return ReadAddrBool(VCSAddr::MenuUseRoot).value_or(true);
}

// Find a blip of one kind. The NEAREST one rather than the first, which only matters for mission
// markers - a mission can have several up at once, and the one you are being sent to next is the
// one closest to you. There is only ever a single dropped waypoint, so it costs nothing there.
static bool FindBlipOfType(u32 wantType, float *x, float *y, bool requireNoIcon = false) {
	const std::optional<u32> store = ReadAddrU32(VCSAddr::BlipManager);
	if (!store || *store == 0) {
		return false;
	}
	float px = 0.0f, py = 0.0f;
	bool havePlayer = false;
	if (const std::optional<u32> pb = ReadAddrU32(VCSAddr::PlayerBase)) {
		const std::optional<float> fx = ReadFloat(*pb + kVCSEntityPositionOffset);
		const std::optional<float> fy = ReadFloat(*pb + kVCSEntityPositionOffset + 4);
		if (fx && fy) { px = *fx; py = *fy; havePlayer = true; }
	}

	bool found = false;
	float bestSq = 0.0f;
	for (int i = 0; i < kVCSBlipMaxEntries; i++) {
		const u32 entry = *store + kVCSBlipArray + (u32)i * kVCSBlipStride;
		const std::optional<u32> type = ReadU32(entry + kVCSBlipType);
		const std::optional<u32> active = ReadU32(entry + kVCSBlipActive);
		if (!type || !active || *type != wantType || *active == 0) {
			continue;
		}
		if (requireNoIcon) {
			const std::optional<u8> icon = ReadU8(entry + kVCSBlipIcon);
			if (!icon || *icon != 0) {
				continue;
			}
		}
		const std::optional<float> bx = ReadFloat(entry + kVCSBlipX);
		const std::optional<float> by = ReadFloat(entry + kVCSBlipY);
		if (!bx || !by) {
			continue;
		}
		// A blip of the right type still has to be somewhere on the map. This rejects a slot that
		// is mid-allocation, whose type is already written and whose position is not.
		if (*bx < -4000.0f || *bx > 4000.0f || *by < -4000.0f || *by > 4000.0f) {
			continue;
		}
		const float dx = *bx - px;
		const float dy = *by - py;
		const float d = havePlayer ? (dx * dx + dy * dy) : 0.0f;
		if (!found || d < bestSq) {
			found = true;
			bestSq = d;
			*x = *bx;
			*y = *by;
		}
		if (!havePlayer) {
			break;   // no way to rank them; first will do
		}
	}
	return found;
}

bool FindWaypoint(float *x, float *y) {
	return FindBlipOfType(kVCSBlipMarkerType, x, y);
}

bool FindMissionMarker(float *x, float *y) {
	// The icon test is what makes this "a mission is sending you somewhere" rather than "the game
	// has an objective marker up" - see the note over kVCSBlipIcon.
	return FindBlipOfType(kVCSBlipMissionType, x, y, true);
}

bool FindRouteDestination(float *x, float *y, bool *isMission) {
	const bool preferMission = FrontEndSettings().preferMissionMarker;
	if (isMission) {
		*isMission = false;
	}
	if (preferMission && FindMissionMarker(x, y)) {
		if (isMission) {
			*isMission = true;
		}
		return true;
	}
	if (FindWaypoint(x, y)) {
		return true;
	}
	// Only reached when the preference is off: the dropped marker wins, and the mission is the
	// fallback rather than the other way round.
	if (FindMissionMarker(x, y)) {
		if (isMission) {
			*isMission = true;
		}
		return true;
	}
	return false;
}

bool GameMenuActive() {
	return ReadAddrBool(VCSAddr::MenuActive).value_or(false);
}

int GameMenuPage() {
	const std::optional<u32> page = ReadAddrAsU32(VCSAddr::MenuPage);
	if (!page) {
		return -1;
	}
	// Stored signed, read unsigned - the closed state is -1 and would otherwise come back as 255,
	// which is a page number the walk would chase forever.
	return (int)(s8)*page;
}

namespace {

enum class Phase {
	Idle,
	PressStart,   // holding Start, waiting for the menu to take it
	WaitMenu,     // Start released, waiting for MenuActive
	TabHold,      // holding the next-page control
	TabGap,       // released, letting the front end act on it
	EnterWait,    // arrived; letting the front end settle before asking where focus is
	DismissHold,  // tapping Cross to descend from the tab strip into the page
	DismissGap,   // released, then back to EnterWait to check whether it took
	ClosePress,   // holding Back, to shut the game's menu on the way out
	CloseGap,     // released, checking whether it shut
	Settle,       // nothing pressed, letting the world see the release before the player acts
	JumpVerify,   // wrote the page index; waiting a game frame to see whether it held
	Done,         // handed over; the player is working the menu now
};

Phase g_phase = Phase::Idle;
FrontEndTarget g_target = FrontEndTarget::Menu;
int g_wantPage = -1;
int g_remaining = 0;
int g_waited = 0;
int g_presses = 0;
int g_lastSeenPage = -1;
u32 g_mask = 0;
const char *g_status = "idle";

// Whether the menu on screen is one this bridge opened, as opposed to one the game put
// up itself. Declared here rather than beside the map drag because Finish and GoIdle -
// far above it - are what set and clear it.
bool g_bridgeOwnsMenu = false;

// The game logic clock, exactly as the cheat sequencer uses it and for the same reason: the front
// end reads the pad once per logic frame, and a press measured in vblanks is a press held for an
// unpredictable number of samples.
u32 g_lastFrame = 0;
bool g_haveFrame = false;
int g_vblanks = 0;

std::atomic<int> g_request{-1};       // a FrontEndTarget, or -1
std::atomic<bool> g_saveRequest{false};

bool GameFrameAdvanced() {
	const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
	if (frame) {
		g_vblanks = 0;
		if (!g_haveFrame) {
			g_haveFrame = true;
			g_lastFrame = *frame;
			return false;
		}
		if (*frame == g_lastFrame) {
			return false;
		}
		g_lastFrame = *frame;
		return true;
	}
	g_haveFrame = false;
	if (++g_vblanks < 2) {
		return false;
	}
	g_vblanks = 0;
	return true;
}

// Defined further down, once ChildWidgets exists - the walk needs them, but the tree helpers
// belong beside the chrome pass that is their main user.
void ShowPagesAgain();
void HidePagesForWalk();

// How many times the descend loop will press Cross before giving up. Three is generous - one
// press moves focus - and the bound matters more than the number: Cross means SELECT once
// focus is inside the page, so a loop that never stopped would work its way down the Game
// page's entries.
constexpr int kMaxEnterPresses = 3;

// The waypoint press runs outside the state machine - it is the player's key, not a step in a
// sequence - so it cannot share GameFrameAdvanced's edge, which the machine consumes.
u32 g_wpFrame = 0;
bool g_haveWpFrame = false;

bool GameFrameAdvancedForWaypoint() {
	const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
	if (!frame) {
		return true;
	}
	if (!g_haveWpFrame) {
		g_haveWpFrame = true;
		g_wpFrame = *frame;
		return false;
	}
	if (*frame == g_wpFrame) {
		return false;
	}
	g_wpFrame = *frame;
	return true;
}

int Frames(int value) {
	return std::max(value, 1);
}

void Finish(const char *why) {
	// Everything Finish is reached from is the bridge having put a page on screen.
	g_bridgeOwnsMenu = true;
	// Whatever happened - arrived, gave up, the menu closed under us - the pages come back. A
	// walk that ends without this leaves the whole front end invisible.
	ShowPagesAgain();
	g_phase = Phase::Done;
	g_mask = 0;
	g_status = why;
}

// Landing on the requested page, which for the map is one press short of what was asked for.
//
// The map opens with its legend over the city - the key, plus "Hold X to display the map
// options" - and it does not time out. Cross puts it away, which is the game's own `select`, so
// the bridge taps it exactly as a player would rather than hunting for whatever draws it.
//
// The tap is SHORT on purpose. The same button HELD is what opens the map options, so a press
// measured in game frames rather than in seconds is the difference between dismissing the legend
// and opening the thing the legend is advertising.
void Arrive(const char *why) {
	const VCSFrontEndSettings &s = FrontEndSettings();
	// Descend from the tab strip into the page - but only where descending is what the player
	// wants, which is not every page.
	//
	// Checked in a LOOP rather than once, because arriving and the front end having settled are
	// not the same instant: `MenuActive` flips as the menu opens and `MenuUseRoot` is set
	// somewhere in the same sequence, so a single reading taken on the tick the menu appeared can
	// see focus already inside the page and skip the press. That is the map coming up with its
	// legend still on it.
	//
	// The map needs it: inside the page is where the d-pad pans and where the legend goes away.
	// A page with entries needs it: LOAD GAME does not respond from the strip.
	//
	// **Stats and Brief must NOT have it.** Stats rolls its lines down on its own while the strip
	// has focus, and descending stops the roll dead - reported as "stats doesn't want to scroll,
	// and it starts for a split second when I press Escape", which is the roll resuming as Back
	// lifts focus back to the strip on the way out. Their pages are read, not operated.
	const bool wantsDescent = g_target == FrontEndTarget::Map || MenuPageHasItems();
	if (s.enterPageOnArrival && wantsDescent) {
		// The pages come back BEFORE the descend, not after it, and that ordering is the whole
		// bug behind "the map still shows its legend".
		//
		// Descending is what puts the legend away, and it does that by turning a widget off. But
		// the walk had every page's widgets hidden, with their previous visibility recorded to be
		// restored - so the sequence was: record legend=visible, hide it, press Cross (the game
		// sets legend=hidden), then restore the recorded value and put it straight back. The
		// press worked every time; the restore undid it.
		//
		// Revealing first costs the fraction of a second the descend takes, on a page the player
		// asked to see anyway. It cannot restore over a change it made no record of.
		ShowPagesAgain();
		g_presses = 0;
		g_phase = Phase::EnterWait;
		g_remaining = Frames(s.gapFrames);
		g_mask = 0;
		g_status = "entering page";
		return;
	}
	Finish(why);
}

void GoIdle(const char *why) {
	g_bridgeOwnsMenu = false;
	ShowPagesAgain();
	g_phase = Phase::Idle;
	g_mask = 0;
	g_presses = 0;
	g_waited = 0;
	g_remaining = 0;
	g_status = why;
}

// Pages already visited since the last row change. The walk uses this to notice it has been
// all the way round one row, which is the moment to change rows rather than keep pressing.
// A u32 covers 32 pages; this front end has 11.
u32 g_seenInRow = 0;

// The next press: along the row while it still has somewhere new to go, down to the next row
// when it does not. Sets the mask and records where we are.
void NextPress(int page) {
	if (page >= 0 && page < 32) {
		if (g_seenInRow & (1u << page)) {
			// Been here before without arriving, so this row is exhausted. Change rows and start
			// counting again - see the GRID note in the header.
			g_seenInRow = 0;
			g_mask = CTRL_DOWN;
			return;
		}
		g_seenInRow |= 1u << page;
	}
	g_mask = CTRL_RIGHT;
}

// One widget we turned off, and what it was before. Used by both the chrome pass and the walk,
// which is why it is declared up here rather than beside either of them.
struct HiddenWidget {
	u32 addr = 0;
	u8 wasVisible = 1;
};

// Every page's widgets, hidden while a walk is in progress so the tabs stepped through on the way
// are not drawn. Separate from g_hidden, which is the chrome and outlives any one walk.
std::vector<HiddenWidget> g_walkHidden;

// Press the game's own navigation until the index matches. The slow, certain path.
void StartTabWalk() {
	HidePagesForWalk();
	g_seenInRow = 0;
	g_presses = 0;
	NextPress(GameMenuPage());
	g_phase = Phase::TabHold;
	g_remaining = Frames(FrontEndSettings().holdFrames);
	g_status = "tabbing";
}

// Start the walk, or finish immediately when there is nowhere to walk to.
void BeginWalk() {
	const VCSFrontEndSettings &s = FrontEndSettings();
	g_presses = 0;
	g_lastSeenPage = GameMenuPage();

	if (g_wantPage < 0) {
		// Either the player asked for the menu itself, or the page for what they asked for has
		// not been measured yet. Both are "you are in the menu now", which is the honest result.
		Finish("menu open");
		return;
	}
	if (g_lastSeenPage == g_wantPage) {
		Arrive("on page");
		return;
	}

	// The fast path: write the index instead of pressing a control until it arrives. Every tab the
	// walk steps through is a frame that gets drawn, which is the riffle past Controls/Audio/
	// Display you see going from Stats to Game; a store lands with nothing in between.
	//
	// Verified a GAME FRAME LATER, not immediately, and that distinction is the whole safety of
	// it. Reading the index straight back only proves our own store landed - the game has not run
	// since - so a front end that reconciles the index against navigation state we have not found
	// would rewrite it on its next frame and the check would already have said "worked".
	if (s.jumpDirectlyToPage) {
		WriteAddrU8(VCSAddr::MenuPage, (u8)(s8)g_wantPage);
		g_phase = Phase::JumpVerify;
		g_remaining = Frames(s.gapFrames);
		g_mask = 0;
		g_status = "jumping";
		return;
	}

	StartTabWalk();
}

}  // namespace

void RequestGameMenu(FrontEndTarget target) {
	g_request.store((int)target, std::memory_order_relaxed);
}

void RequestCloseGameMenu() {
	// Only if nothing else is already asked for, and that is not a nicety. This is called from the
	// menu screen's destructor - i.e. on EVERY way out of it - so a row that queued MAP on its way
	// to closing the screen would have its request overwritten by the teardown that follows it.
	// Asked for by a row: go there. Asked for by nothing: shut the game's menu.
	int expected = -1;
	g_request.compare_exchange_strong(expected, (int)FrontEndTarget::Close,
		std::memory_order_relaxed);
}

void RequestSaveMenu() {
	g_saveRequest.store(true, std::memory_order_relaxed);
}

bool FrontEndDriving() {
	switch (g_phase) {
	case Phase::PressStart:
	case Phase::WaitMenu:
	case Phase::TabHold:
	case Phase::TabGap:
	case Phase::DismissHold:
	case Phase::DismissGap:
	case Phase::ClosePress:
	case Phase::CloseGap:
	case Phase::JumpVerify:
	case Phase::Settle:
	case Phase::EnterWait:
		return true;
	default:
		return false;
	}
}

u32 FrontEndButtonMask() {
	return g_mask;
}

// --- The menu chrome ------------------------------------------------------------------------
//
// Everything below hides widgets by NAME. The front end is a tree of named objects - the retail
// build kept the debug names, and every widget's first field points at its own - so nothing here
// is identified by index, by allocation order, or by address. See "The front end is a named
// widget tree" in docs/VCS_ADDRESSES.md.
//
// That is worth insisting on, because the indices were what the first version used and they were
// only ever right by luck: the eight tab labels happened to be widgets 1..8 of the root page in
// this run. Names cannot drift the way that can.
//
// Three things go, and only one of them is a visibility flag:
//
//   *_t on the root page   the eight tab labels - Map_t, Brief_t, Game_t ...
//   BUTTONS* children      the hint row - Move, Select, back, placemarker and their glyphs
//   Map_AE's height        224 -> 272, because the map stops short to leave room for the strip
//
// The last is why this is not simply "hide some widgets": with the strip gone the map still ended
// at y=224 and the 48px below it stayed black, which is exactly what the strip used to sit on.

namespace {

struct GrownWidget {
	u32 addr = 0;
	int originalHeight = 0;
};

std::vector<HiddenWidget> g_hidden;
std::vector<GrownWidget> g_grown;
bool g_chromeApplied = false;

// The widgets that are meant to fill the screen behind everything else. By name, because a rule
// like "any full-width widget" would also catch `Reset_MI`, which is a 480-wide MENU ROW - and
// stretching that would turn one entry of the Game page into the whole page.
const char *const kBackdropNames[] = {
	"Background",  // MASTER's, and MEMCARD_FULL_PAGE's
	"Map_AE",      // the map itself
	"WRAPPER",     // MP_P1_PAGE's, laid out 480x224 like the map
};

// A widget's own name, as ASCII. Empty when the pointer is not readable or the bytes are not a
// plausible name - which is the same discipline the rest of this layer follows: a bad read is a
// normal condition, not a fault.
bool WidgetName(u32 widget, char *out, size_t size) {
	out[0] = '\0';
	const std::optional<u32> namePtr = ReadU32(widget + kVCSWidgetName);
	if (!namePtr || *namePtr == 0) {
		return false;
	}
	for (size_t i = 0; i + 1 < size; i++) {
		const std::optional<u8> c = ReadU8(*namePtr + (u32)i);
		if (!c) {
			return false;
		}
		if (*c == 0) {
			out[i] = '\0';
			return i > 0;
		}
		if (*c < 32 || *c >= 127) {
			return false;
		}
		out[i] = (char)*c;
	}
	out[size - 1] = '\0';
	return true;
}

bool EndsWith(const char *s, const char *suffix) {
	const size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

// The widgets a page or group owns, as a vector<Widget*> at +0x24 / +0x28.
std::vector<u32> ChildWidgets(u32 obj) {
	std::vector<u32> out;
	const std::optional<u32> begin = ReadU32(obj + kVCSWidgetsBegin);
	const std::optional<u32> end = ReadU32(obj + kVCSWidgetsEnd);
	if (!begin || !end || *end < *begin) {
		return out;
	}
	const u32 count = (*end - *begin) / 4;
	// The tab strip's pages - map, brief, game, stats, controls, audio, display, multiplayer. The rest
// of the eleven are dialogs the front end pushes on top of them.
// A sane bound rather than a trusted one: this walks pointers out of game memory, and a
	// half-built vector during a page transition would otherwise ask for millions of reads.
	if (count == 0 || count > 64) {
		return out;
	}
	for (u32 i = 0; i < count; i++) {
		const std::optional<u32> w = ReadU32(*begin + i * 4);
		if (w && *w != 0) {
			out.push_back(*w);
		}
	}
	return out;
}

void ShowPagesAgain() {
	for (const HiddenWidget &h : g_walkHidden) {
		WriteU8(h.addr + kVCSWidgetVisible, h.wasVisible);
	}
	g_walkHidden.clear();
}

// Called every tick of a walk, not once at the start.
//
// A page turns its own widgets on when it becomes current, so hiding them once and stepping
// through four pages shows three of them anyway - which is why the first version of this still
// flickered. The ORIGINAL value is only recorded the first time a widget is seen, so re-asserting
// cannot lose it.
void HidePagesForWalk() {
	if (!FrontEndSettings().hidePagesWhileWalking) {
		return;
	}
	const std::optional<u32> begin = ReadAddrU32(VCSAddr::MenuPagesBegin);
	const std::optional<u32> end = ReadAddrU32(VCSAddr::MenuPagesEnd);
	if (!begin || !end || *end < *begin) {
		return;
	}
	const u32 pages = (*end - *begin) / 4;
	for (u32 i = 0; i < pages && i < 32; i++) {
		const std::optional<u32> page = ReadU32(*begin + i * 4);
		if (!page || *page == 0) {
			continue;
		}
		for (u32 w : ChildWidgets(*page)) {
			const std::optional<u8> vis = ReadU8(w + kVCSWidgetVisible);
			if (!vis || *vis == 0) {
				continue;
			}
			bool known = false;
			for (const HiddenWidget &h : g_walkHidden) {
				if (h.addr == w) {
					known = true;
					break;
				}
			}
			if (!known) {
				HiddenWidget h;
				h.addr = w;
				h.wasVisible = *vis;
				g_walkHidden.push_back(h);
			}
			WriteU8(w + kVCSWidgetVisible, 0);
		}
	}
}

// --- Dragging the map -------------------------------------------------------------------------
//
// `Map_AE` keeps what the map is centred on at +0xc0 / +0xc4, and holding a direction moves them
// symmetrically - right took +0xc0 down by 166.69 over the same interval that down took +0xc4 by.
// So they are an x/y pair in map units and the drag is an addition, not a mechanism to drive.
//
// This is the one place in the front-end work that writes game state rather than pressing a
// control, and it earns the exception: there is no button that means "pan by this many units", the
// same way there was no PSP input meaning "rotate the camera by this many radians" - which is why
// VCSCamera writes angles too. A held direction is a RATE; a mouse gives a DISPLACEMENT.
//
// Nothing pans while the tab strip has focus, which is why an earlier search for these variables
// found nothing: it was looking on a page whose arrow keys were changing tab.
// Held-distance since the drag button went down, so a click can be told from a drag, plus the
// countdown for the Square we send when it turns out to be a click.
float g_dragTravel = 0.0f;
bool g_dragHeld = false;
bool g_spaceHeld = false;
int g_waypointFrames = 0;
int g_zoomFrames = 0;
u32 g_zoomMask = 0;
bool g_mapOpenedThisSession = false;

// Ask for the game's own place-marker control, for a few game frames so it reads as a press.
void PlaceWaypoint() {
	if (g_waypointFrames <= 0) {
		g_waypointFrames = Frames(FrontEndSettings().enterFrames);
	}
}

namespace {

// The nopped-out crosshair call, and where our replacement goes.
bool g_crosshairPatched = false;
u32 g_crosshairOriginal = 0;

std::mutex g_cursorLock;
bool g_haveCursor = false;         // guarded by g_cursorLock
float g_cursorX = 0.0f;            // guarded by g_cursorLock
float g_cursorY = 0.0f;            // guarded by g_cursorLock

// Put the game's crosshair away, or bring it back.
//
// One instruction either way. The read goes through Memory::Read_Instruction rather than a raw
// load because once a block has been compiled the raw word at its first instruction is PPSSPP's
// own RUNBLOCK marker rather than the game's opcode - and the restore checks nothing else, so a
// stored original that was really a marker would be written back as one.
//
// It refuses anything that is not the `jal` it expects. A wrong address here does not produce a
// wrong crosshair, it produces a nop somewhere in the middle of the map's draw.
void SetCrosshairHidden(bool hide) {
	if (hide == g_crosshairPatched) {
		return;
	}
	if (!Memory::IsValidAddress(kVCSMapCrosshairCall)) {
		return;
	}
	if (hide) {
		const MIPSOpcode op = Memory::Read_Instruction(kVCSMapCrosshairCall, true);
		if ((op.encoding >> 26) != 3) {
			return;
		}
		g_crosshairOriginal = op.encoding;
		WriteU32(kVCSMapCrosshairCall, 0);
		currentMIPS->InvalidateICache(kVCSMapCrosshairCall, 4);
		g_crosshairPatched = true;
	} else {
		WriteU32(kVCSMapCrosshairCall, g_crosshairOriginal);
		currentMIPS->InvalidateICache(kVCSMapCrosshairCall, 4);
		g_crosshairPatched = false;
	}
}

// The Map_AE widget, or 0. Same walk the drag uses.
u32 MapWidget() {
	const VCSFrontEndSettings &s = FrontEndSettings();
	if (s.mapPage < 0) {
		return 0;
	}
	const std::optional<u32> begin = ReadAddrU32(VCSAddr::MenuPagesBegin);
	if (!begin) {
		return 0;
	}
	const std::optional<u32> page = ReadU32(*begin + (u32)s.mapPage * 4);
	if (!page || *page == 0) {
		return 0;
	}
	char name[32];
	for (u32 w : ChildWidgets(*page)) {
		if (WidgetName(w, name, sizeof(name)) && strcmp(name, "Map_AE") == 0) {
			return w;
		}
	}
	return 0;
}

}  // namespace

void MapDragTick() {
	const VCSFrontEndSettings &s = FrontEndSettings();
	if (!GameMenuActive() || MenuOnTabStrip()) {
		g_dragHeld = false;
		g_spaceHeld = false;
		g_dragTravel = 0.0f;
		return;
	}
	if (s.mapPage < 0 || GameMenuPage() != s.mapPage) {
		g_dragHeld = false;
		g_spaceHeld = false;
		return;
	}

	// The wheel zooms, one latched press per notch. Notches are counted as they arrive because a
	// notch is a press and a release in the same instant - see TakeWheelNotches.
	int wheelUp = 0, wheelDown = 0;
	TakeWheelNotches(&wheelUp, &wheelDown);
	if (s.mapZoomWithWheel && (wheelUp || wheelDown) && g_zoomFrames <= 0) {
		g_zoomMask = wheelUp ? (u32)CTRL_RTRIGGER : (u32)CTRL_LTRIGGER;
		g_zoomFrames = Frames(s.zoomInFrames);
	}

	// Space places a marker outright. The drag button places one too, but only if it turns out to
	// have been a CLICK - press and release without meaningful travel - because it is also how the
	// map is dragged, and a drag that dropped a marker at the end would be unusable.
	if (s.mapWaypoint) {
		const bool space = IsHostKeyDown(NKCODE_SPACE);
		if (space && !g_spaceHeld) {
			PlaceWaypoint();
		}
		g_spaceHeld = space;
	}

	if (!s.mapDrag) {
		return;
	}
	const bool held = IsHostKeyDown(NKCODE_EXT_MOUSEBUTTON_1);
	if (!held) {
		if (g_dragHeld) {
			// Released. Barely moved, so it was a click, not a drag.
			if (s.mapWaypoint && g_dragTravel <= s.waypointClickSlop) {
				PlaceWaypoint();
			}
			g_dragHeld = false;
		}
		g_dragTravel = 0.0f;
		// The accumulator is left alone deliberately: CameraTick drains it every frame whether it
		// uses it or not, so a delta gathered while the button was up cannot pile up and then jump
		// the map on the next press.
		return;
	}
	if (!g_dragHeld) {
		g_dragHeld = true;
		g_dragTravel = 0.0f;
	}

	float dx = 0.0f, dy = 0.0f;
	TakeMouseDelta(&dx, &dy);
	g_dragTravel += (dx < 0.0f ? -dx : dx) + (dy < 0.0f ? -dy : dy);
	if (dx == 0.0f && dy == 0.0f) {
		return;
	}

	const std::optional<u32> begin = ReadAddrU32(VCSAddr::MenuPagesBegin);
	if (!begin) {
		return;
	}
	const std::optional<u32> page = ReadU32(*begin + (u32)s.mapPage * 4);
	if (!page || *page == 0) {
		return;
	}
	char name[32];
	for (u32 w : ChildWidgets(*page)) {
		if (!WidgetName(w, name, sizeof(name)) || strcmp(name, "Map_AE") != 0) {
			continue;
		}
		const std::optional<float> x = ReadFloat(w + kVCSMapPanX);
		const std::optional<float> y = ReadFloat(w + kVCSMapPanY);
		if (!x || !y) {
			return;
		}
		// Plus, not minus: the map follows the cursor, so dragging right brings the city right.
		// Holding the d-pad right does the opposite - it moves the VIEW right, which is why the
		// measured deltas were negative - and a drag is the other verb.
		WriteFloat(w + kVCSMapPanX, *x + dx * s.mapDragSpeed);
		WriteFloat(w + kVCSMapPanY, *y + dy * s.mapDragSpeed);
		return;
	}
}

void HideWidget(u32 widget) {
	const std::optional<u8> vis = ReadU8(widget + kVCSWidgetVisible);
	if (!vis) {
		return;
	}
	if (*vis != 0) {
		HiddenWidget h;
		h.addr = widget;
		h.wasVisible = *vis;
		g_hidden.push_back(h);
		WriteU8(widget + kVCSWidgetVisible, 0);
	}
}

// Put everything back exactly as it was. Called when the menu closes and on shutdown, so a player
// who turns the setting off - or a savestate taken with the menu open - does not inherit a front
// end with half its furniture missing.
void RestoreChrome() {
	ShowPagesAgain();
	for (const HiddenWidget &h : g_hidden) {
		WriteU8(h.addr + kVCSWidgetVisible, h.wasVisible);
	}
	g_hidden.clear();
	for (const GrownWidget &g : g_grown) {
		WriteU32(g.addr + kVCSWidgetH, (u32)g.originalHeight);
	}
	g_grown.clear();
	g_chromeApplied = false;
}

void ApplyChrome() {
	const VCSFrontEndSettings &s = FrontEndSettings();
	char name[32];

	// The tab strip: the root page's own *_t widgets. Matched on the suffix rather than on the
	// eight names, so a build with a tab this one does not have still loses it.
	const std::optional<u32> root = ReadAddrU32(VCSAddr::MenuRootPage);
	if (root && *root != 0) {
		for (u32 w : ChildWidgets(*root)) {
			if (WidgetName(w, name, sizeof(name)) && EndsWith(name, "_t")) {
				HideWidget(w);
			}
		}
	}

	// The hint row: every child of every BUTTONS group. All four groups, not just the one the
	// current page uses - the page can change under us and this way the answer does not depend
	// on which it is.
	const std::optional<u32> obegin = ReadAddrU32(VCSAddr::MenuOverlaysBegin);
	const std::optional<u32> oend = ReadAddrU32(VCSAddr::MenuOverlaysEnd);
	if (obegin && oend && *oend >= *obegin) {
		const u32 count = (*oend - *obegin) / 4;
		for (u32 i = 0; i < count && i < 16; i++) {
			const std::optional<u32> group = ReadU32(*obegin + i * 4);
			if (!group || *group == 0) {
				continue;
			}
			if (!WidgetName(*group, name, sizeof(name)) || strncmp(name, "BUTTONS", 7) != 0) {
				continue;
			}
			for (u32 w : ChildWidgets(*group)) {
				HideWidget(w);
			}
		}
	}

	// And the black band along the bottom, which is not a gap the strip left behind - it is screen
	// below the 272 rows the menu lays itself out in. Every backdrop gets stretched past it.
	if (!s.fillMenuBackdrop) {
		return;
	}
	const int wanted = s.backdropHeight;

	auto growBackdrops = [&](u32 owner) {
		for (u32 w : ChildWidgets(owner)) {
			if (!WidgetName(w, name, sizeof(name))) {
				continue;
			}
			bool isBackdrop = false;
			for (const char *b : kBackdropNames) {
				if (strcmp(name, b) == 0) {
					isBackdrop = true;
					break;
				}
			}
			if (!isBackdrop) {
				continue;
			}
			const std::optional<u32> hh = ReadU32(w + kVCSWidgetH);
			if (!hh || (int)*hh >= wanted) {
				continue;
			}
			GrownWidget g;
			g.addr = w;
			g.originalHeight = (int)*hh;
			g_grown.push_back(g);
			WriteU32(w + kVCSWidgetH, (u32)wanted);
		}
	};

	if (root && *root != 0) {
		growBackdrops(*root);
	}
	const std::optional<u32> pbegin = ReadAddrU32(VCSAddr::MenuPagesBegin);
	const std::optional<u32> pend = ReadAddrU32(VCSAddr::MenuPagesEnd);
	if (!pbegin || !pend || *pend < *pbegin) {
		return;
	}
	const u32 pages = (*pend - *pbegin) / 4;
	for (u32 i = 0; i < pages && i < 32; i++) {
		const std::optional<u32> page = ReadU32(*pbegin + i * 4);
		if (page && *page != 0) {
			growBackdrops(*page);
		}
	}
}

// Once per tick. Applied on the edge into "menu up", undone on the edge out - not re-asserted
// every frame, because nothing here is contested: the game writes these fields when it builds a
// page and not afterwards, which the poke tests confirmed by sticking.
void ChromeTick() {
	const bool want = FrontEndSettings().hideMenuChrome && GameMenuActive();
	if (want == g_chromeApplied) {
		return;
	}
	if (want) {
		ApplyChrome();
		g_chromeApplied = true;
	} else {
		RestoreChrome();
	}
}

}  // namespace

void MapCursorTick() {
	const VCSFrontEndSettings &s = FrontEndSettings();
	const bool onMap = s.smallMapCursor && GameMenuActive() && s.mapPage >= 0 &&
	                   GameMenuPage() == s.mapPage;

	// Patched only while the map is actually up. The map's draw is the only caller, so leaving it
	// nopped would be harmless - but a game whose code differs from ours only while a particular
	// screen is open is far easier to reason about than one that is permanently altered.
	SetCrosshairHidden(onMap);

	if (!onMap) {
		std::lock_guard<std::mutex> guard(g_cursorLock);
		g_haveCursor = false;
		return;
	}

	const u32 ae = MapWidget();
	float x = 0.0f, y = 0.0f;
	bool ok = false;
	if (ae != 0) {
		const std::optional<u32> w = ReadU32(ae + kVCSWidgetW);
		const std::optional<u32> h = ReadU32(ae + kVCSWidgetH);
		// Read, never written. The pair is the map's own pan state and +0xd4 is how its vertical
		// axis is steered - writing it froze up and down while left and right went on working.
		// See the note where `centreMapCursor` used to be, in VCSFrontEnd.h.
		const std::optional<float> ox = ReadFloat(ae + kVCSMapCursorOffsetX);
		const std::optional<float> oy = ReadFloat(ae + kVCSMapCursorOffsetY);
		if (w && h && ox && oy) {
			// The game's own arithmetic - see the note over kVCSMapCursorOffsetX.
			x = (float)((int)*w / 2) + *ox;
			y = (float)((int)*h / 2) + *oy;
			ok = x > 0.0f && y > 0.0f && x < 1024.0f && y < 1024.0f;
		}
	}
	std::lock_guard<std::mutex> guard(g_cursorLock);
	g_haveCursor = ok;
	g_cursorX = x;
	g_cursorY = y;
}

bool MapCursorScreen(float *x, float *y) {
	std::lock_guard<std::mutex> guard(g_cursorLock);
	if (!g_haveCursor) {
		return false;
	}
	*x = g_cursorX;
	*y = g_cursorY;
	return true;
}

void MapCursorReset() {
	SetCrosshairHidden(false);
	std::lock_guard<std::mutex> guard(g_cursorLock);
	g_haveCursor = false;
}


u32 MapWaypointButtonMask() {
	return g_waypointFrames > 0 ? (u32)CTRL_SQUARE : 0u;
}

u32 MapZoomButtonMask() {
	return g_zoomFrames > 0 ? g_zoomMask : 0u;
}

bool BridgeOwnsMenu() {
	return g_bridgeOwnsMenu && GameMenuActive();
}

int HiddenChromeCount() {
	return (int)g_hidden.size();
}

bool MenuPageHasItems() {
	static int cachedPage = -2;
	static bool cachedAnswer = false;

	const int page = GameMenuPage();
	if (page == cachedPage) {
		return cachedAnswer;
	}
	cachedPage = page;
	cachedAnswer = false;

	// An UNREADABLE or out-of-band page never locks anything, and getting this backwards is what
	// made the save slots unnavigable.
	//
	// The save menu is not tabbed to - it is opened from the world, by walking into the save icon
	// at a safe house - so `MenuPage` does not name a tab page while it is up. The old default
	// answered "no items", which dropped up and down, and the slot list could not be moved
	// through. Load and Delete were fine only because they are reached from the Game page, which
	// does name a tab and does have entries.
	//
	// So the rule is: take the player's arrow keys only when we positively know they are on a tab
	// page with nothing on it to select. Anything we cannot identify keeps its controls.
	if (page < 0) {
		cachedAnswer = true;
		return true;
	}

	// Only the eight TAB pages are ever locked. Anything past them is a dialog the front end has
	// pushed - CONFIRM_PAGE, MEMCARD_FULL_PAGE, MP_ERROR_PAGE - and a dialog is never something
	// you tab away from, so there is nothing to protect and everything to lose by taking its
	// arrow keys. Reported as a yes/no prompt that could not be answered.
	if (page >= kMenuTabPages) {
		cachedAnswer = true;
		return true;
	}

	const std::optional<u32> begin = ReadAddrU32(VCSAddr::MenuPagesBegin);
	const std::optional<u32> end = ReadAddrU32(VCSAddr::MenuPagesEnd);
	if (!begin || !end || *end < *begin) {
		return false;
	}
	if ((u32)page >= (*end - *begin) / 4) {
		return false;
	}
	const std::optional<u32> obj = ReadU32(*begin + (u32)page * 4);
	if (!obj || *obj == 0) {
		return false;
	}
	char name[32];
	for (u32 w : ChildWidgets(*obj)) {
		if (!WidgetName(w, name, sizeof(name))) {
			continue;
		}
		// `_MI` is a menu item - LoadGame_MI, Brightness_MI. `BTN_` is the other kind, and
		// leaving it out is what made the "you will lose unsaved progress" prompt unusable:
		// CONFIRM_PAGE's two entries are BTN_SPECIAL_CANCEL and BTN_SPECIAL_CONFIRM, so the page
		// looked like it had nothing to move between and up/down stayed locked on a yes/no
		// question. A page that ASKS something always has something to answer it with.
		if (EndsWith(name, "_MI") || strncmp(name, "BTN_", 4) == 0) {
			cachedAnswer = true;
			break;
		}
	}
	return cachedAnswer;
}

void FrontEndTick() {
	// Before anything else: the menu's furniture follows whether the menu is up, not whether the
	// bridge is doing something, so it must run even on the ticks this function returns early.
	ChromeTick();

	// Takes the mouse delta before CameraTick drains it, which is the whole reason FrontEndTick
	// runs first in VCSGame::Tick.
	MapDragTick();

	// The marker press is measured in game frames like every other press here.
	if (GameFrameAdvancedForWaypoint()) {
		if (g_waypointFrames > 0) {
			g_waypointFrames--;
		}
		if (g_zoomFrames > 0 && --g_zoomFrames == 0) {
			g_zoomMask = 0;
		}
	}

	// The save request is a write rather than a walk, so it is not part of the state machine at
	// all - see the header. Done first and unconditionally, because it costs one store and the
	// front end is what decides when to act on it.
	if (g_saveRequest.exchange(false, std::memory_order_relaxed)) {
		if (WriteAddrU8(VCSAddr::SaveMenuRequest, 1)) {
			g_OSD.Show(OSDType::MESSAGE_INFO, "Opening the save menu", 2.0f, "vcs_frontend");
		} else {
			g_OSD.Show(OSDType::MESSAGE_ERROR, "Save menu address unset", 3.0f, "vcs_frontend");
		}
	}

	if (g_phase == Phase::Done && !GameMenuActive()) {
		// Hand-over ends when the player closes the menu.
		GoIdle("idle");
	}

	// A request is taken in Idle and in Done, and taking it in Done is not a refinement - it is
	// the difference between the second row working and appearing dead. Opening the map leaves
	// the bridge handed over with the menu still up, so asking for LOAD GAME next arrives here,
	// not in Idle; deferring it until the menu closed meant the row did nothing at the moment it
	// was pressed and then something surprising later.
	if (g_phase == Phase::Idle || g_phase == Phase::Done) {
		const int req = g_request.exchange(-1, std::memory_order_relaxed);
		if (req < 0) {
			return;
		}
		g_target = (FrontEndTarget)req;
		const VCSFrontEndSettings &s = FrontEndSettings();

		if (g_target == FrontEndTarget::Close) {
			if (!GameMenuActive()) {
				GoIdle("idle");
				return;
			}
			g_presses = 0;
			g_phase = Phase::ClosePress;
			g_remaining = Frames(s.holdFrames);
			g_mask = CTRL_CIRCLE;
			g_status = "closing";
			return;
		}
		switch (g_target) {
		case FrontEndTarget::Map:   g_wantPage = s.mapPage; break;
		case FrontEndTarget::Brief: g_wantPage = s.briefPage; break;
		case FrontEndTarget::Game:  g_wantPage = s.gamePage; break;
		case FrontEndTarget::Stats: g_wantPage = s.statsPage; break;
		default:                    g_wantPage = -1; break;
		}

		if (GameMenuActive()) {
			// Already up - nothing to open, just walk.
			BeginWalk();
			return;
		}
		// Hidden before Start is even pressed. The menu opens on page 0 whatever was asked for, so
		// without this the map is drawn for the frame or two between the menu appearing and the
		// walk starting - which is the map flashing up on the way to Brief, Stats or Game.
		if (g_wantPage >= 0) {
			HidePagesForWalk();
		}
		g_phase = Phase::PressStart;
		g_remaining = Frames(s.holdFrames);
		g_waited = 0;
		g_mask = CTRL_START;
		g_status = "pressing start";
		return;
	}

	// While a walk is running the pages stay hidden, re-asserted every tick - see HidePagesForWalk.
	switch (g_phase) {
	case Phase::PressStart:
	case Phase::WaitMenu:
	case Phase::TabHold:
	case Phase::TabGap:
		HidePagesForWalk();
		break;
	// EnterWait / DismissHold / DismissGap deliberately absent: by then we have arrived and the
	// pages are back up, and re-hiding them would re-arm the restore that was eating the descend.
	default:
		break;
	}

	// Back closes the menu on the PRESS, and the rest of the hold then lands in the world - which
	// on foot is a punch, reported as "the character punches whenever I leave the map". So the
	// button goes up the moment the menu is gone, checked every tick rather than once per game
	// frame: the whole failure is one frame long and a game-frame check can sit right through it.
	if (g_phase == Phase::ClosePress && !GameMenuActive()) {
		g_mask = 0;
		g_phase = Phase::Settle;
		g_remaining = Frames(FrontEndSettings().gapFrames);
		g_status = "closed";
	}

	if (!GameFrameAdvanced()) {
		return;
	}

	const VCSFrontEndSettings &s = FrontEndSettings();

	switch (g_phase) {
	case Phase::PressStart:
		if (--g_remaining > 0) {
			break;
		}
		// Release it. The menu usually arrives a frame or two later, so waiting is a separate
		// phase rather than more of this one - holding Start into an open menu would be a second
		// press, and in this front end that closes it again.
		g_mask = 0;
		g_phase = Phase::WaitMenu;
		g_status = "waiting for menu";
		break;

	case Phase::WaitMenu:
		if (GameMenuActive()) {
			BeginWalk();
			break;
		}
		if (++g_waited >= Frames(s.openTimeoutFrames)) {
			// Start did not open anything. Nothing is half-done - we pressed a button and the
			// game declined - so there is nothing to undo, and saying so beats retrying.
			GoIdle("menu never opened");
			g_OSD.Show(OSDType::MESSAGE_ERROR, "The game's menu did not open", 3.0f,
				"vcs_frontend");
		}
		break;

	case Phase::TabHold:
		if (--g_remaining > 0) {
			break;
		}
		g_mask = 0;
		g_phase = Phase::TabGap;
		g_remaining = Frames(s.gapFrames);
		break;

	case Phase::JumpVerify:
		if (--g_remaining > 0) {
			break;
		}
		if (!GameMenuActive()) {
			GoIdle("menu closed");
		} else if (GameMenuPage() == g_wantPage) {
			Arrive("jumped");
		} else {
			// The game put it back. Nothing is half-done - the index is one page or the other -
			// so do it the slow way, flicker and all.
			StartTabWalk();
		}
		break;

	case Phase::ClosePress:
		if (--g_remaining > 0) {
			break;
		}
		g_mask = 0;
		g_phase = Phase::CloseGap;
		g_remaining = Frames(s.gapFrames);
		break;

	case Phase::Settle:
		// Still holding the pad, still pressing nothing. The player's own buttons stay suppressed
		// for these few frames too, so whatever they were holding when they hit Escape cannot be
		// read as a fresh press on the frame the world comes back.
		if (--g_remaining > 0) {
			break;
		}
		GoIdle("idle");
		break;

	case Phase::CloseGap:
		if (--g_remaining > 0) {
			break;
		}
		if (!GameMenuActive()) {
			g_mask = 0;
			g_phase = Phase::Settle;
			g_remaining = Frames(s.gapFrames);
			g_status = "closed";
			break;
		}
		// Back moves up one level, so a page reached from inside another needs more than one
		// press. Bounded by the same budget the walk uses.
		if (++g_presses >= Frames(s.maxTabPresses)) {
			GoIdle("could not close");
			break;
		}
		g_phase = Phase::ClosePress;
		g_remaining = Frames(s.holdFrames);
		g_mask = CTRL_CIRCLE;
		break;

	case Phase::EnterWait:
		if (--g_remaining > 0) {
			break;
		}
		if (!GameMenuActive()) {
			GoIdle("menu closed");
			break;
		}
		if (!MenuOnTabStrip()) {
			// Inside the page. One more Cross, once per session, only on the map: the press that
			// got us here was spent descending, and on the map Cross is also what toggles the
			// legend - see extraCrossOnFirstMap.
			if (g_target == FrontEndTarget::Map && s.extraCrossOnFirstMap &&
				!g_mapOpenedThisSession) {
				g_mapOpenedThisSession = true;
				g_phase = Phase::DismissHold;
				g_remaining = Frames(s.enterFrames);
				g_mask = CTRL_CROSS;
				g_status = "clearing legend";
				break;
			}
			// Already inside the page. Pressing Cross here would not descend, it would activate
			// whatever is selected - on the Game page, LOAD GAME.
			Finish("arrived");
			break;
		}
		if (++g_presses > kMaxEnterPresses) {
			// Focus will not move. Leaving the player on the page at strip level is a working
			// outcome; pressing a select button repeatedly at a menu that is ignoring it is not.
			Finish("could not enter page");
			break;
		}
		g_phase = Phase::DismissHold;
		g_remaining = Frames(s.enterFrames);
		g_mask = CTRL_CROSS;
		break;

	case Phase::DismissHold:
		if (--g_remaining > 0) {
			break;
		}
		g_mask = 0;
		g_phase = Phase::DismissGap;
		g_remaining = Frames(s.gapFrames);
		break;

	case Phase::DismissGap:
		if (--g_remaining > 0) {
			break;
		}
		// Back to the check rather than straight to done - that is what makes this a loop.
		g_phase = Phase::EnterWait;
		g_remaining = Frames(s.gapFrames);
		break;

	case Phase::TabGap:
		if (--g_remaining > 0) {
			break;
		}
		if (!GameMenuActive()) {
			// The player closed it mid-walk, or a press landed on something that did. Their menu,
			// their call.
			GoIdle("menu closed");
			break;
		}
		{
			const int page = GameMenuPage();
			if (page == g_wantPage) {
				Arrive("arrived");
				break;
			}
			if (++g_presses >= Frames(s.maxTabPresses)) {
				// Two full laps without landing on it. Either the page number is wrong or the
				// index does not mean what this code thinks - and in both cases the player is
				// sitting in the game's own menu, which is a working outcome rather than a
				// broken one.
				Finish(page == g_lastSeenPage ? "page never moved" : "page not found");
				g_OSD.Show(OSDType::MESSAGE_WARNING,
					page == g_lastSeenPage
						? "Could not tab the game's menu - see the VCS debugger"
						: "Could not find that page - see the VCS debugger",
					4.0f, "vcs_frontend");
				break;
			}
			g_lastSeenPage = page;
			g_phase = Phase::TabHold;
			g_remaining = Frames(s.holdFrames);
			NextPress(page);
		}
		break;

	default:
		break;
	}
}

void FrontEndReset() {
	RestoreChrome();
	GoIdle("idle");
	g_target = FrontEndTarget::Menu;
	g_wantPage = -1;
	g_lastSeenPage = -1;
	g_seenInRow = 0;
	g_mapOpenedThisSession = false;
	g_waypointFrames = 0;
	g_zoomFrames = 0;
	g_zoomMask = 0;
	g_dragHeld = false;
	g_spaceHeld = false;
	g_dragTravel = 0.0f;
	g_haveFrame = false;
	g_lastFrame = 0;
	g_vblanks = 0;
	g_request.store(-1, std::memory_order_relaxed);
	g_saveRequest.store(false, std::memory_order_relaxed);
}

const char *FrontEndStatus() {
	return g_status;
}

}  // namespace VCS
