/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2013 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

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

#include "../platform.h"

#ifdef USE_WXWIDGETS

#include "../io.h"
#include "wx_cmdline.h"
#ifdef USE_SDL2_GAMEPAD
#include <SDL.h>
#endif
#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>

// XInitThreads() must be called before any X11/GLX calls, including before
// GTK and wxWidgets initialize.  On Linux/X11, all threads share a single
// Display* connection; without this call, concurrent access from the render
// thread (GLX) and the main thread (GTK/Cairo) causes GLXBadContextTag.
// The earliest safe point is a global constructor, which runs before main()
// and therefore before IMPLEMENT_APP bootstraps wxWidgets.
#ifdef _LINUX
#include <X11/Xlib.h>
namespace {
	struct XThreadInit {
		XThreadInit() { XInitThreads(); }
	} g_x_thread_init;
} // namespace
#endif//_LINUX

#ifdef USE_SDL2_GAMEPAD
namespace {
    struct SdlGamepadInit {
        SdlGamepadInit() {
            if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
                SDL_GameControllerEventState(SDL_ENABLE);
            }
        }
        ~SdlGamepadInit() {
            SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
    } g_sdl_gamepad_init;
} // namespace
#endif

namespace xPlatform
{

	void InitSound();
	void DoneSound();

	wxWindow* CreateFrame(const wxString& title, const wxPoint& pos, const eCmdLine& cmdline);

	//=============================================================================
	//	App
	//-----------------------------------------------------------------------------
	class App : public wxApp
	{
		virtual bool OnInit()
		{
#ifdef _WINDOWS
			if (access("res", 0) == -1)
			{
				wchar_t resource_path[xIo::MAX_PATH_LEN];
				int l = GetModuleFileName(NULL, resource_path, xIo::MAX_PATH_LEN);
				for (; --l >= 0 && resource_path[l] != '\\'; )
				{
				}
				resource_path[++l] = '\0';
				char buf[xIo::MAX_PATH_LEN];
				l = WideCharToMultiByte(CP_ACP, 0, resource_path, -1, buf, xIo::MAX_PATH_LEN, NULL, NULL);
				buf[l] = '\0';
				xIo::SetResourcePath(buf);
			}
#endif//_WINDOWS
#ifdef _LINUX
			// Disable NVIDIA Threaded Optimization before any GL context is created.
			// The driver reads this variable at context creation time; setting it
			// afterwards has no effect.  0 = do not overwrite if user already set it.
			setenv("__GL_THREADED_OPTIMIZATIONS", "0", 0);
#endif
			if (!wxApp::OnInit())
				return false;
			wxString cfg_dir = wxStandardPaths::Get().GetUserDataDir() + L"/";
			if (!wxDirExists(cfg_dir))
				wxMkdir(cfg_dir);
			xIo::SetProfilePath(wxConvertWX2MB(cfg_dir));
			Handler()->OnInit();
			const char* c = Handler()->WindowCaption();
#if wxMAJOR_VERSION >= 3
			SetAppDisplayName(wxConvertMB2WX(c));
#endif//wxMAJOR_VERSION
			CreateFrame(wxConvertMB2WX(c), wxPoint(100, 100), cmdline);
			InitSound();
			return true;
		}
		virtual int OnExit()
		{
			DoneSound();
			Handler()->OnDone();
			return wxApp::OnExit();
		}
		virtual void MacOpenFile(const wxString& fileName)
		{
			Handler()->OnOpenFile(wxConvertWX2MB(fileName.c_str()));
		}
		virtual void OnInitCmdLine(wxCmdLineParser& parser)
		{
			cmdline.Init(parser);
		}
		virtual bool OnCmdLineParsed(wxCmdLineParser& parser)
		{
			cmdline.Parse(parser);
			return true;
		}
		eCmdLine cmdline;
	};

}//namespace xPlatform

IMPLEMENT_APP(xPlatform::App)

namespace xIo
{
	bool MkDir(const char* path)
	{
		wxString p(wxConvertMB2WX(path));
		return wxDirExists(p) || wxFileName::Mkdir(p);
	}
}//namespace xIo

#endif//USE_WXWIDGETS