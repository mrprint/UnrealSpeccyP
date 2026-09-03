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
//  platform/sdl2_desktop/sdl2_desktop.cpp
//
//  Entry point + single-threaded main loop for the new "sdl2_desktop"
//  platform. Deliberately close to platform/sdl2/sdl2.cpp (same eOptionSpeed,
//  same home-path/profile setup, same Init/Done/Loop/Loop1 structure), with
//  two real differences:
//   - every SDL event is fed to Dear ImGui and, for keyboard/mouse events,
//     buffered; only *after* xImGui::BeginFrame() has processed the whole
//     batch (so io.WantCaptureKeyboard/Mouse are current) are the buffered
//     events routed to ProcessKey()/ProcessMouse() or swallowed by the UI -
//     see the comment in Loop1() for why the order matters,
//   - there is no SDL_AddEventWatch()/WINDOWEVENT_EXPOSED redraw hook (see
//     the comment in Init()): it would call back into Dear ImGui's
//     NewFrame()/Render() reentrantly, which Dear ImGui does not tolerate.
//
//  Everything - event polling, emulator tick, video+UI render, audio - runs
//  on this one thread. There is no render thread and no cross-thread GL
//  context hand-off to synchronize.
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#include <vector>
#include "imgui_shared.h"
#include "sdl2_desktop_gamepad.h"
#include "../../options_common.h"
#include "../../tools/options.h"
#include "../../tools/tick.h"
#include "../io.h"

#ifdef _WINAPI
#include <windows.h>
#endif//_WINAPI

namespace xPlatform
{

// If launched from a console (e.g. FAR Manager, cmd.exe) rather than by
// double-clicking, we inherit that console's standard input handle even
// though we never read from it. The console host (conhost.exe/Windows
// Terminal) keeps its own ENABLE_MOUSE_INPUT tracking active regardless of
// which process is attached, queuing a MOUSE_EVENT_RECORD for every click
// that lands on it - and while our window is on top and focused, clicks
// meant for the emulator still land on the console positionally, so they
// get queued there too, even though nothing here ever calls
// ReadConsoleInput() to consume them. The console only processes that
// backlog once we exit and it resumes reading its own input, at which point
// it can replay a bare button-down with no matching button-up (the pair was
// split across our own window's grab-driven handling and the console's
// blind queuing) - looking exactly like a stuck mouse button. Suppressing
// ENABLE_MOUSE_INPUT for the lifetime of our process stops any of this from
// being queued in the first place; see ConsoleModeGuard below.

#ifdef _WINAPI
// RAII guard: saves the console input mode on construction, disables
// ENABLE_MOUSE_INPUT (see comment above), and restores it + flushes stale
// events on destruction — so whatever launched us (FAR Manager, cmd.exe)
// resumes reading its console input from a clean slate instead of replaying
// a stray, unmatched button transition. The destructor runs automatically at
// scope exit or program termination, eliminating the manual save/restore pair
// that was previously spread across Init()/Done().
class ConsoleModeGuard {
public:
    ConsoleModeGuard() : handle_(INVALID_HANDLE_VALUE), mode_(0), saved_(false) {
        handle_ = GetStdHandle(STD_INPUT_HANDLE);
        if (handle_ != INVALID_HANDLE_VALUE && GetConsoleMode(handle_, &mode_)) {
            saved_ = true;
            SetConsoleMode(handle_, mode_ & ~ENABLE_MOUSE_INPUT);
        }
    }

    ~ConsoleModeGuard() {
        if (!saved_) return;
        // Restore the console's own mouse-tracking mode exactly as we found it,
        // then discard anything that queued up regardless (e.g. from a click
        // right at the Init()/Done() edge, before/after the mode change above
        // took effect) - so whatever launched us resumes reading its console
        // input from a clean slate instead of replaying a stray, unmatched
        // button transition. No-op if we were never attached to a real console.
        SetConsoleMode(handle_, mode_);
        FlushConsoleInputBuffer(handle_);
    }

private:
    HANDLE handle_;
    DWORD mode_;
    bool saved_;
};
#endif//_WINAPI

static struct eOptionSpeed : public xOptions::eOptionInt
{
	const char* Name() const override { return "speed"; }
	const char** Values() const override
	{
		static const char* values[] = { "1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x", nullptr };
		return values;
	}
	void Change(bool next = true) override
	{
		eOptionInt::Change(0, 10, next);
	}
	int Order() const override { return 69; }
} op_speed;

int OpSpeed() { return op_speed; }
void OpSpeed(int v) { op_speed.Set(v); }

static bool sdl_inited = false;

bool InitVideo();
bool InitAudio();
void DoneVideo();
void DoneAudio();
void UpdateAudio();
void UpdateScreen();
void ProcessKey(SDL_Event& e);

namespace xImGui
{
void FeedEvent(const SDL_Event& e);
void BeginFrame();
bool WantCaptureKeyboard();
bool WantCaptureMouse();
void InitMenu();
}
//namespace xImGui

#ifdef SDL_USE_MOUSE
void ProcessMouse(SDL_Event& e);
// Non-owning access to the GLWindow's window handle (owned by sdl2_desktop_video.cpp).
// Used here only to watch grab state for the status bar message.
SDL_Window* GetVideoWindow();
#endif//SDL_USE_MOUSE

#ifndef SDL_DEFAULT_FOLDER
// Use standard platform config paths to stay consistent with wxwidgets:
// Windows: %APPDATA%/unreal_speccy_portable/
// Linux:   ~/.config/unreal_speccy_portable/
// macOS:   ~/Library/Application Support/unreal_speccy_portable/
static const char* USP_HomePath()
{
	static char usp_home_path[xIo::MAX_PATH_LEN];

#ifdef _WINAPI
	const char* appdata = getenv("APPDATA");
	if(appdata)
	{
		snprintf(usp_home_path, sizeof(usp_home_path), "%s/unreal_speccy_portable/", appdata);
		return usp_home_path;
	}
#elif defined(__APPLE__)
	const char* home = getenv("HOME");
	if(home)
	{
		snprintf(usp_home_path, sizeof(usp_home_path), "%s/Library/Application Support/unreal_speccy_portable/", home);
		return usp_home_path;
	}
#else // Linux and other Unix-like systems
	const char* home = getenv("HOME");
	if(home)
	{
		snprintf(usp_home_path, sizeof(usp_home_path), "%s/.config/unreal_speccy_portable/", home);
		return usp_home_path;
	}
#endif
	return nullptr;
}
#endif//SDL_DEFAULT_FOLDER

bool Init()
{
#ifndef SDL_DEFAULT_FOLDER
	const char* usp_home_path = USP_HomePath();
	if(usp_home_path)
	{
		xIo::PathCreate(usp_home_path);
		xIo::SetProfilePath(usp_home_path);
		OpLastFile(usp_home_path);
	}
#else//SDL_DEFAULT_FOLDER
	xIo::SetProfilePath(SDL_DEFAULT_FOLDER);
	OpLastFile(SDL_DEFAULT_FOLDER);
#endif//SDL_DEFAULT_FOLDER
	Handler()->OnInit();

	// Must be set before SDL_Init(SDL_INIT_VIDEO): on Windows, an app with no
	// manifest is DPI-unaware by default, and the OS bitmap-stretches its
	// output to match the physical display - imperceptible on most content,
	// but very visible on fine periodic detail like the CRT phosphor-mask
	// columns, and Windows applies it most aggressively to fullscreen/
	// maximized windows (matching the "fullscreen only, any windowed size is
	// fine" symptom). platform/wxwidgets doesn't hit this because wxWidgets
	// links its own manifest declaring DPI awareness by default; this SDL2
	// executable has no manifest at all, so it needs the equivalent
	// declared explicitly. "permonitorv2" doesn't switch SDL to a virtualized
	// coordinate system (unlike SDL_HINT_WINDOWS_DPI_SCALING), so it stays a
	// no-op for every raw-pixel size/position value already used elsewhere in
	// this file (op_window_state, SDL_GL_GetDrawableSize, ...). Ignored on
	// non-Windows platforms.
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

#ifdef _WINAPI
	// RAII: ConsoleModeGuard saves the console mode on construction (disabling
	// ENABLE_MOUSE_INPUT) and restores it + flushes stale events on destruction.
	// Placed in a function-local static so its destructor runs at program exit,
	// matching the previous Init()/Done() lifecycle without manual cleanup code.
	static ConsoleModeGuard g_console_mode_guard;
#endif//_WINAPI

	Uint32 init_flags = SDL_INIT_VIDEO|SDL_INIT_AUDIO;
#ifdef SDL_USE_JOYSTICK
	init_flags |= SDL_INIT_GAMECONTROLLER;
#endif//SDL_USE_JOYSTICK
	if(SDL_Init(init_flags) < 0)
		return false;

#ifdef SDL_USE_JOYSTICK
	SDL_GameControllerEventState(SDL_ENABLE);
	// Actually open every gamepad already plugged in at startup - a
	// controller connected before the app starts would otherwise only be
	// picked up once SDL happens to deliver a queued
	// SDL_CONTROLLERDEVICEADDED, which HandleControllerEvent() already
	// guards against double-opening. Matches Frame::Frame() in wx_frame.cpp.
	GamepadBackend().Initialize();
#endif//SDL_USE_JOYSTICK

	sdl_inited = true;

	if(!InitVideo())
		return false;
	if(!InitAudio())
		return false;

	xImGui::InitMenu();

	// Deliberately NOT registering an SDL_AddEventWatch()/SDL_WINDOWEVENT_EXPOSED
	// handler here, unlike platform/sdl2/sdl2.cpp. That handler runs
	// synchronously *from inside* SDL_PollEvent()/SDL_PumpEvents() - on
	// Windows, SDL_WINDOWEVENT_EXPOSED fires liberally while the window is
	// being interacted with (including an interactive resize-drag, or just
	// clicking a menu), which would call back into this same frame's
	// in-progress event handling. That is harmless for platform/gles2's
	// stateless per-frame draw, but Dear ImGui's NewFrame()/Render() pairing
	// must never nest or repeat without a fresh NewFrame() in between - doing
	// so corrupts its popup/ID stack (menus stop closing on outside click,
	// overlay text can render twice in the same spot). The trade-off is a
	// window that can go blank for the brief duration of an actual live
	// resize-drag on Windows (SDL's own nested modal loop blocks regular
	// SDL_PollEvent() during that drag either way) - correctness of the UI
	// state machine matters more than smooth repaint during that one
	// interaction, and this is the trade most non-ImGui SDL apps make anyway.
	return true;
}

void Done()
{
#ifdef SDL_USE_JOYSTICK
	GamepadBackend().Shutdown();
#endif//SDL_USE_JOYSTICK
	DoneAudio();
	DoneVideo();
	if(sdl_inited)
		SDL_Quit();
	Handler()->OnDone();
}

static bool quit = false;

#ifdef SDL_USE_JOYSTICK
static JoystickMapper joystick_mapper;
#endif//SDL_USE_JOYSTICK

// SDL events that might be consumed by Dear ImGui (keyboard/mouse) are
// buffered here during the poll loop instead of being routed immediately -
// see the big comment in Loop1() for why.
static std::vector<SDL_Event> game_input_events;

void Loop1()
{
	game_input_events.clear();

	SDL_Event e;
	while(SDL_PollEvent(&e))
	{
		xImGui::FeedEvent(e); // accumulate raw input only, no capture-flag decision here yet

		switch(e.type)
		{
		case SDL_QUIT:
			quit = true;
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
#ifdef SDL_USE_MOUSE
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEMOTION:
#endif//SDL_USE_MOUSE
			// Routing decision (emulator vs Dear ImGui) is deferred until
			// after xImGui::BeginFrame() below. io.WantCaptureKeyboard/Mouse
			// are only correct for *this* batch of events once ImGui has
			// processed them via NewFrame() - checking them earlier, per
			// event as it's polled, makes the very first click on a
			// still-unseen menu item read as
			// "not over the UI" and leak through to the emulator. That
			// matters here specifically because platform/sdl2/sdl2_mouse.cpp
			// has no bounds check of its own: it grabs the mouse
			// unconditionally on any click it receives while the window
			// isn't already grabbed, trusting the caller to have already
			// filtered out clicks meant for the UI.
			game_input_events.push_back(e);
			break;
#ifdef SDL_USE_JOYSTICK
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
		case SDL_CONTROLLERAXISMOTION:
		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
			// Per-player button mapping/gameplay translation happens once a
			// frame below (see the JoystickMapper polling loop after this
			// switch), same as GLCanvas::OnGamepadPoll() in wx_canvas.cpp -
			// this only needs to keep GamepadBackend's raw per-device state
			// (open/closed, button/axis values) current.
			GamepadBackend().HandleControllerEvent(e);
			break;
#endif//SDL_USE_JOYSTICK
		case SDL_DROPFILE:
			if(e.drop.file)
			{
				Handler()->OnOpenFile(e.drop.file);
				SDL_free(e.drop.file);
			}
			break;
		default:
			break;
		}
	}

	xImGui::BeginFrame(); // io.WantCapture* now reflect this whole batch, incl. the events buffered above

	bool ui_want_keyboard = xImGui::WantCaptureKeyboard();
	bool ui_want_mouse = xImGui::WantCaptureMouse();
	for(size_t i = 0; i < game_input_events.size(); ++i)
	{
		SDL_Event& ge = game_input_events[i];
		switch(ge.type)
		{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if(!ui_want_keyboard)
				ProcessKey(ge);
			break;
#ifdef SDL_USE_MOUSE
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if(!ui_want_mouse)
			{
				// A click that lands on the running emulator view while
				// About/Options is still open elsewhere is a clear "I want
				// to interact with the game now" signal - honour that by
				// dismissing the open dialog(s) instead of also letting
				// this same click reach sdl2_mouse.cpp's Kempston-mouse
				// grab below, which would otherwise hide/capture the
				// cursor while leaving the dialog visible but unreachable
				// until Escape. Only the button-down needs this: with
				// nothing grabbed, the matching button-up falls through to
				// ProcessMouse() same as always, where its own
				// SDL_GetWindowGrab() check makes it a no-op.
				if(ge.type == SDL_MOUSEBUTTONDOWN && xImGui::AnyMenuDialogActive())
				{
					xImGui::CloseMenuDialogs();
					break;
				}
				// wx's evtMouseCapture (posted from wx_mouse.cpp whenever
				// SDL_SetWindowGrab()'s state actually changes) drives
				// Frame::OnMouseCapture()'s status text; sdl2_mouse.cpp
				// (reused as-is) has no such notification of its own, so the
				// same message is derived here instead, from the grab state
				// before/after the call that might change it.
				SDL_Window* win = GetVideoWindow();
				bool grabbed_before = SDL_GetWindowGrab(win) != SDL_FALSE;
				ProcessMouse(ge);
				bool grabbed_after = SDL_GetWindowGrab(win) != SDL_FALSE;
				if(grabbed_after != grabbed_before)
					xImGui::SetStatusText(grabbed_after ? "Mouse captured, press ESC to cancel" : "Mouse released");
			}
			break;
		case SDL_MOUSEMOTION:
			if(!ui_want_mouse)
				ProcessMouse(ge);
			break;
#endif//SDL_USE_MOUSE
		default:
			break;
		}
	}

#ifdef SDL_USE_JOYSTICK
	// Per-player gamepad -> ZX-keyboard translation, once a frame - the same
	// thing GLCanvas::OnGamepadPoll() does on its own wxTimer in the wx
	// build, just driven by this platform's single frame loop instead.
	// Profiles are re-read from xOptions every frame rather than cached:
	// parsing a short string is free next to everything else this loop
	// already does each frame, and it means the Options dialog's OK handler
	// doesn't need to separately poke this file to pick up a change (unlike
	// wx_canvas.cpp's ReloadGamepadProfiles(), called explicitly from both
	// wx_frame.cpp and wx_optionsdialog.cpp for exactly that reason).
	for(int player = 0; player < 2; ++player)
	{
		JoystickProfile profile;
		DeserializeProfile(OpJoystickMappingData(player), profile);
		int hinted_index = OpHostGamepadDevice(player);
		std::string resolved_guid;
		profile.host_device_index = ResolveDeviceIndexForGuid(profile.device_guid, hinted_index, &resolved_guid);

		std::vector<JoystickMapper::EmulatedKeyEvent> key_events;
		if(profile.IsEnabled())
		{
			GamepadBackend().RefreshDeviceState(profile.host_device_index);
			const GamepadState& state = GamepadBackend().GetState(profile.host_device_index);
			key_events = joystick_mapper.ProcessEvent(profile, player, state, profile.host_device_index);
		}
		else
		{
			// Device just vanished for this player (unplugged, or its GUID no
			// longer resolves to a live one) - release anything still held so
			// it doesn't get stuck down. See ReleaseAll()'s comment.
			key_events = joystick_mapper.ReleaseAll(player);
		}

		for(const auto& ke : key_events)
		{
			dword flags = OpJoyKeyFlags();
			if(ke.is_down)
				flags |= KF_DOWN;
			Handler()->OnKey(ke.key, flags);
		}
	}
#endif//SDL_USE_JOYSTICK

	Handler()->OnLoop();
	UpdateScreen(); // DrawGL() + xImGui::EndFrame() (draws the menu/About/toast + Render) + SwapWindow
	UpdateAudio();
}

void Loop()
{
	eTick last_tick;
	last_tick.SetCurrent();
	while(!quit)
	{
		for(int i = OpSpeed(); --i >= 0;)
			Handler()->OnLoop();
		Loop1();
		while(last_tick.Passed().Ms() < 15)
		{
			SDL_Delay(3);
		}
		last_tick.SetCurrent();
		if(OpQuit())
			quit = true;
	}
}

}
//namespace xPlatform

int main(int argc, char* argv[])
{
	if(!xPlatform::Init())
	{
		xPlatform::Done();
		return -1;
	}
	if(argc > 1)
		xPlatform::Handler()->OnOpenFile(argv[1]);
	xPlatform::Loop();
	xPlatform::Done();
	return 0;
}

#endif//USE_SDL2_DESKTOP
