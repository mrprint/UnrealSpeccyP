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
//  platform/sdl2_desktop/sdl2_desktop_imgui.cpp
//
//  Core Dear ImGui glue for the "sdl2_desktop" platform: desktop GL backend
//  init/shutdown, per-frame Begin/EndFrame split (see the big comment on
//  BeginFrame() for why events must be fed and NewFrame() called before any
//  WantCaptureMouse/Keyboard check), style/font, the persistent status bar,
//  and the generic xOptions<->widget helpers shared by the menu bar
//  (sdl2_desktop_menu.cpp) and the Options dialog (sdl2_desktop_options.cpp).
//
//  The actual GUI content - menu bar, About window, Options dialog - lives
//  in sdl2_desktop_menu.cpp / sdl2_desktop_options.cpp, mirroring
//  platform/wxwidgets/wx_frame.cpp / wx_optionsdialog.cpp respectively; see
//  imgui_shared.h for how the pieces connect.
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <cstdio>
#include <cstring>
#include <cmath>
#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include "imgui_shared.h"
#include "../../tools/options.h"
#include "../../options_common.h"

#ifdef _WINAPI
#include <windows.h>
#elif defined(_MAC)
#include <CoreFoundation/CoreFoundation.h>
#include <strings.h> // strcasecmp()
#endif//_LINUX needs neither - popen()/fgets()/strstr() below come from <cstdio>/<cstring> already included above

namespace xPlatform
{
namespace xImGui
{

#if defined(_WINAPI) && defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib") // RegGetValueW()
#endif//defined(_WINAPI) && defined(_MSC_VER)

// Detects the OS-level light/dark appearance preference so the ImGui
// overlay matches the desktop the same way platform/wxwidgets' native
// widgets automatically do - ImGui isn't a native toolkit, so this has to
// be done by hand, on all three supported platforms. Re-checked
// periodically from BeginFrame() below (see g_last_theme_check_ms), not
// just once here, so toggling the OS theme while the emulator is already
// running updates the overlay too, instead of only taking effect on the
// next launch.
enum class eSystemTheme { Light, Dark };

static eSystemTheme DetectSystemTheme()
{
#ifdef _WINAPI
	// Absent key (fresh Windows install pre-dating this setting, or a
	// registry-editing tool that only ever set it while toggling *to* dark
	// and cleared it rather than writing 1 back) - treat as light, matching
	// what Windows itself assumes as the default.
	DWORD value = 1;
	DWORD size = sizeof(value);
	LONG result = RegGetValueW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size);
	if(result == ERROR_SUCCESS && value == 0)
		return eSystemTheme::Dark;
	return eSystemTheme::Light;
#elif defined(_MAC)
	// Apple simply omits this key entirely in light mode - its absence *is*
	// the light-mode signal here, not a lookup failure to fall back from.
	eSystemTheme theme = eSystemTheme::Light;
	CFStringRef value = (CFStringRef)CFPreferencesCopyAppValue(
		CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
	if(value)
	{
		char buf[64] = {};
		if(CFStringGetCString(value, buf, sizeof(buf), kCFStringEncodingUTF8)
			&& strcasecmp(buf, "Dark") == 0)
			theme = eSystemTheme::Dark;
		CFRelease(value);
	}
	return theme;
#else//_LINUX
	// No single universal API here; org.gnome.desktop.interface's
	// color-scheme key is honoured (directly, or mapped from
	// org.freedesktop.appearance's own color-scheme via a compatibility
	// shim built into gsettings-desktop-schemas) by GNOME, Cinnamon,
	// Budgie, MATE, XFCE-with-gsettings, and KDE Plasma 5.24+ - covers the
	// large majority of desktops without pulling in a libdbus dependency
	// just for this one lookup. Falls back to light if gsettings isn't
	// present at all (e.g. a minimal window-manager-only setup).
	eSystemTheme theme = eSystemTheme::Light;
	FILE* pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
	if(pipe)
	{
		char buf[128] = {};
		if(fgets(buf, sizeof(buf), pipe) && strstr(buf, "dark"))
			theme = eSystemTheme::Dark;
		pclose(pipe);
	}
	return theme;
#endif
}

static bool FileExists(const char* path)
{
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

static void ApplyStyle(float dpi_scale, eSystemTheme theme)
{
	bool dark = (theme == eSystemTheme::Dark);
	if(dark)
		ImGui::StyleColorsDark();
	else
		ImGui::StyleColorsLight();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding  = 3.0f;
	style.GrabRounding   = 3.0f;
	style.PopupRounding  = 4.0f;
	style.WindowPadding  = ImVec2(10, 10);
	style.FramePadding   = ImVec2(6, 4);
	style.ItemSpacing    = ImVec2(8, 6);

	// Same accent hue and layout in both variants, just re-balanced for a
	// light-on-dark vs. dark-on-light background - not just the two
	// built-in ImGui::StyleColorsDark()/Light() bases (those alone clash
	// with each other's rounded/padded layout above and don't share an
	// accent colour), so switching between them still reads as "the same
	// theme, inverted" rather than two unrelated skins.
	ImVec4* c = style.Colors;
	if(dark)
	{
		c[ImGuiCol_WindowBg]         = ImVec4(0.09f, 0.10f, 0.12f, 0.96f);
		c[ImGuiCol_MenuBarBg]        = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
		c[ImGuiCol_PopupBg]          = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
		c[ImGuiCol_FrameBg]          = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
		c[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f, 0.23f, 0.27f, 1.00f);
		c[ImGuiCol_FrameBgActive]    = ImVec4(0.25f, 0.26f, 0.30f, 1.00f);
		c[ImGuiCol_CheckMark]        = ImVec4(0.30f, 0.75f, 0.95f, 1.00f);
		c[ImGuiCol_SliderGrab]       = ImVec4(0.30f, 0.75f, 0.95f, 1.00f);
		c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.82f, 1.00f, 1.00f);
		c[ImGuiCol_Button]           = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
		c[ImGuiCol_ButtonHovered]    = ImVec4(0.27f, 0.55f, 0.68f, 1.00f);
		c[ImGuiCol_ButtonActive]     = ImVec4(0.30f, 0.75f, 0.95f, 1.00f);
		c[ImGuiCol_Header]           = ImVec4(0.20f, 0.45f, 0.55f, 0.80f);
		c[ImGuiCol_HeaderHovered]    = ImVec4(0.27f, 0.55f, 0.68f, 0.90f);
		c[ImGuiCol_HeaderActive]     = ImVec4(0.30f, 0.75f, 0.95f, 1.00f);
		c[ImGuiCol_Tab]              = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
		c[ImGuiCol_TabHovered]       = ImVec4(0.27f, 0.55f, 0.68f, 0.90f);
		c[ImGuiCol_TabActive]        = ImVec4(0.20f, 0.45f, 0.55f, 1.00f);
	}
	else
	{
		c[ImGuiCol_WindowBg]         = ImVec4(0.94f, 0.94f, 0.96f, 0.98f);
		c[ImGuiCol_MenuBarBg]        = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
		c[ImGuiCol_PopupBg]          = ImVec4(0.97f, 0.97f, 0.98f, 0.98f);
		c[ImGuiCol_FrameBg]          = ImVec4(0.86f, 0.86f, 0.89f, 1.00f);
		c[ImGuiCol_FrameBgHovered]   = ImVec4(0.80f, 0.80f, 0.84f, 1.00f);
		c[ImGuiCol_FrameBgActive]    = ImVec4(0.75f, 0.75f, 0.80f, 1.00f);
		c[ImGuiCol_CheckMark]        = ImVec4(0.10f, 0.55f, 0.75f, 1.00f);
		c[ImGuiCol_SliderGrab]       = ImVec4(0.10f, 0.55f, 0.75f, 1.00f);
		c[ImGuiCol_SliderGrabActive] = ImVec4(0.05f, 0.45f, 0.65f, 1.00f);
		c[ImGuiCol_Button]           = ImVec4(0.85f, 0.87f, 0.90f, 1.00f);
		c[ImGuiCol_ButtonHovered]    = ImVec4(0.75f, 0.87f, 0.93f, 1.00f);
		c[ImGuiCol_ButtonActive]     = ImVec4(0.55f, 0.80f, 0.92f, 1.00f);
		c[ImGuiCol_Header]           = ImVec4(0.70f, 0.87f, 0.93f, 0.80f);
		c[ImGuiCol_HeaderHovered]    = ImVec4(0.60f, 0.82f, 0.90f, 0.90f);
		c[ImGuiCol_HeaderActive]     = ImVec4(0.30f, 0.75f, 0.95f, 1.00f);
		c[ImGuiCol_Tab]              = ImVec4(0.88f, 0.89f, 0.92f, 1.00f);
		c[ImGuiCol_TabHovered]       = ImVec4(0.60f, 0.82f, 0.90f, 0.90f);
		c[ImGuiCol_TabActive]        = ImVec4(0.70f, 0.87f, 0.93f, 1.00f);
	}

	// Style is reset to the fixed base values above on every call, so
	// scaling by the *absolute* current dpi_scale here is safe to repeat
	// (e.g. when LoadFont() rebakes for a new scale) - unlike calling
	// ScaleAllSizes() as a one-off post-step, which would compound if ever
	// invoked more than once.
	if(dpi_scale != 1.0f)
		style.ScaleAllSizes(dpi_scale);
}

static bool g_imgui_inited = false; // guards Done() against running without a matching Init() (see sdl2_desktop_video.cpp's graphics_inited comment - same failure class)
static float g_font_baked_scale = 0.0f;
static eSystemTheme g_current_theme = eSystemTheme::Dark; // overwritten by DetectSystemTheme() before first use, in Init()
static Uint32 g_last_theme_check_ms = 0;

// Loads (or reloads) the font atlas at the given DPI scale and uploads it to
// the GPU. Called once from Init(), and again from BeginFrame() whenever the
// effective scale changes (window dragged to a different-DPI monitor, or a
// fullscreen toggle lands on a different effective resolution) - baking once
// at startup and never adjusting left the fixed-resolution glyph bitmap
// being GPU-scaled to whatever the *current* scale happened to be, which
// looks aliased/blurry instead of crisp.
static void LoadFont(float dpi_scale)
{
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	ImFontConfig cfg;
	cfg.OversampleH = 3; // supersample horizontally before downscaling into the atlas - the standard Dear ImGui fix for jagged/aliased glyph edges
	cfg.OversampleV = 1; // vertical oversampling matters much less for typical UI text sizes, not worth the extra atlas memory
	cfg.PixelSnapH = true; // snap glyph advances to whole pixels - keeps small text crisp instead of blurring across pixel boundaries

	const char* font_path = "res/font/Roboto-Regular.ttf";
	ImFont* font = NULL;
	if(FileExists(font_path))
		font = io.Fonts->AddFontFromFileTTF(font_path, 18.0f * dpi_scale, &cfg, io.Fonts->GetGlyphRangesCyrillic());
	if(!font)
		io.Fonts->AddFontDefault(); // built-in font ignores cfg (fixed-size bitmap), but it's only a fallback for a missing TTF

	io.Fonts->Build();

	g_font_baked_scale = dpi_scale;
}

void Init(SDL_Window* window, SDL_GLContext context)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = NULL; // window state is already persisted via xOptions; skip imgui.ini
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	int win_w = 1, win_h = 1, drawable_w = 1, drawable_h = 1;
	SDL_GetWindowSize(window, &win_w, &win_h);
	SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
	float dpi_scale = (win_w > 0) ? (float)drawable_w / (float)win_w : 1.0f;
	if(dpi_scale < 1.0f)
		dpi_scale = 1.0f;

	g_current_theme = DetectSystemTheme();
	ApplyStyle(dpi_scale, g_current_theme);
	g_last_theme_check_ms = SDL_GetTicks();

	ImGui_ImplSDL2_InitForOpenGL(window, context);
	ImGui_ImplOpenGL3_Init("#version 330");

	LoadFont(dpi_scale); // after backend Init() - DestroyFontsTexture()/CreateFontsTexture() need it ready

	g_imgui_inited = true;
}

void Done()
{
	if(!g_imgui_inited)
		return;
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	g_imgui_inited = false;
}

// ---------------------------------------------------------------------------
// Generic xOptions <-> ImGui widget helpers - used by both the menu bar
// (checkable items) and the Options dialog (checkboxes/combos/sliders).
// ---------------------------------------------------------------------------

void OptionCheckbox(const char* option_name, const char* label)
{
	xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find(option_name);
	if(!op)
		return;
	bool v = *op;
	if(ImGui::Checkbox(label, &v))
		op->Set(v);
}

void OptionCombo(const char* option_name, const char* label)
{
	xOptions::eOptionB* raw = xOptions::eOptionB::Find(option_name);
	xOptions::eOption<int>* op = xOptions::eOption<int>::Find(option_name);
	if(!raw || !op)
		return;
	const char** values = raw->Values();
	if(!values)
		return;
	int count = 0;
	while(values[count])
		++count;
	int current = *op;
	const char* preview = (current >= 0 && current < count) ? values[current] : "?";
	if(ImGui::BeginCombo(label, preview))
	{
		for(int i = 0; i < count; ++i)
		{
			bool selected = (i == current);
			if(ImGui::Selectable(values[i], selected))
				op->Set(i);
			if(selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
}

void OptionSliderInt(const char* option_name, const char* label, int lo, int hi)
{
	xOptions::eOption<int>* op = xOptions::eOption<int>::Find(option_name);
	if(!op)
		return;
	int v = *op;
	if(ImGui::SliderInt(label, &v, lo, hi))
		op->Set(v);
}

// ---------------------------------------------------------------------------
// Persistent status bar - equivalent of wxFrame::SetStatusText(): one line,
// always visible at the bottom, replaced (not queued) by the next call.
// Matches wx exactly, including the default "Ready..." text (set from
// sdl2_desktop_menu.cpp at startup, mirroring Frame::Frame()).
// ---------------------------------------------------------------------------

static char g_status_text[256] = "";

void SetStatusText(const char* text)
{
	strncpy(g_status_text, text, sizeof(g_status_text) - 1);
	g_status_text[sizeof(g_status_text) - 1] = 0;
}

static void DrawStatusBar()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	float h = ImGui::GetFrameHeight();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	if(ImGui::Begin("##statusbar", NULL, flags))
		ImGui::TextUnformatted(g_status_text);
	ImGui::End();
	ImGui::PopStyleVar(2);
}

// Split in two (instead of one "Update()") on purpose: io.WantCaptureMouse/
// WantCaptureKeyboard only become correct for *this* batch of input AFTER
// ImGui::NewFrame() has processed it - so the caller must feed every SDL
// event first (FeedEvent), then call BeginFrame(), and only *after* that
// check WantCaptureMouse()/WantCaptureKeyboard() to decide whether each
// buffered keyboard/mouse event should also reach the emulator. Checking
// those flags before BeginFrame() makes the very first click on a
// still-unseen menu item read as "not over the UI" and leak through to the
// emulator - see platform/sdl2/sdl2_mouse.cpp,
// which has no bounds check of its own and grabs the mouse unconditionally
// on any click it receives while the window isn't already grabbed.
void BeginFrame()
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();

	// ImGui_ImplSDL2_NewFrame() just recomputed io.DisplayFramebufferScale
	// from the window's current drawable-vs-logical size ratio - if that
	// differs meaningfully from what the font/style were last baked/scaled
	// for (window dragged to a different-DPI monitor, or a fullscreen
	// toggle landed on a different effective resolution), rebake now rather
	// than letting the old, fixed-resolution glyph bitmap get GPU-scaled to
	// the new size, which is what read as aliased/blurry text.
	float dpi_scale = ImGui::GetIO().DisplayFramebufferScale.x;
	if(dpi_scale < 1.0f)
		dpi_scale = 1.0f;
	bool dpi_changed = std::abs(dpi_scale - g_font_baked_scale) > 0.05f;

	// DetectSystemTheme() shells out on Linux (gsettings) and hits the
	// registry/CFPreferences on Windows/macOS - cheap individually, but not
	// something to redo every single frame at 60+ fps for no reason. Once a
	// second is frequent enough that a live OS theme switch while the
	// emulator is running feels immediate, without measurable overhead.
	Uint32 now = SDL_GetTicks();
	bool theme_changed = false;
	if(now - g_last_theme_check_ms >= 1000)
	{
		g_last_theme_check_ms = now;
		eSystemTheme detected = DetectSystemTheme();
		if(detected != g_current_theme)
		{
			g_current_theme = detected;
			theme_changed = true;
		}
	}

	if(dpi_changed || theme_changed)
	{
		ApplyStyle(dpi_scale, g_current_theme);
		if(dpi_changed)
			LoadFont(dpi_scale);
	}

	ImGui::NewFrame();
}

void EndFrame()
{
	// wx_frame.cpp's ShowFullScreen(true, wxFULLSCREEN_ALL) hides the menu
	// bar and status bar along with the window chrome; matching that here
	// means not drawing them at all while fullscreen, rather than leaving
	// them floating over the game image. Already-open floating windows
	// (Options, file browser, About) are left alone either way - a fullscreen
	// toggle happening while one is open shouldn't make it unreachable.
	bool fullscreen = false;
	{
		xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("full screen");
		if(op)
			fullscreen = *op;
	}
	if(!fullscreen)
	{
		DrawMenuBar();
		DrawStatusBar();
	}
	DrawMenuDialogs();
	DrawOptionsDialog();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void FeedEvent(const SDL_Event& e)
{
	ImGui_ImplSDL2_ProcessEvent(&e);
}

bool WantCaptureKeyboard() { return ImGui::GetIO().WantCaptureKeyboard; }
bool WantCaptureMouse() { return ImGui::GetIO().WantCaptureMouse; }

}
//namespace xImGui

// Called from platform/gl/draw.cpp (USE_GL) whenever it switches between the
// full and the lightweight shader path - every USE_GL platform must provide
// this. platform/wxwidgets/wx_canvas.cpp posts the wx equivalent to its
// native status bar via evtSetStatusText; here it's the exact same status
// bar (sdl2_desktop_imgui.cpp's, not a separate notification mechanism).
void LightweightShadersMessage(bool prev_use_lightweight, bool use_lightweight)
{
	if(prev_use_lightweight == use_lightweight)
		return;
	xImGui::SetStatusText(use_lightweight ? "Lightweight shader enabled" : "Full-quality shader enabled");
}

}
//namespace xPlatform

#endif//USE_SDL2_DESKTOP

