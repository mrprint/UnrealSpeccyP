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
//  platform/sdl2_desktop/sdl2_desktop_i18n.cpp
//
//  See sdl2_desktop_i18n.h for the public API, the res/lang/*.xml format,
//  and why res/lang/ itself is a build output (tracked source is
//  platform/sdl2_desktop/lang/) rather than the other way around.
//
//  Parser choice: tinyxml2, not a new dependency - tools/options.cpp already
//  links it (USE_CONFIG's Load()/Store(), reading/writing the settings
//  file), and it's unconditionally part of every platform's build (see
//  build/cmake/CMakeLists.txt: the tinyxml2 sources/include dir are added
//  once, near the top, well before the per-platform branches), so this file
//  gets it for free.
//
//  The "language" xOption (an eOptionString, like the rest of xOptions -
//  see tools/options.h) is registered right here rather than in
//  options_common.cpp: it's specific to this platform's ImGui UI, the same
//  reason "full screen"/"zoom" live in sdl2_desktop_video.cpp and
//  platform/gl/draw.cpp instead of the shared file. Its Apply() override is
//  the single place that pushes a value into the i18n runtime - both the
//  language picker combo (sdl2_desktop_options.cpp) and xOptions::Load()
//  restoring a saved choice at startup go through it, so there's exactly
//  one path from "language changed" to "string tables reloaded".
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#include "sdl2_desktop_i18n.h"
#include "../../tools/options.h"

namespace xPlatform
{
namespace xI18n
{

namespace {

namespace fs = std::filesystem;

// Where res/lang/*.xml actually live at runtime - next to the running
// executable itself, NOT relative to whatever the current working
// directory happens to be (unlike res/font and res/rom, which do rely on
// cwd == the directory the binary was launched from - see LoadFont() in
// sdl2_desktop_imgui.cpp). That's deliberate: build/cmake/CMakeLists.txt
// writes res/lang/ into this platform's own build output directory
// (right next to the built binary), never into the shared top-level
// res/ any other platform's build might also read from - see the
// sdl2_desktop_lang custom target there for the full reasoning, which
// includes a confirmed, reproduced bug from writing it anywhere in the
// shared source tree, even transiently: a plain-SDL2 or wx build,
// invoked from the very same checkout right after an sdl2_desktop
// build/dobuild.bat with no cleanup in between, would otherwise still
// find (and, packaged, previously did bundle) a leftover res/lang that
// has nothing to do with what's actually being built.
//
// SDL_GetBasePath() answers "where am I actually running from" directly
// (GetModuleFileName on Windows, /proc/self/exe on Linux, etc.) without
// needing SDL_Init() first - same as SDL_GetPreferredLocales() below,
// already called this early in Init(). Falls back to the old cwd-relative
// "res/lang" only if SDL can't answer that (rare - e.g. some sandboxed or
// unusual platform SDL doesn't have a base-path implementation for).
std::string ResolveLangDir()
{
	std::string result = "res/lang";
	if(char* base = SDL_GetBasePath())
	{
		result = std::string(base) + "res/lang";
		SDL_free(base);
	}
	return result;
}

std::vector<LanguageInfo> g_available;
std::unordered_map<std::string, std::string> g_english; // permanent fallback layer, res/lang/en.xml
std::unordered_map<std::string, std::string> g_active;  // currently selected language, if not English itself
std::string g_language_setting = "auto";                // raw xOption value
std::string g_current_language = "en";                  // what "auto" (or an explicit choice) resolved to
std::string g_lang_dir;                                 // resolved once, by Init(), via ResolveLangDir()
bool g_scanned = false;

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
	if(a.size() != b.size())
		return false;
	for(size_t i = 0; i < a.size(); ++i)
		if(std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
			return false;
	return true;
}

// Parses one res/lang/<code>.xml file. `strings` may be null (metadata-only
// scan, used while building the language picker's list). Returns false
// (leaving the outputs untouched) if the file is missing or malformed -
// same "missing resource degrades instead of crashing" spirit as
// sdl2_desktop_imgui.cpp's FileExists()/LoadFont().
bool LoadLanguageFile(const std::string& path, std::unordered_map<std::string, std::string>* strings,
	std::string* out_code, std::string* out_name)
{
	tinyxml2::XMLDocument doc;
	if(doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
		return false;
	tinyxml2::XMLElement* root = doc.RootElement();
	if(!root)
		return false;

	const char* code = root->Attribute("code");
	if(!code || !*code)
		return false; // a language file with no code can never be selected - not usable
	if(out_code)
		*out_code = code;

	if(out_name)
	{
		const char* name = root->Attribute("name");
		*out_name = (name && *name) ? name : code;
	}

	if(strings)
	{
		for(tinyxml2::XMLElement* e = root->FirstChildElement("string"); e; e = e->NextSiblingElement("string"))
		{
			const char* id = e->Attribute("id");
			const char* text = e->GetText();
			if(id && *id && text)
				(*strings)[id] = text;
		}
	}
	return true;
}

// Scans res/lang/*.xml once for available languages (metadata only - the
// id->string tables are loaded lazily, only for English and whichever
// language is actually active, by LoadLanguageFile() calls elsewhere). Safe to call
// when res/lang/ doesn't exist at all: leaves g_available empty, and
// everything downstream (SetLanguage(), Tr()) already tolerates that.
void ScanAvailableLanguages()
{
	g_available.clear();
	std::error_code ec;
	if(!fs::exists(g_lang_dir, ec) || ec || !fs::is_directory(g_lang_dir, ec) || ec)
		return;

	for(const auto& entry : fs::directory_iterator(g_lang_dir, ec))
	{
		if(ec)
			break;
		if(!entry.is_regular_file())
			continue;
		if(entry.path().extension() != ".xml")
			continue;
		std::string code, name;
		if(LoadLanguageFile(entry.path().string(), nullptr, &code, &name))
			g_available.push_back(LanguageInfo{code, name});
	}

	// Stable, predictable order for the picker combo: English first (it's
	// the one guaranteed to be complete), then alphabetically by code.
	std::sort(g_available.begin(), g_available.end(), [](const LanguageInfo& a, const LanguageInfo& b)
	{
		if(a.code == "en" || b.code == "en")
			return a.code == "en" && b.code != "en";
		return a.code < b.code;
	});
}

// Matches SDL_GetPreferredLocales() against g_available; first preferred
// locale (in the user's own OS-level ranked order) whose language part
// matches an available code wins - the country part (e.g. the "_UA" in
// "ru_UA") is deliberately ignored, since res/lang/ only ever indexes by
// bare language code. Falls back to English if available, else whatever
// Init() found first, else "en" regardless (Tr() then just shows raw ids).
std::string DetectSystemLanguage()
{
	if(SDL_Locale* locales = SDL_GetPreferredLocales())
	{
		for(SDL_Locale* l = locales; l->language; ++l)
		{
			for(const LanguageInfo& info : g_available)
			{
				if(EqualsIgnoreCase(info.code, l->language))
				{
					std::string found = info.code;
					SDL_free(locales);
					return found;
				}
			}
		}
		SDL_free(locales);
	}
	for(const LanguageInfo& info : g_available)
		if(info.code == "en")
			return "en";
	if(!g_available.empty())
		return g_available.front().code;
	return "en";
}

}//anonymous namespace

void Init()
{
	if(g_scanned)
		return;
	g_scanned = true;

	g_lang_dir = ResolveLangDir();
	ScanAvailableLanguages();

	g_english.clear();
	std::string unused_code, unused_name;
	LoadLanguageFile(g_lang_dir + "/en.xml", &g_english, &unused_code, &unused_name);

	// Resolves whatever the "language" xOption currently holds (still its
	// just-constructed default, "auto", this early - see the file comment
	// on why Init() runs before xOptions::Load()) now that g_available/
	// g_english actually exist.
	SetLanguage(g_language_setting);
}

void SetLanguage(const std::string& code)
{
	g_language_setting = code;

	std::string resolved = code;
	if(resolved.empty() || resolved == "auto")
		resolved = DetectSystemLanguage();

	bool found = false;
	for(const LanguageInfo& info : g_available)
		if(info.code == resolved)
			{ found = true; break; }
	if(!found)
		resolved = "en"; // unknown/uninstalled code - English fallback layer always covers this

	g_current_language = resolved;

	g_active.clear();
	if(resolved != "en")
	{
		std::string unused_code, unused_name;
		LoadLanguageFile(g_lang_dir + "/" + resolved + ".xml", &g_active, &unused_code, &unused_name);
	}
}

const std::string& LanguageSetting()
{
	return g_language_setting;
}

const std::string& CurrentLanguage()
{
	return g_current_language;
}

const std::vector<LanguageInfo>& AvailableLanguages()
{
	return g_available;
}

const char* Tr(const char* id)
{
	if(!id)
		return "";
	if(!g_active.empty())
	{
		auto it = g_active.find(id);
		if(it != g_active.end())
			return it->second.c_str();
	}
	auto it = g_english.find(id);
	if(it != g_english.end())
		return it->second.c_str();
	return id; // no translation anywhere - show the id itself rather than nothing
}

// ---------------------------------------------------------------------------
// "language" xOption - see the file comment above.
// ---------------------------------------------------------------------------

namespace {

struct eOptionLanguage : public xOptions::eOptionString
{
	eOptionLanguage() { Value("auto"); }
	const char* Name() const override { return "language"; }
	void Apply() override { SetLanguage(Value()); }
} op_language;

}//anonymous namespace

}//namespace xI18n
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
