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

#ifndef __SDL2_DESKTOP_DIRWATCH_H__
#define __SDL2_DESKTOP_DIRWATCH_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_dirwatch.h
//
//  Watches one directory for changes made by anything *other* than this
//  process - another app adding/deleting/renaming a file, a USB stick being
//  repopulated, a shell command run in a terminal, and so on - so
//  sdl2_desktop_filedialog.cpp's browser can refresh its own listing
//  without the user having to back out and back in.
//
//  One native OS watch per instance, not copyable (owns a kernel handle/fd,
//  same reasoning as any other RAII handle wrapper). PollChanged() is
//  non-blocking and meant to be polled once a frame from the single main
//  loop - no extra thread, no callbacks, matching how the rest of this
//  platform (single-threaded event polling + emulator tick + render) works.
//
//  Backed by, per platform:
//    _LINUX : inotify
//    _WINAPI: FindFirstChangeNotificationW
//    _MAC   : kqueue / EVFILT_VNODE
//  If none of those match, or the OS watch itself fails to set up (missing
//  permission, exhausted inotify/fd limits, ...), PollChanged() just always
//  returns false - the browser still works, it only loses the auto-refresh.
// =============================================================================

#include <string>

namespace xPlatform {

class DirWatcher {
public:
    DirWatcher() = default;
    ~DirWatcher();
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;
    DirWatcher(DirWatcher&& other) noexcept;
    DirWatcher& operator=(DirWatcher&& other) noexcept;

    // (Re)starts watching `dir_utf8` (UTF-8 - same encoding PathToUtf8()
    // uses everywhere else in the file dialog). Safe to call again to
    // switch directories: implicitly stops watching whatever came before.
    void Watch(const std::string& dir_utf8);

    // Stops watching. Same effect as never having called Watch(); safe to
    // call when already inactive.
    void Reset();

    // Non-blocking - never waits, just reports whether anything changed in
    // the watched directory since the last call (or since Watch()). May
    // coalesce several external changes into a single true.
    bool PollChanged();

private:
#if defined(_LINUX)
    int m_inotify_fd = -1;
    int m_watch_desc = -1;
#elif defined(_WINAPI)
    // HANDLE from FindFirstChangeNotificationW, kept opaque so this header
    // doesn't need <windows.h>.
    void* m_change_handle = nullptr;
#elif defined(_MAC)
    int m_kqueue_fd = -1;
    int m_dir_fd = -1;
#endif
};

} // namespace xPlatform

#endif//__SDL2_DESKTOP_DIRWATCH_H__
