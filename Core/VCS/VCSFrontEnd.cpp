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
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/File/DirListing.h"
#include "Common/File/Path.h"
#include "Common/StringUtils.h"
#include "Core/HLE/sceCtrl.h"
#include "Common/Input/KeyCodes.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include <mutex>
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSCheats.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSSaveDialog.h"

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

	// The load sequence, past the point the map and stats rows stop at. Both of these look at
	// the page's SELECTED WIDGET by name rather than counting presses, for the reason the chrome
	// pass works by name: the entries are LoadGame_MI and BTN_SPECIAL_CONFIRM whatever order the
	// page happens to build them in.
	LoadPick,     // inside the Game page, putting the highlight on LOAD GAME
	LoadPrompt,   // on the "all unsaved progress will be lost" page, answering it

	// The firmware's savedata dialog - the slot list, the overwrite prompt, the acknowledgement.
	// Paced in VBLANKS, not game frames: the world is stopped behind that dialog, so the clock
	// every other phase here runs on has stopped with it.
	Dialog,

	// A press with somewhere to go afterwards. The phases above predate this and keep their own
	// hold/gap pairs; there was no reason to rewrite them.
	PressHold,
	PressGap,

	// Nothing left to press. A sequence that ends without a dialog behind it - NEW GAME - lands
	// here rather than on Idle directly, so the tidy-up in GoIdle still happens.
	EndSequence,
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

// --- The load and save sequences --------------------------------------------------------------

// Where a scheduled press goes when it is done, and how long it lasts. `g_pressOnVblank` decides
// which clock counts it down - the game's logic frames for the menu steps, vblanks for the
// firmware dialog, which the game's clock does not run behind.
Phase g_pressReturn = Phase::Idle;
int g_pressHold = 0;
int g_pressGap = 0;
bool g_pressOnVblank = false;

// A watchdog on a press counted in GAME frames, because that clock can stop with the button still
// down.
//
// NEW GAME is where this was found and it is the clearest case of it: confirming tears the world
// down, the frame counter stops dead, and the hold never finishes - so the game comes back up
// with Cross held, and hangs on a black screen with its own confirm page still open and the
// counter racing. The same presses sent from outside, released on a wall clock, start a new game
// every time; that control run is what separated our timing from the game's behaviour.
//
// Three quarters of a second, which is far longer than any press here wants and far shorter than
// the load it must not outlive. It exists for correctness, not for pacing: when the game's clock
// is running, the frame count always wins.
int g_pressVblanks = 0;
constexpr int kPressVblankLimit = 45;

// Bounded everywhere, because every one of these loops is "press something and look again" and
// the failure mode without a bound is a fork holding the player's pad forever.
int g_seqPresses = 0;
int g_dialogWaited = 0;
bool g_sawDialog = false;
constexpr int kMaxSeqPresses = 12;

// Skip the boot's auto-load exactly once, because this boot IS the new game.
//
// **Deliberately not cleared by FrontEndReset**, which is the one piece of state here that is
// not: Reset runs from VCS::Init, i.e. on the very boot this flag exists to talk to. Clearing it
// there would be clearing the message on delivery.
bool g_skipAutoLoadOnce = false;

// And the same message for the GAME's own boot autoload, which is a different thing entirely and
// has to be told separately. See TakeNewGameBoot in VCSSaveDialog.h. Not cleared by FrontEndReset
// either, and for the same reason.
bool g_newGameBoot = false;

// Which save the load is after: a slot number, or -1 for "whatever the firmware's list arrives
// on", which is the most recent and is what the auto-load wants. Set by the request, read in the
// dialog.
int g_wantSlot = -1;

// Retries of the Start press, for the auto-load only. The boot seam lands in the opening scene,
// where the first Start may be spent skipping something rather than opening the menu.
int g_startAttempts = 0;
constexpr int kMaxStartAttempts = 3;

// --- The curtain over the boot load -----------------------------------------------------------
//
// Atomic because the input gate reads it from the input thread; everything else here is the emu
// thread's.
std::atomic<int> g_curtainKind{(int)CurtainKind::None};
bool g_curtainSawSequence = false;
int g_curtainSettle = 0;
int g_curtainVblanks = 0;

// Game frames of a running world before the curtain comes down. The load hands back a world that
// is still streaming itself in, and a second of that is the difference between arriving somewhere
// and watching it arrive.
constexpr int kCurtainSettleFrames = 30;

// And a hard ceiling in vblanks, because a curtain that can get stuck is a game that cannot be
// played: it hides the screen AND holds the pad. Every timeout inside the sequence is far shorter
// than this, so reaching it means something happened that none of them describe.
constexpr int kCurtainMaxVblanks = 1800;

// Its own read of the game clock, for the reason the auto-save has one: the edge is consumed by
// whoever reads it.
u32 g_curtainFrame = 0;
bool g_haveCurtainFrame = false;

bool GameFrameAdvancedForCurtain() {
	const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
	if (!frame) {
		return false;
	}
	if (!g_haveCurtainFrame) {
		g_haveCurtainFrame = true;
		g_curtainFrame = *frame;
		return false;
	}
	if (*frame == g_curtainFrame) {
		return false;
	}
	g_curtainFrame = *frame;
	return true;
}

void DropCurtain() {
	g_curtainKind.store((int)CurtainKind::None, std::memory_order_relaxed);
	g_curtainSawSequence = false;
	g_curtainSettle = 0;
	g_curtainVblanks = 0;
	g_haveCurtainFrame = false;
}

// Up, and with a word on it. Raised where the automatic sequences are ASKED for rather than where
// they start, so there is no frame of the game's menu opening in front of the player.
void RaiseCurtain(CurtainKind kind) {
	g_curtainSawSequence = false;
	g_curtainSettle = kCurtainSettleFrames;
	g_curtainVblanks = 0;
	g_haveCurtainFrame = false;
	g_curtainKind.store((int)kind, std::memory_order_relaxed);
}

void CurtainTick() {
	if (g_curtainKind.load(std::memory_order_relaxed) == (int)CurtainKind::None) {
		return;
	}
	if (++g_curtainVblanks >= kCurtainMaxVblanks) {
		DropCurtain();
		return;
	}
	// Still walking - or not started yet, which is the tick or two between the request being
	// stored and the machine taking it. Both are "keep it up", and telling them apart is what
	// g_curtainSawSequence is for: without it the gap before the walk starts looks exactly like
	// the walk having finished.
	if (!g_curtainSawSequence || g_phase != Phase::Idle) {
		g_curtainSettle = kCurtainSettleFrames;
		return;
	}
	if (GameFrameAdvancedForCurtain() && --g_curtainSettle <= 0) {
		DropCurtain();
	}
}

// The mission-passed watch. Eight bytes, because the keys differ in their last character.
u32 g_missionKeyLo = 0;
u32 g_missionKeyHi = 0;
bool g_haveMissionKey = false;
u32 g_asLastFrame = 0;
int g_autoSaveDelay = 0;
bool g_autoSaveArmed = false;
int g_autoSaveWaited = 0;
// Game frames after a world restart during which the mission key is re-baselined and nothing can
// arm. See MissionWatchTick for the save this prevented.
int g_missionSettle = 0;
constexpr int kMissionSettleFrames = 120;
// A minute of game frames. If the world never becomes a good moment to save in - the player is
// in another menu, or a cheat sequence never ends - the auto-save gives up rather than firing at
// some unrelated moment much later.
constexpr int kAutoSavePatience = 1800;

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
bool WidgetName(u32 widget, char *out, size_t size);

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

// The third consumer of the same edge, and it needs its own copy for the reason the waypoint
// does: the edge is CONSUMED by whoever reads it, so a shared one would have the state machine
// and the auto-save delay each seeing half the frames.
//
// The auto-save's delay is counted on the game's clock rather than in vblanks on purpose. That
// clock stops for a load and for the fades a mission ends into, so a delay measured on it waits
// for the world to be running again instead of expiring inside a black screen.
u32 g_asFrame = 0;
bool g_haveAsFrame = false;

bool GameFrameAdvancedForAutoSave() {
	const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
	if (!frame) {
		return false;
	}
	if (!g_haveAsFrame) {
		g_haveAsFrame = true;
		g_asFrame = *frame;
		return false;
	}
	if (*frame == g_asFrame) {
		return false;
	}
	g_asFrame = *frame;
	return true;
}

int Frames(int value) {
	return std::max(value, 1);
}

// The page object the front end is showing, or 0. The same lookup the game's own accessor at
// 0x0882e950 does, minus the root-page branch: everything here works on a real page.
u32 CurrentPageObject() {
	const int page = GameMenuPage();
	if (page < 0) {
		return 0;
	}
	const std::optional<u32> begin = ReadAddrU32(VCSAddr::MenuPagesBegin);
	const std::optional<u32> end = ReadAddrU32(VCSAddr::MenuPagesEnd);
	if (!begin || !end || *end < *begin || (u32)page >= (*end - *begin) / 4) {
		return 0;
	}
	return ReadU32(*begin + (u32)page * 4).value_or(0);
}

// What the page has highlighted, by name. This is what makes the load sequence a closed loop
// rather than a count of presses: every step asks "is the thing I want selected yet".
bool SelectedWidgetName(char *out, size_t size) {
	out[0] = '\0';
	const u32 page = CurrentPageObject();
	if (page == 0) {
		return false;
	}
	const std::optional<u32> sel = ReadU32(page + kVCSWidgetSelected);
	if (!sel || *sel == 0) {
		return false;
	}
	return WidgetName(*sel, out, size);
}

// Hold a button, release it, then go to `back`. Both halves are counted on whichever clock the
// caller says, because the firmware's dialog runs with the game's logic stopped.
void Press(u32 mask, Phase back, int hold, int gap, bool onVblank) {
	g_pressVblanks = 0;
	g_mask = mask;
	g_pressHold = Frames(hold);
	g_pressGap = Frames(gap);
	g_pressReturn = back;
	g_pressOnVblank = onVblank;
	g_phase = Phase::PressHold;
}

// Give up on a sequence and put the player back in the WORLD rather than leaving them standing
// in a menu they never opened - which is what an auto-load failing halfway would otherwise do,
// on the first screen of a session, to someone who has pressed nothing.
//
// The close goes through the ordinary request path so it is taken from Idle on the next tick,
// exactly like a row asking for it.
void AbandonSequence(const char *why, const char *message);

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

void RestoreScriptAfterSave();

void GoIdle(const char *why) {
	// Every way a sequence ends comes through here, which is why the save's script flags are put
	// back here rather than on the one path that succeeds. Leaving `$ONMISSION` at 1 because a
	// save was abandoned halfway would quietly stop every mission trigger in the city.
	RestoreScriptAfterSave();
	g_bridgeOwnsMenu = false;
	ShowPagesAgain();
	g_phase = Phase::Idle;
	g_mask = 0;
	g_presses = 0;
	g_waited = 0;
	g_remaining = 0;
	g_seqPresses = 0;
	g_dialogWaited = 0;
	g_sawDialog = false;
	g_startAttempts = 0;
	g_status = why;
}

void AbandonSequence(const char *why, const char *message) {
	if (message) {
		g_OSD.Show(OSDType::MESSAGE_WARNING, message, 4.0f, "vcs_frontend");
	}
	const bool menuUp = GameMenuActive();
	GoIdle(why);
	if (menuUp) {
		g_request.store((int)FrontEndTarget::Close, std::memory_order_relaxed);
	}
}

// --- Driving the firmware's savedata dialog ---------------------------------------------------
//
// One phase rather than five, because the dialog is already a state machine and this is the
// closed loop around it: look at what is on screen, press the one thing that moves it on, look
// again. See Core/VCS/VCSSaveDialog.h for where the answer comes from and why a fork needs it.
//
// Everything about it is bounded - the wait for it to appear, the number of presses, the walk to
// a slot - because it can end up on screens this does not drive: a memory stick error, a "no
// data" notice, a mode nobody here anticipated. The player's pad is taken away while this runs,
// so the sequence has to be able to give up.
void DialogTick() {
	const VCSFrontEndSettings &s = FrontEndSettings();

	SaveDialogPeek peek;
	const bool up = PeekSaveDialog(&peek);

	if (!up) {
		if (g_sawDialog) {
			// It came and went. For a load that means the world is restarting under us, and for
			// a save that the write is done - either way there is nothing left to press.
			//
			// The game's own menu is usually gone with it, and where it is not - a save that
			// returns to the page it was asked from - closing it is the difference between
			// "saved" and "saved, and now you are in a menu".
			const bool menuUp = GameMenuActive();
			GoIdle(g_target == FrontEndTarget::Load ? "loaded" : "saved");
			if (menuUp) {
				g_request.store((int)FrontEndTarget::Close, std::memory_order_relaxed);
			}
			return;
		}
		if (++g_dialogWaited >= Frames(s.dialogTimeoutFrames)) {
			AbandonSequence("dialog never appeared", "The save browser did not open");
		}
		return;
	}

	g_sawDialog = true;
	if (++g_dialogWaited >= Frames(s.dialogTimeoutFrames)) {
		// Up, but not moving through anything this knows how to answer. Hand it back rather than
		// pressing at it: the dialog is the player's now, and it is on screen for them to use.
		GoIdle("dialog not answered");
		g_OSD.Show(OSDType::MESSAGE_WARNING, "Finish in the save browser - see the VCS debugger",
			5.0f, "vcs_frontend");
		return;
	}

	// Never press into IO or a fade. A press the screen underneath does not see is one this
	// sequence goes on believing it made.
	if (peek.busy) {
		return;
	}

	const int hold = s.dialogHoldFrames;
	const int gap = s.dialogGapFrames;
	// The dialog's own idea of which button is which, because it is a firmware setting and not a
	// constant. Falling back to the western layout only matters if a build ever reports neither.
	const u32 ok = peek.okButton ? peek.okButton : (u32)CTRL_CROSS;
	const u32 back = peek.cancelButton ? peek.cancelButton : (u32)CTRL_CIRCLE;

	if (peek.list) {
		// Which entry to take. A save always walks to its dedicated slot; a load takes the one
		// the game asked the firmware to focus - the LATEST, which is the whole definition of
		// "continue" - unless the player picked a particular one out of our own save list, in
		// which case it walks there the same way.
		//
		// Walked one press at a time watching the index, rather than counted, for the reason
		// every other loop here watches something: a list that scrolls differently than expected
		// stops the walk instead of running off the end of it.
		const bool steer = peek.save || g_wantSlot >= 0;
		if (steer && peek.selected >= 0 && peek.count > 0) {
			int want = peek.save ? s.autoSaveSlot : g_wantSlot;
			if (want < 0) want = 0;
			if (want >= peek.count) want = peek.count - 1;
			if (peek.selected != want) {
				if (++g_seqPresses > kMaxSeqPresses) {
					GoIdle("could not reach the slot");
					g_OSD.Show(OSDType::MESSAGE_ERROR, "Could not reach that save slot", 4.0f,
						"vcs_frontend");
					return;
				}
				Press(peek.selected > want ? CTRL_UP : CTRL_DOWN, Phase::Dialog, hold, gap, true);
				return;
			}
		}
		Press(ok, Phase::Dialog, hold, gap, true);
		return;
	}

	if (peek.confirm) {
		// The prompt opens on NO and LEFT is what moves it to YES. An OK sent without that press
		// cancels the very save it was meant to confirm.
		if (peek.yesno != 1) {
			Press(CTRL_LEFT, Phase::Dialog, hold, gap, true);
			return;
		}
		Press(ok, Phase::Dialog, hold, gap, true);
		return;
	}

	if (peek.done) {
		// BACK, not OK. "Save completed" offers one button and it is that one - measured, with
		// the save already on the memory stick and the sequence pressing OK at a screen that
		// ignores it until the whole thing timed out.
		Press(back, Phase::Dialog, hold, gap, true);
		return;
	}
}

// --- Making a save a save ---------------------------------------------------------------------
//
// The request byte opens the save menu; it does not prepare a save. The script does that, in the
// eight instructions before it calls `0260`, and without them the file that gets written loads
// into the opening mission with your money and your clothes on. See the note over
// kVCSGlobalLoadedGame in VCSAddresses.h for the decoded original and the evidence.
//
// So this does what those instructions do, and puts back what it changed afterwards - which is
// also what the script does, four lines further down its own routine.

u32 g_scriptGlobals = 0;      // the space these were written in, so the restore cannot cross a load
bool g_savePrepared = false;
u32 g_savedOnMission = 0;
u32 g_savedLoadedFlag = 0;

u32 GlobalAddr(u32 base, u32 index) {
	return base + index * 4;
}

// Returns false when the script space cannot be read, which is the one case where asking for a
// save is worse than not: the menu would open and write a file that starts the game over.
bool PrepareScriptForSave() {
	const std::optional<u32> space = ReadAddrU32(VCSAddr::ScriptSpace);
	if (!space || *space == 0) {
		return false;
	}
	const u32 g = *space;
	const std::optional<u32> onMission = ReadU32(GlobalAddr(g, kVCSGlobalOnMission));
	const std::optional<u32> loaded = ReadU32(GlobalAddr(g, kVCSGlobalLoadedGame));
	if (!onMission || !loaded) {
		return false;
	}

	// The restart position, copied exactly as `$284 = $783` does. `$783..785` is where the last
	// save pickup was collected, and the load path copies it straight back out again.
	float x = 0.0f, y = 0.0f, z = 0.0f;
	const std::optional<float> sx = ReadFloat(GlobalAddr(g, kVCSGlobalSavePointX));
	const std::optional<float> sy = ReadFloat(GlobalAddr(g, kVCSGlobalSavePointX + 1));
	const std::optional<float> sz = ReadFloat(GlobalAddr(g, kVCSGlobalSavePointX + 2));
	if (sx && sy && sz) {
		x = *sx; y = *sy; z = *sz;
	}
	// Zero means the player has never used a safe house in this game, which for an auto-save
	// fired by a mission is entirely possible - the first one passes long before anyone walks
	// into a save icon. Restarting at the map's origin is not a place; where they are standing
	// is. The game's own routine cannot meet this case, because collecting the pickup is what
	// sets those globals in the first place.
	if (x == 0.0f && y == 0.0f && z == 0.0f) {
		if (const std::optional<u32> player = ReadAddrU32(VCSAddr::PlayerBase)) {
			const std::optional<float> px = ReadFloat(*player + kVCSEntityPositionOffset);
			const std::optional<float> py = ReadFloat(*player + kVCSEntityPositionOffset + 4);
			const std::optional<float> pz = ReadFloat(*player + kVCSEntityPositionOffset + 8);
			if (px && py && pz) {
				x = *px; y = *py; z = *pz;
			}
		}
	}

	g_scriptGlobals = g;
	g_savedOnMission = *onMission;
	g_savedLoadedFlag = *loaded;
	g_savePrepared = true;

	WriteU32(GlobalAddr(g, kVCSGlobalOnMission), 1);
	WriteU32(GlobalAddr(g, kVCSGlobalLoadedGame), 1);
	WriteFloat(GlobalAddr(g, kVCSGlobalRestartX), x);
	WriteFloat(GlobalAddr(g, kVCSGlobalRestartX + 1), y);
	WriteFloat(GlobalAddr(g, kVCSGlobalRestartX + 2), z);
	return true;
}

// Put the two flags back, whatever happened to the save. Their PREVIOUS values rather than zero:
// the script's own routine can assume it was called with no mission running, and this cannot -
// an auto-save that gave up halfway must not be the reason a mission stops being one.
//
// Skipped when the script space has moved under us, which is what a load looks like: the flags
// belong to a game that no longer exists, and the game that replaced it has its own.
void RestoreScriptAfterSave() {
	if (!g_savePrepared) {
		return;
	}
	g_savePrepared = false;
	const std::optional<u32> space = ReadAddrU32(VCSAddr::ScriptSpace);
	if (!space || *space != g_scriptGlobals) {
		return;
	}
	WriteU32(GlobalAddr(g_scriptGlobals, kVCSGlobalOnMission), g_savedOnMission);
	WriteU32(GlobalAddr(g_scriptGlobals, kVCSGlobalLoadedGame), g_savedLoadedFlag);
}

// Whether the memory stick holds a save for this disc at all. Asked before the auto-load presses
// anything, because the alternative is walking the whole menu to a list with nothing in it and
// then having to find the way back out of a screen this does not drive.
bool AnySaveOnMemoryStick() {
	const std::string &disc = GetDiscID();
	if (disc.empty()) {
		return false;
	}
	const Path dir = GetSysDirectory(PSPDirectories::DIRECTORY_SAVEDATA);
	std::vector<File::FileInfo> entries;
	if (!File::GetFilesInDir(dir, &entries)) {
		return false;
	}
	for (const File::FileInfo &e : entries) {
		if (!e.isDirectory || !startsWith(e.name, disc)) {
			continue;
		}
		// PARAM.SFO rather than the game's own DATA.BIN: it is what makes a directory a save as
		// far as the firmware is concerned, and it is what the list the dialog builds reads.
		if (File::Exists(dir / e.name / "PARAM.SFO")) {
			return true;
		}
	}
	return false;
}

// The mission-passed watch, once per tick.
//
// The trigger is the eight-byte GXT key at LatestMissionKey, which `01EB register_mission_passed`
// writes - see the note over the address. Odd jobs move the counter beside it and leave the key
// alone, so races and empire work deliberately do not fire this.
void MissionWatchTick() {
	const std::optional<u32> lo = ReadAddrU32(VCSAddr::LatestMissionKey);
	const std::optional<u32> hi = ReadAddrU32(VCSAddr::LatestMissionKey2);
	if (!lo || !hi) {
		g_haveMissionKey = false;
		return;
	}

	// A world that RESTARTED - a load, or NEW GAME - brings a different key with it, and the
	// frame counter rewinding is how that is told from a mission being passed.
	//
	// **The rewind and the key are not on the same tick**, and that cost a spurious save before
	// it was written down: the counter goes back to 1 the instant the world starts, and the save
	// data lands in the globals somewhere after it, so a check that only disarmed on the tick of
	// the rewind saw a perfectly ordinary key change a moment later and called it a mission. Seen
	// in the wild - slot 0 written four seconds after a boot in which nothing was played.
	//
	// So a restart opens a WINDOW rather than firing once, and while it is open the key is
	// re-baselined every tick and nothing can arm. Four seconds, which is longer than the gap has
	// ever been and far shorter than the quickest mission.
	bool restarted = false;
	if (const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter)) {
		restarted = *frame < g_asLastFrame;
		g_asLastFrame = *frame;
	}
	if (restarted) {
		g_missionSettle = kMissionSettleFrames;
		g_autoSaveArmed = false;
		g_autoSaveDelay = 0;
	}

	const bool changed = g_haveMissionKey && (*lo != g_missionKeyLo || *hi != g_missionKeyHi);
	g_missionKeyLo = *lo;
	g_missionKeyHi = *hi;
	const bool first = !g_haveMissionKey;
	g_haveMissionKey = true;

	if (g_missionSettle > 0) {
		// Counted on the game's clock like the delay, so a settle cannot expire during a load -
		// the counter is not moving then, and the world it is waiting for has not arrived.
		//
		// It shares the auto-save's frame consumer, which is safe by construction rather than by
		// luck: the two are mutually exclusive. A settle clears `armed` when it opens and nothing
		// can arm while it is open, so the frame edge is never wanted by both.
		if (GameFrameAdvancedForAutoSave()) {
			g_missionSettle--;
		}
		return;
	}
	if (first || !changed || *lo == 0) {
		return;
	}
	if (!FrontEndSettings().autoSaveOnMissionPassed || g_autoSaveArmed) {
		return;
	}
	g_autoSaveArmed = true;
	g_autoSaveDelay = Frames(FrontEndSettings().autoSaveDelayFrames);
	g_autoSaveWaited = 0;
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
	// Forwarded rather than poking the request byte directly, which is what this used to do. On
	// its own that byte opens the save menu and writes a file that loads into the opening
	// mission - see PrepareScriptForSave. There is one way to save here now, and it is the one
	// that works.
	RequestGameMenu(FrontEndTarget::Save);
}

bool RequestAutoLoad() {
	if (!FrontEndSettings().autoLoadOnBoot) {
		return false;
	}
	// The player asked for a new game and this is the boot they asked for it with.
	if (g_skipAutoLoadOnce) {
		g_skipAutoLoadOnce = false;
		return false;
	}
	// The filesystem question is answered HERE, on the thread that asked, rather than inside the
	// sequence: it is the one part of this that has nothing to do with the game, and a first run
	// with an empty memory stick should press nothing at all rather than walk to an empty list.
	if (!AnySaveOnMemoryStick()) {
		return false;
	}
	// -1: take the entry the firmware's list arrives on, which VCS asks to be the latest save.
	g_wantSlot = -1;
	RaiseCurtain(CurtainKind::Loading);
	RequestGameMenu(FrontEndTarget::Load);
	return true;
}

void RequestLoadSlot(int slot) {
	// The slot goes in before the request, not with it: the request is one atomic int and this is
	// the second half of the same instruction as far as the sequence is concerned. Nothing can
	// read it in between - FrontEndTick takes the request on the emu thread, and it is the only
	// thing that reads either.
	g_wantSlot = slot;
	RaiseCurtain(CurtainKind::Loading);
	RequestGameMenu(FrontEndTarget::Load);
}

void RequestNewGame() {
	// A reboot of the disc, not a walk to NEW GAME - and the reason is worth keeping, because the
	// walk was written first and it works right up until the moment it matters.
	//
	// Confirming the game's own NEW GAME tears the world down. The bridge's presses are paced on
	// the game's logic clock, and that clock STOPS during the teardown, so the sequence cannot
	// finish and the game comes back with the front end still open on a black screen, its frame
	// counter racing and nothing on screen. Reproduced every time; a watchdog that releases the
	// button on the wall clock did not fix it, which is what ruled out the stuck press and left
	// the teardown itself as the thing not to be standing in the middle of.
	//
	// And the reboot is not a workaround, it is the shorter road to the same place. **VCS has no
	// new-game screen**: it boots logos, credits, then walks itself into the story - measured, in
	// "The boot sequence" - so starting the disc again IS starting a new game, on the path this
	// port already takes every single launch. The only thing that has to be said is "do not load
	// anything this time".
	g_skipAutoLoadOnce = true;
	g_newGameBoot = true;
	System_PostUIMessage(UIMessage::REQUEST_GAME_RESET);
}

bool TakeNewGameBoot() {
	const bool want = g_newGameBoot;
	g_newGameBoot = false;
	return want;
}

CurtainKind AutoCurtain() {
	return (CurtainKind)g_curtainKind.load(std::memory_order_relaxed);
}

bool AutoSavePending() {
	return g_autoSaveArmed;
}

int AutoSaveDelayLeft() {
	return g_autoSaveDelay;
}

void LatestMissionKey(char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	const std::optional<u32> lo = ReadAddrU32(VCSAddr::LatestMissionKey);
	const std::optional<u32> hi = ReadAddrU32(VCSAddr::LatestMissionKey2);
	if (!lo || !hi) {
		return;
	}
	const u32 words[2] = { *lo, *hi };
	size_t n = 0;
	for (size_t i = 0; i < 8 && n + 1 < size; i++) {
		const char c = (char)((words[i / 4] >> ((i % 4) * 8)) & 0xff);
		if (c == 0) {
			break;
		}
		out[n++] = (c >= 32 && c < 127) ? c : '?';
	}
	out[n] = '\0';
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
	// The load and save sequences hold the pad for the same reason the walk does, and they hold
	// it for longer: a stray press from the player lands in a list of save slots.
	case Phase::LoadPick:
	case Phase::LoadPrompt:
	case Phase::Dialog:
	case Phase::PressHold:
	case Phase::PressGap:
	case Phase::EndSequence:
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

	// The curtain first of all: it hides a walk that has not started yet as readily as one in
	// progress, and every path below this can return early.
	CurtainTick();

	// The mission-passed watch and the auto-save's delay, both before anything that can return
	// early below - a tick that finds nothing to do is still a tick a mission can have been
	// passed on.
	MissionWatchTick();
	if (g_autoSaveArmed && GameFrameAdvancedForAutoSave()) {
		if (g_autoSaveDelay > 0) {
			g_autoSaveDelay--;
		} else if (g_phase == Phase::Idle && !GameMenuActive() && !CheatEntryInProgress()) {
			// Only from a standing start: the world running, no menu up, nothing else driving the
			// pad. Anything else and it waits, because a save asked for on top of another
			// sequence is two things pressing buttons at one game.
			g_autoSaveArmed = false;
			// Behind the curtain, same as the boot load and for the same reason: what is about to
			// be on screen is the game's save menu opening by itself and a firmware dialog being
			// walked through. RequestSaveMenu deliberately does not do this - a player who asked
			// for the save menu went looking for it.
			RaiseCurtain(CurtainKind::Saving);
			g_request.store((int)FrontEndTarget::Save, std::memory_order_relaxed);
			g_OSD.Show(OSDType::MESSAGE_INFO, "Auto-saving", 2.0f, "vcs_frontend");
		} else if (++g_autoSaveWaited >= kAutoSavePatience) {
			// A minute of never being a good moment. Firing much later, at some unrelated point,
			// would be worse than not firing at all.
			g_autoSaveArmed = false;
			g_status = "auto-save gave up waiting";
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
		g_seqPresses = 0;
		g_dialogWaited = 0;
		g_sawDialog = false;
		g_startAttempts = 0;
		// A sequence has begun, which is what the curtain waits to see before it starts counting
		// its way back down. It covers whichever one raised it and nothing else - a request that
		// arrives with no curtain up simply never sets this. See CurtainTick.
		g_curtainSawSequence = true;

		if (g_target == FrontEndTarget::Save) {
			// The save UI has an inbox and the load list does not, so this half walks nothing at
			// all: one write, and the game's own front end does the opening. From there it is the
			// same dialog the load ends at, driven the same way.
			//
			// The script state goes FIRST and the request second, in that order for the reason
			// the script does it in that order: the menu can start writing the file the moment it
			// is asked for, and a flag set afterwards is a flag that did not get saved.
			if (!PrepareScriptForSave()) {
				GoIdle("script globals unreadable");
				g_OSD.Show(OSDType::MESSAGE_ERROR, "Could not prepare the save", 3.0f,
					"vcs_frontend");
				return;
			}
			if (!WriteAddrU8(VCSAddr::SaveMenuRequest, 1)) {
				RestoreScriptAfterSave();
				GoIdle("save menu address unset");
				g_OSD.Show(OSDType::MESSAGE_ERROR, "Save menu address unset", 3.0f,
					"vcs_frontend");
				return;
			}
			g_phase = Phase::Dialog;
			g_mask = 0;
			g_status = "saving";
			return;
		}

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
		// LOAD GAME is an entry ON the Game page, not a page of its own, so the load walks to
		// exactly where the GAME row walks to and then carries on pressing.
		case FrontEndTarget::Load:
			g_wantPage = s.gamePage;
			break;
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

	// The savedata half runs on VBLANKS and returns before the gate below, because the world is
	// stopped behind that dialog: the game's logic clock, which everything else in this machine
	// is paced by, has stopped with it and would never hand out another frame.
	if (g_phase == Phase::Dialog) {
		DialogTick();
		return;
	}
	// The watchdog above, ticked on the only clock that cannot stop. A press that has outlived it
	// is released and moved on whatever the game's counter is doing.
	if (!g_pressOnVblank && (g_phase == Phase::PressHold || g_phase == Phase::PressGap)) {
		if (++g_pressVblanks >= kPressVblankLimit) {
			g_pressVblanks = 0;
			if (g_phase == Phase::PressHold) {
				g_mask = 0;
				g_phase = Phase::PressGap;
			} else {
				g_phase = g_pressReturn;
			}
			return;
		}
	}
	if (g_pressOnVblank && (g_phase == Phase::PressHold || g_phase == Phase::PressGap)) {
		if (g_phase == Phase::PressHold) {
			if (--g_pressHold <= 0) {
				g_mask = 0;
				g_phase = Phase::PressGap;
			}
		} else if (--g_pressGap <= 0) {
			g_phase = g_pressReturn;
		}
		return;
	}

	if (!GameFrameAdvanced()) {
		return;
	}

	const VCSFrontEndSettings &s = FrontEndSettings();

	switch (g_phase) {
	case Phase::PressHold:
		if (--g_pressHold <= 0) {
			g_mask = 0;
			g_phase = Phase::PressGap;
		}
		break;

	case Phase::PressGap:
		if (--g_pressGap <= 0) {
			g_phase = g_pressReturn;
		}
		break;

	case Phase::EndSequence:
		GoIdle("done");
		break;

	// Put the highlight on LOAD GAME and press it. The Game page opens with that entry selected
	// anyway - it is the first - so the usual path is one read and one Cross; the press up is
	// there for the case where it is not, and it is bounded because a page that will not move its
	// highlight is not going to start.
	case Phase::LoadPick: {
		if (!GameMenuActive()) {
			GoIdle("menu closed");
			break;
		}
		// Which entry, and which way to walk to it. The page is `LoadGame_MI, NewGame_MI,
		// DeleteGame_MI, GameTitle, Reset_MI` in that order and it arrives with the first one
		// selected - measured - so LOAD is already there and NEW is one press down. The name is
		// still checked every time rather than trusted: a press is only made because the
		// selection is not what was asked for yet.
		const char *want = "LoadGame_MI";
		char name[32];
		const bool have = SelectedWidgetName(name, sizeof(name));
		if (have && strcmp(name, want) == 0) {
			g_seqPresses = 0;
			g_waited = 0;
			Press(CTRL_CROSS, Phase::LoadPrompt, s.enterFrames, s.gapFrames, false);
			break;
		}
		if (++g_seqPresses > kMaxSeqPresses) {
			AbandonSequence("could not select the entry", "Could not reach that menu entry");
			break;
		}
		// A page that reports no selection at all gets the same press: moving is what makes it
		// name one.
		Press(CTRL_UP, Phase::LoadPick, s.holdFrames, s.gapFrames, false);
		break;
	}

	// "All unsaved progress in your current game will be lost. Proceed with loading?" - a page
	// the front end pushes, whose two entries are BTN_SPECIAL_CANCEL and BTN_SPECIAL_CONFIRM. It
	// opens on CANCEL, which is the right default for a player and one press short for us.
	case Phase::LoadPrompt: {
		if (!GameMenuActive()) {
			GoIdle("menu closed");
			break;
		}
		char name[32];
		const bool have = SelectedWidgetName(name, sizeof(name));
		if (!have || strncmp(name, "BTN_SPECIAL_", 12) != 0) {
			// Not up yet. Waiting is the whole answer: the press that asks for it has been made
			// and the page is built on the game's own schedule.
			if (++g_waited >= Frames(s.openTimeoutFrames)) {
				AbandonSequence("load prompt never appeared", "LOAD GAME did not respond");
			}
			break;
		}
		if (strcmp(name, "BTN_SPECIAL_CONFIRM") == 0) {
			g_seqPresses = 0;
			g_dialogWaited = 0;
			g_sawDialog = false;
			Press(CTRL_CROSS, Phase::Dialog, s.enterFrames, s.gapFrames, false);
			break;
		}
		if (++g_seqPresses > kMaxSeqPresses) {
			AbandonSequence("could not answer the prompt", "Could not answer the load prompt");
			break;
		}
		Press(CTRL_DOWN, Phase::LoadPrompt, s.holdFrames, s.gapFrames, false);
		break;
	}

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
			//
			// Except at the boot seam, which is the one place a first Start really can be spent
			// on something else: the world starts inside the opening scene, where the button
			// skips rather than opens. So the auto-load gets a few goes before it gives up, and
			// nothing else does.
			if (g_target == FrontEndTarget::Load && ++g_startAttempts < kMaxStartAttempts) {
				g_waited = 0;
				g_phase = Phase::PressStart;
				g_remaining = Frames(s.holdFrames);
				g_mask = CTRL_START;
				g_status = "pressing start again";
				break;
			}
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
			//
			// Which is exactly what the load wants next, so that is where it goes rather than
			// handing over: same walk, four more presses.
			if (g_target == FrontEndTarget::Load) {
				g_seqPresses = 0;
				g_waited = 0;
				g_phase = Phase::LoadPick;
				g_status = "picking load game";
				break;
			}
			Finish("arrived");
			break;
		}
		if (++g_presses > kMaxEnterPresses) {
			// Focus will not move. Leaving the player on the page at strip level is a working
			// outcome; pressing a select button repeatedly at a menu that is ignoring it is not.
			if (g_target == FrontEndTarget::Load) {
				AbandonSequence("could not enter the game page", "Could not open LOAD GAME");
				break;
			}
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
	g_pressOnVblank = false;
	g_pressHold = 0;
	g_pressGap = 0;
	g_pressReturn = Phase::Idle;
	// The mission key is dropped rather than kept: the next reading becomes the baseline, so a
	// game that boots with a key already in it - which is every load - does not read as a mission
	// having been passed while nobody was watching.
	g_haveMissionKey = false;
	g_missionKeyLo = 0;
	g_missionKeyHi = 0;
	g_asLastFrame = 0;
	g_haveAsFrame = false;
	g_autoSaveArmed = false;
	g_autoSaveDelay = 0;
	g_autoSaveWaited = 0;
	g_missionSettle = 0;
	g_wantSlot = -1;
	g_request.store(-1, std::memory_order_relaxed);
	// Dropped rather than restored: the game these belonged to is going away, and the write
	// would land in whatever replaces it.
	g_savePrepared = false;
	g_scriptGlobals = 0;
	DropCurtain();
}

const char *FrontEndStatus() {
	return g_status;
}

}  // namespace VCS
