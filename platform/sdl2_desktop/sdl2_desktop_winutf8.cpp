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
//  platform/sdl2_desktop/sdl2_desktop_winutf8.cpp
//
//  See sdl2_desktop_winutf8.h (fopen/fopen_s) and sdl2_desktop_winutf8_mkdir.h
//  (CreateDirectoryA). This file is compiled with the first of those
//  force-included too, hence the #undef below: the real functions are what it
//  falls back to.
// =============================================================================

#ifdef _WIN32

#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#undef fopen
#undef fopen_s

#ifdef _MSC_VER
#pragma warning(disable: 4996) // the plain fopen() is meant here
#endif

namespace
{

bool IsAscii(const char* s)
{
	for(; *s; ++s)
		if((unsigned char)*s >= 0x80)
			return false;
	return true;
}

// UTF-8 -> UTF-16 (malloc'ed; free() it). NULL when there is nothing to convert
// - a plain ASCII string, which every code page reads the same - or when the
// string is not valid UTF-8 (an ANSI one from somewhere): the caller then uses
// the narrow function, as it would have without this file.
wchar_t* Widen(const char* s)
{
	if(!s || IsAscii(s))
		return NULL;
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0); // with the terminating zero
	if(n <= 0)
		return NULL;
	wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)n);
	if(!w)
		return NULL;
	if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n)
	{
		free(w);
		return NULL;
	}
	return w;
}

}//namespace

extern "C" FILE* usp_fopen_utf8(const char* name, const char* mode)
{
	wchar_t* wname = Widen(name);
	if(!wname)
		return fopen(name, mode);
	wchar_t wmode[16]; // "rb", "wb", "w": always ASCII
	size_t i = 0;
	for(; mode[i] && i < 15; ++i)
		wmode[i] = (wchar_t)(unsigned char)mode[i];
	wmode[i] = 0;
	FILE* f = _wfopen(wname, wmode);
	int err = errno;
	free(wname);
	errno = err;
	return f;
}

extern "C" int usp_fopen_s_utf8(FILE** file, const char* name, const char* mode)
{
	if(!file)
		return EINVAL;
	*file = usp_fopen_utf8(name, mode);
	return *file ? 0 : (errno ? errno : EINVAL);
}

extern "C" BOOL WINAPI usp_CreateDirectoryUtf8(LPCSTR path, LPSECURITY_ATTRIBUTES attrs)
{
	wchar_t* wpath = Widen(path);
	if(!wpath)
		return CreateDirectoryA(path, attrs);
	BOOL rc = CreateDirectoryW(wpath, attrs);
	DWORD err = GetLastError(); // MkDir() looks at ERROR_ALREADY_EXISTS
	free(wpath);
	SetLastError(err);
	return rc;
}

#endif//_WIN32
