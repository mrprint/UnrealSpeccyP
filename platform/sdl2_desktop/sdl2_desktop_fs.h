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
//  platform/sdl2_desktop/sdl2_desktop_fs.h
//
//  Windows 7-compatible filesystem helpers. Replaces std::filesystem, which on
//  MSVC uses CreateFile2 (Windows 8+).
//
//  Every string here is UTF-8, on every platform. On Windows it is converted to
//  UTF-16 at the boundary and the wide ("W") API is called - NOT the ANSI one:
//  the "UTF-8 active code page" of the manifest only exists from Windows 10
//  1903 on, and on Windows 7 an ANSI call turns a name the code page cannot
//  represent (Cyrillic on a non-Cyrillic system, say) into '?' for good.
//
//  A path class for path manipulation and free functions for filesystem
//  operations, following std::filesystem naming conventions.
// =============================================================================

#ifndef SDL2_DESKTOP_FS_H
#define SDL2_DESKTOP_FS_H

#include <string>
#include <vector>
#include <algorithm>

#ifdef _WINAPI
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#endif

namespace xPlatform
{
namespace xFs
{

// ---------------------------------------------------------------------------
// Path string logic. Pure functions on UTF-8 strings, no filesystem access;
// `win` selects the Windows rules (both '\' and '/' separate, drive letters
// and UNC roots) - they take it as an argument so the rules of the other
// platform can be tested on any one.
// ---------------------------------------------------------------------------

namespace detail
{

#ifdef _WINAPI
const bool kWin = true;
#else
const bool kWin = false;
#endif

inline bool IsSep(char c, bool win) { return c == '/' || (win && c == '\\'); }

// Length of the root part: "/" (1), "C:" (2), "C:\" (3), "\\server\share\", or 0.
inline size_t RootLength(const std::string& s, bool win)
{
    if (win)
    {
        if (s.size() >= 2 && s[1] == ':')
            return (s.size() >= 3 && IsSep(s[2], win)) ? 3 : 2;
        if (s.size() >= 2 && IsSep(s[0], win) && IsSep(s[1], win)) // \\server\share\ ...
        {
            size_t p = s.find_first_of("\\/", 2);       // end of the server name
            if (p == std::string::npos)
                return s.size();
            p = s.find_first_of("\\/", p + 1);          // end of the share name
            return p == std::string::npos ? s.size() : p + 1;
        }
    }
    return (!s.empty() && IsSep(s[0], win)) ? 1 : 0;
}

inline size_t LastSep(const std::string& s, bool win)
{
    for (size_t i = s.size(); i-- > 0;)
        if (IsSep(s[i], win))
            return i;
    return std::string::npos;
}

inline std::string Filename(const std::string& s, bool win)
{
    size_t pos = LastSep(s, win);
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

// ".tap" of "a.tap"; empty for "a", ".hidden", "." and ".." (as std::filesystem does).
inline std::string Extension(const std::string& s, bool win)
{
    std::string name = Filename(s, win);
    if (name == "." || name == "..")
        return "";
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos || pos == 0)
        return "";
    return name.substr(pos);
}

inline std::string Stem(const std::string& s, bool win)
{
    std::string name = Filename(s, win);
    if (name == "." || name == "..")
        return name;
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos || pos == 0)
        return name;
    return name.substr(0, pos);
}

// The directory above. A trailing separator does not count ("C:\a\" -> "C:\"),
// a root is its own parent ("C:\" -> "C:\", "/" -> "/"), and "C:\a" is under
// "C:\" - never under "C:", which on Windows is the current directory of C:.
// A bare name has no parent: "".
inline std::string Parent(const std::string& path, bool win)
{
    std::string s = path;
    const size_t root = RootLength(s, win);
    while (s.size() > root && IsSep(s.back(), win))
        s.pop_back();
    if (s.size() <= root)
        return s;
    size_t pos = LastSep(s, win);
    if (pos == std::string::npos)
        return root ? s.substr(0, root) : std::string();
    if (pos < root)
        return s.substr(0, root);
    return s.substr(0, pos);
}

// a / b. An absolute b replaces a, an empty a gives b.
inline std::string Join(const std::string& a, const std::string& b, bool win)
{
    if (a.empty() || RootLength(b, win) > 0)
        return b;
    std::string r = a;
    if (!IsSep(r.back(), win) && !(win && r.size() == 2 && r[1] == ':'))
        r += win ? '\\' : '/';
    return r + b;
}

// Removes "." and "..", repeated and trailing separators. No filesystem access,
// so a symlink is not resolved: "a/link/.." is "a". On Windows the separator
// of the result is '\'.
inline std::string Normalize(const std::string& path, bool win)
{
    const size_t root_len = RootLength(path, win);
    std::string root = path.substr(0, root_len);
    if (win)
        std::replace(root.begin(), root.end(), '/', '\\');
    std::vector<std::string> parts;
    size_t i = root_len;
    while (i <= path.size())
    {
        size_t j = i;
        while (j < path.size() && !IsSep(path[j], win))
            ++j;
        std::string seg = path.substr(i, j - i);
        if (seg == "..")
        {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (root_len == 0)
                parts.push_back(seg);                    // nothing above a relative path to drop
            // else: above a root there is nothing - stay at the root
        }
        else if (!seg.empty() && seg != ".")
            parts.push_back(seg);
        i = j + 1;
    }
    std::string r = root;
    for (size_t k = 0; k < parts.size(); ++k)
    {
        if (k > 0 || (!r.empty() && !IsSep(r.back(), win) && !(win && r.size() == 2 && r[1] == ':')))
            r += win ? '\\' : '/';
        r += parts[k];
    }
    return r.empty() ? std::string(".") : r;
}

#ifdef _WINAPI

// UTF-8 -> UTF-16. Empty for an empty string and for malformed UTF-8 (so that
// nothing is opened under a name that is not the one that was asked for).
inline std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string Narrow(const wchar_t* w, size_t len)
{
    if (len == 0)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
    return s;
}

inline std::string Narrow(const std::wstring& w) { return Narrow(w.data(), w.size()); }

// An empty drive (optical, card reader) makes Windows put up a modal "There is no
// disk in the drive" box for any call that touches it. For the calls of this
// thread, while this object lives, it just fails.
struct NoCriticalErrors
{
    DWORD m_old = 0;
    NoCriticalErrors() { SetThreadErrorMode(SEM_FAILCRITICALERRORS, &m_old); }
    ~NoCriticalErrors() { SetThreadErrorMode(m_old, nullptr); }
};

#endif//_WINAPI

} // namespace detail

// ---------------------------------------------------------------------------
// path - lightweight path class for platform-independent path manipulation.
// Follows std::filesystem::path conventions for method names.
// ---------------------------------------------------------------------------

class path
{
public:
    path() = default;
    path(const std::string& p) : m_path(p) {}
    path(const char* p) : m_path(p) {}

    // Convert to string
    explicit operator std::string() const { return m_path; }
    const std::string& string() const { return m_path; }
    const char* c_str() const { return m_path.c_str(); }

    // Path components
    std::string filename() const { return detail::Filename(m_path, detail::kWin); }
    std::string extension() const { return detail::Extension(m_path, detail::kWin); }
    path parent_path() const { return path(detail::Parent(m_path, detail::kWin)); }
    path stem() const { return path(detail::Stem(m_path, detail::kWin)); }

    // Path predicates
    bool empty() const { return m_path.empty(); }

    // Path concatenation
    path operator/(const path& other) const { return path(detail::Join(m_path, other.m_path, detail::kWin)); }
    path& operator/=(const path& other) { *this = *this / other; return *this; }

    // Comparison
    bool operator==(const path& other) const { return m_path == other.m_path; }
    bool operator!=(const path& other) const { return m_path != other.m_path; }

private:
    std::string m_path;
};

// ---------------------------------------------------------------------------
// Free functions - filesystem operations.
// Follow std::filesystem naming conventions.
// ---------------------------------------------------------------------------

// Lexical normalization (no filesystem access): "." and ".." are folded away,
// so are repeated and trailing separators. What std::filesystem's
// weakly_canonical() gave the file dialog, minus resolving symlinks.
inline path normalize(const path& p)
{
    return path(detail::Normalize(p.string(), detail::kWin));
}

// Check if path exists (file or directory)
inline bool exists(const path& p)
{
#ifdef _WINAPI
    detail::NoCriticalErrors guard;
    DWORD attrs = GetFileAttributesW(detail::Widen(p.string()).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

// Check if path is a directory
inline bool is_directory(const path& p)
{
#ifdef _WINAPI
    detail::NoCriticalErrors guard;
    DWORD attrs = GetFileAttributesW(detail::Widen(p.string()).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
#endif
}

// List directory contents
// Returns false if directory doesn't exist or can't be read
inline bool list_directory(const path& dir, std::vector<std::string>& out_files, std::vector<std::string>& out_dirs)
{
    out_files.clear();
    out_dirs.clear();

#ifdef _WINAPI
    detail::NoCriticalErrors guard;
    std::wstring pattern = detail::Widen(dir.string());
    if (pattern.empty())
        return false;
    if (pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        std::wstring wname = ffd.cFileName;
        if (wname == L"." || wname == L"..")
            continue;
        std::string name = detail::Narrow(wname);
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            out_dirs.push_back(name);
        else
            out_files.push_back(name);
    } while (FindNextFileW(h, &ffd) != 0);

    FindClose(h);
    return true;
#else
    DIR* d = opendir(dir.c_str());
    if (!d)
        return false;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        path full_path = dir / name;
        if (is_directory(full_path))
            out_dirs.push_back(name);
        else
            out_files.push_back(name);
    }

    closedir(d);
    return true;
#endif
}

// Get current working directory
inline path current_path()
{
#ifdef _WINAPI
    DWORD n = GetCurrentDirectoryW(0, nullptr);          // with the terminating zero
    if (n == 0)
        return path(".");
    std::wstring w((size_t)n, L'\0');
    n = GetCurrentDirectoryW((DWORD)w.size(), &w[0]);    // without it
    if (n == 0 || n >= w.size())
        return path(".");
    return path(detail::Narrow(w.data(), (size_t)n));
#else
    char buf[4096];
    if (getcwd(buf, sizeof(buf)))
        return path(buf);
    return path(".");
#endif
}

// Check if path is a drive root (Windows only)
inline bool is_drive_root(const path& p)
{
#ifdef _WINAPI
    const std::string& s = p.string();
    return s.size() == 3 && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
#else
    (void)p;
    return false;
#endif
}

// Get list of drive roots (Windows only)
inline std::vector<path> drive_roots()
{
    std::vector<path> roots;
#ifdef _WINAPI
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (mask & (1u << i))
        {
            char drive[3] = { char('A' + i), ':', '\\' };
            roots.push_back(path(std::string(drive, 3)));
        }
    }
#endif
    return roots;
}

} // namespace xFs
} // namespace xPlatform

#endif // SDL2_DESKTOP_FS_H
