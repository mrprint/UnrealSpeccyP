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
//  platform/sdl2_desktop/sdl2_desktop_winutf8.h
//
//  Force-included (/FI, see build/cmake/CMakeLists.txt) into every source file
//  of the Windows sdl2_desktop build - the emulation core, tinyxml2 and minizip
//  among them. None of those is edited for it: they keep calling fopen().
//
//  Why: every path in this program is UTF-8 (SDL, Dear ImGui and the file
//  dialog hand them out so). On Windows 10 1903+ the "UTF-8 active code page"
//  of the manifest makes the narrow C runtime functions take that. Windows 7
//  has no such code page: fopen() reads a UTF-8 name as ANSI, and a name with
//  letters the system code page lacks (Cyrillic on a non-Cyrillic system, say)
//  is not just misread but turned into '?' for good.
//
//  What: fopen() and fopen_s() (tinyxml2 calls the latter when built with MSVC)
//  are redirected to versions that open the name as UTF-16. A name that is not
//  valid UTF-8 goes to the original function, exactly as before. Defined in
//  sdl2_desktop_winutf8.cpp.
// =============================================================================

#ifndef SDL2_DESKTOP_WINUTF8_H
#define SDL2_DESKTOP_WINUTF8_H

#if defined(_WIN32)

// The standard headers first, so that the macros below do not rename their declarations.
// <cstdio> in particular: libstdc++'s #undef's every function of <stdio.h>, fopen among
// them - included later, it would wipe the macro out of the C++ files that include it.
#ifdef __cplusplus
#include <cstdio>
#endif
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
FILE* usp_fopen_utf8(const char* name, const char* mode);
int usp_fopen_s_utf8(FILE** file, const char* name, const char* mode);
#ifdef __cplusplus
}
namespace std { using ::usp_fopen_utf8; } // for std::fopen(), which the macro turns into std::usp_fopen_utf8()
#endif

#define fopen usp_fopen_utf8
#define fopen_s usp_fopen_s_utf8

#endif//_WIN32

#endif//SDL2_DESKTOP_WINUTF8_H
