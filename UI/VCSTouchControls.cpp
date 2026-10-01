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
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "Common/Data/Color/RGBAUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/Render/Text/draw_text.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "GPU/Common/PresentationCommon.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSRadar.h"
#include "Core/VCS/VCSInput.h"
#include "UI/GamepadEmu.h"
#include "UI/VCSTouchControls.h"

// ONE TABLE, AND WHY THE POSITIONS ARE IN IT
//
// A control is a row: which pad control it presses, what it is called here, how big it is, where
// it sits, which contexts it belongs to and what else has to be true. That makes adding one a
// single line, and - more to the point - it makes the whole scheme readable in one place, which a
// pile of view constructors never is.
//
// Positions are dp from an EDGE, not fractions of the screen: a thumb's reach is a fixed distance
// in centimetres, so a button 78dp from the bottom-right corner is in the same place on a 5" phone
// and a 7" one, where a fraction would put it out of reach on the larger. Negative means "from
// the far edge", which is where nearly everything is.
//
// The one exception is the row of small controls beside the radar, which is anchored to the RADAR
// rather than to a corner - see Anchor::Radar. Those used to sit in the top-right, on top of the
// game's own clock, money and weapon panel, which is the one part of the screen this overlay has
// no business covering.

namespace {

using namespace UI;

// The three pad controls this file names rather than spells. They are named in VCSInput for the
// reason that file gives - an intent asked of one device is a bug waiting for another - and
// naming them here too would be a fourth copy free to drift from the other three.
using VCS::kVCSPadAimButton;
using VCS::kVCSPadJumpButton;
using VCS::kVCSPadSubMissionButton;

// Which pointers the buttons and the stick have taken this frame. The look zone ignores those,
// which is the whole of how "drag anywhere that is not a button" works. PPSSPP's own gamepad
// keeps exactly this mask for the same reason - it is file-static there, so this is a second
// copy rather than a shared one.
uint32_t g_claimed = 0;

bool PointerClaimed(int id) {
	return (g_claimed & (1 << id)) != 0;
}
void ClaimPointer(int id) {
	g_claimed |= 1 << id;
}
void ReleasePointer(int id) {
	g_claimed &= ~(1 << id);
}

// Steering by arrows rather than by stick: two buttons that between them write the stick's X axis.
// Held apart from the buttons because neither of them presses a pad control - they ARE the stick,
// and the game steers on an axis.
int g_steerLeft = 0;
int g_steerRight = 0;

void ApplySteering() {
	VCS::SetTouchStick((float)(g_steerRight - g_steerLeft), 0.0f);
}

// Whether the aim TOGGLE currently holds the aim control.
//
// File-scope because two different controls reach for that one control and only one of them may
// let go of it: the toggle latches it, and the fire button borrows it for the length of a shot
// (see kAutoLockLeadFrames). Without this, firing once while the toggle was on ended with the
// shot releasing an aim the player had asked to keep - which reads as the toggle silently
// switching itself off, a long way from anything that looks like its cause.
bool g_aimToggled = false;

// A control that presses a SECOND control alongside its own, counted rather than set.
//
// Only the drive-by pair uses this - each of them fires as well as glancing - and both can be
// held at once, so a plain set-and-clear would have the first one released cancelling the shot
// the second is still asking for.
std::map<InputKeyCode, int> g_secondHolds;

void HoldSecond(InputKeyCode code, bool down) {
	int &count = g_secondHolds[code];
	if (down) {
		if (count++ == 0) {
			VCS::SetTouchButton(code, true);
		}
	} else if (count > 0 && --count == 0) {
		VCS::SetTouchButton(code, false);
	}
}

// What has to be true for a control to be on screen, beyond the context.
enum Cond : uint32_t {
	CondAlways = 0,
	CondArmed = 1 << 0,     // something that shoots is in hand
	CondMelee = 1 << 1,     // fists or a melee weapon
	CondScoped = 1 << 2,    // a scope, the binoculars or the camera is up
	CondLockedOn = 1 << 3,  // the game has a target
	CondScopeZooms = 1 << 4,       // the sniper, the camera or the binoculars is up
	CondScopeCannotShoot = 1 << 5, // the camera or the binoculars is up
	CondCameraUp = 1 << 6,         // the camera is up
	CondBike = 1 << 7,             // riding a motorcycle
	CondMapPage = 1 << 8,          // the game's own map page
	CondPlane = 1 << 9,            // flying a plane rather than a helicopter
	CondWeapon = CondArmed | CondMelee,
};

constexpr uint32_t Ctx(VCS::VCSInputContext context) {
	return 1u << (uint32_t)context;
}

constexpr uint32_t kOnFoot = Ctx(VCS::VCSInputContext::OnFoot);
constexpr uint32_t kAiming = Ctx(VCS::VCSInputContext::Aiming);
constexpr uint32_t kVehicle = Ctx(VCS::VCSInputContext::InVehicle);
constexpr uint32_t kAircraft = Ctx(VCS::VCSInputContext::InAircraft);
constexpr uint32_t kMenu = Ctx(VCS::VCSInputContext::Menu);

// Not a context the input layer knows about - the game is in a cutscene, which is a state that
// sits ACROSS the contexts rather than beside them: the player is still on foot or still in a
// car, and simply has no control over either. The top bit, so it cannot collide with a context
// however many of those there come to be.
constexpr uint32_t kCutscene = 1u << 31;

// What a control does with a press. One field rather than a behaviour plus a "this row is
// special" flag, because the rows that press no pad control at all - the menu, the steering
// arrows, the cutscene skip - are not a different kind of thing from the rows that do. They are
// the same kind of thing doing something else.
enum class Kind {
	Hold,        // down while touched, which is nearly everything
	Toggle,      // a press latches it and the next press lets it go: the aim control
	JumpSprint,  // held it sprints, tapped twice it jumps - see PressJumpSprint
	Menu,        // opens this fork's menu
	Skip,        // skips a cutscene
	SteerLeft,
	SteerRight,
};

enum class Anchor {
	Edge,   // x and y are dp from an edge; negative means from the far one
	Radar,  // x is dp right of the radar, y is dp from the radar's own centre line
};

struct ControlSpec {
	// What this row is called in vcs.ini, where a player's own position for it is kept.
	//
	// A NAME rather than the row's index, because an index is only stable until somebody adds a
	// control: inserting one in the middle would silently shift every saved layout by one button.
	// It is also the only part of this table a player ever sees, so it is readable.
	const char *id;
	InputKeyCode pad;        // the pad control this presses; unused on a row that presses none
	InputKeyCode padSecond;  // a second control - see Kind for what "second" means on each
	Kind kind;
	const char *icon;        // assets/vcs/touch_<icon>.png
	const char *iconArmed;   // and what to draw instead with something that shoots in hand
	const char *label;       // what it is called - drawn only when the icon is missing
	float x;
	float y;
	float radius;            // dp
	Anchor anchor;
	uint32_t contexts;
	uint32_t cond;           // show only when one of these is true
	uint32_t hide;           // ... and never when one of THESE is
};

// --- The layout ----------------------------------------------------------------------------------
//
// Two columns down the right-hand side, and the one a thumb reaches without moving is the outer
// one. The left is empty except in a vehicle, where the steering arrows and the drive-by pair
// live - on foot that half of the screen is the stick, and everywhere it is the camera.

constexpr float kColA = -72.0f;   // the outer column, under the thumb
constexpr float kColB = -158.0f;  // and the one inside it
constexpr float kRow1 = -80.0f;
constexpr float kRow2 = -168.0f;
constexpr float kRow3 = -250.0f;

constexpr float kLeftA = 54.0f;
constexpr float kLeftB = 148.0f;
constexpr float kWheelY = -78.0f;
constexpr float kGlanceY = -198.0f;

constexpr float kBig = 40.0f;    // the one the thumb rests on: attack, the accelerator
constexpr float kMid = 31.0f;
constexpr float kSmall = 27.0f;
constexpr float kTiny = 21.0f;   // the row beside the radar

// The radar row: from the radar's right edge to the first button's centre, then one step per
// button after it.
constexpr float kRadarGap = 34.0f;
constexpr float kRadarStep = 48.0f;

const ControlSpec kControls[] = {
	// --- On foot ------------------------------------------------------------------------------
	//
	// Three down the outer column - attack, move, get in - with the aim toggle inside them. It is
	// the shape the trilogy's phone ports use and it is not arbitrary: the button a thumb is on
	// most is the one nearest the corner, and the two it reaches for deliberately sit further up
	// where a mis-tap cannot fire a gun.
	{ "foot.fist",
		 NKCODE_BUTTON_B,      NKCODE_UNKNOWN,       Kind::Hold,
		"fist", "gun", "ATTACK",       kColA, kRow1, kBig,   Anchor::Edge, kOnFoot | kAiming, CondAlways, CondScopeCannotShoot },
	// Jump and sprint on one button: held it sprints, tapped twice it jumps. Two of the four
	// face buttons on one control, which is what a screen with room for three has to do - and
	// what a phone player already expects, because every touch platformer since has done it.
	{ "foot.run",
		 NKCODE_BUTTON_A,      kVCSPadJumpButton,    Kind::JumpSprint,
		"run", nullptr, "RUN",         kColA, kRow2, kMid,   Anchor::Edge, kOnFoot,           CondAlways, 0 },
	{ "foot.car",
		 NKCODE_BUTTON_Y,      NKCODE_UNKNOWN,       Kind::Hold,
		"car", nullptr, "ENTER",       kColA, kRow3, kSmall, Anchor::Edge, kOnFoot,           CondAlways, 0 },
	// Aim is a TOGGLE here and a held trigger on every other device, and that is the one place
	// this overlay deliberately disagrees with the pad scheme it otherwise stands on. A trigger
	// is held because a finger can rest on it; a thumb holding a patch of glass is a thumb that
	// cannot do anything else, and aiming is exactly when the other hand is busy.
	{ "foot.crosshair",
		 kVCSPadAimButton,     NKCODE_UNKNOWN,       Kind::Toggle,
		"crosshair", nullptr, "TARGET", kColB, kRow1, kMid,  Anchor::Edge, kOnFoot | kAiming, CondWeapon | CondScoped, 0 },
	{ "foot.pickup",
		 NKCODE_BUTTON_THUMBL, NKCODE_UNKNOWN,       Kind::Hold,
		"pickup", nullptr, "PICKUP",   kColB, kRow2, kSmall, Anchor::Edge, kOnFoot,           CondAlways, 0 },

	// --- Aiming ------------------------------------------------------------------------------
	//
	// THE MELEE SET IS MELEE-ONLY, which is a change from the first build and the point of the
	// `hide` column. Block, heavy and grab are what the four face buttons become when the game
	// decides you are fighting rather than shooting - with a gun in hand the same three buttons
	// do nothing at all, so three dead controls appeared the moment anyone aimed a pistol.
	{ "aim.heavy",
		 NKCODE_BUTTON_A,      NKCODE_UNKNOWN,       Kind::Hold,
		"heavy", nullptr, "HEAVY",     kColA, kRow2, kMid,   Anchor::Edge, kAiming,           CondMelee,  0 },
	{ "aim.grab",
		 NKCODE_BUTTON_Y,      NKCODE_UNKNOWN,       Kind::Hold,
		"grab", nullptr, "GRAB",       kColA, kRow3, kSmall, Anchor::Edge, kAiming,           CondMelee,  0 },
	{ "aim.shield",
		 NKCODE_BUTTON_X,      NKCODE_UNKNOWN,       Kind::Hold,
		"shield", nullptr, "BLOCK",    kColB, kRow2, kSmall, Anchor::Edge, kAiming,           CondMelee,  0 },
	// Picking the target, which is the one thing the weapon panel on the HUD cannot be tapped
	// for: with aim held that same control cycles targets rather than weapons. Not down a scope -
	// there is no lock-on to cycle, and the zoom pair wants these two places.
	{ "aim.arrow_left",
		 NKCODE_DPAD_LEFT,     NKCODE_UNKNOWN,       Kind::Hold,
		"arrow_left", nullptr, "PREV", kColB, kRow2, kSmall, Anchor::Edge, kAiming,           CondArmed,  CondScoped },
	{ "aim.arrow_right",
		 NKCODE_DPAD_RIGHT,    NKCODE_UNKNOWN,       Kind::Hold,
		"arrow_right", nullptr, "NEXT", kColA, kRow2, kMid,  Anchor::Edge, kAiming,           CondArmed,  CondScoped },
	// The scope's own two, and the reason the table carries a condition at all: with anything but
	// a scope in hand these are Block and Heavy Hit, so an ungated pair would throw punches.
	// The camera's shutter, which is the fire control under another name - and where the fire
	// button would be, because that is where the thumb already is.
	{ "aim.shutter",
		 NKCODE_BUTTON_B,      NKCODE_UNKNOWN,       Kind::Hold,
		"shutter", nullptr, "PHOTO",   kColA, kRow1, kBig,   Anchor::Edge, kAiming,           CondCameraUp, 0 },
	{ "aim.zoom_in",
		 NKCODE_BUTTON_R1,     NKCODE_UNKNOWN,       Kind::Hold,
		"zoom_in", nullptr, "ZOOM +",  kColA, kRow2, kMid,   Anchor::Edge, kAiming,           CondScopeZooms, 0 },
	{ "aim.zoom_out",
		 NKCODE_BUTTON_L1,     NKCODE_UNKNOWN,       Kind::Hold,
		"zoom_out", nullptr, "ZOOM -", kColB, kRow2, kSmall, Anchor::Edge, kAiming,           CondScopeZooms, 0 },

	// --- Driving -------------------------------------------------------------------------------
	{ "car.pedal_gas",
		 NKCODE_BUTTON_R2,     NKCODE_UNKNOWN,       Kind::Hold,
		"pedal_gas", nullptr, "GAS",   kColA, kRow1, kBig,   Anchor::Edge, kVehicle,          CondAlways, 0 },
	{ "car.pedal_brake",
		 NKCODE_BUTTON_L2,     NKCODE_UNKNOWN,       Kind::Hold,
		"pedal_brake", nullptr, "BRAKE", kColB, -84.0f, kMid, Anchor::Edge, kVehicle,         CondAlways, 0 },
	{ "car.handbrake",
		 NKCODE_BUTTON_X,      NKCODE_UNKNOWN,       Kind::Hold,
		"handbrake", nullptr, "HAND",   kColA, kRow2, kMid,  Anchor::Edge, kVehicle,          CondAlways, 0 },
	{ "car.horn",
		 NKCODE_BUTTON_A,      NKCODE_UNKNOWN,       Kind::Hold,
		"horn", nullptr, "HORN",        kColB, -176.0f, kSmall, Anchor::Edge, kVehicle,       CondAlways, 0 },
	{ "car.car",
		 NKCODE_BUTTON_Y,      NKCODE_UNKNOWN,       Kind::Hold,
		"car", nullptr, "EXIT",         kColA, kRow3, kSmall, Anchor::Edge, kVehicle,         CondAlways, 0 },
	// THE GLANCES SHOOT. On the PSP a drive-by is the glance modifier and a stick direction, and
	// then Circle - which is three controls for one act, and the third of them is the only part a
	// thumb would have to find. So these two press both: the glance decides the side and the
	// trigger goes down with it. The glance half is worth having on its own, so the trigger is
	// added only with something to shoot - see Press.
	{ "car.driveby_left",
		 NKCODE_BUTTON_L1,     NKCODE_BUTTON_B,      Kind::Hold,
		"driveby_left", nullptr, "LOOK", kLeftA, kGlanceY, kMid, Anchor::Edge, kVehicle,      CondAlways, 0 },
	{ "car.driveby_right",
		 NKCODE_BUTTON_R1,     NKCODE_BUTTON_B,      Kind::Hold,
		"driveby_right", nullptr, "LOOK", kLeftB, kGlanceY, kMid, Anchor::Edge, kVehicle,     CondAlways, 0 },
	// A motorcycle also fires straight ahead, which is the drive-by control with no glance held -
	// so this one presses the fire control alone. To the right of the pair rather than between
	// them, because between them there is no room, and it can be dragged wherever suits.
	{ "car.driveby_forward",
		 NKCODE_BUTTON_B,      NKCODE_UNKNOWN,       Kind::Hold,
		"driveby_forward", nullptr, "SHOOT", kLeftB + 94.0f, kGlanceY, kMid, Anchor::Edge, kVehicle, CondBike, 0 },
	{ "car.arrow_left",
		 NKCODE_UNKNOWN,       NKCODE_UNKNOWN,       Kind::SteerLeft,
		"arrow_left", nullptr, "<",     kLeftA, kWheelY, kBig, Anchor::Edge, kVehicle,        CondAlways, 0 },
	{ "car.arrow_right",
		 NKCODE_UNKNOWN,       NKCODE_UNKNOWN,       Kind::SteerRight,
		"arrow_right", nullptr, ">",    kLeftB, kWheelY, kBig, Anchor::Edge, kVehicle,        CondAlways, 0 },

	// --- Flying --------------------------------------------------------------------------------
	//
	// The stick is pitch and roll here, which is why there is no steering row: pitch is what turns
	// a rotor's lift into forward flight, and a pair of arrows cannot express it.
	{ "air.descend",
		 NKCODE_BUTTON_L2,     NKCODE_UNKNOWN,       Kind::Hold,
		"descend", nullptr, "DOWN",     kColA, kRow1, kBig,   Anchor::Edge, kAircraft,        CondAlways, CondPlane },
	{ "air.climb",
		 NKCODE_BUTTON_R2,     NKCODE_UNKNOWN,       Kind::Hold,
		"climb", nullptr, "UP",         kColA, kRow2, kMid,   Anchor::Edge, kAircraft,        CondAlways, CondPlane },
	// A plane flies on the same two controls, but they are its THROTTLE: Cross is what moves it
	// forward and the stick is what lifts it. Up and down arrows there read as a helicopter's
	// lift, which a plane does not have - so it gets the car's pedals, where the car has them.
	{ "air.pedal_gas",
		 NKCODE_BUTTON_R2,     NKCODE_UNKNOWN,       Kind::Hold,
		"pedal_gas", nullptr, "GAS",   kColA, kRow1, kBig,   Anchor::Edge, kAircraft,         CondPlane, 0 },
	{ "air.pedal_brake",
		 NKCODE_BUTTON_L2,     NKCODE_UNKNOWN,       Kind::Hold,
		"pedal_brake", nullptr, "BRAKE", kColB, -84.0f, kMid, Anchor::Edge, kAircraft,        CondPlane, 0 },
	{ "air.gun",
		 NKCODE_BUTTON_B,      NKCODE_UNKNOWN,       Kind::Hold,
		"missile", nullptr, "FIRE",     kColB, -96.0f, kSmall, Anchor::Edge, kAircraft,       CondAlways, 0 },
	{ "air.car",
		 NKCODE_BUTTON_Y,      NKCODE_UNKNOWN,       Kind::Hold,
		"car", nullptr, "EXIT",         kColA, kRow3, kSmall, Anchor::Edge, kAircraft,        CondAlways, 0 },
	{ "air.yaw_left",
		 NKCODE_BUTTON_L1,     NKCODE_UNKNOWN,       Kind::Hold,
		"yaw_left", nullptr, "YAW",     kLeftA, kGlanceY, kMid, Anchor::Edge, kAircraft,      CondAlways, 0 },
	{ "air.yaw_right",
		 NKCODE_BUTTON_R1,     NKCODE_UNKNOWN,       Kind::Hold,
		"yaw_right", nullptr, "YAW",    kLeftB, kGlanceY, kMid, Anchor::Edge, kAircraft,      CondAlways, 0 },

	// --- Beside the radar ------------------------------------------------------------------------
	//
	// The controls nobody presses in a hurry: the camera, the sub-mission button, the radio. They
	// were in the top-right corner, sitting on the game's own clock, money and weapon panel - the
	// one piece of the screen that is information rather than scenery. The radar's own corner has
	// nothing to its right, and these are small.
	{ "foot.camera",
		 NKCODE_BUTTON_THUMBR, NKCODE_UNKNOWN,       Kind::Hold,
		"camera", nullptr, "CAM",       kRadarGap, 0.0f, kTiny, Anchor::Radar,
		kOnFoot | kVehicle | kAircraft, CondAlways, 0 },
	{ "foot.submission",
		 kVCSPadSubMissionButton, NKCODE_UNKNOWN,    Kind::Hold,
		"submission", nullptr, "JOB",   kRadarGap + kRadarStep, 0.0f, kTiny, Anchor::Radar,
		kOnFoot | kAiming | kVehicle | kAircraft, CondAlways, 0 },
	{ "car.radio_prev",
		 NKCODE_DPAD_LEFT,     NKCODE_UNKNOWN,       Kind::Hold,
		"radio_prev", nullptr, "RADIO", kRadarGap + kRadarStep * 2.0f, 0.0f, kTiny, Anchor::Radar,
		kVehicle | kAircraft, CondAlways, 0 },
	{ "car.radio_next",
		 NKCODE_DPAD_RIGHT,    NKCODE_UNKNOWN,       Kind::Hold,
		"radio_next", nullptr, "RADIO", kRadarGap + kRadarStep * 3.0f, 0.0f, kTiny, Anchor::Radar,
		kVehicle | kAircraft, CondAlways, 0 },

	// --- The game's own pages: the map, the briefs, the stats ----------------------------------
	//
	// Ours drives most of the front end, so all these pages need is a way back. The map adds its
	// Cross and the Square that plants a marker. The d-pad arrows that used to sit here were not
	// needed on any of the three.
	{ "menu.select",
		 NKCODE_BUTTON_A,      NKCODE_UNKNOWN,       Kind::Hold,
		"select", nullptr, "SELECT",    kColA, kRow1, kMid,   Anchor::Edge, kMenu,            CondMapPage, 0 },
	{ "menu.back",
		 NKCODE_BUTTON_B,      NKCODE_UNKNOWN,       Kind::Hold,
		"back", nullptr, "BACK",        kColB, -76.0f, kSmall, Anchor::Edge, kMenu,           CondAlways, 0 },
	{ "menu.marker",
		 NKCODE_BUTTON_X,      NKCODE_UNKNOWN,       Kind::Hold,
		"marker", nullptr, "MARKER",    kColA, kRow2, kSmall, Anchor::Edge, kMenu,            CondMapPage, 0 },
	// Only on the game's own pages, where there is no radar to tap.
	{ "menu.menu",
		 NKCODE_UNKNOWN,       NKCODE_UNKNOWN,       Kind::Menu,
		"menu", nullptr, "MENU",        46.0f, 46.0f, kSmall, Anchor::Edge, kMenu,            CondAlways, 0 },

	// --- A cutscene ------------------------------------------------------------------------------
	//
	// Everything else comes off and these two go up. Skip is where the thumb already is; pause is
	// in the corner the game has just cleared of its own panel, and it is not optional furniture -
	// the way into this fork's menu during play is a tap on the radar, and a cutscene takes the
	// radar away.
	{ "cut.skip",
		 NKCODE_UNKNOWN,       NKCODE_UNKNOWN,       Kind::Skip,
		"skip", nullptr, "SKIP",        kColA, kRow1, kBig,   Anchor::Edge, kCutscene,        CondAlways, 0 },
	{ "cut.pause",
		 NKCODE_UNKNOWN,       NKCODE_UNKNOWN,       Kind::Menu,
		"pause", nullptr, "PAUSE",      -56.0f, 46.0f, kSmall, Anchor::Edge, kCutscene,       CondAlways, 0 },
};

// How long the auto-lock press leads the shot by, in frames of this view's Update.
//
// Firing a gun holds the aim control as well, which is what the trilogy's phone ports do and what
// this fork's pad already does - a thumb cannot hold a trigger and place a crosshair at once, so
// the game's assist is the point rather than a concession. The lead exists because lock-on is not
// instant: pressing both in the same frame fires before the game has chosen a target, which is a
// wasted shot into the middle distance. Two frames is the shortest lead that lands.
//
// It is NOT the 9-tick delay the free-aim entry used to carry, and the difference is the whole
// lesson from that bug: there the delay was open season for the game to take a lock nobody wanted,
// here the lock IS what was wanted.
constexpr int kAutoLockLeadFrames = 2;

// The jump-and-sprint button's two timings.
//
// A press shorter than kTapSeconds is a tap rather than the start of a sprint, and a second tap
// within kDoubleTapSeconds of the first one's release is a jump. Wall clock rather than frames,
// because a double tap is a thing a hand does and a hand does not know what the frame rate is.
//
// A single tap is therefore a few frames of sprint, and that is deliberately not defended
// against: sprint on foot is a held control that does nothing in the time a tap lasts, so the
// alternative - waiting out the double-tap window before sprinting at all - would buy nothing and
// cost every sprint a tenth of a second of delay.
constexpr double kTapSeconds = 0.25;
constexpr double kDoubleTapSeconds = 0.30;
// How long the jump itself is held for, in Update frames. The game samples the pad at its own
// 30Hz while this runs at vblank, so a press released on the next frame is a press it can miss.
constexpr int kJumpHoldFrames = 8;

// Everything in the layout table is in dp and multiplied by this, so the controls grow and shrink
// as one cluster anchored to its corner. Read once when the layout is built rather than per frame:
// changing the size rebuilds the views, because a button's bounds are its layout parameters.
float TouchScale() {
	const float s = VCS::TouchSettings().scale;
	return s > 0.2f && s < 4.0f ? s : 1.0f;
}

// Testing only: VCS_TOUCH_TEST=1 in the environment puts this overlay up on a desktop build,
// where ShowTouchControls is off, and keeps it from fading - so the art and the layout can be
// looked at, and clicked, without a phone. Nothing is written to any ini.
bool DesktopTouchTest() {
	static const bool on = getenv("VCS_TOUCH_TEST") != nullptr;
	return on;
}

uint32_t ButtonColor(bool down, float opacity) {
	const uint32_t rgb = g_Config.iTouchButtonStyle != 0 ? 0xFFFFFF : 0xE8E0D0;
	return colorAlpha(rgb, opacity * (down ? 0.75f : 0.32f));
}

// --- The icons ------------------------------------------------------------------------------------
//
// Baked PNGs out of assets/vcs, one per control, drawn white so the overlay's own opacity is the
// only thing that decides how solid they look - see Tools/vcstouchicons.py.
//
// Loaded on the first draw, because that is the first time a Draw::DrawContext is in hand, and
// cached until the device goes. A MISS is cached as nullptr on purpose: without it a control
// whose art was not deployed would go back to the VFS on every single frame, and the whole set is
// missing or none of it is. The label in the table is what draws instead, which is also what
// makes an unfinished icon set a cosmetic problem rather than a blank screen.
//
// The cache is a file-scope object released from EmuScreen::deviceLost through
// ReleaseVCSTouchArt, exactly as the boot curtain's art is, rather than a member of the layout:
// the layout is rebuilt whenever the screen resizes, and rebuilding it has nothing to do with
// whether the textures are still valid.
struct VCSTouchArt {
	~VCSTouchArt() {
		for (auto &pair : icons) {
			if (pair.second) {
				pair.second->Release();
			}
		}
	}

	// `prefix` is "touch" for a white glyph drawn on our own disc, or "touchbtn" for a whole
	// button - disc, outline and glyph in their own colours - that replaces the disc outright.
	Draw::Texture *Icon(UIContext &dc, const char *prefix, const char *name) {
		char path[128];
		snprintf(path, sizeof(path), "vcs/%s_%s.png", prefix, name);
		auto iter = icons.find(path);
		if (iter != icons.end()) {
			return iter->second;
		}
		size_t size = 0;
		uint8_t *data = g_VFS.ReadFile(path, &size);
		Draw::Texture *tex = nullptr;
		if (data) {
			tex = CreateTextureFromFileData(dc.GetDrawContext(), data, size,
				ImageFileType::DETECT, false, path);
			delete[] data;
		}
		icons[path] = tex;
		return tex;
	}

	std::map<std::string, Draw::Texture *> icons;
};

std::unique_ptr<VCSTouchArt> g_art;

void DrawIcon(UIContext &dc, Draw::Texture *tex, float cx, float cy, float size, uint32_t color) {
	const Bounds box(cx - size * 0.5f, cy - size * 0.5f, size, size);
	dc.Flush();
	dc.Begin();
	dc.GetDrawContext()->BindTexture(0, tex);
	dc.Draw()->DrawTexRect(box, 0.0f, 0.0f, 1.0f, 1.0f, color);
	dc.Flush();
	dc.RebindTexture();
}

// --- The controls themselves --------------------------------------------------------------------

class VCSTouchButton : public UI::View {
public:
	VCSTouchButton(const ControlSpec &spec, float scale, bool *pause, UI::LayoutParams *layoutParams)
		: UI::View(layoutParams), spec_(spec), scale_(scale), pause_(pause) {}

	bool Touch(const TouchInput &input) override;
	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		w = h = Radius() * 2.0f;
	}
	bool CanBeFocused() const override { return false; }

	const ControlSpec &Spec() const { return spec_; }
	bool IsDown() const { return downMask_ != 0; }
	float Radius() const { return spec_.radius * scale_; }

	// Editing: a control is picked up and put down somewhere else, and what it remembers is how
	// far from where the table put it - see VCSTouchOffset. Kept in dp, unscaled, so changing the
	// control SIZE afterwards moves the cluster as one rather than scattering it.
	void SetEditing(bool editing) { editing_ = editing; }
	bool Editing() const { return editing_; }

	// Let go, without the player's finger having done it. Called when a control leaves the screen
	// under a thumb - a weapon put away, a car entered - because a hidden view is never told about
	// the release, and a control held forever latches its context for the rest of the session.
	//
	// This is the STRONGER of the two releases and the difference is the aim toggle: an ordinary
	// finger-up leaves a latch exactly where it was, which is the whole point of latching it,
	// while a control that has left the screen has nothing left to press again.
	void ForceUp();

	void SetArmed(bool armed) { armed_ = armed; }

private:
	bool Inside(float x, float y) const;
	void Press(bool down);
	void PressJumpSprint(bool down);
	void FingerUp();
	void ReleaseBorrowedAim();

	const ControlSpec &spec_;
	float scale_ = 1.0f;
	bool *pause_;
	bool editing_ = false;
	uint32_t downMask_ = 0;
	bool armed_ = false;
	// Set when this press is holding the aim control open as well - see kAutoLockLeadFrames.
	bool holdingAim_ = false;
	int autoLockCountdown_ = 0;
	// Kind::Toggle: whether this control is latched down.
	bool toggled_ = false;
	// Kind::JumpSprint: when this press went down, when the last one came up, and whether a jump
	// is still being held out.
	double pressedAt_ = 0.0;
	double releasedAt_ = -1.0;
	int jumpFrames_ = 0;

	friend class VCSTouchLayout;
};

bool VCSTouchButton::Inside(float x, float y) const {
	const float dx = x - bounds_.centerX();
	const float dy = y - bounds_.centerY();
	// A touch target wants to be bigger than the thing drawn - a finger is wider than a fingertip
	// and lands short of where it is aiming. 1.25 is about 4mm of grace at these sizes.
	const float r = Radius() * 1.25f;
	return dx * dx + dy * dy <= r * r;
}

// Hold to sprint, tap twice to jump.
//
// The two are told apart on the way DOWN rather than on the way up, which is what makes a jump
// feel like a press instead of like a delayed reaction: the second tap jumps the moment it lands.
// The release of the FIRST tap is what arms it, so a slow double tap is simply two sprints.
void VCSTouchButton::PressJumpSprint(bool down) {
	const double now = time_now_d();
	if (!down) {
		VCS::SetTouchButton(spec_.pad, false);
		// A short press arms the second tap; a long one was a sprint and arms nothing.
		releasedAt_ = (now - pressedAt_) <= kTapSeconds ? now : -1.0;
		return;
	}

	pressedAt_ = now;
	if (releasedAt_ >= 0.0 && now - releasedAt_ <= kDoubleTapSeconds) {
		// The second tap. The jump is held out for its own few frames rather than for as long as
		// the finger is down, because the finger is already on its way up.
		releasedAt_ = -1.0;
		jumpFrames_ = kJumpHoldFrames;
		VCS::SetTouchButton(spec_.padSecond, true);
		return;
	}
	VCS::SetTouchButton(spec_.pad, true);
}

void VCSTouchButton::Press(bool down) {
	switch (spec_.kind) {
	case Kind::Menu:
		if (down && pause_) {
			*pause_ = true;
		}
		return;
	case Kind::Skip:
		if (down) {
			VCS::RequestCutsceneSkip();
		}
		return;
	case Kind::SteerLeft:
	case Kind::SteerRight:
		(spec_.kind == Kind::SteerLeft ? g_steerLeft : g_steerRight) = down ? 1 : 0;
		ApplySteering();
		return;
	case Kind::JumpSprint:
		PressJumpSprint(down);
		return;
	case Kind::Toggle:
		// A press flips it; the release does nothing at all. ForceUp is what lets it go when the
		// control leaves the screen, which is also the only thing that can.
		if (down) {
			toggled_ = !toggled_;
			// Only the row that owns the aim control speaks for it. Written as a test on the
			// control rather than on "this is the toggle" because a second toggle over some
			// other control would otherwise clear the aim flag every time it was switched off.
			if (spec_.pad == kVCSPadAimButton) {
				g_aimToggled = toggled_;
			}
			VCS::SetTouchButton(spec_.pad, toggled_);
		}
		return;
	case Kind::Hold:
		break;
	}

	VCS::SetTouchButton(spec_.pad, down);
	// The second control, which today is the drive-by trigger and nothing else. Only with
	// something to shoot: the glance is worth having on its own, and Circle in a car with empty
	// hands is a punch out of the window.
	if (spec_.padSecond != NKCODE_UNKNOWN && (armed_ || !down)) {
		HoldSecond(spec_.padSecond, down);
	}
}

bool VCSTouchButton::Touch(const TouchInput &input) {
	bool claimed = false;
	const uint32_t bit = 1 << input.id;

	// While the layout is being edited a control is furniture, not a control: the layout above
	// has already taken the event to drag it with, and pressing what you are moving would fire a
	// gun into the paused game.
	if (editing_) {
		return false;
	}

	if (input.flags & TouchInputFlags::RELEASE_ALL) {
		ForceUp();
		return false;
	}
	if ((input.flags & TouchInputFlags::DOWN) && !PointerClaimed(input.id) && Inside(input.x, input.y)) {
		const bool wasDown = downMask_ != 0;
		downMask_ |= bit;
		ClaimPointer(input.id);
		claimed = true;
		if (!wasDown) {
			GamepadTouch();
			if (VCS::TouchSettings().haptics && g_Config.bHapticFeedback) {
				System_Vibrate(HAPTIC_VIRTUAL_KEY);
			}
			// Firing a gun asks the game to lock on first. The shot follows a couple of frames
			// later, from Update - see kAutoLockLeadFrames.
			//
			// On foot only: in a car the aim control is the BRAKE and in the air it is DESCEND,
			// so a fire button there that borrowed it would slow you down with every shot.
			const VCS::VCSTouchState state = VCS::TouchState();
			const bool onFoot = state.context == VCS::VCSInputContext::OnFoot ||
				state.context == VCS::VCSInputContext::Aiming;
			if (spec_.kind == Kind::Hold && spec_.pad == NKCODE_BUTTON_B && onFoot &&
					state.armed && !state.scoped &&
					!VCS::IsTouchButtonDown(kVCSPadAimButton)) {
				VCS::SetTouchButton(kVCSPadAimButton, true);
				holdingAim_ = true;
				autoLockCountdown_ = kAutoLockLeadFrames;
			} else {
				Press(true);
			}
		}
	}
	if ((input.flags & TouchInputFlags::MOVE) && (downMask_ & bit)) {
		// A thumb that has slid well clear of the button has left it. Generous, because a thumb
		// rolls as it presses and the last thing a fire button should do is let go mid-burst.
		const float dx = input.x - bounds_.centerX();
		const float dy = input.y - bounds_.centerY();
		const float r = Radius() * 2.0f;
		if (dx * dx + dy * dy > r * r) {
			downMask_ &= ~bit;
			ReleasePointer(input.id);
			if (!downMask_) {
				FingerUp();
			}
		} else {
			claimed = true;
		}
	}
	if (input.flags & TouchInputFlags::UP) {
		if (downMask_ & bit) {
			downMask_ &= ~bit;
			ReleasePointer(input.id);
			claimed = true;
			if (!downMask_) {
				FingerUp();
			}
		}
	}
	return claimed;
}

void VCSTouchButton::ReleaseBorrowedAim() {
	if (!holdingAim_) {
		return;
	}
	// Only ever released by whoever took it: the TARGET toggle holds the same control, and a shot
	// ending must not end an aim the player asked to keep.
	if (!g_aimToggled) {
		VCS::SetTouchButton(kVCSPadAimButton, false);
	}
	holdingAim_ = false;
}

void VCSTouchButton::FingerUp() {
	downMask_ = 0;
	autoLockCountdown_ = 0;
	// A latch outlives the finger; everything else lets go with it. The jump a double tap asked
	// for outlives it too - see jumpFrames_ - so neither is touched here.
	if (spec_.kind != Kind::Toggle) {
		Press(false);
	}
	ReleaseBorrowedAim();
}

void VCSTouchButton::ForceUp() {
	// The latch, which is the one thing a finger-up leaves alone. Done first so that a borrowed
	// aim released just below sees the toggle already gone.
	if (toggled_) {
		toggled_ = false;
		if (spec_.pad == kVCSPadAimButton) {
			g_aimToggled = false;
		}
		// Released here rather than through Press, whose Toggle arm only ever flips.
		VCS::SetTouchButton(spec_.pad, false);
	}
	if (jumpFrames_ > 0) {
		jumpFrames_ = 0;
		VCS::SetTouchButton(spec_.padSecond, false);
	}
	FingerUp();
}

void VCSTouchButton::Draw(UIContext &dc) {
	const float opacity = GamepadGetOpacity();
	if (opacity <= 0.0f) {
		return;
	}

	const bool down = downMask_ != 0 || toggled_;
	const float cx = bounds_.centerX();
	const float cy = bounds_.centerY();
	const char *icon = armed_ && spec_.iconArmed ? spec_.iconArmed : spec_.icon;
	if (icon && !g_art) {
		g_art.reset(new VCSTouchArt());
	}

	// A whole button, drawn as it was painted. Its disc spans 236 of the image's 256 pixels, so it
	// is scaled up to put that disc - not the image - on the control's radius, and a press lifts it
	// to full brightness, which is the only change multiplying a coloured image can make.
	if (icon) {
		if (Draw::Texture *tex = g_art->Icon(dc, "touchbtn", icon)) {
			const float size = Radius() * 2.0f * (256.0f / 236.0f) * (down ? 1.06f : 1.0f);
			DrawIcon(dc, tex, cx, cy, size, colorAlpha(down ? 0xFFFFFF : 0xD8D8D8, opacity));
			return;
		}
	}

	float imgW = 0.0f, imgH = 0.0f;
	dc.Draw()->GetAtlas()->measureImage(ImageID("I_ROUND"), &imgW, &imgH);
	const float scale = imgW > 0.0f ? (Radius() * 2.0f) / imgW : 1.0f;
	dc.Draw()->DrawImage(ImageID("I_ROUND"), cx, cy, scale * (down ? 1.06f : 1.0f),
		ButtonColor(down, opacity), ALIGN_CENTER);

	const uint32_t inkColor = colorAlpha(0xFFFFFF, opacity * (down ? 1.0f : 0.85f));
	if (icon) {
		if (Draw::Texture *tex = g_art->Icon(dc, "touch", icon)) {
			DrawIcon(dc, tex, cx, cy, Radius() * 1.2f, inkColor);
			return;
		}
	}

	if (spec_.label && *spec_.label) {
		// Pricedown, because this is the game's own face and these read as part of it rather than
		// as an emulator's overlay. Sized to the button so the short words fill it.
		const int size = (int)std::max(11.0f, Radius() * 0.46f);
		dc.SetFontStyle(FontStyle(FontFamily::Display, size, FontStyleFlags::Default));
		dc.DrawText(spec_.label, cx, cy, inkColor, ALIGN_CENTER);
		dc.SetFontStyle(dc.GetTheme().uiFont);
	}
}

// A patch of the GAME's own HUD, made touchable: the radar, and the weapon in the corner.
//
// These are not controls of ours and they are deliberately not drawn - the thing you press is
// already on screen, drawn by the game, which is exactly how the phone ports of the trilogy did
// it. They are placed in the PSP's own 480x272 screen coordinates and converted to the display,
// so they sit on the artwork rather than near it.
//
// The radar's rect comes from VCSRadar rather than from a constant here, because this port moves
// the radar to the top-left corner and both halves have to agree about where it ended up.
class VCSHudZone : public UI::View {
public:
	enum class Action {
		Menu,        // the radar: opens this fork's pause menu
		NextWeapon,  // the weapon icon: the game's own next-weapon control
	};

	VCSHudZone(Action action, bool *pause, UI::LayoutParams *layoutParams)
		: UI::View(layoutParams), action_(action), pause_(pause) {}

	bool Touch(const TouchInput &input) override;
	void Draw(UIContext &dc) override;
	bool CanBeFocused() const override { return false; }
	void ForceUp();

private:
	Action action_;
	bool *pause_;
	int pointer_ = -1;
};

bool VCSHudZone::Touch(const TouchInput &input) {
	if (input.flags & TouchInputFlags::RELEASE_ALL) {
		ForceUp();
		return false;
	}
	if ((input.flags & TouchInputFlags::DOWN) && pointer_ == -1 && !PointerClaimed(input.id) &&
			bounds_.Contains(input.x, input.y)) {
		pointer_ = input.id;
		ClaimPointer(input.id);
		GamepadTouch();
		if (VCS::TouchSettings().haptics && g_Config.bHapticFeedback) {
			System_Vibrate(HAPTIC_VIRTUAL_KEY);
		}
		if (action_ == Action::NextWeapon) {
			VCS::SetTouchButton(NKCODE_DPAD_RIGHT, true);
		}
		return true;
	}
	if ((input.flags & TouchInputFlags::UP) && input.id == pointer_) {
		const bool inside = bounds_.Contains(input.x, input.y);
		ForceUp();
		// The menu opens on RELEASE, not on press: a tap that slid off the radar was a drag of
		// the camera that happened to start there, and pausing the game on it would be a menu
		// nobody asked for.
		if (inside && action_ == Action::Menu && pause_) {
			*pause_ = true;
		}
		return true;
	}
	return false;
}

void VCSHudZone::ForceUp() {
	if (pointer_ != -1) {
		ReleasePointer(pointer_);
	}
	pointer_ = -1;
	if (action_ == Action::NextWeapon) {
		VCS::SetTouchButton(NKCODE_DPAD_RIGHT, false);
	}
}

void VCSHudZone::Draw(UIContext &dc) {
	// A press shows as a soft square over the game's own artwork, so a tap that did nothing can
	// be told from one that missed. Nothing at all at rest.
	if (pointer_ == -1) {
		return;
	}
	const float opacity = GamepadGetOpacity();
	if (opacity <= 0.0f) {
		return;
	}
	dc.FillRect(UI::Drawable(colorAlpha(0xFFFFFF, opacity * 0.18f)), bounds_);
}

// The movement stick. Floating by default: it appears wherever the thumb lands inside its zone,
// which is what every one of the trilogy's phone ports does and the single biggest difference
// between a stick you can use without looking and one you cannot.
class VCSTouchStick : public UI::View {
public:
	explicit VCSTouchStick(UI::LayoutParams *layoutParams) : UI::View(layoutParams) {}

	bool Touch(const TouchInput &input) override;
	void Draw(UIContext &dc) override;
	bool CanBeFocused() const override { return false; }
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		w = bounds_.w;
		h = bounds_.h;
	}
	void ForceUp();

private:
	void Move(float x, float y);

	int pointer_ = -1;
	float centerX_ = 0.0f;
	float centerY_ = 0.0f;
	float knobX_ = 0.0f;
	float knobY_ = 0.0f;
};

void VCSTouchStick::Move(float x, float y) {
	const VCS::VCSTouchSettings &touch = VCS::TouchSettings();
	float dx = (x - centerX_) / touch.stickRadius;
	float dy = (y - centerY_) / touch.stickRadius;
	const float length = std::sqrt(dx * dx + dy * dy);
	if (length > 1.0f) {
		dx /= length;
		dy /= length;
	}
	knobX_ = dx;
	knobY_ = dy;
	if (length < touch.stickDeadzone) {
		VCS::SetTouchStick(0.0f, 0.0f);
		return;
	}
	// The PSP's stick is positive AWAY from the player and a screen's Y grows downward, so the
	// one negation lives here rather than in every consumer.
	VCS::SetTouchStick(dx, -dy);
}

bool VCSTouchStick::Touch(const TouchInput &input) {
	if (input.flags & TouchInputFlags::RELEASE_ALL) {
		ForceUp();
		return false;
	}
	if ((input.flags & TouchInputFlags::DOWN) && pointer_ == -1 && !PointerClaimed(input.id) &&
			bounds_.Contains(input.x, input.y)) {
		pointer_ = input.id;
		ClaimPointer(input.id);
		GamepadTouch();
		if (VCS::TouchSettings().floatingStick) {
			centerX_ = input.x;
			centerY_ = input.y;
		} else {
			centerX_ = bounds_.x + VCS::TouchSettings().stickRadius * 1.4f;
			centerY_ = bounds_.y2() - VCS::TouchSettings().stickRadius * 1.4f;
		}
		Move(input.x, input.y);
		return true;
	}
	if ((input.flags & TouchInputFlags::MOVE) && input.id == pointer_) {
		Move(input.x, input.y);
		return true;
	}
	if ((input.flags & TouchInputFlags::UP) && input.id == pointer_) {
		ForceUp();
		return true;
	}
	return false;
}

void VCSTouchStick::ForceUp() {
	if (pointer_ != -1) {
		ReleasePointer(pointer_);
	}
	pointer_ = -1;
	knobX_ = 0.0f;
	knobY_ = 0.0f;
	VCS::SetTouchStick(0.0f, 0.0f);
}

void VCSTouchStick::Draw(UIContext &dc) {
	const float opacity = GamepadGetOpacity();
	if (opacity <= 0.0f || pointer_ == -1) {
		// Nothing is drawn until a thumb is down, which is the other half of a floating stick:
		// an empty corner rather than a ring sitting over the game asking to be aimed at.
		return;
	}

	const VCS::VCSTouchSettings &touch = VCS::TouchSettings();
	float imgW = 0.0f, imgH = 0.0f;
	dc.Draw()->GetAtlas()->measureImage(ImageID("I_ROUND"), &imgW, &imgH);
	if (imgW <= 0.0f) {
		return;
	}
	const float ringScale = (touch.stickRadius * 2.0f) / imgW;
	dc.Draw()->DrawImage(ImageID("I_ROUND"), centerX_, centerY_, ringScale,
		colorAlpha(0xFFFFFF, opacity * 0.16f), ALIGN_CENTER);
	const float knobScale = (touch.stickRadius * 0.8f) / imgW;
	dc.Draw()->DrawImage(ImageID("I_ROUND"),
		centerX_ + knobX_ * touch.stickRadius, centerY_ + knobY_ * touch.stickRadius,
		knobScale, colorAlpha(0xFFFFFF, opacity * 0.55f), ALIGN_CENTER);
}

// Everything that is not a control: drag to look.
//
// Added LAST so the buttons and the stick are dispatched first and have already claimed their
// pointers by the time this sees the same touch - ViewGroup hands every child every event, in
// order, so "not on a button" can only be answered after the buttons have answered.
class VCSLookZone : public UI::View {
public:
	explicit VCSLookZone(UI::LayoutParams *layoutParams) : UI::View(layoutParams) {}

	bool Touch(const TouchInput &input) override;
	void Draw(UIContext &dc) override {}
	bool CanBeFocused() const override { return false; }
	void ForceUp();

private:
	// On the game's own map a drag pans and a tap plants a marker, which MapDragTick already does
	// for a mouse - gated on the left button being held. So a drag there holds that button for as
	// long as the finger is down, and the same code runs: the pan, the travel measurement, and the
	// click-versus-drag test that decides whether a tap is a waypoint.
	void SetMapDrag(bool down);

	bool mapDrag_ = false;
	int pointer_ = -1;
	float lastX_ = 0.0f;
	float lastY_ = 0.0f;
};

bool VCSLookZone::Touch(const TouchInput &input) {
	if (input.flags & TouchInputFlags::RELEASE_ALL) {
		ForceUp();
		return false;
	}
	if ((input.flags & TouchInputFlags::DOWN) && pointer_ == -1 && !PointerClaimed(input.id) &&
			bounds_.Contains(input.x, input.y)) {
		// A tap during the logos and the credit roll skips them, which is the one thing a player
		// wants from the screen before the game has started.
		if (VCS::GetBootPhase() == VCS::BootPhase::Intro) {
			VCS::RequestIntroSkip();
			return true;
		}
		pointer_ = input.id;
		ClaimPointer(input.id);
		lastX_ = input.x;
		lastY_ = input.y;
		GamepadTouch();
		if (VCS::TouchState().context == VCS::VCSInputContext::Menu) {
			SetMapDrag(true);
		}
		return true;
	}
	if ((input.flags & TouchInputFlags::MOVE) && input.id == pointer_) {
		VCS::AddTouchLook(input.x - lastX_, input.y - lastY_);
		lastX_ = input.x;
		lastY_ = input.y;
		return true;
	}
	if ((input.flags & TouchInputFlags::UP) && input.id == pointer_) {
		ReleasePointer(input.id);
		pointer_ = -1;
		SetMapDrag(false);
		return true;
	}
	return false;
}

void VCSLookZone::ForceUp() {
	if (pointer_ != -1) {
		ReleasePointer(pointer_);
	}
	pointer_ = -1;
	SetMapDrag(false);
}

void VCSLookZone::SetMapDrag(bool down) {
	if (mapDrag_ == down) {
		return;
	}
	mapDrag_ = down;
	// The mouse's own button, in the keyboard's held set, because that is the question MapDragTick
	// asks. Nothing else in the Menu context maps it, so this says "a pointer is down" and nothing
	// more.
	VCS::SetHostKeyDown(NKCODE_EXT_MOUSEBUTTON_1, down);
}

// --- The layout ---------------------------------------------------------------------------------

// Where on the screen the GAME is drawn, and where the radar ended up inside it.
//
// It is NOT the whole screen, and assuming it was is a bug this overlay inherited from its first
// build: PSP 480x272 is 16:9.1 and a modern phone is 19.5:9 or wider, so the game sits in a
// pillarboxed strip with black down both sides - and a patch of "HUD" measured as a fraction of
// the SCREEN lands well to the left of the radar it is meant to cover. The same applies to
// integer scaling, to a stretched aspect and to any window that is not the game's own shape.
//
// This is the mapping the route overlay already uses for exactly this reason, and the one the
// display layout screen is built on - see DrawVCSRouteOverlay.
struct HudGeometry {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	float radarRight = 0.0f;
	float radarCentreY = 0.0f;

	float X(float psp) const { return x + psp / 480.0f * w; }
	float Y(float psp) const { return y + psp / 272.0f * h; }

	bool Differs(const HudGeometry &o) const {
		const float e = 0.5f;
		return std::fabs(x - o.x) > e || std::fabs(y - o.y) > e ||
			std::fabs(w - o.w) > e || std::fabs(h - o.h) > e ||
			std::fabs(radarRight - o.radarRight) > e ||
			std::fabs(radarCentreY - o.radarCentreY) > e;
	}
};

HudGeometry ComputeHudGeometry(float xres, float yres) {
	DisplayLayoutConfig &display = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
	const FRect screenFrame = GetScreenFrame(display.bIgnoreScreenInsets,
		(float)g_display.pixel_xres, (float)g_display.pixel_yres);
	FRect rect;
	CalculateDisplayOutputRect(display, &rect, 480.0f, 272.0f, screenFrame,
		display.iInternalScreenRotation);

	HudGeometry hud;
	hud.x = rect.x * g_display.dpi_scale_x;
	hud.y = rect.y * g_display.dpi_scale_y;
	hud.w = rect.w * g_display.dpi_scale_x;
	hud.h = rect.h * g_display.dpi_scale_y;
	// A rect of zero size would put every HUD zone on one point. Nothing should produce one, and
	// the whole screen is the right thing to fall back to rather than a pile in the corner.
	if (hud.w <= 1.0f || hud.h <= 1.0f) {
		hud.x = hud.y = 0.0f;
		hud.w = xres;
		hud.h = yres;
	}
	// And the widescreen fix's HUD half, which moves the game's own HUD into the middle band.
	// The rect above is where the FRAME is drawn; this is where the HUD inside it ends up.
	VCS::ApplyHudSquash(&hud.x, &hud.w);

	const VCS::VCSRadarSettings &radar = VCS::RadarSettings();
	hud.radarRight = hud.X(radar.centreX + radar.radius);
	hud.radarCentreY = hud.Y(radar.centreY);
	return hud;
}

class VCSTouchLayout : public UI::AnchorLayout {
public:
	VCSTouchLayout(float xres, float yres, bool *pause, UI::LayoutParams *layoutParams);

	void Update() override;
	bool Touch(const TouchInput &input) override;

	// Turn the overlay into something to arrange rather than something to press. In this mode
	// every control that belongs to `context` is on screen whatever the game is doing, nothing is
	// pressed, and dragging one moves it - see Touch.
	void SetEditing(bool editing, VCS::VCSInputContext context);

private:
	bool ShouldShow(const ControlSpec &spec, const VCS::VCSTouchState &state) const;
	void ReleaseEverything();
	void PlaceHudViews();
	// Put one control where its row and the player's offset for it say. The only place that
	// arithmetic lives, so building the layout, moving the radar row and dragging a control by
	// hand cannot each place it slightly differently.
	void PlaceButton(VCSTouchButton *button) const;
	// Where the finger went, in the dp the offsets are kept in.
	void DragPicked(float x, float y);

	std::vector<VCSTouchButton *> buttons_;
	// The subset anchored to the radar rather than to a screen edge, kept so they can be moved
	// again when it turns out not to be where it was.
	std::vector<VCSTouchButton *> radarButtons_;
	VCSTouchStick *stick_ = nullptr;
	VCSLookZone *look_ = nullptr;
	VCSHudZone *radarZone_ = nullptr;
	VCSHudZone *weaponZone_ = nullptr;
	float xres_ = 0.0f;
	float yres_ = 0.0f;
	float scale_ = 1.0f;
	HudGeometry hud_;
	uint32_t layoutGeneration_ = 0;
	bool visible_ = false;

	// Editing state. `editContext_` is which set of controls is being arranged, which is a
	// choice the player makes rather than something the game is doing - the game is paused.
	bool editing_ = false;
	VCS::VCSInputContext editContext_ = VCS::VCSInputContext::OnFoot;
	VCSTouchButton *picked_ = nullptr;
	int pickedPointer_ = -1;
	float pickedGrabX_ = 0.0f;
	float pickedGrabY_ = 0.0f;
	float pickedStartDx_ = 0.0f;
	float pickedStartDy_ = 0.0f;
};

VCSTouchLayout::VCSTouchLayout(float xres, float yres, bool *pause, UI::LayoutParams *layoutParams)
	: UI::AnchorLayout(layoutParams), xres_(xres), yres_(yres), scale_(TouchScale()) {
	hud_ = ComputeHudGeometry(xres, yres);
	layoutGeneration_ = VCS::TouchLayoutGeneration();

	for (const ControlSpec &spec : kControls) {
		VCSTouchButton *button = Add(new VCSTouchButton(spec, scale_, pause,
			new AnchorLayoutParams(0.0f, 0.0f, 0.0f, 0.0f, NONE, NONE)));
		button->SetVisibility(V_GONE);
		buttons_.push_back(button);
		if (spec.anchor == Anchor::Radar) {
			radarButtons_.push_back(button);
		}
		PlaceButton(button);
	}

	auto hudZone = [&](VCSHudZone::Action action) {
		VCSHudZone *zone = Add(new VCSHudZone(action, pause,
			new AnchorLayoutParams(0.0f, 0.0f, 0.0f, 0.0f, NONE, NONE)));
		zone->SetVisibility(V_GONE);
		return zone;
	};
	radarZone_ = hudZone(VCSHudZone::Action::Menu);
	weaponZone_ = hudZone(VCSHudZone::Action::NextWeapon);
	PlaceHudViews();

	// The stick's zone is the bottom-left of the screen, wide enough that a thumb landing
	// anywhere natural is inside it and short of the middle, where the look drag belongs.
	//
	// AFTER the buttons, and that is the fix for a real collision rather than tidying: the
	// vehicle's steering arrows and its drive-by pair are both inside this rectangle, and a
	// ViewGroup hands every child the same event in order - so a stick added first would have
	// claimed every touch aimed at them. It still goes BEFORE the look zone, which has to be
	// last for the same reason.
	stick_ = Add(new VCSTouchStick(new AnchorLayoutParams(
		xres * 0.42f, yres * 0.62f, 0.0f, NONE, NONE, 0.0f)));

	look_ = Add(new VCSLookZone(new AnchorLayoutParams(FILL_PARENT, FILL_PARENT, 0.0f, 0.0f, 0.0f, 0.0f)));
}

// Put everything that belongs on the game's own HUD where the game's own HUD currently is.
//
// Split out from the constructor and re-run whenever that moves, which it does at least once per
// boot for a reason nothing here can control: this port moves the radar to the top-left corner by
// patching the module, and that patch lands in __KernelLoadExec - after the screen whose views
// these are was built. Placed once at construction, the whole row sat beside where the radar used
// to be, in the bottom-left. It also covers a rotation, a change of display layout, and a player
// moving the radar themselves from the debugger.
void VCSTouchLayout::PlaceHudViews() {
	for (VCSTouchButton *button : radarButtons_) {
		PlaceButton(button);
	}

	auto place = [&](VCSHudZone *zone, float x0, float y0, float x1, float y1) {
		if (!zone) {
			return;
		}
		const float l = hud_.X(x0);
		const float t = hud_.Y(y0);
		zone->ReplaceLayoutParams(new AnchorLayoutParams(
			hud_.X(x1) - l, hud_.Y(y1) - t, l, t, NONE, NONE));
	};
	// A little wider than the disc: a thumb aimed at a 66dp circle in the corner of a phone lands
	// short of it more often than not.
	const VCS::VCSRadarSettings &radar = VCS::RadarSettings();
	const float pad = 6.0f;
	place(radarZone_,
		radar.centreX - radar.radius - pad, radar.centreY - radar.radius - pad,
		radar.centreX + radar.radius + pad, radar.centreY + radar.radius + pad);
	// The weapon panel, measured off a frame: the icon's box runs x 423..471, y 15..62 in the
	// PSP's screen. Rounded outwards to the corner, because there is nothing else up there.
	place(weaponZone_, 416.0f, 8.0f, 480.0f, 70.0f);
}

// The player's own position for a control is ADDED to the dp its row asks for rather than
// replacing it, so a control nobody has touched follows the table when the table changes - and
// the edge it is anchored to is still its edge, so the cluster stays where it belongs when the
// screen changes shape.
void VCSTouchLayout::PlaceButton(VCSTouchButton *button) const {
	const ControlSpec &spec = button->Spec();
	const float r = spec.radius * scale_;
	const VCS::VCSTouchOffset offset = VCS::TouchOffsetFor(spec.id);
	const float x = spec.x * scale_ + offset.dx;
	const float y = spec.y * scale_ + offset.dy;

	if (spec.anchor == Anchor::Radar) {
		button->ReplaceLayoutParams(new AnchorLayoutParams(r * 2.0f, r * 2.0f,
			hud_.radarRight + x - r, hud_.radarCentreY + y - r, NONE, NONE));
		return;
	}
	button->ReplaceLayoutParams(new AnchorLayoutParams(r * 2.0f, r * 2.0f,
		spec.x >= 0.0f ? x - r : NONE,
		spec.y >= 0.0f ? y - r : NONE,
		spec.x < 0.0f ? -x - r : NONE,
		spec.y < 0.0f ? -y - r : NONE));
}

bool VCSTouchLayout::ShouldShow(const ControlSpec &spec, const VCS::VCSTouchState &state) const {
	// A cutscene is answered before anything else, in both directions: its own two controls are
	// the only ones on screen while it runs, and they are on no other screen.
	const bool cutscene = state.cutscene;
	if (cutscene != ((spec.contexts & kCutscene) != 0)) {
		return false;
	}
	if (cutscene) {
		return true;
	}

	if (!(spec.contexts & Ctx(state.context))) {
		return false;
	}
	// Never, whatever the rest of the row says. This is how the melee set stays melee-only and
	// how the target-cycling pair stands down for a scope that has no targets to cycle.
	if (((spec.hide & CondArmed) && state.armed) ||
			((spec.hide & CondMelee) && state.melee) ||
			((spec.hide & CondScoped) && state.scoped) ||
			((spec.hide & CondLockedOn) && state.lockedOn) ||
			((spec.hide & CondScopeCannotShoot) && state.scopeCannotShoot) ||
			((spec.hide & CondPlane) && state.vehicleClass == VCS::VehicleClass::Plane)) {
		return false;
	}
	if (spec.cond != CondAlways) {
		return ((spec.cond & CondArmed) && state.armed) ||
			((spec.cond & CondMelee) && state.melee) ||
			((spec.cond & CondScoped) && state.scoped) ||
			((spec.cond & CondLockedOn) && state.lockedOn) ||
			((spec.cond & CondScopeZooms) && state.scopeZooms) ||
			((spec.cond & CondCameraUp) && state.cameraUp) ||
			((spec.cond & CondBike) && state.vehicleClass == VCS::VehicleClass::Bike) ||
			((spec.cond & CondMapPage) && state.mapPage) ||
			((spec.cond & CondPlane) && state.vehicleClass == VCS::VehicleClass::Plane);
	}
	// Steering by stick means no arrows, and by arrows means no stick. Both at once would be two
	// things writing one axis.
	if (spec.kind == Kind::SteerLeft || spec.kind == Kind::SteerRight) {
		return VCS::TouchSettings().steering == 0;
	}
	return true;
}

void VCSTouchLayout::ReleaseEverything() {
	for (VCSTouchButton *button : buttons_) {
		button->ForceUp();
		button->SetVisibility(V_GONE);
	}
	if (stick_) {
		stick_->ForceUp();
		stick_->SetVisibility(V_GONE);
	}
	if (look_) {
		look_->ForceUp();
		look_->SetVisibility(V_GONE);
	}
	for (VCSHudZone *zone : { radarZone_, weaponZone_ }) {
		if (zone) {
			zone->ForceUp();
			zone->SetVisibility(V_GONE);
		}
	}
	g_claimed = 0;
}

void VCSTouchLayout::Update() {
	UI::AnchorLayout::Update();
	// The fade, and the opacity setting behind it. PPSSPP's own pad layout drives this from its
	// Update and nothing else does - so without this call every control here draws at whatever
	// opacity the last screen left behind, which on a fresh boot is nothing at all.
	//
	// FORCED WHILE EDITING, and this is not a polish detail - it is why the editor came up empty.
	// The fade runs on time since the last GamepadTouch, and nothing in a menu calls that: by the
	// time somebody has walked three pages to reach ARRANGE CONTROLS the overlay has been faded
	// out for a while, so every control was laid out correctly, hit-tested correctly, dragged
	// correctly, and drawn at zero alpha. Reported exactly that way - "maybe works? but blindly".
	//
	// Never below 0.6, whatever the opacity setting says. A player who has turned the controls
	// down to a ghost still has to be able to see the one they are moving.
	if (editing_ || DesktopTouchTest()) {
		GamepadUpdateOpacity(std::max(0.6f, g_Config.iTouchButtonOpacity / 100.0f));
	} else {
		GamepadUpdateOpacity();
	}

	// The radar moves once per boot, after this was built - see PlaceHudViews.
	const HudGeometry hud = ComputeHudGeometry(xres_, yres_);
	if (hud.Differs(hud_)) {
		hud_ = hud;
		PlaceHudViews();
	}
	// And somebody may have rearranged the controls in the editor, which is a different overlay
	// entirely - see TouchLayoutGeneration.
	const uint32_t generation = VCS::TouchLayoutGeneration();
	if (generation != layoutGeneration_) {
		layoutGeneration_ = generation;
		for (VCSTouchButton *button : buttons_) {
			PlaceButton(button);
		}
	}

	// Arranging the screen rather than playing on it: every control of the chosen context is up,
	// whatever the game is doing, because the game is paused behind this and cannot say.
	if (editing_) {
		VCS::VCSTouchState pretend;
		pretend.active = true;
		pretend.context = editContext_;
		// Something that shoots in hand, so the armed-only controls are there to be moved - and
		// their melee counterparts are not, which is the same screen the player will see.
		pretend.armed = true;
		for (VCSTouchButton *button : buttons_) {
			const bool show = ShouldShow(button->Spec(), pretend);
			button->SetVisibility(show ? V_VISIBLE : V_GONE);
			button->SetArmed(true);
		}
		return;
	}

	const VCS::VCSTouchState state = VCS::TouchState();

	// Nothing at all while the game is not asking for input: the logos, a loading curtain, our own
	// menu opening. The look zone stays up through the intro, because a tap there skips it.
	const bool curtain = VCS::AutoCurtain() != VCS::CurtainKind::None;
	const bool intro = VCS::GetBootPhase() == VCS::BootPhase::Intro;
	if (!VCS::TouchSettings().enabled ||
			!(g_Config.bShowTouchControls || DesktopTouchTest()) || curtain ||
			(!state.active && !intro)) {
		if (visible_) {
			ReleaseEverything();
			visible_ = false;
		}
		if (look_ && intro && !curtain) {
			look_->SetVisibility(V_VISIBLE);
		}
		return;
	}
	visible_ = true;

	for (VCSTouchButton *button : buttons_) {
		const bool show = ShouldShow(button->Spec(), state);
		if (!show && button->GetVisibility() == V_VISIBLE) {
			// It is leaving the screen, possibly with a thumb on it. See ForceUp.
			button->ForceUp();
		}
		button->SetVisibility(show ? V_VISIBLE : V_GONE);
		button->SetArmed(state.armed);

		// The shot the auto-lock deferred, now that the game has had a frame or two to find a
		// target with the aim control already held.
		if (button->autoLockCountdown_ > 0 && button->IsDown()) {
			if (--button->autoLockCountdown_ == 0) {
				VCS::SetTouchButton(button->Spec().pad, true);
			}
		}
		// And the jump a double tap asked for, which outlives the finger that asked.
		if (button->jumpFrames_ > 0 && --button->jumpFrames_ == 0) {
			VCS::SetTouchButton(button->Spec().padSecond, false);
		}
	}

	// The stick is movement on foot, pitch and roll in the air, and steering in a car only when
	// the arrows are switched off. Never during a cutscene: there is nobody to move.
	const bool wantStick = !state.cutscene &&
		(state.context == VCS::VCSInputContext::OnFoot ||
		 state.context == VCS::VCSInputContext::Aiming ||
		 state.context == VCS::VCSInputContext::InAircraft ||
		 (state.context == VCS::VCSInputContext::InVehicle && VCS::TouchSettings().steering != 0));
	if (stick_) {
		if (!wantStick && stick_->GetVisibility() == V_VISIBLE) {
			stick_->ForceUp();
		}
		stick_->SetVisibility(wantStick ? V_VISIBLE : V_GONE);
	}
	// The camera is the game's during a cutscene, so a drag there would be fighting it - and the
	// zone covers the whole screen, so leaving it up would swallow every tap that missed SKIP.
	if (look_) {
		if (state.cutscene && look_->GetVisibility() == V_VISIBLE) {
			look_->ForceUp();
		}
		look_->SetVisibility(state.cutscene ? V_GONE : V_VISIBLE);
	}

	// The radar is on screen in every driving and on-foot state, and gone on the game's own
	// pages - and gone in a cutscene, which is what takes it away in the first place. The weapon
	// panel is only worth tapping on foot: in a car that same control is the radio, and with aim
	// held it picks the target instead, which the PREV and NEXT buttons say out loud.
	const bool gameplay = state.context != VCS::VCSInputContext::Menu && !state.cutscene;
	if (radarZone_) {
		if (!gameplay) {
			radarZone_->ForceUp();
		}
		radarZone_->SetVisibility(gameplay ? V_VISIBLE : V_GONE);
	}
	const bool weapons = state.context == VCS::VCSInputContext::OnFoot && !state.cutscene;
	if (weaponZone_) {
		if (!weapons) {
			weaponZone_->ForceUp();
		}
		weaponZone_->SetVisibility(weapons ? V_VISIBLE : V_GONE);
	}
}

void VCSTouchLayout::SetEditing(bool editing, VCS::VCSInputContext context) {
	editing_ = editing;
	editContext_ = context;
	picked_ = nullptr;
	pickedPointer_ = -1;
	for (VCSTouchButton *button : buttons_) {
		// Anything held goes up first: a control that was down when the editor opened would
		// otherwise stay down for as long as the player spent arranging the screen.
		button->ForceUp();
		button->SetEditing(editing);
	}
	if (editing) {
		if (stick_) {
			stick_->ForceUp();
			stick_->SetVisibility(V_GONE);
		}
		if (look_) {
			look_->ForceUp();
			look_->SetVisibility(V_GONE);
		}
		for (VCSHudZone *zone : { radarZone_, weaponZone_ }) {
			if (zone) {
				zone->ForceUp();
				zone->SetVisibility(V_GONE);
			}
		}
	}
	g_claimed = 0;
}

// Move the picked control to where the finger is, and remember it.
//
// The offset is kept in the direction the control is ANCHORED in, which is why the sign flips on
// a right- or bottom-anchored row: dragging left has to increase a distance measured from the
// right edge, or the control would run away from the finger.
void VCSTouchLayout::DragPicked(float x, float y) {
	if (!picked_) {
		return;
	}
	const ControlSpec &spec = picked_->Spec();
	const float signX = (spec.anchor == Anchor::Radar || spec.x >= 0.0f) ? 1.0f : -1.0f;
	const float signY = (spec.anchor == Anchor::Radar || spec.y >= 0.0f) ? 1.0f : -1.0f;
	const float dx = pickedStartDx_ + (x - pickedGrabX_) * signX;
	const float dy = pickedStartDy_ + (y - pickedGrabY_) * signY;
	VCS::SetTouchOffset(spec.id, dx, dy);
	PlaceButton(picked_);

	// Then keep it on the screen, measured against WHERE IT LANDED rather than against an
	// estimate: a row anchored to the radar has no other way of knowing, and a control half off
	// the edge is a control that cannot be pressed.
	const Bounds &placed = picked_->GetBounds();
	float clampX = 0.0f, clampY = 0.0f;
	if (placed.x < 0.0f) {
		clampX = -placed.x;
	} else if (placed.x2() > xres_) {
		clampX = xres_ - placed.x2();
	}
	if (placed.y < 0.0f) {
		clampY = -placed.y;
	} else if (placed.y2() > yres_) {
		clampY = yres_ - placed.y2();
	}
	if (clampX != 0.0f || clampY != 0.0f) {
		VCS::SetTouchOffset(spec.id, dx + clampX * signX, dy + clampY * signY);
		PlaceButton(picked_);
	}
}

bool VCSTouchLayout::Touch(const TouchInput &input) {
	// Editing takes the event before the controls see it, which is how a press becomes a drag -
	// the same shape PPSSPP's own touch layout screen uses.
	if (editing_) {
		if (input.flags & TouchInputFlags::RELEASE_ALL) {
			picked_ = nullptr;
			pickedPointer_ = -1;
			return false;
		}
		if ((input.flags & TouchInputFlags::DOWN) && !picked_) {
			for (VCSTouchButton *button : buttons_) {
				if (button->GetVisibility() != V_VISIBLE ||
						!button->GetBounds().Contains(input.x, input.y)) {
					continue;
				}
				picked_ = button;
				pickedPointer_ = input.id;
				pickedGrabX_ = input.x;
				pickedGrabY_ = input.y;
				const VCS::VCSTouchOffset offset = VCS::TouchOffsetFor(button->Spec().id);
				pickedStartDx_ = offset.dx;
				pickedStartDy_ = offset.dy;
				if (VCS::TouchSettings().haptics && g_Config.bHapticFeedback) {
					System_Vibrate(HAPTIC_VIRTUAL_KEY);
				}
				break;
			}
			return picked_ != nullptr;
		}
		if ((input.flags & TouchInputFlags::MOVE) && picked_ && input.id == pickedPointer_) {
			DragPicked(input.x, input.y);
			return true;
		}
		if ((input.flags & TouchInputFlags::UP) && input.id == pickedPointer_) {
			picked_ = nullptr;
			pickedPointer_ = -1;
			return true;
		}
		return false;
	}

	const bool handled = UI::AnchorLayout::Touch(input);
	if (input.flags & TouchInputFlags::RELEASE_ALL) {
		g_claimed = 0;
	}
	return handled;
}

// --- The layout editor ---------------------------------------------------------------------------
//
// A screen of its own rather than a mode of EmuScreen, for the reason every other screen in this
// fork is one: a UIScreen pauses the emulator, which is exactly what arranging the controls wants.
// The game stays on the screen behind it, so a control can be put where it does not cover
// anything - which is the whole question a player is answering here and cannot answer from a
// settings row.
//
// The controls themselves are the SAME layout the game uses, in editing mode, rather than a set
// of stand-ins: what you drag is the thing, at the size and with the icon it really has.
class VCSTouchEditScreen : public UIDialogScreen {
public:
	VCSTouchEditScreen() {}

	const char *tag() const override { return "VCSTouchEdit"; }
	void CreateViews() override;
	void DrawBackground(UIContext &dc) override;

private:
	void Rebuild();

	VCSTouchLayout *layout_ = nullptr;
	VCS::VCSInputContext context_ = VCS::VCSInputContext::OnFoot;
};

void VCSTouchEditScreen::DrawBackground(UIContext &dc) {
	// A wash rather than a curtain: the point of editing here is to see what the controls are
	// covering, so the game has to stay visible under them.
	dc.Flush();
	dc.Begin();
	dc.FillRect(UI::Drawable(0x60000000), dc.GetBounds());
	dc.Flush();
}

void VCSTouchEditScreen::CreateViews() {
	using namespace UI;

	const Bounds &bounds = screenManager()->getUIContext()->GetBounds();
	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));

	// The controls go in FIRST, which is what leaves the bar pressable: a ViewGroup offers each
	// touch to its children in order, and in editing mode the layout only claims one that landed
	// on a control. Anything else falls through to the bar added after it.
	g_claimed = 0;
	g_secondHolds.clear();
	VCS::ResetTouchInput();
	//
	// AnchorLayoutParams, not LayoutParams, and that is not a formality: AnchorLayout asks each
	// child's params `As<AnchorLayoutParams>()` and gets nullptr for a plain one, so the layout
	// was measured at wrap-content and placed at the origin - and every control inside it landed
	// somewhere off the screen. The editor came up with a tab bar, a hint line and nothing to
	// drag.
	layout_ = root_->Add(new VCSTouchLayout(bounds.w, bounds.h, nullptr,
		new AnchorLayoutParams(FILL_PARENT, FILL_PARENT, 0.0f, 0.0f, 0.0f, 0.0f)));
	layout_->SetEditing(true, context_);

	// Which set of controls is being arranged. They are per-context, so there is no one screen to
	// lay out - and a player who never drives should not have to look at a car's pedals.
	static const struct { const char *label; VCS::VCSInputContext context; } kTabs[] = {
		{ "ON FOOT", VCS::VCSInputContext::OnFoot },
		{ "AIMING", VCS::VCSInputContext::Aiming },
		{ "DRIVING", VCS::VCSInputContext::InVehicle },
		{ "FLYING", VCS::VCSInputContext::InAircraft },
	};
	LinearLayout *bar = root_->Add(new LinearLayout(ORIENT_HORIZONTAL,
		new AnchorLayoutParams(FILL_PARENT, WRAP_CONTENT, 0.0f, 0.0f, 0.0f, NONE)));
	bar->SetSpacing(4.0f);
	for (const auto &tab : kTabs) {
		Choice *choice = bar->Add(new Choice(tab.label, new LinearLayoutParams(1.0f)));
		const VCS::VCSInputContext target = tab.context;
		choice->OnClick.Add([this, target](UI::EventParams &e) {
			context_ = target;
			RecreateViews();
		});
	}
	bar->Add(new Choice("RESET", new LinearLayoutParams(1.0f)))->OnClick.Add(
		[this](UI::EventParams &e) {
			// Every control, not just this page's: "reset" on a screen with four tabs on it means
			// the layout, and a player who has to visit four tabs to undo one mistake has not
			// been given a reset.
			VCS::ClearTouchLayout();
			RecreateViews();
		});
	bar->Add(new Choice("DONE", new LinearLayoutParams(1.0f)))->OnClick.Add(
		[this](UI::EventParams &e) {
			TriggerFinish(DR_OK);
		});

	root_->Add(new TextView("Drag a control to move it.", ALIGN_CENTER, false,
		new AnchorLayoutParams(FILL_PARENT, WRAP_CONTENT, 0.0f, NONE, 0.0f, 8.0f)));
}

}  // namespace

Screen *CreateVCSTouchEditScreen() {
	return new VCSTouchEditScreen();
}

UI::ViewGroup *CreateVCSTouchLayout(float xres, float yres, bool *pause) {
	g_claimed = 0;
	g_steerLeft = 0;
	g_steerRight = 0;
	g_aimToggled = false;
	g_secondHolds.clear();
	VCS::ResetTouchInput();
	return new VCSTouchLayout(xres, yres, pause,
		new UI::LayoutParams(UI::FILL_PARENT, UI::FILL_PARENT));
}

void ReleaseVCSTouchArt() {
	g_art.reset();
}
