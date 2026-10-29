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
//  in sdl2_desktop_menu.cpp / sdl2_desktop_options.cpp; see imgui_shared.h
//  for how the pieces connect.
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
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
#else//_LINUX
// popen()/fgets()/strstr() come from <cstdio>/<cstring> already included above.
// The Linux theme lookup forks a process and waits for it, so it runs on a
// small background thread instead of the render thread - see ThemeWatcher.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>   // getenv()
#include <fstream>   // kdeglobals
#include <string>
#include <vector>
#include <strings.h> // strncasecmp()
#include <sys/wait.h> // WIFEXITED()/WEXITSTATUS() for popen()'s pclose()
#define XIMGUI_THEME_WATCHER 1
#endif//_WINAPI / _MAC / _LINUX

namespace xPlatform
{
namespace xImGui
{

#if defined(_WINAPI) && defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib") // RegGetValueW()
#endif//defined(_WINAPI) && defined(_MSC_VER)

// Detects the OS-level light/dark appearance preference so the ImGui
// overlay matches the desktop - ImGui isn't a native toolkit, so this has
// to be done by hand, on all three supported platforms. Re-checked
// periodically from ImGuiBackend::BeginFrame() below (see
// ImGuiBackend::m_last_theme_check_ms), not just once here, so toggling the
// OS theme while the emulator is already running updates the overlay too,
// instead of only taking effect on the next launch. (On Linux that polling
// happens on a background thread - see ThemeWatcher below.) The "ui theme"
// option below can pin Light or Dark instead, in which case none of this runs.
enum class eSystemTheme { Light, Dark };

#ifdef XIMGUI_THEME_WATCHER
// Outcome of one lookup of the OS theme.
enum class eThemeQuery
{
	Answered,   // `theme` is valid
	NoAnswer,   // gave nothing, cause unknown (no session bus yet, schema missing, ...) - worth retrying soon
	Unavailable // the lookup tool isn't there at all - not worth asking again for a long while
};

// Asks gsettings for the current preference; `theme` is only written when it
// answers.
//
// popen() runs the command through /bin/sh, which exits with 127 when the
// command isn't found (also what the dynamic loader exits with for a missing
// library, i.e. just as unusable). That is told apart from a mere empty answer
// so a machine without gsettings isn't made to fork a shell every few seconds
// for the whole session.
static eThemeQuery QueryGsettingsTheme(eSystemTheme& theme)
{
	FILE* pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
	if(!pipe)
		return eThemeQuery::NoAnswer; // fork/pipe failure: resource pressure, may well pass
	char buf[128] = {};
	const bool got_line = fgets(buf, sizeof(buf), pipe) != nullptr;
	const int status = pclose(pipe); // reaps the child either way
	if(got_line)
	{
		theme = strstr(buf, "dark") ? eSystemTheme::Dark : eSystemTheme::Light;
		return eThemeQuery::Answered;
	}
	// status is -1 if the child couldn't be waited for (e.g. SIGCHLD ignored) -
	// then nothing is known, which is the NoAnswer case.
	if(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 127)
		return eThemeQuery::Unavailable;
	return eThemeQuery::NoAnswer;
}

// ---- KDE Plasma ------------------------------------------------------------
// Plasma keeps its colour scheme in kdeglobals, and that file is what Plasma
// itself (and every Qt/KDE app) goes by, whereas the gsettings key GNOME-style
// desktops use may not be kept in step with it on every Plasma version or
// setup. So on a Plasma session the file is read directly: no process to
// spawn, no D-Bus dependency, and a switch of the global theme (including
// Plasma's automatic day/night switching) shows up as soon as it is written.

// Where the theme is read from, decided once from the environment on the
// main thread: getenv() from the worker thread could race with a setenv()
// somewhere else in the process (SDL does call it).
struct ThemeSource
{
	bool kde = false;                    // Plasma session: read kdeglobals instead of asking gsettings
	std::vector<std::string> kdeglobals; // candidate files, highest priority first
};

// KDE_FULL_SESSION is set by Plasma's session startup; XDG_CURRENT_DESKTOP is a
// colon-separated list ("KDE", "ubuntu:GNOME", ...), matched per entry so
// that e.g. "KDEX" doesn't count.
static bool IsKdeSession()
{
	const char* full = getenv("KDE_FULL_SESSION");
	if(full && *full)
		return true;
	for(const char* p = getenv("XDG_CURRENT_DESKTOP"); p && *p; )
	{
		const char* end = strchr(p, ':');
		const size_t len = end ? (size_t)(end - p) : strlen(p);
		if(len == 3 && strncasecmp(p, "KDE", 3) == 0)
			return true;
		p = end ? end + 1 : nullptr;
	}
	return false;
}

static ThemeSource DetectThemeSource()
{
	ThemeSource src;
	src.kde = IsKdeSession();
	if(!src.kde)
		return src;
	// The lookup order KDE's own config system uses: the user's file first,
	// then each XDG_CONFIG_DIRS entry (default /etc/xdg), so a distro-wide
	// default scheme still applies until the user picks one of their own.
	const char* cfg_home = getenv("XDG_CONFIG_HOME");
	const char* home = getenv("HOME");
	if(cfg_home && *cfg_home)
		src.kdeglobals.push_back(std::string(cfg_home) + "/kdeglobals");
	else if(home && *home)
		src.kdeglobals.push_back(std::string(home) + "/.config/kdeglobals");
	const char* dirs_env = getenv("XDG_CONFIG_DIRS");
	const std::string dirs = (dirs_env && *dirs_env) ? dirs_env : "/etc/xdg";
	for(size_t pos = 0; pos <= dirs.size(); )
	{
		size_t end = dirs.find(':', pos);
		if(end == std::string::npos)
			end = dirs.size();
		if(end > pos)
			src.kdeglobals.push_back(dirs.substr(pos, end - pos) + "/kdeglobals");
		pos = end + 1;
	}
	return src;
}

// Pulls BackgroundNormal=R,G,B out of the [Colors:Window] section of a
// kdeglobals-style INI stream. False if the stream doesn't define it.
static bool ParseKdeWindowBackground(std::istream& in, int& r, int& g, int& b)
{
	static const char kKey[] = "BackgroundNormal=";
	const size_t key_len = sizeof(kKey) - 1;
	bool in_window = false;
	std::string line;
	while(std::getline(in, line))
	{
		if(!line.empty() && line.back() == '\r')
			line.pop_back();
		if(!line.empty() && line[0] == '[')
		{
			in_window = (line == "[Colors:Window]");
			continue;
		}
		if(in_window && line.compare(0, key_len, kKey) == 0
			&& sscanf(line.c_str() + key_len, "%d,%d,%d", &r, &g, &b) == 3)
			return true;
	}
	return false;
}

// The scheme's own name ("BreezeDark", a user's custom one, ...) says little;
// the window background it actually paints with decides, by perceived
// luminance. Always answers: a Plasma that never wrote any colours is on its
// stock scheme, which is light.
static eThemeQuery QueryKdeTheme(const ThemeSource& src, eSystemTheme& theme)
{
	for(const std::string& path : src.kdeglobals)
	{
		std::ifstream file(path);
		int r = 0, g = 0, b = 0;
		if(file && ParseKdeWindowBackground(file, r, g, b))
		{
			theme = (299 * r + 587 * g + 114 * b < 128 * 1000) ? eSystemTheme::Dark : eSystemTheme::Light;
			return eThemeQuery::Answered;
		}
	}
	theme = eSystemTheme::Light;
	return eThemeQuery::Answered;
}

// One lookup, whichever way the desktop wants it.
static eThemeQuery QueryTheme(const ThemeSource& src, eSystemTheme& theme)
{
	return src.kde ? QueryKdeTheme(src, theme) : QueryGsettingsTheme(theme);
}

// ---- ThemeWatcher ----------------------------------------------------------
// Keeps track of the Linux theme on a background thread. On GNOME-style
// desktops the lookup is a popen() of gsettings: fork + exec + wait, several
// milliseconds, and unbounded if gsettings stalls on a broken session bus -
// far too long to block the render thread, where it showed up as a periodic
// hitch in the frame pacing (the KDE file read is cheap, but goes through the
// same thread so there is a single path). The render thread only ever does
// Current(), one atomic load.
//
// All shared state lives in a reference-counted State that the thread holds
// a share of, and the thread is detached: Stop() just raises the flag and
// lets go, so shutting down never waits for a gsettings that is hung, and a
// late-finishing thread can't touch freed memory.
class ThemeWatcher
{
public:
	~ThemeWatcher() { Stop(); }

	// `initial` is what Current() reports until the thread has an answer: either
	// what the caller already detected synchronously (first frame is right
	// without waiting for the first poll), or - on resuming after a pause - what
	// is on screen right now, so nothing flickers. With `poll_now` the thread
	// asks straight away instead of after the first interval.
	void Start(eSystemTheme initial, bool poll_now = false)
	{
		Stop();
		m_state = std::make_shared<State>();
		m_state->dark.store(initial == eSystemTheme::Dark, std::memory_order_relaxed);
		m_state->source = DetectThemeSource();
		m_state->poll_now = poll_now;
		std::thread(&ThemeWatcher::Run, m_state).detach();
	}

	void Stop()
	{
		if(!m_state)
			return;
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->stop = true;
		}
		m_state->cv.notify_all();
		m_state.reset();
	}

	bool Running() const { return m_state != nullptr; }

	// Latest answer; `fallback` while the watcher isn't running.
	eSystemTheme Current(eSystemTheme fallback) const
	{
		if(!m_state)
			return fallback;
		return m_state->dark.load(std::memory_order_relaxed) ? eSystemTheme::Dark : eSystemTheme::Light;
	}

private:
	struct State
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool stop = false; // guarded by mutex
		std::atomic<bool> dark{false};
		ThemeSource source; // written before the thread starts, read-only afterwards
		bool poll_now = false; // likewise
	};

	static void Run(std::shared_ptr<State> s)
	{
		using namespace std::chrono;
		const milliseconds kPoll(2000);     // a live OS theme switch still feels immediate
		const milliseconds kRetry(30000);   // no answer, cause unknown: try again, but not hammering
		const milliseconds kGone(600000);   // gsettings isn't installed: look again only now and then
		milliseconds interval = kPoll;
		bool first = s->poll_now;
		std::unique_lock<std::mutex> lock(s->mutex);
		// wait_for() with a predicate returns true once `stop` is set (leave),
		// false on timeout (poll again); `first` skips the wait once for an
		// immediate poll. The lock is dropped around the slow part so Stop() is
		// never held up by it.
		while(first || !s->cv.wait_for(lock, interval, [&s] { return s->stop; }))
		{
			first = false;
			lock.unlock();
			eSystemTheme theme = eSystemTheme::Light;
			const eThemeQuery result = QueryTheme(s->source, theme);
			lock.lock();
			if(result == eThemeQuery::Answered)
			{
				s->dark.store(theme == eSystemTheme::Dark, std::memory_order_relaxed);
				interval = kPoll;
			}
			else // keep the last known value rather than flipping to Light on a failure
				interval = (result == eThemeQuery::Unavailable) ? kGone : kRetry;
		}
	}

	std::shared_ptr<State> m_state;
};
#endif//XIMGUI_THEME_WATCHER

// ---------------------------------------------------------------------------
// "ui theme" xOption: whether the overlay follows the OS (Auto, the default)
// or is pinned to Light or Dark - the only way to get a dark UI on a system
// whose theme can't be detected, and an override anywhere else. Stored as
// text ("auto"/"light"/"dark") so the config file stays readable and doesn't
// depend on enum order; anything else (hand-edited, or from a future
// version) reads as Auto.
// ---------------------------------------------------------------------------
namespace {

struct eOptionUiTheme : public xOptions::eOptionString
{
	eOptionUiTheme() { Value("auto"); }
	const char* Name() const override { return "ui theme"; }
} op_ui_theme;

}//anonymous namespace

eThemeMode GetThemeMode()
{
	const char* v = op_ui_theme.Value();
	if(v && !strcmp(v, "light"))
		return eThemeMode::Light;
	if(v && !strcmp(v, "dark"))
		return eThemeMode::Dark;
	return eThemeMode::Auto;
}

void SetThemeMode(eThemeMode mode)
{
	op_ui_theme.Value(mode == eThemeMode::Light ? "light" : mode == eThemeMode::Dark ? "dark" : "auto");
	op_ui_theme.Apply(); // no-op today; keeps this in line with how every other option is set
}

// What to show: a pinned choice wins, otherwise whatever the OS says.
static eSystemTheme ResolveTheme(eThemeMode mode, eSystemTheme os)
{
	if(mode == eThemeMode::Light)
		return eSystemTheme::Light;
	if(mode == eThemeMode::Dark)
		return eSystemTheme::Dark;
	return os;
}

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
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
	if(result == ERROR_SUCCESS && value == 0)
		return eSystemTheme::Dark;
	return eSystemTheme::Light;
#elif defined(_MAC)
	// Apple simply omits this key entirely in light mode - its absence *is*
	// the light-mode signal here, not a lookup failure to fall back from.
	eSystemTheme theme = eSystemTheme::Light;
	// RAII: CFRelease on scope exit — no need for manual release or worrying
	// about early returns between Copy and Release. CoreFoundation types are
	// opaque pointer typedefs (CFStringRef is already a pointer), so we can't
	// use unique_ptr<T> directly without getting the indirection wrong; instead
	// a tiny scope-guard struct handles it cleanly.
	struct CFScopeRelease {
		CFStringRef ref = nullptr;
		~CFScopeRelease() { if(ref) CFRelease(ref); }
	};
	CFScopeRelease cf_value;
	cf_value.ref = (CFStringRef)CFPreferencesCopyAppValue(
		CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
	if(cf_value.ref)
	{
		char buf[64] = {};
		if(CFStringGetCString(cf_value.ref, buf, sizeof(buf), kCFStringEncodingUTF8)
			&& strcasecmp(buf, "Dark") == 0)
			theme = eSystemTheme::Dark;
	}
	return theme;
#else//_LINUX
	// No single universal API here. A KDE Plasma session is read straight from
	// kdeglobals (see QueryKdeTheme). Everything else goes through
	// org.gnome.desktop.interface's color-scheme key via gsettings, which
	// (directly, or mapped from org.freedesktop.appearance's own color-scheme
	// via a compatibility shim built into gsettings-desktop-schemas) is
	// honoured by GNOME, Cinnamon, Budgie, MATE and XFCE-with-gsettings -
	// without pulling in a libdbus dependency just for this one lookup. Falls
	// back to light if nothing answers (e.g. a minimal window-manager-only
	// setup).
	// One-shot, blocking: only used for the very first frame (ImGuiBackend::
	// Init()). Later changes are picked up by ThemeWatcher's thread instead.
	eSystemTheme theme = eSystemTheme::Light;
	QueryTheme(DetectThemeSource(), theme); // no answer leaves the Light default
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
	// Back to ImGui's pristine defaults first: StyleColors*() below only
	// resets the colours, so every size this function doesn't assign itself
	// (ScrollbarSize, IndentSpacing, TabRounding, ...) would otherwise keep
	// the *scaled* value from the previous call, and ScaleAllSizes() at the
	// end would then scale it again - compounding on every theme change or
	// DPI change instead of being applied once.
	ImGuiStyle& style = ImGui::GetStyle();
	style = ImGuiStyle();
	if(dark)
		ImGui::StyleColorsDark();
	else
		ImGui::StyleColorsLight();
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

	// Style was reset to the fixed base values above on every call, so
	// scaling by the *absolute* current dpi_scale here is safe to repeat
	// (e.g. when LoadFont() rebakes for a new scale) - unlike calling
	// ScaleAllSizes() as a one-off post-step, which would compound if ever
	// invoked more than once.
	if(dpi_scale != 1.0f)
		style.ScaleAllSizes(dpi_scale);
}

// ---------------------------------------------------------------------------
// StatusBar: one line, always visible at the bottom, replaced (not queued)
// by the next call. The default "Ready..." text is set from
// sdl2_desktop_menu.cpp at startup.
//
// Its only state is the text buffer below - a distinct concern from
// ImGuiBackend's lifecycle/DPI/font/theme state further down.
// ---------------------------------------------------------------------------

class StatusBar
{
public:
	void Set(const char* text)
	{
		CopyToBuffer(m_text, sizeof(m_text), text);
	}

	void Draw() const
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
		if(ImGui::Begin("##statusbar", nullptr, flags))
			ImGui::TextUnformatted(m_text);
		ImGui::End();
		ImGui::PopStyleVar(2);
	}

private:
	char m_text[256] = "";
};

static StatusBar g_status_bar;

// ---------------------------------------------------------------------------
// ImGuiBackend - desktop GL backend init/shutdown, the per-frame Begin/End
// split, and the DPI/theme/font upkeep that BeginFrame() runs each frame.
// One instance only (g_backend below).
// ---------------------------------------------------------------------------

class ImGuiBackend
{
public:
	void Init(SDL_Window* window, SDL_GLContext context);
	void Done();

	// Split in two (instead of one "Update()") on purpose: io.WantCaptureMouse/
	// WantCaptureKeyboard only become correct for *this* batch of input AFTER
	// ImGui::NewFrame() has processed it - so the caller must feed every SDL
	// event first (FeedEvent), then call BeginFrame(), and only *after* that
	// check WantCaptureMouse()/WantCaptureKeyboard() to decide whether each
	// buffered keyboard/mouse event should also reach the emulator. Checking
	// those flags before BeginFrame() makes the very first click on a
	// still-unseen menu item read as "not over the UI" and leak through to
	// the emulator - see sdl2_mouse.cpp, which has no bounds
	// check of its own and grabs the mouse unconditionally on any click it
	// receives while the window isn't already grabbed.
	void BeginFrame();
	void EndFrame();

private:
	SDL_Window* m_window = nullptr; // only to ask whether the mouse is captured, see BeginFrame()
	bool m_inited = false; // guards Done() against running without a matching Init() (see sdl2_desktop_video.cpp's graphics_inited comment - same failure class)
	float m_font_baked_scale = 0.0f;
	eSystemTheme m_current_theme = eSystemTheme::Dark; // what is applied right now; set in Init() before first use
	eSystemTheme m_os_theme = eSystemTheme::Light;      // last known OS preference (meaningful while the mode is Auto)
	eThemeMode m_theme_mode = eThemeMode::Auto;         // mode as of the last frame, to notice a change in the option
	Uint32 m_last_theme_check_ms = 0; // Windows/macOS polling only; Linux uses m_theme_watcher
#ifdef XIMGUI_THEME_WATCHER
	ThemeWatcher m_theme_watcher;
#endif

	// Loads (or reloads) the font atlas at the given DPI scale and uploads
	// it to the GPU. Called once from Init(), and again from BeginFrame()
	// whenever the effective scale changes (window dragged to a
	// different-DPI monitor, or a fullscreen toggle lands on a different
	// effective resolution) - baking once at startup and never adjusting
	// left the fixed-resolution glyph bitmap being GPU-scaled to whatever
	// the *current* scale happened to be, which looks aliased/blurry
	// instead of crisp.
	void LoadFont(float dpi_scale);
};

void ImGuiBackend::LoadFont(float dpi_scale)
{
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	ImFontConfig cfg;
	cfg.OversampleH = 3; // supersample horizontally before downscaling into the atlas - the standard Dear ImGui fix for jagged/aliased glyph edges
	cfg.OversampleV = 1; // vertical oversampling matters much less for typical UI text sizes, not worth the extra atlas memory
	cfg.PixelSnapH = true; // snap glyph advances to whole pixels - keeps small text crisp instead of blurring across pixel boundaries

	// Glyph ranges baked into the atlas - must cover every res/lang/*.xml
	// this build ships, in every language a translator might add later.
	// GetGlyphRangesCyrillic() already bundles Basic Latin + Latin-1
	// Supplement + the Cyrillic block (see Dear ImGui's own
	// imgui_draw.cpp), which alone covers English plus every Latin-1-
	// diacritic language (German, French, Spanish, Italian, Portuguese,
	// Dutch, the Scandinavian languages) and every Cyrillic-script
	// language relevant to the ZX Spectrum's history (Russian, Ukrainian,
	// Bulgarian, Serbian/Macedonian).
	//
	// Added by hand on top of that:
	//  - Latin Extended-A (U+0100-017F): Czech, Slovak, Polish, Hungarian,
	//    Croatian, Slovenian - all had real ZX Spectrum/clone history
	//    (Didaktik in Czechoslovakia especially).
	//  - Modern Greek (U+0391-03CE).
	//  - Four Romanian letters with a comma below, ș/ț/Ș/Ț (U+0218-021B) -
	//    outside Latin Extended-A, technically in Extended-B, but this
	//    specific font happens to include exactly these four even though
	//    it doesn't cover the rest of that block (checked below).
	//
	// Verified against the actual shipped res/font/Roboto-Regular.ttf with
	// fontTools (TTFont(...).getBestCmap()) rather than assumed from
	// Roboto's general reputation: Latin Extended-A is 128/128 present,
	// the four Romanian codepoints above are present, and modern Greek is
	// complete apart from U+03A2, which isn't a real letter - Unicode
	// itself leaves that one codepoint unassigned.
	static ImVector<ImWchar> s_glyph_ranges;
	if(s_glyph_ranges.empty())
	{
		ImFontGlyphRangesBuilder builder;
		builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
		static const ImWchar extra_ranges[] = {
			0x0100, 0x017F, // Latin Extended-A
			0x0391, 0x03CE, // Greek
			0x0218, 0x021B, // Romanian ș ț Ș Ț
			0,
		};
		builder.AddRanges(extra_ranges);
		builder.BuildRanges(&s_glyph_ranges);
	}

	const char* font_path = "res/font/Roboto-Regular.ttf";
	ImFont* font = nullptr;
	if(FileExists(font_path))
		font = io.Fonts->AddFontFromFileTTF(font_path, 18.0f * dpi_scale, &cfg, s_glyph_ranges.Data);
	if(!font)
		io.Fonts->AddFontDefault(); // built-in font ignores cfg (fixed-size bitmap), but it's only a fallback for a missing TTF

	io.Fonts->Build();

	m_font_baked_scale = dpi_scale;
}

void ImGuiBackend::Init(SDL_Window* window, SDL_GLContext context)
{
	m_window = window;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr; // window state is already persisted via xOptions; skip imgui.ini
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	int win_w = 1, win_h = 1, drawable_w = 1, drawable_h = 1;
	SDL_GetWindowSize(window, &win_w, &win_h);
	SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
	float dpi_scale = (win_w > 0) ? (float)drawable_w / (float)win_w : 1.0f;
	if(dpi_scale < 1.0f)
		dpi_scale = 1.0f;

	// xOptions::Load() has already run (Handler()->OnInit() comes before video
	// init), so a saved Light/Dark choice is known here. Pinned, there is no OS
	// lookup to make at all - in particular no blocking gsettings call at startup.
	m_theme_mode = GetThemeMode();
	if(m_theme_mode == eThemeMode::Auto)
		m_os_theme = DetectSystemTheme();
	m_current_theme = ResolveTheme(m_theme_mode, m_os_theme);
	ApplyStyle(dpi_scale, m_current_theme);
	m_last_theme_check_ms = SDL_GetTicks();
#ifdef XIMGUI_THEME_WATCHER
	if(m_theme_mode == eThemeMode::Auto)
		m_theme_watcher.Start(m_os_theme);
#endif

	ImGui_ImplSDL2_InitForOpenGL(window, context);
	ImGui_ImplOpenGL3_Init("#version 330");

	LoadFont(dpi_scale); // after backend Init() - DestroyFontsTexture()/CreateFontsTexture() need it ready

	m_inited = true;
}

void ImGuiBackend::Done()
{
	if(!m_inited)
		return;
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
#ifdef XIMGUI_THEME_WATCHER
	m_theme_watcher.Stop();
#endif
	ImGui::DestroyContext();
	m_inited = false;
}

void ImGuiBackend::BeginFrame()
{
	// While the mouse is captured for the emulated (Kempston) mouse the real pointer is hidden, and the position SDL
	// reports is a virtual one that drifts with the relative motion: it can sit over the menu bar or a dialog with nothing
	// on screen to show it. The UI must not react to a pointer nobody can see, so it is deaf to the mouse for as long as
	// the capture lasts - nothing is hovered, hence nothing can be clicked or opened. (Keyboard use of the UI is not
	// affected. It is the same grab that UpdateCursorVisibility() in sdl2_desktop.cpp looks at.)
	// This alone does not hand the mouse to the emulator: io.WantCaptureMouse can stay true (a button held, a popup
	// open) - Loop1() overrides it while captured.
	{
		ImGuiIO& io = ImGui::GetIO();
		if(m_window && SDL_GetWindowGrab(m_window) != SDL_FALSE)
			io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		else
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
	}

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
	bool dpi_changed = std::abs(dpi_scale - m_font_baked_scale) > 0.05f;

	// Theme: a pinned Light/Dark choice (the "ui theme" option, changed from the
	// Options dialog), or the OS's own. The OS is only asked while the mode is
	// Auto, so switching to a pinned theme stops the lookups and switching back
	// resumes them.
	const eThemeMode mode = GetThemeMode();
	if(mode != m_theme_mode)
	{
		const bool was_auto = (m_theme_mode == eThemeMode::Auto);
		m_theme_mode = mode;
		if(mode == eThemeMode::Auto && !was_auto)
		{
#ifdef XIMGUI_THEME_WATCHER
			// Keep showing what is on screen until the first fresh answer comes
			// in (a few ms), instead of flashing a stale value.
			m_os_theme = m_current_theme;
			m_theme_watcher.Start(m_os_theme, true);
#else
			m_os_theme = DetectSystemTheme();
			m_last_theme_check_ms = SDL_GetTicks();
#endif
		}
#ifdef XIMGUI_THEME_WATCHER
		else if(mode != eThemeMode::Auto)
			m_theme_watcher.Stop(); // pinned: nobody reads the answer, so stop asking for it
#endif
	}
	if(mode == eThemeMode::Auto)
	{
		// Windows/macOS: one registry/CFPreferences read, effectively free, so once
		// a second keeps a live OS theme switch feeling immediate. Linux: the
		// answer is kept up to date by ThemeWatcher's background thread (the
		// lookup forks gsettings and waits for it - see there), so all that is
		// left here is one atomic load, cheap enough to do on every frame.
#ifdef XIMGUI_THEME_WATCHER
		m_os_theme = m_theme_watcher.Current(m_os_theme);
#else
		const Uint32 now = SDL_GetTicks();
		if(now - m_last_theme_check_ms >= 1000)
		{
			m_last_theme_check_ms = now;
			m_os_theme = DetectSystemTheme();
		}
#endif
	}
	bool theme_changed = false;
	const eSystemTheme wanted = ResolveTheme(mode, m_os_theme);
	if(wanted != m_current_theme)
	{
		m_current_theme = wanted;
		theme_changed = true;
	}

	if(dpi_changed || theme_changed)
	{
		ApplyStyle(dpi_scale, m_current_theme);
		if(dpi_changed)
			LoadFont(dpi_scale);
	}

	ImGui::NewFrame();
}

void ImGuiBackend::EndFrame()
{
	// While fullscreen, don't draw the menu bar and status bar at all
	// (rather than leaving them floating over the game image). Already-open
	// floating windows (Options, file browser, About) are left alone either
	// way - a fullscreen toggle happening while one is open shouldn't make
	// it unreachable.
	bool fullscreen = false;
	{
		xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("full screen");
		if(op)
			fullscreen = *op;
	}
	if(!fullscreen)
	{
		DrawMenuBar();
		g_status_bar.Draw();
	}
	DrawMenuDialogs();
	DrawOptionsDialog();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static ImGuiBackend g_backend;

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
// Public API - thin facades over the g_backend/g_status_bar instances above.
// ---------------------------------------------------------------------------

void SetStatusText(const char* text)
{
	g_status_bar.Set(text);
}

void Init(SDL_Window* window, SDL_GLContext context)
{
	g_backend.Init(window, context);
}

void Done()
{
	g_backend.Done();
}

void BeginFrame()
{
	g_backend.BeginFrame();
}

void EndFrame()
{
	g_backend.EndFrame();
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
// this.
void LightweightShadersMessage(bool prev_use_lightweight, bool use_lightweight)
{
	if(prev_use_lightweight == use_lightweight)
		return;
	xImGui::SetStatusText(use_lightweight ? xImGui::Tr("status.shader.lightweight") : xImGui::Tr("status.shader.full_quality"));
}

}
//namespace xPlatform

#endif//USE_SDL2_DESKTOP
