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

#include "sdl2_desktop_dirwatch.h"

#if defined(_LINUX)
#include <sys/inotify.h>
#include <unistd.h>
#include <cstdint>
#elif defined(_WINAPI)
#include <windows.h>
#elif defined(_MAC)
#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#endif

namespace xPlatform {

#if defined(_LINUX)

// ---------------------------------------------------------------------------
// Linux: inotify. One inotify instance + one watch per DirWatcher - simplest
// mapping, and this class only ever needs to watch a single directory at a
// time anyway (the file browser only ever shows one folder).
// ---------------------------------------------------------------------------

DirWatcher::~DirWatcher() { Reset(); }

DirWatcher::DirWatcher(DirWatcher&& other) noexcept
    : m_inotify_fd(other.m_inotify_fd), m_watch_desc(other.m_watch_desc)
{
    other.m_inotify_fd = -1;
    other.m_watch_desc = -1;
}

DirWatcher& DirWatcher::operator=(DirWatcher&& other) noexcept
{
    if (this != &other) {
        Reset();
        m_inotify_fd = other.m_inotify_fd; other.m_inotify_fd = -1;
        m_watch_desc = other.m_watch_desc; other.m_watch_desc = -1;
    }
    return *this;
}

void DirWatcher::Watch(const std::string& dir_utf8)
{
    Reset();
    // Non-blocking so PollChanged() can read() it every frame without ever
    // stalling the single main loop.
    m_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (m_inotify_fd < 0)
        return;
    // Linux paths are just bytes and the kernel doesn't interpret their
    // encoding - dir_utf8 (already UTF-8, same as every path PathToUtf8()
    // hands around elsewhere in the file dialog) can be passed straight
    // through with no conversion, unlike the Windows branch below.
    //
    // Everything a directory *listing* needs to react to: entries
    // appearing/disappearing/renaming (CREATE/DELETE/MOVED_*), plus
    // MODIFY/ATTRIB so an in-place overwrite or permission change is also
    // picked up, and DELETE_SELF/MOVE_SELF in case the watched folder
    // itself goes away - RefreshEntries() will notice that on its own via
    // fs::exists() once this fires.
    uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
        IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;
    m_watch_desc = inotify_add_watch(m_inotify_fd, dir_utf8.c_str(), mask);
    if (m_watch_desc < 0) {
        close(m_inotify_fd);
        m_inotify_fd = -1;
    }
}

void DirWatcher::Reset()
{
    if (m_watch_desc >= 0 && m_inotify_fd >= 0)
        inotify_rm_watch(m_inotify_fd, m_watch_desc);
    m_watch_desc = -1;
    if (m_inotify_fd >= 0)
        close(m_inotify_fd);
    m_inotify_fd = -1;
}

bool DirWatcher::PollChanged()
{
    if (m_inotify_fd < 0)
        return false;
    // Drain every pending event now rather than reading just one: inotify
    // coalesces several filesystem changes into one readable fd, and
    // leaving any of them unread would make the very next PollChanged()
    // spuriously report a change again against what's already up to date.
    alignas(struct inotify_event) char buf[4096];
    bool changed = false;
    for (;;) {
        ssize_t n = read(m_inotify_fd, buf, sizeof(buf));
        if (n <= 0)
            break; // EAGAIN (nothing left, fd is non-blocking) or a real error - either way, done
        changed = true;
    }
    return changed;
}

#elif defined(_WINAPI)

// ---------------------------------------------------------------------------
// Windows: FindFirstChangeNotificationW. Deliberately not
// ReadDirectoryChangesW - that needs either a dedicated thread or overlapped
// I/O with a completion callback pumped through the message/APC queue,
// neither of which this platform's plain single-threaded SDL loop has.
// FindFirstChangeNotificationW instead hands back a waitable HANDLE that
// WaitForSingleObject(..., 0) can poll non-blockingly once a frame, same
// shape as the inotify/kqueue fd polling on the other two platforms - it
// only tells us *that* something changed, never *what*, which is all this
// class exposes anyway.
// ---------------------------------------------------------------------------

DirWatcher::~DirWatcher() { Reset(); }

DirWatcher::DirWatcher(DirWatcher&& other) noexcept
    : m_change_handle(other.m_change_handle)
{
    other.m_change_handle = nullptr;
}

DirWatcher& DirWatcher::operator=(DirWatcher&& other) noexcept
{
    if (this != &other) {
        Reset();
        m_change_handle = other.m_change_handle;
        other.m_change_handle = nullptr;
    }
    return *this;
}

void DirWatcher::Watch(const std::string& dir_utf8)
{
    Reset();
    // FindFirstChangeNotificationW takes a *native* wide path; dir_utf8 is
    // UTF-8 (what PathToUtf8() writes everywhere else in the file dialog),
    // so this needs an explicit conversion rather than being passed as
    // ANSI bytes, which would mangle any folder name outside the system
    // codepage (Cyrillic names, very plausible for ZX Spectrum software).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dir_utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return;
    std::wstring wdir((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir_utf8.c_str(), -1, &wdir[0], wlen);

    // FALSE = don't watch subtrees; the browser only ever lists one folder
    // deep, a change in a subfolder that isn't itself shown doesn't need to
    // trigger a refresh.
    HANDLE h = FindFirstChangeNotificationW(wdir.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE)
        return;
    m_change_handle = h;
}

void DirWatcher::Reset()
{
    if (m_change_handle) {
        FindCloseChangeNotification((HANDLE)m_change_handle);
        m_change_handle = nullptr;
    }
}

bool DirWatcher::PollChanged()
{
    if (!m_change_handle)
        return false;
    if (WaitForSingleObject((HANDLE)m_change_handle, 0) != WAIT_OBJECT_0)
        return false;
    // Must re-arm after every signalled wait, or this handle would never
    // signal again - unlike inotify/kqueue, it's edge-triggered exactly
    // once until explicitly told to watch for the next change.
    FindNextChangeNotification((HANDLE)m_change_handle);
    return true;
}

#elif defined(_MAC)

// ---------------------------------------------------------------------------
// macOS: kqueue + EVFILT_VNODE on an open directory fd. FSEvents is the
// "usual" macOS API for this but expects a CFRunLoop pumping its callback,
// which this platform's plain SDL loop doesn't have; kqueue instead gives a
// pollable fd, so PollChanged() ends up the same shape as inotify above -
// kevent() with a zero timeout, once a frame, never blocking.
// ---------------------------------------------------------------------------

DirWatcher::~DirWatcher() { Reset(); }

DirWatcher::DirWatcher(DirWatcher&& other) noexcept
    : m_kqueue_fd(other.m_kqueue_fd), m_dir_fd(other.m_dir_fd)
{
    other.m_kqueue_fd = -1;
    other.m_dir_fd = -1;
}

DirWatcher& DirWatcher::operator=(DirWatcher&& other) noexcept
{
    if (this != &other) {
        Reset();
        m_kqueue_fd = other.m_kqueue_fd; other.m_kqueue_fd = -1;
        m_dir_fd = other.m_dir_fd; other.m_dir_fd = -1;
    }
    return *this;
}

void DirWatcher::Watch(const std::string& dir_utf8)
{
    Reset();
    // open()/kqueue() take plain bytes on macOS too - no re-encoding needed
    // the way Windows' wide-char API requires above.
    int fd = open(dir_utf8.c_str(), O_EVTONLY);
    if (fd < 0)
        return;
    int kq = kqueue();
    if (kq < 0) {
        close(fd);
        return;
    }
    struct kevent change;
    // NOTE_WRITE on a *directory* fd is what fires for entries being
    // added/removed/renamed inside it - the actual "listing changed"
    // signal. The rest cover the folder itself being renamed, deleted, or
    // having an unlink()'d-but-still-open link revoked out from under us;
    // RefreshEntries() notices any of those on its own via fs::exists().
    // EV_CLEAR re-arms the filter automatically after each delivery, so
    // (unlike the Windows branch) there's no separate re-arm call needed.
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
        NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE | NOTE_EXTEND | NOTE_ATTRIB,
        0, nullptr);
    struct timespec zero_timeout = { 0, 0 };
    if (kevent(kq, &change, 1, nullptr, 0, &zero_timeout) < 0) {
        close(kq);
        close(fd);
        return;
    }
    m_kqueue_fd = kq;
    m_dir_fd = fd;
}

void DirWatcher::Reset()
{
    if (m_kqueue_fd >= 0) { close(m_kqueue_fd); m_kqueue_fd = -1; }
    if (m_dir_fd >= 0) { close(m_dir_fd); m_dir_fd = -1; }
}

bool DirWatcher::PollChanged()
{
    if (m_kqueue_fd < 0)
        return false;
    struct kevent event;
    struct timespec zero_timeout = { 0, 0 };
    int n = kevent(m_kqueue_fd, nullptr, 0, &event, 1, &zero_timeout);
    return n > 0;
}

#else

// No native directory-change notification wired up for this platform -
// same harmless fallback as an OS watch failing to set up on a supported
// one above: the browser still works, it just never auto-refreshes.
DirWatcher::~DirWatcher() {}
DirWatcher::DirWatcher(DirWatcher&&) noexcept {}
DirWatcher& DirWatcher::operator=(DirWatcher&&) noexcept { return *this; }
void DirWatcher::Watch(const std::string&) {}
void DirWatcher::Reset() {}
bool DirWatcher::PollChanged() { return false; }

#endif

} // namespace xPlatform

#endif//USE_SDL2_DESKTOP
