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

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>

#include "sdl2_desktop_pacing.h"

namespace xPlatform
{
namespace xPacing
{

namespace
{
// The wall-clock time of the last present (right after SwapWindow returned).
double g_last_present_time = 0;
}

double NowSeconds()
{
	static const double freq = (double)SDL_GetPerformanceFrequency();
	return (double)SDL_GetPerformanceCounter() / freq;
}

double LastPresentTime() { return g_last_present_time; }

void OnPresent(double now_s) { g_last_present_time = now_s; }

}//namespace xPacing
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
