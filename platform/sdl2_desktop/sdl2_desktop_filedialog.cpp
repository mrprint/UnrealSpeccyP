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
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace xPlatform {
namespace xImGui {

namespace {

struct Entry {
    std::string name;
    bool is_dir;
};

struct BrowserState {
    bool open = false;
    bool save_mode = false;
    std::string title;
    fs::path current_dir;
    std::vector<FileDialogFilter> filters;
    int filter_index = 0; // index into filters; "All files" (if present) is just a normal
                           // entry in filters with an empty extension list, not a sentinel value
    std::function<void(const std::string&)> on_confirm;

    std::vector<Entry> entries;
    int selected = -1; // index into entries

    char path_buf[1024] = {};
    char name_buf[512] = {};   // save_mode only

    bool show_overwrite_confirm = false;
    bool overwrite_popup_pending_open = false; // OpenPopup() must fire exactly once per confirm request, not every frame - see DrawFileBrowser()
    std::string overwrite_path;

    std::string error_text;
};

BrowserState g_state;

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

bool MatchesFilter(const std::string& filename, const FileDialogFilter& filter) {
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

void RefreshEntries() {
    g_state.entries.clear();
    g_state.selected = -1;
    g_state.error_text.clear();

    std::error_code ec;
    if (!fs::exists(g_state.current_dir, ec) || !fs::is_directory(g_state.current_dir, ec)) {
        g_state.error_text = "Cannot open this folder.";
        return;
    }

    const FileDialogFilter* active_filter = nullptr;
    if (!g_state.filters.empty() && g_state.filter_index >= 0 &&
        g_state.filter_index < (int)g_state.filters.size())
        active_filter = &g_state.filters[g_state.filter_index];

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
        for (const auto& de : fs::directory_iterator(g_state.current_dir, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code ec2;
            bool is_dir = de.is_directory(ec2);
            std::string name = PathToUtf8(de.path().filename());
            if (name.empty() || name[0] == '.')
                continue; // hide dotfiles/hidden entries, same spirit as a native picker's default view
            if (!is_dir && active_filter && !MatchesFilter(name, *active_filter))
                continue;
            g_state.entries.push_back({ name, is_dir });
        }
    } catch (const std::exception&) {
        g_state.error_text = "Error reading this folder's contents.";
    }

    std::sort(g_state.entries.begin(), g_state.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // dirs first
        return ToLower(a.name) < ToLower(b.name);
    });

    std::string dir_utf8 = PathToUtf8(g_state.current_dir);
    strncpy(g_state.path_buf, dir_utf8.c_str(), sizeof(g_state.path_buf) - 1);
    g_state.path_buf[sizeof(g_state.path_buf) - 1] = 0;
}

void NavigateTo(const fs::path& dir) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(dir, ec);
    g_state.current_dir = ec ? dir : canon;
    RefreshEntries();
}

std::string CurrentFilterFirstExtension() {
    if (g_state.filters.empty()) return "";
    if (g_state.filter_index < 0 || g_state.filter_index >= (int)g_state.filters.size()) return "";
    const auto& exts = g_state.filters[g_state.filter_index].extensions;
    return exts.empty() ? "" : exts[0];
}

void Confirm(const std::string& path) {
    auto cb = g_state.on_confirm;
    g_state.open = false;
    g_state.show_overwrite_confirm = false;
    if (cb) cb(path);
}

} // anonymous namespace

void OpenFileBrowser(const std::string& title, const std::string& start_dir,
    std::vector<FileDialogFilter> filters, bool save_mode,
    const std::string& default_name,
    std::function<void(const std::string&)> on_confirm) {

    g_state = BrowserState();
    g_state.open = true;
    g_state.save_mode = save_mode;
    g_state.title = title;
    g_state.filters = std::move(filters);
    g_state.filter_index = 0;
    g_state.on_confirm = std::move(on_confirm);

    std::error_code ec;
    fs::path dir = start_dir.empty() ? fs::current_path(ec) : Utf8ToPath(start_dir);
    if (dir.empty() || !fs::exists(dir, ec))
        dir = fs::current_path(ec);

    if (save_mode) {
        strncpy(g_state.name_buf, default_name.c_str(), sizeof(g_state.name_buf) - 1);
        g_state.name_buf[sizeof(g_state.name_buf) - 1] = 0;
    }

    NavigateTo(dir);
}

bool FileBrowserActive() { return g_state.open; }

void DrawFileBrowser() {
    if (!g_state.open)
        return;

    ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_FirstUseEver);
    bool open = g_state.open;
    if (!ImGui::Begin(g_state.title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        g_state.open = open;
        return;
    }

    // --- path bar ---
    if (ImGui::Button("Up")) {
        fs::path parent = g_state.current_dir.parent_path();
        if (!parent.empty())
            NavigateTo(parent);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", g_state.path_buf, sizeof(g_state.path_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        // g_state.path_buf holds UTF-8 (it's what PathToUtf8() wrote into it,
        // and what Dear ImGui's InputText itself works in) - fs::path's own
        // narrow-string constructor instead assumes the *native* encoding
        // (ANSI codepage on Windows), so it needs the explicit UTF-8
        // constructor to round-trip correctly for non-ASCII paths.
        NavigateTo(Utf8ToPath(g_state.path_buf));
    }

    // --- filter combo ---
    if (!g_state.filters.empty()) {
        std::string preview = g_state.filter_index >= 0 && g_state.filter_index < (int)g_state.filters.size()
            ? g_state.filters[g_state.filter_index].label : "All files";
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##filter", preview.c_str())) {
            for (int i = 0; i < (int)g_state.filters.size(); ++i) {
                bool sel = (i == g_state.filter_index);
                if (ImGui::Selectable(g_state.filters[i].label.c_str(), sel)) {
                    g_state.filter_index = i;
                    RefreshEntries();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // --- entry list ---
    float footer_h = g_state.save_mode ? 76.0f : 44.0f;
    ImGui::BeginChild("##entries", ImVec2(-1.0f, -footer_h), true);
    if (!g_state.error_text.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", g_state.error_text.c_str());
    }
    // NavigateTo()/Confirm() below both end up rewriting g_state.entries
    // (via RefreshEntries()) - calling either one *while* this loop is still
    // iterating over g_state.entries and holding `e` as a reference into it
    // is undefined behaviour (the vector's old storage can be freed from
    // under us mid-loop) and was the actual cause of the crash: entering a
    // directory reliably hit exactly this path. Instead, just record what
    // the user asked for here, and act on it once after the loop (and after
    // EndChild()) has finished touching g_state.entries for this frame.
    fs::path pending_navigate;
    bool has_pending_navigate = false;
    std::string pending_confirm;
    bool has_pending_confirm = false;
    for (int i = 0; i < (int)g_state.entries.size(); ++i) {
        const Entry& e = g_state.entries[i];
        std::string label = (e.is_dir ? "[dir] " : "       ") + e.name;
        bool selected = (g_state.selected == i);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_state.selected = i;
            if (!e.is_dir && g_state.save_mode) {
                strncpy(g_state.name_buf, e.name.c_str(), sizeof(g_state.name_buf) - 1);
                g_state.name_buf[sizeof(g_state.name_buf) - 1] = 0;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.is_dir) {
                    // e.name is UTF-8 (from PathToUtf8() in RefreshEntries) -
                    // same u8path() reasoning as the path bar above.
                    pending_navigate = Utf8ToPath(e.name);
                    pending_navigate = g_state.current_dir / pending_navigate;
                    has_pending_navigate = true;
                } else if (!g_state.save_mode) {
                    pending_confirm = PathToUtf8(g_state.current_dir / Utf8ToPath(e.name));
                    has_pending_confirm = true;
                }
            }
        }
    }
    ImGui::EndChild();

    // Safe now: the loop above is done with g_state.entries for this frame.
    if (has_pending_navigate)
        NavigateTo(pending_navigate);
    else if (has_pending_confirm)
        Confirm(pending_confirm);

    // --- save-mode filename field ---
    if (g_state.save_mode) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##filename", g_state.name_buf, sizeof(g_state.name_buf));
    }

    // --- buttons ---
    bool do_confirm = false;
    if (ImGui::Button(g_state.save_mode ? "Save" : "Open")) {
        do_confirm = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        g_state.open = false;
    }

    if (do_confirm) {
        if (g_state.save_mode) {
            // g_state.name_buf is UTF-8 (Dear ImGui's InputText works in
            // UTF-8) - same u8path() reasoning as elsewhere in this file.
            std::string name = g_state.name_buf;
            if (!name.empty()) {
                std::string ext = ToLower(PathToUtf8(Utf8ToPath(name).extension()));
                if (ext.empty() || ext == ".") {
                    std::string default_ext = CurrentFilterFirstExtension();
                    if (!default_ext.empty())
                        name += "." + default_ext;
                }
                fs::path full = g_state.current_dir / Utf8ToPath(name);
                std::error_code exists_ec;
                if (fs::exists(full, exists_ec)) {
                    g_state.show_overwrite_confirm = true;
                    g_state.overwrite_popup_pending_open = true;
                    g_state.overwrite_path = PathToUtf8(full);
                } else {
                    Confirm(PathToUtf8(full));
                }
            }
        } else if (g_state.selected >= 0 && g_state.selected < (int)g_state.entries.size() &&
                   !g_state.entries[g_state.selected].is_dir) {
            Confirm(PathToUtf8(g_state.current_dir / Utf8ToPath(g_state.entries[g_state.selected].name)));
        }
    }

    ImGui::End();
    g_state.open = g_state.open && open;

    // OpenPopup() must only fire on the frame the request was made - calling
    // it unconditionally every frame while show_overwrite_confirm is true
    // fights ImGui's own popup-close
    // handling: Escape/click-outside/the window's own close button all work
    // by making BeginPopupModal() stop returning true, but with OpenPopup()
    // re-armed every single frame regardless, the very next frame just
    // reopens it immediately - the popup could only ever be dismissed via
    // its own Cancel button.
    if (g_state.overwrite_popup_pending_open) {
        ImGui::OpenPopup("Overwrite file?");
        g_state.overwrite_popup_pending_open = false;
    }
    if (g_state.show_overwrite_confirm) {
        // Modal-in-the-same-frame overlay - still non-native, still no OS
        // event loop, still safe alongside the single-threaded main loop.
        bool popup_still_open = true;
        if (ImGui::BeginPopupModal("Overwrite file?", &popup_still_open, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("A file with this name already exists. Overwrite it?");
            ImGui::Spacing();
            if (ImGui::Button("Overwrite")) {
                ImGui::CloseCurrentPopup();
                g_state.show_overwrite_confirm = false;
                Confirm(g_state.overwrite_path);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
                g_state.show_overwrite_confirm = false;
            }
            ImGui::EndPopup();
        }
        // popup_still_open goes false when ImGui closed it on its own -
        // Escape, a click outside, or the titlebar close button - keep our
        // own state in sync so it doesn't spuriously reopen next frame.
        if (!popup_still_open)
            g_state.show_overwrite_confirm = false;
    }
}

}//namespace xImGui
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
