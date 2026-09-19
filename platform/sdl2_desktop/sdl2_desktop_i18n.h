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

#ifndef __SDL2_DESKTOP_I18N_H__
#define __SDL2_DESKTOP_I18N_H__

#pragma once

#include <string>
#include <vector>

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_i18n.h
//
//  Text localization for this platform's ImGui UI (menu bar, status bar,
//  About window, Options dialog, file browser, gamepad panel).
//
//  Translations live as plain data files, res/lang/<code>.xml. Unlike
//  res/font and res/rom, this is NOT the shared top-level res/ tree every
//  platform's packaging pulls from - it's specific to this one
//  platform's ImGui UI, so the tracked source is platform/sdl2_desktop/
//  lang/, and res/lang/ is a build output that ends up next to the
//  running binary itself (not the shared source tree, and not found via
//  a cwd-relative path the way res/font is - see ResolveLangDir() in the
//  .cpp for why, and TL;DR: putting it anywhere in the shared tree,
//  even transiently, turned out to leak into other platforms' packages
//  and build output in practice, not just in theory). See the
//  sdl2_desktop_lang custom target in build/cmake/CMakeLists.txt for the
//  build side of this, and .gitignore for why res/lang/ itself isn't
//  tracked. Nothing below cares about any of that distinction -
//  Init() just reads res/lang/ like any other resource, exactly as if it
//  had always lived there.
//
//  English (res/lang/en.xml) is always loaded first as the fallback
//  layer, so a string missing from any other language still shows
//  something readable instead of a blank or a crash; a string missing
//  from *every* file falls back to its raw id, which is a deliberately
//  visible way to say "this one wasn't translated" rather than hiding
//  the gap silently.
//
//  Format (deliberately flat - see the file comment in
//  sdl2_desktop_i18n.cpp for why a full XML tree isn't needed here):
//
//    <?xml version="1.0" encoding="UTF-8"?>
//    <language code="ru" name="Русский">
//        <string id="menu.file">Файл</string>
//        <string id="menu.file.open">Открыть...</string>
//        ...
//    </language>
//
//  `code` is what both the "language" xOption and this file's own name
//  (res/lang/<code>.xml) use to identify the language; `name` is what the
//  Options dialog's language picker shows (native spelling, not English).
//
//  The active language can change at any moment while the UI is open (see
//  SetLanguage() below) - the Options dialog's own language combo is the
//  main reason: switching it must take effect immediately, without
//  closing/reopening anything. Every DrawXxx() function already re-reads
//  Tr() fresh each frame (immediate-mode UI), so this "just works" for
//  ordinary widget labels; the one place that needs care is a window or
//  popup whose *title* is translated (About/Options/the overwrite-confirm
//  popup) - Dear ImGui derives that window's persistent ID from its whole
//  title string, so translating it naively would reset the window's
//  position/open state the instant the language changes. TrTitle() in
//  imgui_shared.h is the fix; see its own comment.
// =============================================================================

namespace xPlatform {
namespace xI18n {

struct LanguageInfo
{
	std::string code;        // "en", "ru", ... - matches res/lang/<code>.xml and the "language" xOption's stored value
	std::string native_name; // shown in the language picker, in that language's own script (e.g. "Русский", not "Russian")
};

// Scans res/lang/*.xml for available languages, loads English (res/lang/
// en.xml) as the permanent fallback layer, and resolves the language
// actually active right now from whatever the "language" xOption currently
// holds (likely still its just-constructed default, "auto", this early -
// xOptions::Load() re-applies the saved value shortly after, via the
// eOptionLanguage::Apply() override in sdl2_desktop_i18n.cpp, same pattern
// as e.g. sdl2_desktop_video.cpp's eOptionFullScreen).
//
// Call once, early in xPlatform::Init() (sdl2_desktop.cpp) - specifically
// before Handler()->OnInit(), which is what runs xOptions::Load() - so the
// language list already exists by the time that Apply() pass reaches the
// "language" option.
void Init();

// Switches the active language. "auto" re-runs system-locale detection
// (SDL_GetPreferredLocales()) against whatever Init() found under
// res/lang/; anything else must be one of AvailableLanguages()'s codes.
// An unknown code, or "auto" matching nothing installed, falls back to
// English. Reloads the active string table immediately - safe to call
// with dialogs open (see the file comment above on TrTitle()).
void SetLanguage(const std::string& code);

// The raw xOption value ("auto", "en", "ru", ...) - what the language
// picker combo should show as selected.
const std::string& LanguageSetting();

// What "auto" actually resolved to right now ("en", "ru", ...) - never
// "auto" itself. Same as LanguageSetting() when a specific language was
// chosen explicitly.
const std::string& CurrentLanguage();

// Every language Init() found under res/lang/, in file order. Always at
// least contains English if res/lang/en.xml is present; empty only if
// res/lang/ itself is missing (Tr() then just returns raw ids - see above).
const std::vector<LanguageInfo>& AvailableLanguages();

// Looks up `id` in the active language, falling back to English, falling
// back to `id` itself if neither has it. Never returns null. The returned
// pointer is stable until the next SetLanguage() call (owned by the active
// string table), so it's safe to hand straight to ImGui widgets the way a
// literal would be - just don't cache it across a language switch.
const char* Tr(const char* id);

}//namespace xI18n
}//namespace xPlatform

#endif//__SDL2_DESKTOP_I18N_H__
