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

#ifndef __SDL2_DESKTOP_FILEDIALOG_H__
#define __SDL2_DESKTOP_FILEDIALOG_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_filedialog.h
//
//  In-engine, non-native file browser overlay, drawn by Dear ImGui like
//  every other window on this platform - no OS modal dialog, no second
//  thread needed to keep the emulator alive while it's open (see the
//  original architecture writeup on why that matters here).
//
//  Stands in for wxFileDialog's two uses in platform/wxwidgets/wx_frame.cpp:
//  Frame::OnOpenFile() (wxFD_OPEN, wildcard-filtered) and
//  Frame::OnSaveFile() (wxFD_SAVE, wxFD_OVERWRITE_PROMPT).
// =============================================================================

#include <string>
#include <vector>
#include <functional>

namespace xPlatform {
namespace xImGui {

struct FileDialogFilter {
    std::string label;                    // e.g. "Snapshot files (*.sna;*.z80;*.szx)"
    std::vector<std::string> extensions;  // lowercase, no dot: {"sna","z80","szx"} - empty = match anything
};

// Opens the browser overlay (closes any previous instance). `on_confirm` is
// invoked once, with the chosen absolute path, when the user accepts; never
// invoked if they cancel. `save_mode` switches Open-style single-click-opens-
// folder/select-file behaviour to Save-style (name field + overwrite prompt).
void OpenFileBrowser(const std::string& title, const std::string& start_dir,
    std::vector<FileDialogFilter> filters, bool save_mode,
    const std::string& default_name,
    std::function<void(const std::string& path)> on_confirm);

// Draws the browser if one is open. Call once per frame, alongside the other
// EndFrame()-time windows. No-op if nothing is open.
void DrawFileBrowser();

// True while a browser (or its overwrite-confirm sub-prompt) is open - used
// to decide whether keyboard shortcuts like F3/F2 should be swallowed.
bool FileBrowserActive();

}//namespace xImGui
}//namespace xPlatform

#endif//__SDL2_DESKTOP_FILEDIALOG_H__
