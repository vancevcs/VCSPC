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

#include "Common/CommonTypes.h"

// A bridge into the game's own front end, so the map and the save list are reachable again.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS NEEDED AT ALL
//
// VCS keeps its map, its stats, its briefs and its save list on the pause menu the Start button
// opens - and this fork took Start for its own menu. Everything on that screen therefore became
// unreachable, which is a straight loss against the retail game. This gives it back.
//
// The two things asked for are one mechanism. "Show me the map" and "let me load a save" are the
// same request with a different page number, and both are answered by opening the game's front
// end and then walking to a page.
//
// ---------------------------------------------------------------------------------------------
// THE FRONT END, AS FOUND
//
// `FrontEndMenuManager` is a fixed global at `0x08bc9100`. Three of its fields matter here and
// all three are in the address table:
//
//   +0x1c  s8    the page showing, an index into the eleven-entry page vector at +0x04
//   +0x20  u8    whether the menu is up at all
//   +0x1e  u8    "use the root page instead of the index" - not needed here, but it is why
//                +0x1c reading -1 with the menu closed is a normal value rather than a bad read
//
// See "Reaching the game's own front end" in docs/VCS_ADDRESSES.md for how they were identified.
//
// ---------------------------------------------------------------------------------------------
// IT PRESSES START, IT DOES NOT SET THE FLAG
//
// Writing `MenuActive` would be one instruction and would be wrong. Opening that menu is not one
// boolean - the game builds page objects, stops the world, takes the pad, and starts its own
// music duck - and a flag set from outside claims all of it has happened. Pressing the button the
// player would have pressed gets the whole sequence for free, and it is the same doctrine the
// cheat menu follows and the same one the vault settled on: drive the mechanic, not the symptom.
//
// So the address table's job here is to *watch*, not to steer. `MenuActive` says when the menu
// arrived, and `MenuPage` says where we are - which is what turns the walk to a page from a
// timed guess into a closed loop that can tell whether it worked.
//
// ---------------------------------------------------------------------------------------------
// AND IT TABS, IT DOES NOT WRITE THE PAGE EITHER
//
// Same argument one level down. `MenuPage` is a plain index and writing it would very likely
// move the highlight without running whatever a page does on entry - the map has to build its
// texture, the save list has to enumerate the memory stick. So the walk presses the game's own
// navigation and *watches the index* until it arrives, giving up after a bounded number of
// presses.
//
// **The tabs are a GRID, not a ring, and that is the whole reason the walk is not a for-loop.**
// Measured live: d-pad RIGHT cycles within one row and wraps inside it - 0,1,2,3,4,0 along the
// top and 5,6,7,5 along the bottom - while UP and DOWN switch rows. So pressing right from the
// bottom row can never reach the top one, and a walk built on "press next enough times" would
// spin forever exactly half the time.
//
// The walk therefore presses right, and *remembers which pages it has seen since the last row
// change*. Landing on one twice means the row has been exhausted, so it presses down and starts
// a fresh pass. That is deliberately blind to how many rows there are or which page sits where -
// the only thing it knows is the target index, which is the only thing it was told.
//
// (R trigger was the first guess, from every other GTA pause menu. It moves nothing here, and
// the on-screen legend says so: `move` is drawn against the four d-pad glyphs.)

namespace VCS {

// What the player asked for. The page number comes from settings, not from here.
enum class FrontEndTarget {
	// Open the menu and stop. Always correct, needs no page number, and is what the other two
	// degrade to when their page is set to -1.
	Menu,
	Map,
	Brief,
	Game,
	Stats,

	// Shut the game's menu, so that leaving this fork's menu always means going back to the world.
	// Without it the two read as two menus stacked on each other: RESUME dropped you back onto
	// whichever game tab you had been looking at, rather than into the game.
	Close,
};

struct VCSFrontEndSettings {
	// Measured live, by walking the menu with the debugger attached and watching MenuPage:
	//
	//     top row     map(0)  brief(1)  game(2)  stats(3)  controls(4)
	//     bottom row  audio(5)  display(6)  multiplayer(7)
	//
	// `game` is the tab that holds NEW GAME / LOAD GAME / DELETE SAVE DATA, so it is one row here
	// rather than three - the three live on the page, and the page is what we open. The menu opens
	// on 0, which is why the map row usually arrives without pressing anything.
	//
	// Controls, audio and display are deliberately NOT offered: this fork has its own pages for
	// all three, and a second way in that edits the game's copies instead would be two settings
	// screens quietly disagreeing.
	//
	// Still settings rather than constants, because they are the numbers most likely to differ on
	// a build this fork does not target - and -1 in any of them means "just open the menu", which
	// is the graceful version of not knowing.
	int mapPage = 0;
	int briefPage = 1;
	int gamePage = 2;
	int statsPage = 3;

	// Set the page directly instead of pressing the game's own next-tab control until it arrives.
	//
	// **OFF, because it froze the game**, and the way it did is worth keeping. Writing the index
	// moves what is DRAWN and not what the front end thinks is selected: the page keeps its
	// selected widget in `+0x30`, and after a direct jump that widget belongs to the page you came
	// from. Pressing Enter on the Game page then made a virtual call through it -
	//
	//     08ae22a8  lw    $a0, 0x30($s0)     ; the selected widget
	//     08ae22b0  lw    $a2, 0x24($a0)     ; its function table
	//     08ae22bc  lw    $a2, 4($a2)        ; the method - read as 0
	//     08ae22c0  jalr  $a2
	//
	// - and `Invalid exec address 00000000 pc=00000000 ra=08ae22c8` is that `jalr`. The object was
	// a real widget of the wrong TYPE, which is why it survived two reads and died on the third.
	//
	// The verify-a-frame-later guard did not catch it and could not have: the index really did
	// hold. It was the only thing that did.
	//
	// Kept rather than deleted because the fast path is still the right idea - it just needs the
	// selection moved with the page, and that field is now known. Anyone turning this on should
	// expect the freeze until it also writes `+0x30`.
	bool jumpDirectlyToPage = false;

	// Hide the pages while walking to one, so the tabs stepped through on the way are not drawn.
	//
	// This is the flicker fix that replaced the direct jump. It touches only `visible` bytes -
	// the same mechanism as the chrome, already known to be safe - and changes nothing about what
	// the front end thinks is selected, which is exactly what the jump got wrong. The backdrop
	// stays up, so the walk reads as a pause rather than as a riffle through four pages.
	bool hidePagesWhileWalking = true;

	// Game frames each tab press is held and released, on the same clock and for the same reason
	// as the cheat sequencer - see Core/VCS/VCSCheats.h.
	int holdFrames = 2;
	int gapFrames = 3;

	// The map opens with its legend over the city and it never times out on its own. Cross puts
	// it away - the game's own `select` - so the bridge taps it on arrival.
	//
	// `dismissFrames` is deliberately the shortest thing the game can see as a press: HOLDING the
	// same button is what opens the map options, so a long one would dismiss the legend and then
	// open the panel it was advertising.
	// One Cross on arrival, and it does two jobs because they are the same job.
	//
	// The front end has two levels of focus: `MenuUseRoot` reads 1 while the TAB STRIP has focus
	// and 0 once you are inside the page. A page tabbed to is still at the strip, which is why
	// arriving on the Game page needed one more press before LOAD GAME would respond at all - and
	// on the map, the same press is what puts the legend away.
	//
	// **Only when MenuUseRoot reads 1.** Sent while already inside the page it does not descend,
	// it ACTIVATES the selected entry - measured: a second Cross on the Game page pushes
	// CONFIRM_PAGE, the "you will lose unsaved progress" prompt. The guard is not tidiness.
	//
	// Short on purpose too: the same button HELD opens the map options.
	bool enterPageOnArrival = true;
	int enterFrames = 2;

	// Drag the map with the mouse, the way San Andreas does it. Hold the button and the map
	// follows the cursor.
	//
	// Only works inside the page (MenuUseRoot == 0), which is where the d-pad pans instead of
	// changing tab - and is why an earlier hunt for the pan variables came up empty: it was
	// looking while the tab strip still had focus, where nothing pans because nothing is panning.
	bool mapDrag = true;
	float mapDragSpeed = 1.6f;

	// Space, or a click of the drag button that did not actually drag, places the game's own map
	// marker - the `place marker` on its hint bar, which is Square. Going through the game's
	// control rather than writing a waypoint means the marker is a real one: the radar draws it,
	// and anything that reads it - a route, eventually - sees what the game put there.
	//
	// The marker lands on the map's own cursor at the centre of the screen, not under the mouse.
	// Drag to bring somewhere to the middle, then place. Placing under the cursor would mean
	// panning first so that point IS the middle, which is worth doing later and is not what makes
	// this useful now.
	// The mouse wheel zooms the map, using the game's own zoom controls.
	//
	// L and R are what the game zooms with - found by accident, from Q falling through to the L
	// trigger in a menu and zooming out. They are settings rather than constants because that is
	// an inference from one observation, not a measurement: if a notch zooms the wrong way, swap
	// them here rather than anywhere else.
	bool mapZoomWithWheel = true;

	// Route to the game's objective marker ahead of a marker the player dropped, when both are up.
	//
	// ON, by request, and there is a known rough edge in it that is worth stating rather than
	// leaving to be rediscovered. `0x66` is not "a mission is sending you somewhere" - it is the
	// game's current OBJECTIVE marker, and between jobs the game keeps one pointing at the
	// player's safe house. So outside a mission this preference routes to that hint rather than
	// to a waypoint, which is a real complaint that has already been made once.
	//
	// The two cannot be told apart by kind: the safe-house hint carries the same flag byte and the
	// same empty icon id as a live mission target. Separating them needs either a field that
	// differs between the two - which wants one dump of the store taken while NOT on a mission,
	// to compare against the mid-mission one that identified `0x66` in the first place - or an
	// "is a mission running" script global, which has not been hunted for.
	//
	// Until then this is a straight preference, and turning it off gives waypoint-first with the
	// objective marker as the fallback.
	bool preferMissionMarker = true;
	int zoomInFrames = 2;

	bool mapWaypoint = true;
	// How far the mouse may move between press and release and still count as a click rather than
	// a drag, in mouse counts. Without it every drag would end by dropping a marker.
	float waypointClickSlop = 6.0f;

	// One extra Cross the first time the map is opened in a session.
	//
	// **A hypothesis, not a measurement**, and the only one here that is. The map's hint bar reads
	// `Legend X`, so Cross toggles the legend while you are inside the page - but the press that
	// gets you inside the page is also Cross, and it is spent descending. So the first open of a
	// session descends and leaves the legend up, while every later open finds it already off
	// because the game remembers the toggle. That matches the report exactly: only the very first
	// map open shows it.
	//
	// If it is wrong, the extra press does whatever a second Cross does on the map - visible
	// immediately, and this switch turns it off.
	bool extraCrossOnFirstMap = true;

	// Strip the game's menu chrome while it is up: the eight tab labels along the bottom, the
	// button-hint row beside them, and the 48px of dead screen the map leaves for both.
	//
	// This fork's own menu is the only way into these pages now, and it is what says which page
	// you are on - so the tab strip is a second navigation aid for a navigation the player no
	// longer does, and the hint row explains buttons on a bar that is no longer there.
	bool hideMenuChrome = true;

	// Stretch the menu's full-screen backdrops past the bottom of the screen.
	//
	// The front end lays itself out in 480x272 - the PSP's framebuffer - but what is actually
	// displayed here is about 330 rows tall, measured two ways: a backdrop set to h=272 covered
	// 82% of the frame and one set to h=120 covered 37%, which put the real height at 332 and 324
	// respectively. So the black band along the bottom was never a gap reserved for the tab strip.
	// It was screen the menu does not know exists, and nothing the game draws was ever going to
	// reach it.
	//
	// 400 is therefore deliberately too big rather than measured precisely. Drawing off the bottom
	// costs nothing, an exact value would have to be derived from display settings the player can
	// change, and a backdrop that stops one row short is the entire bug coming back.
	bool fillMenuBackdrop = true;
	int backdropHeight = 400;

	// Stop left/right from switching tabs inside the game's menu.
	//
	// It follows from hiding the strip rather than being a separate opinion: with the tabs drawn,
	// tabbing is navigation the player can see; with them hidden it moves you to an unmarked page
	// for no visible reason, and this fork's own menu is what chooses the page now.
	//
	// Up and down move between the two ROWS of tabs, so they need locking too - but on a page with
	// entries on it they are how those entries are selected, and taking them there would make the
	// Game page unusable.
	//
	// The game says which is which. A page's selectable entries are its `*_MI` widgets - menu
	// item - so `GAME_PAGE` (LoadGame_MI, NewGame_MI, DeleteGame_MI, Reset_MI) keeps up and down
	// while MAP_PAGE (Map_AE, MapTitle) and STATS_PAGE do not. No list of page numbers to keep in
	// step with anything.
	bool lockMenuTabs = true;

	// How long to wait for the menu to appear after Start, and how many tab presses to spend
	// before giving up. Eleven pages means eleven presses is a full lap; twice that is generous
	// and still bounded.
	int openTimeoutFrames = 90;
	int maxTabPresses = 22;
};

VCSFrontEndSettings &FrontEndSettings();

// Whether the game's own front end is up. This is the answer to the `GameState` question the
// input layer has carried a TODO for since the beginning - not the general "what is the game
// doing" enum, but the specific half of it that decides whether the player's keys should be
// driving a character or a menu. Reads memory: emu thread only.
bool GameMenuActive();

// Which page the front end is showing, or -1 when it is closed or unreadable. Emu thread only.
int GameMenuPage();

// Whether the TAB STRIP has focus rather than the page itself. True on a page just tabbed to,
// false once Cross has been pressed to descend into it. Emu thread only.
bool MenuOnTabStrip();

// Whether the page showing has entries of its own to move between - i.e. any `*_MI` widget. False
// on the map, the briefs and the stats, where up and down would only change which row of tabs you
// are on. Cached against the page index, so this is cheap to ask every frame.
bool MenuPageHasItems();

// Whether the menu on screen is one THIS BRIDGE opened - a MAP, BRIEF, STATS or GAME row - as
// opposed to one the game put up by itself, like the save UI you get by walking into the save icon
// at a safe house.
//
// The distinction decides whether the arrow lock applies at all. Hiding the tab strip is something
// we do to a page we navigated to; a menu the game opened was never tabbed to and must keep every
// control it came with.
bool BridgeOwnsMenu();

// Ask for the game's front end. Safe from the UI thread, which is where the menu row calls it.
// `Save` is forwarded to RequestSaveMenu; everything else opens the menu and walks.
//
// Queued rather than done, for the reason the cheat menu queues: this fork's menu pauses the
// emulator, so nothing can be pressed until the player is back in the game. The row requests and
// closes; `FrontEndTick` does the work on the far side of that.
void RequestGameMenu(FrontEndTarget target);

// Shut the game's own menu if it is open, by pressing its back control until it closes. Safe from
// the UI thread; queued like everything else here.
void RequestCloseGameMenu();

// Where the player's map marker is, in world units. False when none is placed.
//
// Scans the blip store for an entry of the marker's type rather than reading a fixed slot - see
// the constants in VCSAddresses.h for why that distinction cost a wrong route once already.
bool FindWaypoint(float *x, float *y);

// Where the mission is sending the player - the pink dot. The nearest one, because a mission can
// have several markers up and the next objective is the close one.
bool FindMissionMarker(float *x, float *y);

// What the GPS should actually route to, resolving the two against each other by way of
// `preferMissionMarker`. `isMission` may be null; it says which of the two won, for the debugger
// and for anything that wants to colour the line differently.
bool FindRouteDestination(float *x, float *y, bool *isMission);

// Ask the front end to open its SAVE menu, the way script opcode `0260 activate_save_menu` does -
// by setting the request flag the front end polls. This one really is a single write, and it is
// the exception that proves the rule above: the flag is not a claim that the menu is open, it is
// the game's own inbox for that request, and the front end does all of the opening itself.
//
// Deliberately NOT how the map and load rows work. There is no equivalent inbox for those.
void RequestSaveMenu();

// Whether the bridge is currently driving the pad - pressing Start, or walking to a page. While
// this is true the player's own bindings stand down, exactly as they do for a cheat combination.
//
// False once the menu is up and the walk has finished, because from that moment the player is
// meant to be working the game's menu themselves - see the Menu input context.
bool FrontEndDriving();

// The map marker button this frame, or zero. Applied by `ApplyMapping` alongside the player's own
// bindings rather than by taking the pad over: placing a marker is something the player asked for
// with a key, not a sequence the bridge is running.
u32 MapWaypointButtonMask();

// The map zoom button this frame, or zero. Latched from a wheel notch, which is far too short a
// press for the front end to see on its own.
u32 MapZoomButtonMask();

// What to press this frame. Zero when not driving. Applied by `ApplyMapping`, which stays the
// only thing in this fork that touches sceCtrl.
u32 FrontEndButtonMask();

// Advance the bridge. Emu thread, once per vblank, before `ApplyMapping`.
void FrontEndTick();

// Abandon anything in flight. Called on init and shutdown.
void FrontEndReset();

// One short phrase for the debugger: what the bridge is doing, and why it stopped if it did.
const char *FrontEndStatus();

// How many widgets the chrome pass is currently holding hidden, for the debugger. Zero with the
// menu closed, and zero-while-the-menu-is-up means the tree walk found nothing - which is what a
// changed build would look like, as opposed to the feature being off.
int HiddenChromeCount();

}  // namespace VCS
