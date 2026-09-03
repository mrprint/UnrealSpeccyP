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
#include "imgui_shared.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace xPlatform {
namespace xImGui {

namespace {

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

// fs::path::string() converts to the *native narrow* encoding - on Windows
// that's the local ANSI codepage, and the conversion throws
// std::system_error for any character that codepage can't represent (which
// includes plenty of legitimate Cyrillic filenames - very plausible for ZX
// Spectrum software). fs::path::u8string() instead re-encodes to UTF-8,
// which every native path (POSIX bytes or Windows UTF-16) can always be
// losslessly represented in, so it doesn't throw - and it's exactly the
// encoding Dear ImGui expects for text anyway (the font here already loads
// a Cyrillic glyph range). Falls back to a placeholder rather than
// propagating an exception on the off chance some other error still occurs.
std::string PathToUtf8(const fs::path& p) {
    try {
        return p.u8string();
    } catch (...) {
        return "<?>";
    }
}

// Mirror of PathToUtf8() for the opposite direction - constructing a path
// from UTF-8 text (path bar input, entry names, the save-mode filename
// field). Utf8ToPath() doesn't take an error_code overload and can in
// principle throw if handed a byte sequence that isn't valid UTF-8; every
// caller here sources its string from PathToUtf8() or Dear ImGui's own
// UTF-8 text buffers, so that shouldn't happen in practice, but there's no
// reason to let a directory listing crash the whole app over it either.
fs::path Utf8ToPath(const std::string& s) {
    try {
        return fs::u8path(s);
    } catch (...) {
        return fs::path();
    }
}

// ---------------------------------------------------------------------------
// FileDialog - owns every piece of the browser's state and every function
// that reads or writes it. Only one instance of this ever exists (g_dialog
// below); the point of the class isn't multiple browsers, it's that
// RefreshEntries()/NavigateTo()/Confirm()/MatchesFilter() can no longer
// accidentally read or write this state from outside the handful of methods
// that are supposed to touch it.
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
    fs::path m_current_dir;
    std::vector<FileDialogFilter> m_filters;
    int m_filter_index = 0; // index into m_filters; "All files" (if present) is just a normal
                             // entry in m_filters with an empty extension list, not a sentinel value
    std::function<void(const std::string&)> m_on_confirm;

    std::vector<Entry> m_entries;
    int m_selected = -1; // index into m_entries

    char m_path_buf[1024] = {};
    char m_name_buf[512] = {};   // save-mode only

    bool m_show_overwrite_confirm = false;
    bool m_overwrite_popup_pending_open = false; // OpenPopup() must fire exactly once per confirm request, not every frame - see Draw()
    std::string m_overwrite_path;

    std::string m_error_text;

    static bool MatchesFilter(const std::string& filename, const FileDialogFilter& filter);
    void RefreshEntries();
    void NavigateTo(const fs::path& dir);
    std::string CurrentFilterFirstExtension() const;
    void Confirm(const std::string& path);
};

bool FileDialog::MatchesFilter(const std::string& filename, const FileDialogFilter& filter) {
    if (filter.extensions.empty())
        return true; // "All files"-style entry
    std::string ext = ToLower(PathToUtf8(Utf8ToPath(filename).extension()));
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

    std::error_code ec;
    if (!fs::exists(m_current_dir, ec) || !fs::is_directory(m_current_dir, ec)) {
        m_error_text = "Cannot open this folder.";
        return;
    }

    const FileDialogFilter* active_filter = nullptr;
    if (!m_filters.empty() && m_filter_index >= 0 &&
        m_filter_index < (int)m_filters.size())
        active_filter = &m_filters[m_filter_index];

    // directory_iterator's constructor is given an error_code above and
    // won't throw for the initial open, but incrementing it (which the
    // range-for below does implicitly) has no such non-throwing overload -
    // it can still throw filesystem_error mid-listing (a file removed/
    // permissions changed while iterating, a broken symlink, ...). Both
    // this and PathToUtf8() failing were plausible causes of "crashes when
    // entering a folder" that a narrower fix wouldn't have caught, so the
    // whole listing is wrapped rather than trying to guard every call site
    // individually.
    try {
        for (const auto& de : fs::directory_iterator(m_current_dir, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code ec2;
            bool is_dir = de.is_directory(ec2);
            std::string name = PathToUtf8(de.path().filename());
            if (name.empty() || name[0] == '.')
                continue; // hide dotfiles/hidden entries, same spirit as a native picker's default view
            if (!is_dir && active_filter && !MatchesFilter(name, *active_filter))
                continue;
            m_entries.push_back({ name, is_dir });
        }
    } catch (const std::exception&) {
        m_error_text = "Error reading this folder's contents.";
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // dirs first
        return ToLower(a.name) < ToLower(b.name);
    });

    std::string dir_utf8 = PathToUtf8(m_current_dir);
    CopyToBuffer(m_path_buf, sizeof(m_path_buf), dir_utf8);
}

void FileDialog::NavigateTo(const fs::path& dir) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(dir, ec);
    m_current_dir = ec ? dir : canon;
    RefreshEntries();
}

std::string FileDialog::CurrentFilterFirstExtension() const {
    if (m_filters.empty()) return "";
    if (m_filter_index < 0 || m_filter_index >= (int)m_filters.size()) return "";
    const auto& exts = m_filters[m_filter_index].extensions;
    return exts.empty() ? "" : exts[0];
}

void FileDialog::Confirm(const std::string& path) {
    auto cb = m_on_confirm;
    m_open = false;
    m_show_overwrite_confirm = false;
    if (cb) cb(path);
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

    std::error_code ec;
    fs::path dir = start_dir.empty() ? fs::current_path(ec) : Utf8ToPath(start_dir);
    if (dir.empty() || !fs::exists(dir, ec))
        dir = fs::current_path(ec);

    if (save_mode) {
        CopyToBuffer(m_name_buf, sizeof(m_name_buf), default_name);
    }

    NavigateTo(dir);
}

void FileDialog::Draw() {
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_FirstUseEver);
    bool open = m_open;
    if (!ImGui::Begin(m_title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        m_open = open;
        return;
    }

    // --- path bar ---
    if (ImGui::Button("Up")) {
        fs::path parent = m_current_dir.parent_path();
        if (!parent.empty())
            NavigateTo(parent);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", m_path_buf, sizeof(m_path_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        // m_path_buf holds UTF-8 (it's what PathToUtf8() wrote into it, and
        // what Dear ImGui's InputText itself works in) - fs::path's own
        // narrow-string constructor instead assumes the *native* encoding
        // (ANSI codepage on Windows), so it needs the explicit UTF-8
        // constructor to round-trip correctly for non-ASCII paths.
        NavigateTo(Utf8ToPath(m_path_buf));
    }

    // --- filter combo ---
    if (!m_filters.empty()) {
        std::string preview = m_filter_index >= 0 && m_filter_index < (int)m_filters.size()
            ? m_filters[m_filter_index].label : "All files";
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##filter", preview.c_str())) {
            for (int i = 0; i < (int)m_filters.size(); ++i) {
                bool sel = (i == m_filter_index);
                if (ImGui::Selectable(m_filters[i].label.c_str(), sel)) {
                    m_filter_index = i;
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
    // us mid-loop) and was the actual cause of the crash: entering a
    // directory reliably hit exactly this path. Instead, just record what
    // the user asked for here, and act on it once after the loop (and after
    // EndChild()) has finished touching m_entries for this frame.
    fs::path pending_navigate;
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
                    // e.name is UTF-8 (from PathToUtf8() in RefreshEntries) -
                    // same u8path() reasoning as the path bar above.
                    pending_navigate = Utf8ToPath(e.name);
                    pending_navigate = m_current_dir / pending_navigate;
                    has_pending_navigate = true;
                } else if (!m_save_mode) {
                    pending_confirm = PathToUtf8(m_current_dir / Utf8ToPath(e.name));
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
    if (ImGui::Button(m_save_mode ? "Save" : "Open")) {
        do_confirm = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_open = false;
    }

    if (do_confirm) {
        if (m_save_mode) {
            // m_name_buf is UTF-8 (Dear ImGui's InputText works in UTF-8) -
            // same u8path() reasoning as elsewhere in this file.
            std::string name = m_name_buf;
            if (!name.empty()) {
                std::string ext = ToLower(PathToUtf8(Utf8ToPath(name).extension()));
                if (ext.empty() || ext == ".") {
                    std::string default_ext = CurrentFilterFirstExtension();
                    if (!default_ext.empty())
                        name += "." + default_ext;
                }
                fs::path full = m_current_dir / Utf8ToPath(name);
                std::error_code exists_ec;
                if (fs::exists(full, exists_ec)) {
                    m_show_overwrite_confirm = true;
                    m_overwrite_popup_pending_open = true;
                    m_overwrite_path = PathToUtf8(full);
                } else {
                    Confirm(PathToUtf8(full));
                }
            }
        } else if (m_selected >= 0 && m_selected < (int)m_entries.size() &&
                   !m_entries[m_selected].is_dir) {
            Confirm(PathToUtf8(m_current_dir / Utf8ToPath(m_entries[m_selected].name)));
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
        ImGui::OpenPopup("Overwrite file?");
        m_overwrite_popup_pending_open = false;
    }
    if (m_show_overwrite_confirm) {
        // Modal-in-the-same-frame overlay - still non-native, still no OS
        // event loop, still safe alongside the single-threaded main loop.
        bool popup_still_open = true;
        if (ImGui::BeginPopupModal("Overwrite file?", &popup_still_open, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("A file with this name already exists. Overwrite it?");
            ImGui::Spacing();
            if (ImGui::Button("Overwrite")) {
                ImGui::CloseCurrentPopup();
                m_show_overwrite_confirm = false;
                Confirm(m_overwrite_path);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
                m_show_overwrite_confirm = false;
            }
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
