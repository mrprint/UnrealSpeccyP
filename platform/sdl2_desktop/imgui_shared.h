/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2026 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __SDL2_DESKTOP_IMGUI_SHARED_H__
#define __SDL2_DESKTOP_IMGUI_SHARED_H__

#pragma once

#include <cstddef>
#include <cstring>
#include <string>

#include "sdl2_desktop_i18n.h"
#include "imgui.h"

// =============================================================================
//  platform/sdl2_desktop/imgui_shared.h
//
//  Declarations shared between this platform's own ImGui-drawing files:
//    sdl2_desktop_imgui.cpp    - core Init/BeginFrame/EndFrame, style, the
//                                generic xOptions<->widget helpers, status bar
//    sdl2_desktop_menu.cpp     - the menu bar, About window,
//                                window-size/quick-save state
//    sdl2_desktop_options.cpp  - the 5-tab Options dialog
//    sdl2_desktop_filedialog.* - generic in-engine file browser (own header)
//    sdl2_desktop_gamepad.*    - gamepad backend (own header)
//    sdl2_desktop_i18n.*       - text localization (res/lang/*.xml)
// =============================================================================

namespace xPlatform {
namespace xImGui {

// Brings xI18n::Tr() in as a short, unqualified Tr() for every file that
// includes this header (which is all of the ones drawing UI text) - see
// sdl2_desktop_i18n.h for what it does and its fallback behaviour.
using xI18n::Tr;

// Dear ImGui derives a window/popup's persistent ID (position, open state,
// which popup is on top of which) from its *entire* title string - see the
// "###" mechanism in Dear ImGui's own docs. A title built from Tr() would
// therefore get a new identity the instant the active language changes,
// silently resetting/duplicating any such window that happens to be open
// at that moment (the Options dialog itself is the concrete case: its own
// language combo lives inside it, so a language switch happening while it
// is open is the common case, not an edge case). TrTitle() keeps the
// visible part translated but pins the identity to `stable_id`:
//   ImGui::Begin(TrTitle("about.title", "About").c_str(), &open, ...)
// Only needed for windows/popups whose open/closed state persists across
// frames (About, Options, the overwrite-confirm popup) - a menu bar entry
// or a Selectable in a list needs no such pinning, see their call sites.
inline std::string TrTitle(const char* id, const char* stable_id)
{
	return std::string(Tr(id)) + "###" + stable_id;
}

// --- tooltips ---
// How long the cursor must rest on an item before its tooltip appears, in
// seconds. Dear ImGui shows tooltips the instant an item is hovered; a short
// delay (standard desktop convention) keeps them from flashing by as the
// cursor sweeps across the UI.
constexpr double kTooltipDelaySeconds = 1.0;

// The delay itself. `hovered` is whether the item in question is hovered
// right now (the caller decides how to ask, see below). Returns true once
// the cursor has stayed on it long enough for the tooltip to appear.
//
// Timed per hovered item: ItemTooltip() is an inline called once per
// widget, so a single shared "cursor is on *some* item" timer would be
// reset by every non-hovered call site that runs after the hovered one in
// draw order, and the tooltip would never fire. The timer is therefore
// keyed on the hovered item's own id (ImGui::GetItemID() - the "last
// item", i.e. the widget each call site just drew): non-hovered call
// sites return early without touching it, and it (re)starts only when the
// hovered id changes. At most one item is hovered at a time, so the timer
// is stable across frames regardless of which call site runs when. The
// hovered id and its start time are function-local statics; this is
// single-threaded immediate-mode UI, so no locking is needed. (The id of
// the item the cursor left is not reset while the cursor is off every
// item, so re-hovering the same item right away can still complete its
// delay - a deliberate, harmless simplification.)
inline bool TooltipDue(bool hovered)
{
	static ImGuiID timer_id = 0;
	static double timer_since = 0.0;
	if(!hovered)
		return false;
	const ImGuiID id = ImGui::GetItemID();
	if(id != timer_id)
	{
		timer_id = id;
		timer_since = ImGui::GetTime();
	}
	return (ImGui::GetTime() - timer_since) >= kTooltipDelaySeconds;
}

// Shows a translated tooltip while the last drawn item is hovered. Call it
// immediately after the widget (ImGui's "last item" is the one just drawn) -
// for a BeginCombo()/EndCombo() pair that means right after EndCombo(). Takes
// the untranslated key (e.g. "tip.menu.view.gigascreen") rather than
// pre-translated text, the same way every other label in this codebase does,
// so a live language switch keeps working. AllowWhenDisabled because a
// couple of call sites (the Gamepads tab's Capture/Capturing buttons) sit
// behind BeginDisabled(), and a plain IsItemHovered() would report false
// for those - the tip is still useful on a disabled button (it explains
// what the button would do / why it's waiting). The tip only appears after
// the cursor has rested on the item for kTooltipDelaySeconds (TooltipDue()).
inline void ItemTooltip(const char* key)
{
	if(TooltipDue(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)))
		ImGui::SetTooltip("%s", Tr(key));
}

// --- generic xOptions <-> ImGui widget helpers (sdl2_desktop_imgui.cpp) ---
// Read the option's current value, draw the widget, write back on change -
// no separate data-binding layer, see the write-up on why this fits ImGui's
// immediate-mode model naturally.
void OptionCheckbox(const char* option_name, const char* label);
void OptionCombo(const char* option_name, const char* label);
void OptionSliderInt(const char* option_name, const char* label, int lo, int hi);

// --- persistent status bar (sdl2_desktop_imgui.cpp) ---
// One line of text, always visible at the bottom, replaced (not
// queued/stacked) by the next call. Also where platform/gl/draw.cpp's
// LightweightShadersMessage() posts to.
void SetStatusText(const char* text);

// --- menu bar + About window + window-size/quick-save state (sdl2_desktop_menu.cpp) ---
void InitMenu(); // call once after Init(), sets the "Ready..." status text
void DrawMenuBar();
// Draws the About window if open, and the file-browser-driven Open/Save flows.
void DrawMenuDialogs();
// True while the About window and/or Options dialog is open. Deliberately
// excludes the file browser - see CloseMenuDialogs() below and Loop1()'s
// SDL_MOUSEBUTTONDOWN handling in sdl2_desktop.cpp for what this drives.
bool AnyMenuDialogActive();
// Dismisses the About window and Options dialog (not the file browser,
// which stays open through an outside click - picking a ROM/snapshot is a
// single focused task, closer to a native modal file picker than to a
// settings panel). Called from sdl2_desktop.cpp's Loop1() when a click
// lands on the running emulator view while a dialog is still open
// elsewhere: that's a clear "I want to interact with the game now" signal,
// so it dismisses the dialog instead of also letting that same click reach
// sdl2_mouse.cpp's Kempston-mouse grab, which would otherwise hide/capture
// the cursor while leaving the dialog visible but unreachable until Escape.
void CloseMenuDialogs();
// Menu accelerator table, checked ahead of ZX keyboard translation - called
// from sdl2_desktop_keys.cpp. Returns true if the event was consumed as a
// shortcut.

// --- Options dialog (sdl2_desktop_options.cpp) ---
void OpenOptionsDialog();
void CloseOptionsDialog(); // see CloseMenuDialogs() above
void DrawOptionsDialog();
bool OptionsDialogActive();

// --- small shared helpers ---
// Copies `src` into the fixed-size `dst` buffer (capacity `cap`), truncating
// if necessary and always NUL-terminating.
inline void CopyToBuffer(char* dst, size_t cap, const std::string& src)
{
	if(!dst || cap == 0)
		return;
	const size_t n = (src.size() < cap - 1) ? src.size() : cap - 1;
	if(n > 0)
		memcpy(dst, src.data(), n);
	dst[n] = 0;
}

inline void CopyToBuffer(char* dst, size_t cap, const char* src)
{
	CopyToBuffer(dst, cap, std::string(src ? src : ""));
}

}//namespace xImGui
}//namespace xPlatform

#endif//__SDL2_DESKTOP_IMGUI_SHARED_H__

