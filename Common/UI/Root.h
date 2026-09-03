#pragma once

#include <functional>

#include "Common/UI/Context.h"
#include "Common/Input/InputState.h"
#include "Common/UI/Screen.h"

namespace UI {

struct Margins;
enum class FocusFlags;

// The ONLY global is the currently focused item.
// Can be and often is null.
void EnableFocusMovement(bool enable);
bool IsFocusMovementEnabled();
View *GetFocusedView();
void SetFocusedView(View *view, FocusFlags cause, bool force = false);
void RemoveQueuedEventsByEvent(Event *e);
void RemoveQueuedEventsByView(View * v);

void EventTriggered(Event *e, EventParams params);
DialogResult DispatchEvents();

class ViewGroup;

void LayoutViewHierarchy(const UIContext &dc, const UI::Margins &rootMargins, UI::ViewGroup *root, ViewLayoutMode layoutMode, bool immersiveMode);
DialogResult UpdateViewHierarchy(ViewGroup *root, bool canEnableFocusMovement = true);

enum class KeyEventResult {
	IGNORE_KEY,  // Don't let it be processed.
	PASS_THROUGH,  // Let it be processed, but return false.
	ACCEPT,  // Let it be processed, but return true.
};

KeyEventResult KeyEventToFocusMoves(const KeyInput &key);

bool KeyEvent(const KeyInput &key, ViewGroup *root);
void TouchEvent(const TouchInput &touch, ViewGroup *root);
void AxisEvent(const AxisInput &axis, ViewGroup *root);

enum class UISound {
	SELECT = 0,
	BACK,
	CONFIRM,
	TOGGLE_ON,
	TOGGLE_OFF,
	ACHIEVEMENT_UNLOCKED,
	LEADERBOARD_SUBMITTED,

	// The GTA: Vice City Stories front end's own three, lifted out of the game's sound bank by
	// Tools/vcsmenusfx.py. Added rather than swapped into SELECT/CONFIRM/BACK so that nothing
	// outside UI/VCSMenuScreen.cpp - which is the only thing that plays them - can hear them:
	// overriding the three shared samples would put GTA blips in PPSSPP's own menus for whatever
	// game got booted next.
	VCS_HIGHLIGHT,
	VCS_SELECT,
	VCS_BACK,

	COUNT,
};

void SetSoundCallback(std::function<void(UISound)> func);

// This is only meant for actual UI navigation sound, not achievements.
// Call directly into the player for other UI effects.
void PlayUISound(UISound sound);

}  // namespace UI
