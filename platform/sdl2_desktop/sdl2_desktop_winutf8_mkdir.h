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
//  platform/sdl2_desktop/sdl2_desktop_winutf8_mkdir.h
//
//  Force-included (/FI, see build/cmake/CMakeLists.txt) into platform/win/
//  io_select_win.cpp ONLY, so that its MkDir() creates a UTF-8 folder name as
//  UTF-16 - see sdl2_desktop_winutf8.h for why. Without it, on Windows 7,
//  saving into a folder with a Cyrillic name would CREATE a folder named with
//  the mojibake of the UTF-8 bytes. Windows.h comes first on purpose: it
//  declares CreateDirectoryA as a dllimport function, which the macro below
//  must not rename. Defined in sdl2_desktop_winutf8.cpp.
// =============================================================================

#ifndef SDL2_DESKTOP_WINUTF8_MKDIR_H
#define SDL2_DESKTOP_WINUTF8_MKDIR_H

#if defined(_WIN32)

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
BOOL WINAPI usp_CreateDirectoryUtf8(LPCSTR path, LPSECURITY_ATTRIBUTES attrs);
#ifdef __cplusplus
}
#endif

#define CreateDirectoryA usp_CreateDirectoryUtf8

#endif//_WIN32

#endif//SDL2_DESKTOP_WINUTF8_MKDIR_H
