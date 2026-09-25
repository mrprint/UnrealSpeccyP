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
//  and keyboard-shortcut table.
//
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#include <cmath>
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
// Disabled at startup, enabled after a successful Open or Quick Load.
static bool g_quick_save_enabled = false;

// ---------------------------------------------------------------------------
// Actions - each one is both a menu command and (via HandleMenuShortcut)
// a keyboard shortcut target.
// ---------------------------------------------------------------------------

static void OnReset()
{
	if(Handler()->OnAction(A_RESET) == AR_OK)
		SetStatusText(Tr("status.reset.ok"));
	else
		SetStatusText(Tr("status.reset.failed"));
}

static void OnOpenFileConfirmed(const std::string& path)
{
	if(Handler()->OnOpenFile(path.c_str()))
	{
		SetStatusText(Tr("status.file_open.ok"));
		g_quick_save_enabled = true;
	}
	else
		SetStatusText(Tr("status.file_open.failed"));
}

static void OnOpenFileAction()
{
	// Filter labels are translated; the "(*.ext;*.ext)" part is data, not
	// prose, so it's baked into each label's translation rather than built
	// separately - a translator can still see/keep it since it's right
	// there in the string being translated.
	std::vector<FileDialogFilter> filters = {
		{Tr("filedialog.filter.supported"),
			{"sna","z80","szx","rzx","trd","scl","fdi","tap","csw","tzx","zip"}},
		{Tr("filedialog.filter.all"), {}},
		{Tr("filedialog.filter.snapshot"), {"sna","z80","szx"}},
		{Tr("filedialog.filter.replay"), {"rzx"}},
		{Tr("filedialog.filter.disk"), {"trd","scl","fdi","td0","udi"}},
		{Tr("filedialog.filter.tape"), {"tap","csw","tzx"}},
		{Tr("filedialog.filter.zip"), {"zip"}},
	};
	OpenFileBrowser(Tr("filedialog.title.open"), OpLastFolder(), filters, false, "", OnOpenFileConfirmed);
}

static void OnSaveFileConfirmed(const std::string& path)
{
	if(Handler()->OnSaveFile(path.c_str()))
		SetStatusText(Tr("status.file_save.ok"));
	else
		SetStatusText(Tr("status.file_save.failed"));
}

static void OnSaveFileAction()
{
	std::vector<FileDialogFilter> filters = {
		{Tr("filedialog.filter.snapshot_save"), {"sna"}},
		{Tr("filedialog.filter.screenshot"), {"png"}},
	};
	OpenFileBrowser(Tr("filedialog.title.save"), OpLastFolder(), filters, true, "", OnSaveFileConfirmed);
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
	case AR_TAPE_STARTED:      SetStatusText(Tr("status.tape.started"));      break;
	case AR_TAPE_STOPPED:      SetStatusText(Tr("status.tape.stopped"));      break;
	case AR_TAPE_NOT_INSERTED: SetStatusText(Tr("status.tape.not_inserted")); break;
	default: break;
	}
}

// Flips a bool xOption and reports the result on the status bar.
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
		SetStatusText(Tr("status.pause.paused"));
	}
	else
	{
		Handler()->VideoPaused(false);
		SetStatusText(Tr("status.pause.ready"));
	}
}

static void OnQuickLoad()
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("load state");
	if(!op) return;
	op->Change();
	SetStatusText(*op ? Tr("status.quickload.ok") : Tr("status.quickload.failed"));
	if(*op) g_quick_save_enabled = true;
}

static void OnQuickSave()
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("save state");
	if(!op) return;
	op->Change();
	SetStatusText(*op ? Tr("status.quicksave.ok") : Tr("status.quicksave.failed"));
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts. Called from sdl2_desktop_keys.cpp, ahead of
// ZX-keyboard translation.
// ---------------------------------------------------------------------------

bool HandleMenuShortcut(SDL_Event& e)
{
	if(e.type != SDL_KEYDOWN && e.type != SDL_KEYUP)
		return false;
	// Let a focused text field (file browser path/name, gamepad rename in
	// the Options dialog, ...) receive its keys normally.
	if(ImGui::GetIO().WantTextInput)
		return false;
	// The file browser and Options dialog aren't OS-modal, so blocking the
	// accelerator table while they're open has to be done explicitly here.
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
		case SDLK_F8:  if(down) ToggleBoolOption("true speed", Tr("status.true_speed.on"), Tr("status.true_speed.off")); return true;
		case SDLK_F9:  if(down) ToggleBoolOption("mode 48k", Tr("status.mode_48k.on"), Tr("status.mode_48k.off")); return true;
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
		case SDLK_g: if(down) ToggleBoolOption("gigascreen", Tr("status.gigascreen.on"), Tr("status.gigascreen.off")); return true;
		case SDLK_s: if(down) ToggleBoolOption("scanlines", Tr("status.scanlines.on"), Tr("status.scanlines.off")); return true;
		case SDLK_p: if(down) ToggleBoolOption("pal effects", Tr("status.pal_effects.on"), Tr("status.pal_effects.off")); return true;
		case SDLK_r: if(down) ToggleBoolOption("Prefer PAL refresh", Tr("status.pal_refresh.on"), Tr("status.pal_refresh.off")); return true;
		default: break;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Language switcher - see sdl2_desktop_i18n.h. A small globe icon at the
// right edge of the menu bar, opening a popup with the language list on
// click - lives here (rather than as a combo inside the Options dialog, an
// earlier iteration of this) so it's reachable in one click from anywhere,
// the way a browser's own language switcher usually is.
//
// Drawn as plain ImDrawList shapes rather than a text glyph: this
// project's Dear ImGui build uses the default 16-bit ImWchar (no
// IMGUI_USE_WCHAR32 anywhere in build/cmake/CMakeLists.txt), so an actual
// "globe" Unicode character such as U+1F310 (outside the Basic
// Multilingual Plane) can't even be represented, let alone rendered by
// Roboto-Regular.ttf - a plain text face with no pictographic glyphs to
// begin with, see LoadFont() in sdl2_desktop_imgui.cpp. Hand-drawn vector
// shapes sidestep both problems and render identically regardless of
// which language - or font - is active.
// ---------------------------------------------------------------------------

static void DrawGlobeIcon(ImDrawList* draw_list, ImVec2 center, float r, ImU32 col, float thickness)
{
	draw_list->AddCircle(center, r, col, 0, thickness);
	draw_list->AddLine(ImVec2(center.x - r, center.y), ImVec2(center.x + r, center.y), col, thickness); // equator
	draw_list->AddLine(ImVec2(center.x, center.y - r), ImVec2(center.x, center.y + r), col, thickness); // prime meridian

	// Two curved meridians, traced as manual polylines rather than via
	// AddEllipse()/AddBezierCubic(): those are comparatively recent Dear
	// ImGui additions, while AddPolyline() has been part of the API since
	// its earliest public releases, so the icon stays buildable regardless
	// of exactly which imgui commit 3rdparty/imgui happens to be pinned to.
	const float kPi = 3.14159265358979323846f;
	const int segments = 10;
	for(int side = -1; side <= 1; side += 2)
	{
		ImVec2 pts[segments + 1];
		for(int i = 0; i <= segments; ++i)
		{
			float t = -kPi * 0.5f + kPi * (float)i / segments;
			pts[i] = ImVec2(center.x + side * r * 0.5f * cosf(t), center.y + r * sinf(t));
		}
		draw_list->AddPolyline(pts, segments + 1, col, ImDrawFlags_None, thickness);
	}
}

// Pushes `code` into the "language" xOption and applies it immediately -
// see sdl2_desktop_i18n.cpp's eOptionLanguage::Apply(), which is the one
// place that actually reloads the active string table.
static void SetLanguageOption(const std::string& code)
{
	xOptions::eOptionB* op = xOptions::eOptionB::Find("language");
	if(!op)
		return;
	op->Value(code.c_str());
	op->Apply();
}

static void DrawLanguageMenu()
{
	const std::vector<xI18n::LanguageInfo>& langs = xI18n::AvailableLanguages();
	if(langs.empty())
		return; // no res/lang/*.xml found (e.g. a stripped-down install) - nothing to offer a choice between

	float size = ImGui::GetFrameHeight();
	ImGui::SameLine(ImGui::GetWindowWidth() - size - ImGui::GetStyle().ItemSpacing.x);

	ImVec2 pos = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton("##language_icon", ImVec2(size, size));
	bool hovered = ImGui::IsItemHovered();

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
	if(hovered)
		draw_list->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size),
			ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
	DrawGlobeIcon(draw_list, center, size * 0.32f, ImGui::GetColorU32(ImGuiCol_Text), 1.3f);

	// Same kTooltipDelaySeconds delay as ItemTooltip() in imgui_shared.h -
	// the icon's tooltip is driven by a hand-computed `hovered` (the button
	// is drawn manually with an InvisibleButton), so it can't reuse
	// ItemTooltip() directly, but TooltipDue() takes the hover state as a
	// plain bool.
	if(TooltipDue(hovered))
		ImGui::SetTooltip("%s", Tr("tip.menu.language"));
	if(clicked)
		ImGui::OpenPopup("##language_popup");

	if(ImGui::BeginPopup("##language_popup"))
	{
		const std::string& setting = xI18n::LanguageSetting(); // "auto", "en", "ru", ...
		bool is_auto = (setting == "auto");

		// "Auto (Русский)" - shows what "auto" actually resolved to right
		// now, otherwise picking it would look like a dead end once a
		// non-English system locale is detected, with no visible
		// confirmation of which language that actually turned into.
		std::string auto_label = Tr("options.language.auto");
		std::string resolved_name = xI18n::CurrentLanguage();
		for(const xI18n::LanguageInfo& l : langs)
			if(l.code == xI18n::CurrentLanguage())
				{ resolved_name = l.native_name; break; }
		auto_label += " (" + resolved_name + ")";

		if(ImGui::Selectable(auto_label.c_str(), is_auto))
			SetLanguageOption("auto");

		ImGui::Separator();
		for(const xI18n::LanguageInfo& l : langs)
		{
			bool selected = (!is_auto && setting == l.code);
			if(ImGui::Selectable(l.native_name.c_str(), selected))
				SetLanguageOption(l.code);
		}
		ImGui::EndPopup();
	}
}

void DrawMenuBar()
{
	if(!ImGui::BeginMainMenuBar())
		return;

	// --- File ---
	if(ImGui::BeginMenu(Tr("menu.file")))
	{
		if(ImGui::MenuItem(Tr("menu.file.open"), "F3"))
			OnOpenFileAction();
		ItemTooltip("tip.menu.file.open");
		if(ImGui::MenuItem(Tr("menu.file.save"), "F2"))
			OnSaveFileAction();
		ItemTooltip("tip.menu.file.save");
		ImGui::Separator();
		if(ImGui::MenuItem(Tr("menu.file.quick_load"), "F4"))
			OnQuickLoad();
		ItemTooltip("tip.menu.file.quick_load");
		if(ImGui::MenuItem(Tr("menu.file.quick_save"), "F6", false, g_quick_save_enabled))
			OnQuickSave();
		ItemTooltip("tip.menu.file.quick_save");
		ImGui::Separator();
		OptionCheckbox("auto play image", Tr("menu.file.auto_launch"));
		ItemTooltip("tip.menu.file.auto_launch");
		ImGui::Separator();
		if(ImGui::MenuItem(Tr("menu.file.exit")))
			OpQuit(true);
		ItemTooltip("tip.menu.file.exit");
		ImGui::EndMenu();
	}

	// --- View ---
	if(ImGui::BeginMenu(Tr("menu.view")))
	{
		xOptions::eOption<int>* op_zoom = xOptions::eOption<int>::Find("zoom");
		int zoom = op_zoom ? (int)*op_zoom : -1;
		if(ImGui::MenuItem(Tr("menu.view.fill_screen"), "Ctrl+Shift+1", zoom == 0))
			SetZoom(0);
		ItemTooltip("tip.menu.view.fill_screen");
		if(ImGui::MenuItem(Tr("menu.view.small_border"), "Ctrl+Shift+2", zoom == 1))
			SetZoom(1);
		ItemTooltip("tip.menu.view.small_border");
		if(ImGui::MenuItem(Tr("menu.view.no_border"), "Ctrl+Shift+3", zoom == 2))
			SetZoom(2);
		ItemTooltip("tip.menu.view.no_border");
		ImGui::Separator();
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("gigascreen");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.view.gigascreen"), "Ctrl+Shift+G", *op))
					ToggleBoolOption("gigascreen", Tr("status.gigascreen.on"), Tr("status.gigascreen.off"));
				ItemTooltip("tip.menu.view.gigascreen");
			}
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("scanlines");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.view.crt_scanlines"), "Ctrl+Shift+S", *op))
					ToggleBoolOption("scanlines", Tr("status.scanlines.on"), Tr("status.scanlines.off"));
				ItemTooltip("tip.menu.view.crt_scanlines");
			}
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("pal effects");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.view.pal_effects"), "Ctrl+Shift+P", *op))
					ToggleBoolOption("pal effects", Tr("status.pal_effects.on"), Tr("status.pal_effects.off"));
				ItemTooltip("tip.menu.view.pal_effects");
			}
		}
		ImGui::Separator();
		if(ImGui::MenuItem(Tr("menu.view.full_screen"), "Ctrl+F"))
			OnFullScreenToggle();
		ItemTooltip("tip.menu.view.full_screen");
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("Prefer PAL refresh");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.view.prefer_pal_refresh"), "Ctrl+Shift+R", *op))
					ToggleBoolOption("Prefer PAL refresh", Tr("status.pal_refresh.on"), Tr("status.pal_refresh.off"));
				ItemTooltip("tip.menu.view.prefer_pal_refresh");
			}
		}
		ImGui::EndMenu();
	}

	// --- Device ---
	if(ImGui::BeginMenu(Tr("menu.device")))
	{
		if(ImGui::MenuItem(Tr("menu.device.start_stop_tape"), "F5"))
			OnTapeToggle();
		ItemTooltip("tip.menu.device.start_stop_tape");
		OptionCheckbox("fast tape", Tr("menu.device.tape_fast"));
		ItemTooltip("tip.menu.device.tape_fast");
		ImGui::Separator();
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("pause");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.device.pause"), "F7", *op))
					OnPauseToggle();
				ItemTooltip("tip.menu.device.pause");
			}
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("true speed");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.device.true_speed"), "F8", *op))
					ToggleBoolOption("true speed", Tr("status.true_speed.on"), Tr("status.true_speed.off"));
				ItemTooltip("tip.menu.device.true_speed");
			}
		}
		{
			xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("mode 48k");
			if(op)
			{
				if(ImGui::MenuItem(Tr("menu.device.mode_48k"), "F9", *op))
					ToggleBoolOption("mode 48k", Tr("status.mode_48k.on"), Tr("status.mode_48k.off"));
				ItemTooltip("tip.menu.device.mode_48k");
			}
		}
		OptionCheckbox("reset to service rom", Tr("menu.device.reset_to_service_rom"));
		ItemTooltip("tip.menu.device.reset_to_service_rom");
		if(ImGui::MenuItem(Tr("menu.device.reset"), "F12"))
			OnReset();
		ItemTooltip("tip.menu.device.reset");
		ImGui::Separator();
		if(ImGui::MenuItem(Tr("menu.device.options")))
		{
			// Re-clicking while already open would otherwise re-run
			// OpenOptionsDialog() below, which calls LoadCurrentSettings()
			// and resets g_active_tab - silently discarding whatever the
			// user has mid-edit in the already-open dialog. Just bring it
			// to front instead; SetWindowFocus() raises a floating window
			// the same way clicking it would. Matches the stable "###Options"
			// id OptionsDialog::Draw() gives the window itself (see
			// TrTitle() in imgui_shared.h) - the focus target has to be the
			// same id ImGui::Begin() registered, translated title and all.
			if(OptionsDialogActive())
				ImGui::SetWindowFocus(TrTitle("options.title", "Options").c_str());
			else
				OpenOptionsDialog();
		}
		ItemTooltip("tip.menu.device.options");
		ImGui::EndMenu();
	}

	// --- Window ---
	if(ImGui::BeginMenu(Tr("menu.window")))
	{
		if(ImGui::MenuItem(Tr("menu.window.size_100"), "Ctrl+1"))
			ResizeToOrgSizeMultiple(1);
		ItemTooltip("tip.menu.window.size_100");
		if(ImGui::MenuItem(Tr("menu.window.size_200"), "Ctrl+2"))
			ResizeToOrgSizeMultiple(2);
		ItemTooltip("tip.menu.window.size_200");
		if(ImGui::MenuItem(Tr("menu.window.size_300"), "Ctrl+3"))
			ResizeToOrgSizeMultiple(3);
		ItemTooltip("tip.menu.window.size_300");
		ImGui::EndMenu();
	}

	// --- Help ---
	if(ImGui::BeginMenu(Tr("menu.help")))
	{
		if(ImGui::MenuItem(Tr("menu.help.about")))
		{
			if(g_show_about)
				ImGui::SetWindowFocus(TrTitle("about.title", "About").c_str()); // already open elsewhere - raise it instead of a no-op re-set
			else
				g_show_about = true;
		}
		ItemTooltip("tip.menu.help.about");
		ImGui::EndMenu();
	}

	DrawLanguageMenu();

	ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------
// About window
// ---------------------------------------------------------------------------

static void DrawAboutWindow()
{
	if(!g_show_about)
		return;
	ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
	if(ImGui::Begin(TrTitle("about.title", "About").c_str(), &g_show_about, ImGuiWindowFlags_NoCollapse))
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
		ImGui::Text("%s", Tr("about.version"));
		ImGui::Separator();
		ImGui::TextWrapped("%s", Tr("about.tagline"));
		ImGui::Spacing();
		// Copyright line, URL and the GPL paragraph below are deliberately
		// left untranslated, as legal/licensing boilerplate normally is -
		// same reasoning as sdl2_desktop_gamepad.cpp's SourceTypeToString()
		// staying untranslated: this text has a specific, citable form that
		// an ad hoc per-string translation shouldn't paraphrase.
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
	SetStatusText(Tr("status.pause.ready"));
}

}//namespace xImGui
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP

