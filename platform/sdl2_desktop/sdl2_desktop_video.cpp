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
//  platform/sdl2_desktop/sdl2_desktop_video.cpp
//
//  Window + desktop OpenGL context for the new "sdl2_desktop" platform.
//
//  Deliberately mirrors platform/sdl2/sdl2_video.cpp (same op_window_state /
//  op_full_screen option names, so existing config files keep working if a
//  user switches builds), but:
//   - requests a desktop GL (core) context instead of GLES2,
//   - draws the emulator screen through platform/gl/draw.cpp (DrawGL(), the
//     same shader-based renderer platform/wxwidgets uses - gigascreen /
//     scanlines / PAL effects / mipmapping), instead of platform/gles2,
//   - renders the Dear ImGui overlay (menu, About window, ...) on top,
//     after the emulator frame, still inside the single call to
//     UpdateScreen() - one thread, one SDL_GL_SwapWindow() per frame.
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <GL/glew.h> // must come before SDL.h/any gl.h-including header, per GLEW's own requirement
#include <SDL.h>
#include "imgui.h"
#include "../../tools/options.h"
#include "../../tools/point.h"
#include "../../devices/video_snapshot.h"

#ifdef _WINAPI
#include <SDL_syswm.h>
#include <shellapi.h> // ExtractIconExA()
#endif//_WINAPI

namespace xPlatform
{

// Implemented in platform/gl/draw.cpp (USE_GL) - the same desktop GL
// renderer platform/wxwidgets/wx_canvas.cpp calls into.
void initGlew();
void initGraphics(int scr_width, int scr_height);
void cleanupGraphics();
bool DrawGL(int vport_x, int vport_y, int vport_width, int vport_height, const VideoSnapshot& snap);

namespace xImGui
{
void Init(SDL_Window* window, SDL_GLContext context);
void Done();
void EndFrame();
}
//namespace xImGui

SDL_Window* window = NULL;
static SDL_GLContext context = NULL;
// Guards DoneVideo()'s cleanup calls: initGlew()/initGraphics()/xImGui::Init()
// are only reached after SDL_CreateWindow() *and* SDL_GL_CreateContext() both
// succeed. If InitVideo() returns false before that point (e.g. no OpenGL
// 3.3 support), those subsystems were never touched, and calling their
// Done()/cleanup functions anyway means ImGui_ImplOpenGL3_Shutdown()/
// ImGui_ImplSDL2_Shutdown() run without a matching Init()
// (undefined: they read state Init() would have set up), and cleanupGraphics()
// runs without GLEW's function pointers ever having been loaded
// (calling through a NULL pointer). Both are real crash paths on any system
// where context creation fails, not just a theoretical concern.
static bool graphics_inited = false;

// Same option name/format as platform/sdl2/sdl2_video.cpp's eOptionWindowState,
// on purpose - both platforms can share the same config file entry.
class eOptionWindowState : public xOptions::eOptionString
{
public:
	eOptionWindowState() { customizable = false; }
	virtual const char* Name() const { return "window state"; }

	bool Get(ePoint* position, ePoint* size, bool* maximized) const
	{
		ePoint p(SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED);
		ePoint s(640, 480); // org_size (320x240) * 2 - same default wx_frame.cpp uses
		int m = 0;
		bool ok = sscanf(Value(), FormatStr(), &p.x, &p.y, &s.x, &s.y, &m) == 5;
		if(position)
			*position = p;
		if(size)
			*size = s;
		if(maximized)
			*maximized = m != 0;
		return ok;
	}
	void Set(const ePoint* position, const ePoint* size, const bool* maximized)
	{
		ePoint p, s;
		bool m;
		Get(&p, &s, &m);
		if(position)
			p = *position;
		if(size)
			s = *size;
		if(maximized)
			m = *maximized;
		char buf[512];
		sprintf(buf, FormatStr(), p.x, p.y, s.x, s.y, m ? 1 : 0);
		Value(buf);
	}
	void Update()
	{
		Uint32 flags = SDL_GetWindowFlags(window);
		if(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))
			return;
		if(!(flags&SDL_WINDOW_MAXIMIZED) && !(flags&SDL_WINDOW_MINIMIZED))
		{
			ePoint p;
			SDL_GetWindowPosition(window, &p.x, &p.y);
			ePoint s;
			SDL_GetWindowSize(window, &s.x, &s.y);
			bool m = false;
			Set(&p, &s, &m);
		}
		else
		{
			bool m = (flags&SDL_WINDOW_MAXIMIZED) != 0;
			Set(NULL, NULL, &m);
		}
	}

private:
	const char* FormatStr() const { return "position(%d, %d); size(%d, %d); maximized(%d)"; }
} op_window_state;

static struct eOptionFullScreen : public xOptions::eOptionBool
{
	virtual const char* Name() const { return "full screen"; }
	virtual int Order() const { return 32; }
	virtual void Set(const bool& v)
	{
		eOptionBool::Set(v);
		Apply();
	}
	virtual void Apply()
	{
		SDL_SetWindowFullscreen(window, (*this) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	}
	void Update()
	{
		bool fs = (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
		if(*this != fs)
			Set(fs);
	}
} op_full_screen;

// sdl2_mouse.cpp (reused as-is from platform/sdl2/) needs this to map window
// coordinates to the emulator's 320x240 screen space for Kempston mouse.
// platform/gles2/gles2.cpp has its own fill/border-aware version; this one
// matches what platform/wxwidgets/wx_mouse.cpp already does - plain 4:3
// aspect-ratio scaling via the shared xPlatform::GetScaleWithAspectRatio43().
void OpZoomGet(float* sx, float* sy, const ePoint& org_size, const ePoint& size)
{
	(void)org_size;
	GetScaleWithAspectRatio43(sx, sy, size.x, size.y);
}

// Same idea as platform/wxwidgets/wx_canvas.cpp's GLCanvas::getMaxDisplayResolution():
// the full-quality FBO in platform/gl/draw.cpp is sized once, at startup, from
// the largest connected display rather than from the current (typically small,
// windowed) drawable size. That way switching to fullscreen never renders the
// full-quality path through an undersized FBO and stretches it back up with
// GL_LINEAR - which is what produced the visible aliasing/blur on this platform.
//
// Deliberately uses SDL_GetCurrentDisplayMode(), NOT SDL_GetDisplayBounds().
// SDL_GetDisplayBounds() reports size in "screen coordinates", which on
// HiDPI/Retina displays are logical points, not the real framebuffer pixel
// count - e.g. 1440x900 points for a 2880x1800-pixel panel at 2x scale.
// SDL_GL_GetDrawableSize() (used for `drawable` below, and by UpdateScreen()
// every frame) is always in actual pixels. Mixing the two unit spaces here
// would silently pick the smaller, points-based value on exactly the
// high-DPI screens where it matters, leaving the FBO under-resolved relative
// to the real fullscreen framebuffer. The visible symptom is subtle: the
// mask itself is still computed per-pixel (gl_FragCoord in the screen pass),
// but the picture it's multiplied over is undersampled, so the fine,
// high-frequency phosphor-mask columns are the first detail to show
// interpolation between neighbouring pixels - looking like a gradient
// instead of sharp columns. SDL_GetCurrentDisplayMode() reports the video
// mode in the same pixel units as SDL_GL_GetDrawableSize(), so the two are
// safe to compare and max() directly.
static ePoint GetMaxDisplayResolution()
{
	ePoint result(-1, -1);
	int count = SDL_GetNumVideoDisplays();
	for(int i = 0; i < count; ++i)
	{
		SDL_DisplayMode mode;
		if(SDL_GetCurrentDisplayMode(i, &mode) != 0)
			continue;
		if(mode.w * mode.h > result.x * result.y)
		{
			result.x = mode.w;
			result.y = mode.h;
		}
	}
	return result;
}

bool InitVideo()
{
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // required for a core-profile context on macOS; harmless elsewhere
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	ePoint pos, size;
	bool maximized;
	op_window_state.Get(&pos, &size, &maximized);
	Uint32 flags = SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI;
	if(maximized)
		flags |= SDL_WINDOW_MAXIMIZED;
	if(op_full_screen)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	window = SDL_CreateWindow(Handler()->WindowCaption(), pos.x, pos.y, size.x, size.y, flags);
	if(!window)
		return false;
	SDL_SetWindowMinimumSize(window, 320, 240); // org_size - matches wx_frame.cpp's SetMinSize(GetSize()) after SetClientSize(org_size)
	context = SDL_GL_CreateContext(window);
	if(!context)
		return false;
	SDL_GL_MakeCurrent(window, context);
	SDL_GL_SetSwapInterval(1); // vsync - single thread, no render-thread hand-off needed

	// SDL's default window has no icon of its own on Windows (unlike a
	// standard Win32/wx window, which normally picks one up from the
	// module's resources) - the title bar, system-menu (top-left corner),
	// taskbar button, and Alt+Tab all end up with a blank/generic icon.
#ifdef _LINUX
	#include "../../build/linux/icon.c"
	SDL_Surface* icon_sufrace = SDL_CreateRGBSurfaceFrom((void*)icon.pixel_data, icon.width, icon.height,
		icon.bytes_per_pixel * 8, icon.bytes_per_pixel*icon.width, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
	SDL_SetWindowIcon(window, icon_sufrace);
	SDL_FreeSurface(icon_sufrace);
#endif//_LINUX
#ifdef _WINAPI
	// Unlike _LINUX above, this doesn't go through SDL_SetWindowIcon() with a
	// decoded pixel buffer - it pulls the icon that CMakeLists.txt now
	// embeds into the .exe as a Win32 resource (SRCRES = the same
	// unreal_speccy_portable.rc/.ico platform/wxwidgets' Windows build
	// already uses, so this is the exact same icon, not just a similar one)
	// and applies it directly via the native HWND, which is what actually
	// drives the title bar / system-menu / taskbar / Alt+Tab icon on
	// Windows. ExtractIconExA() reads whichever icon resource comes first
	// in the exe's resource table (index 0) - the same thing Explorer does
	// to show a .exe's own icon - so this doesn't need to know the specific
	// numeric resource ID the .rc happens to declare it under, and as a
	// side effect the .exe itself now also shows the right icon in
	// Explorer/the taskbar before it's even running, instead of a generic
	// default one.
	char exe_path[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exe_path, MAX_PATH);
	HICON icon_big = NULL, icon_small = NULL;
	if(ExtractIconExA(exe_path, 0, &icon_big, &icon_small, 1) > 0)
	{
		SDL_SysWMinfo wm_info;
		SDL_VERSION(&wm_info.version);
		if(SDL_GetWindowWMInfo(window, &wm_info))
		{
			HWND hwnd = wm_info.info.win.window;
			if(icon_big)
				SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
			if(icon_small)
				SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);
		}
	}
#endif//_WINAPI

	initGlew();
	ePoint drawable;
	SDL_GL_GetDrawableSize(window, &drawable.x, &drawable.y);

	// Size the FBO from whichever is larger: the current drawable, or the
	// biggest connected display (see GetMaxDisplayResolution() above). This
	// covers the common case (start windowed, then go fullscreen) without
	// needing any runtime FBO-resize logic at all.
	ePoint max_display = GetMaxDisplayResolution();
	ePoint init_size(
		(max_display.x > drawable.x) ? max_display.x : drawable.x,
		(max_display.y > drawable.y) ? max_display.y : drawable.y);

	initGraphics(init_size.x, init_size.y);
	graphics_inited = true;

	xImGui::Init(window, context);
	return true;
}

// Window menu's "Size 100%/200%/300%" + Ctrl+1/2/3 - equivalent of
// Frame::OnResize() in wx_frame.cpp: leaves fullscreen/maximized state first
// (a resize request while either is active would otherwise be a no-op, or
// worse, silently ignored), then sets the client area to org_size * mult.
void ResizeToOrgSizeMultiple(int mult)
{
	if(op_full_screen)
		op_full_screen.Set(false);
	if(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)
		SDL_RestoreWindow(window);
	SDL_SetWindowSize(window, 320 * mult, 240 * mult);
}

void DoneVideo()
{
	// Release any active mouse grab/relative-mode *before* tearing down the
	// window, regardless of how we got here. sdl2_mouse.cpp's ProcessMouse()
	// (Kempston-mouse-style input, wired in via sdl2_desktop_keys.cpp) grabs
	// the mouse with SDL_SetWindowGrab()+SDL_SetRelativeMouseMode() on the
	// first click inside the window, and only releases it on Escape. If the
	// app is closed some other way while still grabbed (Alt+F4, the window's
	// close button, File > Exit, ...), that OS-level cursor clip/raw-input
	// capture would otherwise still be active when SDL_DestroyWindow()/
	// SDL_Quit() run. When launched from a console host such as FAR Manager,
	// the console regaining input focus afterwards can then read one of its
	// mouse buttons as still held, since it never saw the matching
	// button-up while relative mode was intercepting input. Calling these
	// unconditionally is harmless when nothing was ever grabbed.
	if(window)
		SDL_SetWindowGrab(window, SDL_FALSE);
	SDL_SetRelativeMouseMode(SDL_FALSE);

	if(graphics_inited)
	{
		xImGui::Done();
		cleanupGraphics();
		graphics_inited = false;
	}
	if(context)
	{
		SDL_GL_DeleteContext(context);
		context = NULL;
	}
	if(window)
	{
		SDL_DestroyWindow(window);
		window = NULL;
	}
}

void UpdateScreen()
{
	op_window_state.Update();
	op_full_screen.Update();

	ePoint s;
	SDL_GL_GetDrawableSize(window, &s.x, &s.y);

	// --- Exclude ImGui menu/status bar from the emulator viewport ---
	// DrawGL renders to fill its entire (vport_x, vport_y, width, height)
	// region. When not fullscreen, ImGui draws a menu bar at the top and a
	// status bar ("notification footer") at the bottom on top of whatever
	// DrawGL produced — with semi-transparent backgrounds, this causes the
	// bars to visually overlap / cover part of the ZX screen image.
	//
	// Fix: compute the menu/status bar heights (each ≈ ImGui::GetFrameHeight()
	// at the current DPI scale) and pass an offset + reduced dimensions so
	// DrawGL only renders into the non-UI region between them. The areas
	// outside are cleared by glClear above, leaving clean space for ImGui.
	int vport_x = 0;
	int vport_y = 0;
	int vport_w = s.x;
	int vport_h = s.y;
	bool fullscreen = false;
	{
		xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find("full screen");
		if(op)
			fullscreen = *op;
	}
	if(!fullscreen && vport_w > 0 && vport_h > 0)
	{
		float ui_h = (float)ImGui::GetFrameHeight(); // menu bar and status bar use the same height
		int ui_total = (int)(ui_h * 2.0f);
		if(ui_total < vport_h)
		{
			vport_y = (int)ui_h;
			vport_h = vport_h - ui_total;
		}
	}

	// platform/gl/draw.cpp only clears the color buffer when the viewport/zoom
	// layout changes (an optimization: the emulator's own textured quad
	// repaints its area every frame anyway). That quad does not necessarily
	// cover the whole window (e.g. the top strip under the menu bar, or any
	// letterboxing), and Dear ImGui's overlay only *adds* draw commands for
	// whatever is currently open - when a menu closes, ImGui simply stops
	// drawing there, it doesn't erase what was there before. Combined, a
	// menu could close while the layout stays the same size, leaving its
	// last-drawn pixels un-overwritten by either subsystem. An unconditional
	// clear here (cheap on any real GPU) keeps this platform's own frame
	// self-contained without needing to touch or replicate draw.cpp's
	// layout-change tracking.
	glClear(GL_COLOR_BUFFER_BIT);

	// Same idiom as wx_canvas.cpp's non-threaded snapshot build - here there
	// is only one thread to begin with, so no mutex/atomics are needed at all.
	VideoSnapshot snap;
	snap.frame = Handler()->VideoFrame();
	memcpy(snap.video, Handler()->VideoData(), sizeof(snap.video));

	DrawGL(vport_x, vport_y, vport_w, vport_h, snap);
	xImGui::EndFrame(); // menu/About/toast overlay, drawn on top, same GL context, same thread
	// (xImGui::BeginFrame() already ran earlier in Loop1(), right after this
	// frame's SDL events were fed in and before they were routed - see there)

	SDL_GL_SwapWindow(window); // the only swap for the whole frame (video + UI)
}

}
//namespace xPlatform

#endif//USE_SDL2_DESKTOP

