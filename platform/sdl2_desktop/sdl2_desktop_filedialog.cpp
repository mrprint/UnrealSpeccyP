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

#include "sdl2_desktop_filedialog.h"
#include "sdl2_desktop_dirwatch.h"
#include "sdl2_desktop_fs.h"
#include "imgui_shared.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace xPlatform {
namespace xImGui {

namespace {

// Every path here is an xFs::path: a plain UTF-8 string - the encoding Dear ImGui works in, and the one
// the callers of the browser use - which sdl2_desktop_fs.h converts to the native one only at the
// operating system's door. Nothing below depends on the system code page (Windows 7 has no UTF-8 one).
using xFs::path;
using xFs::exists;
using xFs::is_directory;
using xFs::list_directory;
using xFs::current_path;
using xFs::is_drive_root;
using xFs::drive_roots;
using xFs::normalize;

std::string ToLower(std::string s) {
    // Only fold plain ASCII letters. std::tolower(unsigned char) is only
    // well-defined for values the current locale's "C"/basic character set
    // covers; applying it byte-by-byte to a UTF-8 string would feed it the
    // individual continuation/lead bytes of any multi-byte character
    // (Cyrillic filenames, in particular, are exactly what this is used to
    // sort and filter) with no defined, correct result. Leaving those bytes
    // untouched keeps the sort merely case-sensitive for non-ASCII names
    // instead of silently mangling them.
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (c < 0x80) ? (char)std::tolower(c) : (char)c;
    });
    return s;
}

// ---------------------------------------------------------------------------
// FileDialog - owns every piece of the browser's state and every function
// that reads or writes it. Only one instance of this ever exists (g_dialog
// below); the point of the class isn't multiple browsers, it's that
// RefreshEntries()/NavigateTo()/Confirm()/MatchesFilter() stay the only
// places that touch this state.
// ---------------------------------------------------------------------------

class FileDialog {
public:
    // Opens the browser overlay (closes any previous instance) - see
    // OpenFileBrowser() in the header for the parameter contract.
    void Open(const std::string& title, const std::string& start_dir,
        std::vector<FileDialogFilter> filters, bool save_mode,
        const std::string& default_name,
        std::function<void(const std::string&)> on_confirm);

    // Draws the browser if one is open. No-op if nothing is open.
    void Draw();

    bool IsActive() const { return m_open; }

private:
    struct Entry {
        std::string name;
        bool is_dir;
    };

    bool m_open = false;
    bool m_save_mode = false;
    std::string m_title;
    path m_current_dir;
    std::vector<FileDialogFilter> m_filters;
    int m_filter_index = 0; // index into m_filters; "All files" (if present) is just a normal
                             // entry in m_filters with an empty extension list, not a sentinel value
    std::function<void(const std::string&)> m_on_confirm;

    std::vector<Entry> m_entries;
    int m_selected = -1; // index into m_entries

    // Windows-only virtual level above every real drive root, listing drive
    // letters as pseudo-directories (m_entries filled by ShowDriveList()
    // rather than RefreshEntries() while this is true) - a path has
    // no notion of "My Computer"/"This PC" above "C:\" and friends, so Up
    // from a drive root needs somewhere to go that isn't just a no-op. See
    // GoUp(). m_current_dir is empty while this is true - nothing else may
    // read it in that state (NavigateTo() clears the flag again before it
    // sets a real m_current_dir).
    bool m_showing_drives = false;

    char m_path_buf[1024] = {};
    char m_name_buf[512] = {};   // save-mode only

    // Auto-refreshes m_entries when something outside this process changes
    // m_current_dir - see Watch() call in NavigateTo() and the PollChanged()
    // check at the top of Draw().
    DirWatcher m_watcher;
    // Set from the path-bar InputText's own ImGui::IsItemActive() at the end
    // of every Draw(); read back at the *start* of the next Draw() to skip
    // that frame's auto-refresh while the user still has the path bar
    // focused, so a background change can't clobber text they're mid-typing
    // (RefreshEntries() below always overwrites m_path_buf from
    // m_current_dir).
    bool m_path_bar_was_active = false;

    bool m_show_overwrite_confirm = false;
    bool m_overwrite_popup_pending_open = false; // OpenPopup() must fire exactly once per confirm request, not every frame - see Draw()
    std::string m_overwrite_path;

    std::string m_error_text;

    static bool MatchesFilter(const std::string& filename, const FileDialogFilter& filter);
    void RefreshEntries();
    void NavigateTo(const path& dir);
    void GoUp(); // "Up" button - handles the drive-root -> virtual drive list step itself
    void GoRoot(); // "Root" button - jumps straight to the top in one click
#ifdef _WINAPI
    void ShowDriveList(); // populates m_entries with drive letters - see m_showing_drives
#endif
    std::string CurrentFilterFirstExtension() const;
    void Confirm(const std::string& p);
};

bool FileDialog::MatchesFilter(const std::string& filename, const FileDialogFilter& filter) {
    if (filter.extensions.empty())
        return true; // "All files"-style entry
    std::string ext = ToLower(path(filename).extension());
    if (!ext.empty() && ext[0] == '.')
        ext = ext.substr(1);
    for (const auto& e : filter.extensions)
        if (ToLower(e) == ext)
            return true;
    return false;
}

void FileDialog::RefreshEntries() {
    m_entries.clear();
    m_selected = -1;
    m_error_text.clear();

    if (!exists(m_current_dir) || !is_directory(m_current_dir)) {
        m_error_text = Tr("filedialog.error.cannot_open");
        return;
    }

    const FileDialogFilter* active_filter = nullptr;
    if (!m_filters.empty() && m_filter_index >= 0 &&
        m_filter_index < (int)m_filters.size())
        active_filter = &m_filters[m_filter_index];

    // list_directory() hands out the names already in UTF-8 and never throws: a folder that cannot be
    // read is just `false` (a file removed or a permission changed mid-listing is skipped, not fatal).
    std::vector<std::string> files, dirs;
    if (!list_directory(m_current_dir, files, dirs)) {
        m_error_text = Tr("filedialog.error.read_failed");
        return;
    }
    // Hide dotfiles/hidden entries, same spirit as a native picker's default view.
    for (const auto& name : dirs) {
        if (!name.empty() && name[0] != '.')
            m_entries.push_back({ name, true });
    }
    for (const auto& name : files) {
        if (name.empty() || name[0] == '.')
            continue;
        if (active_filter && !MatchesFilter(name, *active_filter))
            continue;
        m_entries.push_back({ name, false });
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // dirs first
        return ToLower(a.name) < ToLower(b.name);
    });

    CopyToBuffer(m_path_buf, sizeof(m_path_buf), m_current_dir.string());
}

void FileDialog::NavigateTo(const path& dir) {
    m_showing_drives = false; // always navigating to a real, concrete directory
    // Lexical only: the "." and ".." of what was typed into the path bar, repeated and trailing
    // separators. Unlike weakly_canonical() it does not resolve symlinks.
    m_current_dir = normalize(dir);
    RefreshEntries();
    // (Re)start watching whatever we actually ended up showing - harmless,
    // and cheap, even if it's the same directory as before (Watch() just
    // tears down and re-creates the native watch).
    m_watcher.Watch(m_current_dir.string());
}

void FileDialog::GoUp() {
    if (m_showing_drives)
        return; // already at the top - nothing above the virtual drive list
#ifdef _WINAPI
    // A drive root (e.g. "C:\") has no parent of its own (parent_path() of
    // a root just returns itself) - is_drive_root() detects that directly
    // rather than relying on that self-referential quirk. Surface one more,
    // virtual level here (mirroring Explorer's "This PC") instead of Up
    // silently doing nothing once you reach a drive's root.
    if (is_drive_root(m_current_dir)) {
        ShowDriveList();
        return;
    }
#endif
    path parent = m_current_dir.parent_path();
    if (!parent.empty() && parent != m_current_dir)
        NavigateTo(parent);
}

void FileDialog::GoRoot() {
    if (m_showing_drives)
        return; // already at the top
#ifdef _WINAPI
    // No single "/" on Windows - the nearest equivalent to "jump to the top
    // in one click" is the virtual drive list itself (see ShowDriveList()),
    // the same place repeatedly pressing Up would eventually land on.
    ShowDriveList();
#else
    NavigateTo(path("/"));
#endif
}

#ifdef _WINAPI
void FileDialog::ShowDriveList() {
    m_showing_drives = true;
    m_current_dir = path();
    m_selected = -1;
    m_error_text.clear();
    m_entries.clear();
    m_watcher.Reset(); // nothing real to watch at this virtual level

    // drive_roots() is GetLogicalDrives() underneath - doesn't distinguish an
    // empty optical drive from a populated one, but neither does Explorer's
    // "This PC" list, and RefreshEntries()'s exists()/list_directory() handling
    // already copes fine with a drive that turns out to be inaccessible once
    // actually entered (the sdl2_desktop_fs.h calls fail quietly, without
    // Windows putting up its "no disk in the drive" box).
    for (const auto& root : drive_roots())
        m_entries.push_back({ root.string(), true });
    // Already in A -> Z order - no sort needed.

    // Cosmetic only - m_showing_drives (not this text) is what the rest of
    // the class actually checks.
    CopyToBuffer(m_path_buf, sizeof(m_path_buf), Tr("filedialog.this_pc"));
}
#endif//_WINAPI

std::string FileDialog::CurrentFilterFirstExtension() const {
    if (m_filters.empty()) return "";
    if (m_filter_index < 0 || m_filter_index >= (int)m_filters.size()) return "";
    const auto& exts = m_filters[m_filter_index].extensions;
    return exts.empty() ? "" : exts[0];
}

void FileDialog::Confirm(const std::string& p) {
    auto cb = m_on_confirm;
    m_open = false;
    m_show_overwrite_confirm = false;
    if (cb) cb(p);
}

void FileDialog::Open(const std::string& title, const std::string& start_dir,
    std::vector<FileDialogFilter> filters, bool save_mode,
    const std::string& default_name,
    std::function<void(const std::string&)> on_confirm) {

    // Reset every field to its default before applying the new request.
    *this = FileDialog();
    m_open = true;
    m_save_mode = save_mode;
    m_title = title;
    m_filters = std::move(filters);
    m_filter_index = 0;
    m_on_confirm = std::move(on_confirm);

    path dir = start_dir.empty() ? current_path() : path(start_dir);
    if (dir.empty() || !exists(dir))
        dir = current_path();

    if (save_mode) {
        CopyToBuffer(m_name_buf, sizeof(m_name_buf), default_name);
    }

    NavigateTo(dir);
}

void FileDialog::Draw() {
    if (!m_open)
        return;

    // Pick up changes made outside this process (another app touching the
    // folder we're showing) before drawing anything else this frame - see
    // m_path_bar_was_active's comment for why this is skipped while the
    // path bar is focused.
    if (!m_path_bar_was_active && m_watcher.PollChanged())
        RefreshEntries();

    ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_FirstUseEver);
    bool open = m_open;
    if (!ImGui::Begin(m_title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        m_open = open;
        return;
    }

    // --- path bar ---
    if (!m_showing_drives) {
        if (ImGui::Button(Tr("filedialog.up")))
            GoUp();
        ItemTooltip("tip.filedialog.up");
        ImGui::SameLine();
        if (ImGui::Button(Tr("filedialog.root")))
            GoRoot();
        ItemTooltip("tip.filedialog.root");
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", m_path_buf, sizeof(m_path_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        // m_path_buf holds UTF-8 (it's what RefreshEntries() wrote into it,
        // and what Dear ImGui's InputText itself works in) - and so does
        // xFs::path, so it goes in as it is.
        NavigateTo(path(m_path_buf));
    }
    m_path_bar_was_active = ImGui::IsItemActive();

    // --- filter combo ---
    if (!m_filters.empty()) {
        std::string preview = m_filter_index >= 0 && m_filter_index < (int)m_filters.size()
            ? m_filters[m_filter_index].label : Tr("filedialog.filter.all");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##filter", preview.c_str())) {
            for (int i = 0; i < (int)m_filters.size(); ++i) {
                bool sel = (i == m_filter_index);
                if (ImGui::Selectable(m_filters[i].label.c_str(), sel)) {
                    m_filter_index = i;
                    // Filters only affect real directory listings - the
                    // virtual drive list has no files to filter, and
                    // RefreshEntries() would just misread the empty
                    // m_current_dir as "can't open this folder" and wipe it.
                    if (!m_showing_drives)
                        RefreshEntries();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // --- entry list ---
    float footer_h = m_save_mode ? 76.0f : 44.0f;
    ImGui::BeginChild("##entries", ImVec2(-1.0f, -footer_h), true);
    if (!m_error_text.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", m_error_text.c_str());
    }
    // NavigateTo()/Confirm() below both end up rewriting m_entries (via
    // RefreshEntries()) - calling either one *while* this loop is still
    // iterating over m_entries and holding `e` as a reference into it is
    // undefined behaviour (the vector's old storage can be freed from under
    // us mid-loop). Instead, just record what the user asked for here, and
    // act on it once after the loop (and after EndChild()) has finished
    // touching m_entries for this frame.
    path pending_navigate;
    bool has_pending_navigate = false;
    std::string pending_confirm;
    bool has_pending_confirm = false;
    for (int i = 0; i < (int)m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        std::string label = (e.is_dir ? "[dir] " : "       ") + e.name;
        bool selected = (m_selected == i);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selected = i;
            if (!e.is_dir && m_save_mode) {
                CopyToBuffer(m_name_buf, sizeof(m_name_buf), e.name);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.is_dir) {
                    // e.name is UTF-8 (from list_directory() in RefreshEntries) -
                    // same reasoning as the path bar above. While showing the
                    // virtual drive list, e.name ("C:\\" etc.) is already an
                    // absolute root on its own, not something to join under
                    // m_current_dir (which is empty there).
                    pending_navigate = path(e.name);
                    if (!m_showing_drives)
                        pending_navigate = m_current_dir / pending_navigate;
                    has_pending_navigate = true;
                } else if (!m_save_mode) {
                    pending_confirm = (m_current_dir / e.name).string();
                    has_pending_confirm = true;
                }
            }
        }
    }
    ImGui::EndChild();

    // Safe now: the loop above is done with m_entries for this frame.
    if (has_pending_navigate)
        NavigateTo(pending_navigate);
    else if (has_pending_confirm)
        Confirm(pending_confirm);

    // --- save-mode filename field ---
    if (m_save_mode) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##filename", m_name_buf, sizeof(m_name_buf));
    }

    // --- buttons ---
    bool do_confirm = false;
    if (ImGui::Button(m_save_mode ? Tr("filedialog.save") : Tr("filedialog.open"))) {
        do_confirm = true;
    }
    ItemTooltip(m_save_mode ? "tip.filedialog.save" : "tip.filedialog.open");
    ImGui::SameLine();
    if (ImGui::Button(Tr("filedialog.cancel"))) {
        m_open = false;
    }
    ItemTooltip("tip.filedialog.cancel");

    if (do_confirm) {
        if (m_save_mode) {
            // m_name_buf is UTF-8 (Dear ImGui's InputText works in UTF-8) -
            // as every path in this file is. Nothing to
            // save into yet while m_current_dir is the empty, virtual drive
            // list - m_current_dir / name would silently produce a bogus
            // *relative* path instead.
            std::string name = m_name_buf;
            if (!name.empty() && !m_showing_drives) {
                std::string ext = ToLower(path(name).extension());
                if (ext.empty() || ext == ".") {
                    std::string default_ext = CurrentFilterFirstExtension();
                    if (!default_ext.empty())
                        name += "." + default_ext;
                }
                path full = m_current_dir / name;
                if (exists(full)) {
                    m_show_overwrite_confirm = true;
                    m_overwrite_popup_pending_open = true;
                    m_overwrite_path = full.string();
                } else {
                    Confirm(full.string());
                }
            }
        } else if (m_selected >= 0 && m_selected < (int)m_entries.size() &&
                   !m_entries[m_selected].is_dir) {
            Confirm((m_current_dir / m_entries[m_selected].name).string());
        }
    }

    ImGui::End();
    m_open = m_open && open;

    // OpenPopup() must only fire on the frame the request was made - calling
    // it unconditionally every frame while m_show_overwrite_confirm is true
    // fights ImGui's own popup-close
    // handling: Escape/click-outside/the window's own close button all work
    // by making BeginPopupModal() stop returning true, but with OpenPopup()
    // re-armed every single frame regardless, the very next frame just
    // reopens it immediately - the popup could only ever be dismissed via
    // its own Cancel button.
    if (m_overwrite_popup_pending_open) {
        ImGui::OpenPopup(TrTitle("filedialog.overwrite_title", "OverwriteFile").c_str());
        m_overwrite_popup_pending_open = false;
    }
    if (m_show_overwrite_confirm) {
        // Modal-in-the-same-frame overlay - still non-native, still no OS
        // event loop, still safe alongside the single-threaded main loop.
        bool popup_still_open = true;
        if (ImGui::BeginPopupModal(TrTitle("filedialog.overwrite_title", "OverwriteFile").c_str(), &popup_still_open, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", Tr("filedialog.overwrite_body"));
            ImGui::Spacing();
            if (ImGui::Button(Tr("filedialog.overwrite"))) {
                ImGui::CloseCurrentPopup();
                m_show_overwrite_confirm = false;
                Confirm(m_overwrite_path);
            }
            ItemTooltip("tip.filedialog.overwrite");
            ImGui::SameLine();
            if (ImGui::Button(Tr("filedialog.cancel"))) {
                ImGui::CloseCurrentPopup();
                m_show_overwrite_confirm = false;
            }
            ItemTooltip("tip.filedialog.cancel");
            ImGui::EndPopup();
        }
        // popup_still_open goes false when ImGui closed it on its own -
        // Escape, a click outside, or the titlebar close button - keep our
        // own state in sync so it doesn't spuriously reopen next frame.
        if (!popup_still_open)
            m_show_overwrite_confirm = false;
    }
}

FileDialog g_dialog;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API - thin facades over the one FileDialog instance above.
// ---------------------------------------------------------------------------

void OpenFileBrowser(const std::string& title, const std::string& start_dir,
    std::vector<FileDialogFilter> filters, bool save_mode,
    const std::string& default_name,
    std::function<void(const std::string& path)> on_confirm) {
    g_dialog.Open(title, start_dir, std::move(filters), save_mode, default_name, std::move(on_confirm));
}

bool FileBrowserActive() { return g_dialog.IsActive(); }

void DrawFileBrowser() { g_dialog.Draw(); }

}//namespace xImGui
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
