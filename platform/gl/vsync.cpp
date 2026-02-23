/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2010 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

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

#ifdef USE_GL

#ifdef _WINDOWS
#include <windows.h>
#endif//_WINDOWS

#ifndef _MAC
#include <GL/gl.h>
#else//_MAC
#include <OpenGL/gl.h>
#endif//_MAC

#ifdef _LINUX
#include <GL/glx.h>
#endif//_LINUX

namespace xPlatform
{

#ifdef _LINUX
    void VsyncGL(bool on)
    {
        // Prefer GLX_EXT_swap_control: takes (display, drawable, interval) and
        // does not call XSync internally, making it safe to call from a render
        // thread alongside GTK's X11 usage on the main thread.
        //
        // Fall back to GLX_SGI_swap_control only if EXT is unavailable.
        // The SGI variant calls XSync on Mesa which causes GLXBadContextTag when
        // the main thread is concurrently using the X11 connection; callers on
        // Linux should hold XLockDisplay() around this function to be safe.

        static bool inited = false;
        static PFNGLXSWAPINTERVALEXTPROC    si_ext = nullptr;
        static PFNGLXSWAPINTERVALSGIPROC    si_sgi = nullptr;
        static PFNGLXSWAPINTERVALMESAPROC    si_mes = nullptr;

        if (!inited)
        {
            inited = true;
            si_ext = (PFNGLXSWAPINTERVALEXTPROC)
                glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");
            si_mes = (PFNGLXSWAPINTERVALMESAPROC)
                glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA");
            si_sgi = (PFNGLXSWAPINTERVALSGIPROC)
                glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalSGI");
        }

        unsigned int interval = on ? 1 : 0;

        if (si_ext)
        {
            // EXT: requires display and drawable — safest, no XSync
            Display* dpy = glXGetCurrentDisplay();
            GLXDrawable drawable = glXGetCurrentDrawable();
            if (dpy && drawable)
                si_ext(dpy, drawable, interval);
        }
        else if (si_mes)
        {
            // MESA: simple interval call, generally no XSync
            si_mes(interval);
        }
        else if (si_sgi)
        {
            // SGI: legacy, may call XSync on Mesa — caller holds XLockDisplay
            si_sgi(interval);
        }
    }
#endif//_LINUX

#ifdef _WINDOWS
    typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);
    void VsyncGL(bool on)
    {
        static bool inited = false;
        static PFNWGLSWAPINTERVALEXTPROC si = NULL;
        if (!inited)
        {
            si = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
            inited = true;
        }
        if (si)
            si(on);
    }
#endif//_WINDOWS

#ifdef _MAC
    void VsyncGL(bool on)
    {
    }
#endif//_MAC

}//namespace xPlatform

#endif//USE_GL
