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

// =============================================================================
//  platform/sdl2_desktop/imgui_shared.h
//
//  Declarations shared between this platform's own ImGui-drawing files:
//    sdl2_desktop_imgui.cpp    - core Init/BeginFrame/EndFrame, style, the
//                                generic xOptions<->widget helpers, status bar
//    sdl2_desktop_menu.cpp     - the wx_frame.cpp-equivalent menu bar, About
//                                window, window-size/quick-save state
//    sdl2_desktop_options.cpp  - the wx_optionsdialog.cpp-equivalent 5-tab
//                                Options dialog
//    sdl2_desktop_filedialog.* - generic in-engine file browser (own header)
//    sdl2_desktop_gamepad.*    - ported wx_gamepad/joystick_mapper (own header)
// =============================================================================

namespace xPlatform {
namespace xImGui {

// --- generic xOptions <-> ImGui widget helpers (sdl2_desktop_imgui.cpp) ---
// Read the option's current value, draw the widget, write back on change -
// no separate data-binding layer, see the write-up on why this fits ImGui's
// immediate-mode model naturally.
void OptionCheckbox(const char* option_name, const char* label);
void OptionCombo(const char* option_name, const char* label);
void OptionSliderInt(const char* option_name, const char* label, int lo, int hi);

// --- persistent status bar (sdl2_desktop_imgui.cpp) ---
// Equivalent of wx's wxFrame::SetStatusText() - one line of text, always
// visible at the bottom, replaced (not queued/stacked) by the next call.
// Also where platform/gl/draw.cpp's LightweightShadersMessage() posts to,
// same as it posts to the *same* native status bar in the wx build.
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
// wx_frame.cpp's SHORTCUT_* accelerator table, checked ahead of ZX keyboard
// translation - called from sdl2_desktop_keys.cpp. Returns true if the event
// was consumed as a shortcut.

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

