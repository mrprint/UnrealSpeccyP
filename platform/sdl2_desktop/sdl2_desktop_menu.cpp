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

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_menu.cpp
//
//  Menu bar (File/View/Device/Window/Help), status bar text, About window,
//  and keyboard-shortcut table - a port of platform/wxwidgets/wx_frame.cpp's
//  Frame class to this platform's free-function/xOptions style. Menu item
//  wording, order, grouping and the SHORTCUT_* keys below are copied from
//  wx_frame.cpp as closely as ImGui's model allows.
//
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#include "imgui.h"
#include "imgui_shared.h"
#include "sdl2_desktop_filedialog.h"
#include "../../tools/options.h"
#include "../../options_common.h"

namespace xPlatform
{

// sdl2_desktop_video.cpp
void ResizeToOrgSizeMultiple(int mult); // 1/2/3 = 100%/200%/300% of org_size (320x240)

namespace xImGui
{

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool g_show_about = false;
// Mirrors wx's menu_quick_save->Enable(false) at startup, ->Enable(true)
// after a successful Open or Quick Load - see Frame::OnOpenFile()/OnQuickLoad().
static bool g_quick_save_enabled = false;

// ---------------------------------------------------------------------------
// Actions - each one is both a menu command and (via HandleMenuShortcut)
// a keyboard shortcut target, exactly like a single wx EVT_MENU handler
// serves both the menu click and the accelerator.
// ---------------------------------------------------------------------------

static void OnReset()
{
	if(Handler()->OnAction(A_RESET) == AR_OK)
		SetStatusText("Reset OK");
	else
		SetStatusText("Reset FAILED");
}

static void OnOpenFileConfirmed(const std::string& path)
{
	if(Handler()->OnOpenFile(path.c_str()))
	{
		SetStatusText("File open OK");
		g_quick_save_enabled = true;
	}
	else
		SetStatusText("File open FAILED");
}

static void OnOpenFileAction()
{
	std::vector<FileDialogFilter> filters = {
		{"Supported files (*.sna;*.z80;*.szx;*.rzx;*.trd;*.scl;*.fdi;*.tap;*.csw;*.tzx;*.zip)",
			{"sna","z80","szx","rzx","trd","scl","fdi","tap","csw","tzx","zip"}},
		{"All files", {}},
		{"Snapshot files (*.sna;*.z80;*.szx)", {"sna","z80","szx"}},
		{"Replay files (*.rzx)", {"rzx"}},
		{"Disk images (*.trd;*.scl;*.fdi;*.td0;*.udi)", {"trd","scl","fdi","td0","udi"}},
		{"Tape files (*.tap;*.csw;*.tzx)", {"tap","csw","tzx"}},
		{"ZIP archives (*.zip)", {"zip"}},
	};
	OpenFileBrowser("Open", OpLastFolder(), filters, false, "", OnOpenFileConfirmed);
}

static void OnSaveFileConfirmed(const std::string& path)
{
	if(Handler()->OnSaveFile(path.c_str()))
		SetStatusText("File save OK");
	else
		SetStatusText("File save FAILED");
}

static void OnSaveFileAction()
{
	std::vector<FileDialogFilter> filters = {
		{"Snapshot files (*.sna)", {"sna"}},
		{"Screenshot files (*.png)", {"png"}},
	};
	OpenFileBrowser("Save", OpLastFolder(), filters, true, "", OnSaveFileConfirmed);
}

static void OnFullScreenToggle()
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("full screen");
	if(op) op->Set(!*op);
}

static void SetZoom(int v)
{
	xOptions::eOption<int>* op = xOptions::eOption<int>::Find("zoom");
	if(op) op->Set(v);
}

static void OnTapeToggle()
{
	switch(Handler()->OnAction(A_TAPE_TOGGLE))
	{
	case AR_TAPE_STARTED:      SetStatusText("Tape started");      break;
	case AR_TAPE_STOPPED:      SetStatusText("Tape stopped");      break;
	case AR_TAPE_NOT_INSERTED: SetStatusText("Tape not inserted"); break;
	default: break;
	}
}

// Flips a bool xOption and reports the result on the status bar - the exact
// same three lines that OnTrueSpeedToggle/OnMode48kToggle/
// OnViewGigascreenToggle/OnViewScanlinesToggle/OnViewPalEffectsToggle used to
// each repeat as a whole separate function, with only the option name and
// on/off message text changed; call sites now just pass those three things
// in directly (see DrawMenuBar()/HandleMenuShortcut() below) instead of
// going through a named wrapper per option.
// (OnFullScreenToggle/OnPauseToggle/OnTapeToggle/OnQuickLoad/OnQuickSave/
// OnReset each do something extra beyond this shape - no status message, an
// additional Handler() call, ->Change() instead of ->Set(!*) - so those keep
// their own functions rather than being forced in here.
static void ToggleBoolOption(const char* option_name, const char* on_msg, const char* off_msg)
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find(option_name);
	if(!op) return;
	op->Set(!*op);
	SetStatusText(*op ? on_msg : off_msg);
}

static void OnPauseToggle()
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("pause");
	if(!op) return;
	op->Set(!*op);
	if(*op)
	{
		Handler()->VideoPaused(true);
		SetStatusText("Paused...");
	}
	else
	{
		Handler()->VideoPaused(false);
		SetStatusText("Ready...");
	}
}

static void OnQuickLoad()
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("load state");
	if(!op) return;
	op->Change();
	SetStatusText(*op ? "Quick load OK" : "Quick load FAILED");
	if(*op) g_quick_save_enabled = true;
}

static void OnQuickSave()
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("save state");
	if(!op) return;
	op->Change();
	SetStatusText(*op ? "Quick save OK" : "Quick save FAILED");
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts - see wx_frame.cpp's SHORTCUT_* defines (non-_MAC set).
// Called from sdl2_desktop_keys.cpp, ahead of ZX-keyboard translation.
// ---------------------------------------------------------------------------

bool HandleMenuShortcut(SDL_Event& e)
{
	if(e.type != SDL_KEYDOWN && e.type != SDL_KEYUP)
		return false;
	// Let a focused text field (file browser path/name, gamepad rename in
	// the Options dialog, ...) receive its keys normally - wx has no
	// equivalent concern since its file/options dialogs are separate native
	// windows that simply don't share the main frame's accelerator table
	// while open.
	if(ImGui::GetIO().WantTextInput)
		return false;
	// wx's modal wxFileDialog/OptionsDialog block the parent frame's own
	// accelerator table for as long as they're open; these overlays aren't
	// OS-modal, so that has to be done explicitly here instead.
	if(FileBrowserActive() || OptionsDialogActive())
		return false;

	SDL_Keycode key = e.key.keysym.sym;
	Uint16 mod = e.key.keysym.mod;
	bool ctrl = (mod & KMOD_CTRL) != 0;
	bool shift = (mod & KMOD_SHIFT) != 0;
	bool down = (e.type == SDL_KEYDOWN);

	if(!ctrl && !shift)
	{
		switch(key)
		{
		case SDLK_F3:  if(down) OnOpenFileAction();          return true;
		case SDLK_F2:  if(down) OnSaveFileAction();          return true;
		case SDLK_F4:  if(down) OnQuickLoad();                return true;
		case SDLK_F6:  if(down && g_quick_save_enabled) OnQuickSave(); return true;
		case SDLK_F5:  if(down) OnTapeToggle();               return true;
		case SDLK_F7:  if(down) OnPauseToggle();              return true;
		case SDLK_F8:  if(down) ToggleBoolOption("true speed", "True speed (50Hz mode) on", "True speed off"); return true;
		case SDLK_F9:  if(down) ToggleBoolOption("mode 48k", "Mode 48k on", "Mode 48k off"); return true;
		case SDLK_F12: if(down) OnReset();                    return true;
		default: break;
		}
	}
	if(ctrl && !shift)
	{
		switch(key)
		{
		case SDLK_1: if(down) ResizeToOrgSizeMultiple(1); return true;
		case SDLK_2: if(down) ResizeToOrgSizeMultiple(2); return true;
		case SDLK_3: if(down) ResizeToOrgSizeMultiple(3); return true;
		case SDLK_f: if(down) OnFullScreenToggle();       return true;
		default: break;
		}
	}
	if(ctrl && shift)
	{
		switch(key)
		{
		case SDLK_1: if(down) SetZoom(0);                return true;
		case SDLK_2: if(down) SetZoom(1);                return true;
		case SDLK_3: if(down) SetZoom(2);                return true;
		case SDLK_g: if(down) ToggleBoolOption("gigascreen", "Gigascreen on", "Gigascreen off"); return true;
		case SDLK_s: if(down) ToggleBoolOption("scanlines", "CRT scanlines simulation on", "CRT scanlines simulation off"); return true;
		case SDLK_p: if(down) ToggleBoolOption("pal effects", "PAL effects on", "PAL effects off"); return true;
		default: break;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void DrawMenuBar()
{
	if(!ImGui::BeginMainMenuBar())
		return;

	// --- File ---
	if(ImGui::BeginMenu("File"))
	{
		if(ImGui::MenuItem("Open...", "F3"))
			OnOpenFileAction();
		if(ImGui::MenuItem("Save...", "F2"))
			OnSaveFileAction();
		ImGui::Separator();
		if(ImGui::MenuItem("Quick Load", "F4"))
			OnQuickLoad();
		if(ImGui::MenuItem("Quick Save", "F6", false, g_quick_save_enabled))
			OnQuickSave();
		ImGui::Separator();
		OptionCheckbox("auto play image", "Auto launch programs");
		ImGui::Separator();
		if(ImGui::MenuItem("Exit"))
			OpQuit(true);
		ImGui::EndMenu();
	}

	// --- View ---
	if(ImGui::BeginMenu("View"))
	{
		xOptions::eOption<int>* op_zoom = xOptions::eOption<int>::Find("zoom");
		int zoom = op_zoom ? (int)*op_zoom : -1;
		if(ImGui::MenuItem("Fill screen", "Ctrl+Shift+1", zoom == 0))
			SetZoom(0);
		if(ImGui::MenuItem("Small border", "Ctrl+Shift+2", zoom == 1))
			SetZoom(1);
		if(ImGui::MenuItem("No border", "Ctrl+Shift+3", zoom == 2))
			SetZoom(2);
		ImGui::Separator();
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("gigascreen");
			if(op && ImGui::MenuItem("Gigascreen", "Ctrl+Shift+G", *op))
				ToggleBoolOption("gigascreen", "Gigascreen on", "Gigascreen off");
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("scanlines");
			if(op && ImGui::MenuItem("CRT scanlines", "Ctrl+Shift+S", *op))
				ToggleBoolOption("scanlines", "CRT scanlines simulation on", "CRT scanlines simulation off");
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("pal effects");
			if(op && ImGui::MenuItem("PAL effects", "Ctrl+Shift+P", *op))
				ToggleBoolOption("pal effects", "PAL effects on", "PAL effects off");
		}
		ImGui::Separator();
		if(ImGui::MenuItem("Full screen", "Ctrl+F"))
			OnFullScreenToggle();
		ImGui::EndMenu();
	}

	// --- Device ---
	if(ImGui::BeginMenu("Device"))
	{
		if(ImGui::MenuItem("Start/Stop tape", "F5"))
			OnTapeToggle();
		OptionCheckbox("fast tape", "Tape fast");
		ImGui::Separator();
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("pause");
			if(op && ImGui::MenuItem("Pause", "F7", *op))
				OnPauseToggle();
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("true speed");
			if(op && ImGui::MenuItem("True speed", "F8", *op))
				ToggleBoolOption("true speed", "True speed (50Hz mode) on", "True speed off");
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("mode 48k");
			if(op && ImGui::MenuItem("Mode 48k", "F9", *op))
				ToggleBoolOption("mode 48k", "Mode 48k on", "Mode 48k off");
		}
		OptionCheckbox("reset to service rom", "Reset to service ROM");
		if(ImGui::MenuItem("Reset", "F12"))
			OnReset();
		ImGui::Separator();
		if(ImGui::MenuItem("Options..."))
		{
			// Re-clicking while already open would otherwise re-run
			// OpenOptionsDialog() below, which calls LoadCurrentSettings()
			// and resets g_active_tab - silently discarding whatever the
			// user has mid-edit in the already-open dialog. Just bring it
			// to front instead; SetWindowFocus() raises a floating window
			// the same way clicking it would.
			if(OptionsDialogActive())
				ImGui::SetWindowFocus("Options");
			else
				OpenOptionsDialog();
		}
		ImGui::EndMenu();
	}

	// --- Window ---
	if(ImGui::BeginMenu("Window"))
	{
		if(ImGui::MenuItem("Size 100%", "Ctrl+1"))
			ResizeToOrgSizeMultiple(1);
		if(ImGui::MenuItem("Size 200%", "Ctrl+2"))
			ResizeToOrgSizeMultiple(2);
		if(ImGui::MenuItem("Size 300%", "Ctrl+3"))
			ResizeToOrgSizeMultiple(3);
		ImGui::EndMenu();
	}

	// --- Help ---
	if(ImGui::BeginMenu("Help"))
	{
		if(ImGui::MenuItem("About"))
		{
			if(g_show_about)
				ImGui::SetWindowFocus("About"); // already open elsewhere - raise it instead of a no-op re-set
			else
				g_show_about = true;
		}
		ImGui::EndMenu();
	}

	ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------
// About window - content matches Frame::OnAbout()'s wxAboutDialogInfo
// (non-_MAC branch).
// ---------------------------------------------------------------------------

static void DrawAboutWindow()
{
	if(!g_show_about)
		return;
	ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
	if(ImGui::Begin("About", &g_show_about, ImGuiWindowFlags_NoCollapse))
	{
		// Esc closes the topmost dialog while UI has keyboard focus (see
		// Loop1()'s ui_want_keyboard gate in sdl2_desktop.cpp - Esc is only
		// even offered to this window when the UI genuinely wants the
		// keyboard right now, not e.g. while the emulated ZX keyboard has
		// it with this window merely sitting open in the background).
		// RootAndChildWindows so this still fires no matter which part of
		// the window has focus, not only its exact root.
		if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
			g_show_about = false;
		ImGui::TextUnformatted(Handler()->WindowCaption());
		ImGui::Text("Version 0.0.90");
		ImGui::Separator();
		ImGui::TextWrapped("Portable ZX Spectrum emulator.");
		ImGui::Spacing();
		ImGui::TextWrapped("Copyright (C) 2001-2020 SMT, Dexus, Alone Coder, deathsoft, djdron, scor.");
		ImGui::Spacing();
		ImGui::TextUnformatted("https://bitbucket.org/djdron/unrealspeccyp");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextWrapped(
			"This program is free software: you can redistribute it and/or modify "
			"it under the terms of the GNU General Public License as published by "
			"the Free Software Foundation, either version 3 of the License, or "
			"(at your option) any later version.\n\n"
			"This program is distributed in the hope that it will be useful, "
			"but WITHOUT ANY WARRANTY; without even the implied warranty of "
			"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the "
			"GNU General Public License for more details.\n\n"
			"You should have received a copy of the GNU General Public License "
			"along with this program. If not, see <http://www.gnu.org/licenses/>.");
	}
	ImGui::End();
}

void DrawMenuDialogs()
{
	DrawAboutWindow();
	DrawFileBrowser();
}

// See imgui_shared.h - deliberately excludes the file browser.
bool AnyMenuDialogActive()
{
	return g_show_about || OptionsDialogActive();
}

void CloseMenuDialogs()
{
	g_show_about = false;
	CloseOptionsDialog();
}

void InitMenu()
{
	SetStatusText("Ready...");
}

}//namespace xImGui
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP

