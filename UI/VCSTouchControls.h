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

#include "Common/UI/ScreenManager.h"
#include "Common/UI/ViewGroup.h"

// The phone's controls for VCS, in place of PPSSPP's on-screen PSP pad.
//
// WHY A SECOND SET OF TOUCH CONTROLS EXISTS AT ALL
// PPSSPP's own buttons are the PSP's: a d-pad, four face buttons and a stick, each writing
// straight into sceCtrl. That is right for an emulator and wrong for this, twice over. It skips
// the VCS layer entirely, so none of the context rules this fork is built on apply - the same
// button cannot be Jump on foot and Handbrake in a car. And it puts every control on screen at
// once, including the ones that mean nothing where you are standing.
//
// So this draws what the GTA trilogy's phone ports drew: a stick that appears under your thumb,
// the rest of the screen as the camera, one big action button showing the weapon, and controls
// that come and go with the situation. Underneath, every one of them presses a PAD control
// (see the touch section of Core/VCS/VCSInput.h), so the whole context-aware scheme applies to a
// thumb without being taught that one exists.
//
// The layout is rebuilt when the context changes and hidden entirely during cutscenes and
// loading, which is the one thing a fixed overlay can never do.
UI::ViewGroup *CreateVCSTouchLayout(float xres, float yres, bool *pause);

// The layout editor: the same overlay, paused, with the controls draggable. Pushed from the
// TOUCH settings page - see VCSTouchEditScreen for why it is a screen rather than a mode.
Screen *CreateVCSTouchEditScreen();

// The controls' icons are textures, and a screen outlives the graphics device - so they are
// released when the device goes rather than when the layout is rebuilt, which is the same split
// the menu's title art and the boot curtain already make. Called from EmuScreen::deviceLost.
void ReleaseVCSTouchArt();
