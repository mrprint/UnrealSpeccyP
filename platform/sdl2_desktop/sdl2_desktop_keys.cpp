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

// F-keys/Ctrl+.../Ctrl+Shift+... are intercepted before TranslateKey() by
// HandleMenuShortcut() (sdl2_desktop_menu.cpp) - the menu accelerator table.

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#include <map>
#include "../../tools/options.h"
#include "../../options_common.h"

namespace xPlatform
{

#ifdef SDL_USE_MOUSE
bool ProcessMouseGrab(SDL_Event& e); // sdl2_mouse.cpp
#endif//SDL_USE_MOUSE

// sdl2_desktop_menu.cpp - menu accelerators (File/View/Device/Window).
// Returns true if this event was consumed as a shortcut and must
// not also reach the ZX keyboard emulation below.
namespace xImGui { bool HandleMenuShortcut(SDL_Event& e); }

static bool PreProcessKey(SDL_Event& e)
{
#ifdef SDL_USE_MOUSE
	if(ProcessMouseGrab(e))
		return true;
#endif//SDL_USE_MOUSE
	if(xImGui::HandleMenuShortcut(e))
		return true;
	return false;
}

static byte TranslateKey(SDL_Keycode _key, dword& _flags)
{
	switch(_key)
	{
	case SDLK_BACKQUOTE:
	case SDLK_ESCAPE:	return 'm';
	case SDLK_LSHIFT:	return 'c';
	case SDLK_RSHIFT:	return 'c';
	case SDLK_LALT:		return 's';
	case SDLK_RALT:		return 's';
	case SDLK_RETURN:	return 'e';
	case SDLK_BACKSPACE:
		_flags |= KF_SHIFT;
		return '0';
	case SDLK_QUOTE:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return 'P';
		}
		else
			return '7';
	case SDLK_COMMA:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return 'R';
		}
		else
			return 'N';
	case SDLK_PERIOD:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return 'T';
		}
		else
			return 'M';
	case SDLK_SEMICOLON:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return 'Z';
		}
		else
			return 'O';
	case SDLK_SLASH:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return 'C';
		}
		else
			return 'V';
	case SDLK_MINUS:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return '0';
		}
		else
			return 'J';
	case SDLK_EQUALS:
		_flags |= KF_ALT;
		if(_flags&KF_SHIFT)
		{
			_flags &= ~KF_SHIFT;
			return 'K';
		}
		else
			return 'L';
	case SDLK_TAB:
		_flags |= KF_ALT;
		_flags |= KF_SHIFT;
		return 0;
	case SDLK_LEFT:		return 'l';
	case SDLK_RIGHT:	return 'r';
	case SDLK_UP:		return 'u';
	case SDLK_DOWN:		return 'd';
	case SDLK_INSERT:
	case SDLK_RCTRL:
	case SDLK_LCTRL:	return 'f';
	default:
		break;
	}
	if(_key >= SDLK_0 && _key <= SDLK_9)
		return _key;
	if(_key >= SDLK_a && _key <= SDLK_z)
		return 'A' + _key - SDLK_a;
	if(_key == SDLK_SPACE)
		return _key;
	return 0;
}

// What the emulator was actually told when a physical key went down, keyed by
// the physical key (scancode), not by SDL_Keycode: the matching key-up must
// undo exactly that, no matter what the modifiers, the layout or the selected
// joystick type (OpJoyKeyFlags()) look like by the time the key comes up.
// Re-deriving the ZX key from the key-up event itself (what this file used to
// do) can release a different ZX key than the one that was pressed, and - the
// bigger problem - it relies on the key-up being delivered at all.
// An emulated key that is never released is auto-repeated by the Spectrum ROM
// with a click on every repeat: an endless row of key clicks.
struct PressedKey
{
	byte key;
	dword joy_flags;
};
static std::map<SDL_Scancode, PressedKey> pressed_keys;

static void ReleaseKey(SDL_Scancode sc)
{
	std::map<SDL_Scancode, PressedKey>::iterator it = pressed_keys.find(sc);
	if(it == pressed_keys.end())
		return;
	Handler()->OnKey(it->second.key, it->second.joy_flags);
	pressed_keys.erase(it);
}

// Releases every key this file has pressed in the emulator. Called whenever
// key-ups may never arrive: the window lost focus (an exclusive-fullscreen
// mode switch - "Prefer PAL refresh" - does exactly that, and the key-ups
// SDL synthesizes for it are not guaranteed to reach us).
void ReleaseAllKeys()
{
	while(!pressed_keys.empty())
		ReleaseKey(pressed_keys.begin()->first);
}

void ProcessKey(SDL_Event& e)
{
	switch(e.type)
	{
	case SDL_KEYDOWN:
		if(PreProcessKey(e))
		{
			// A shortcut (Ctrl+F, Ctrl+1.., Ctrl+Shift+R, ...) consumed this
			// key, but Ctrl itself already went down into the emulator
			// before the second key of the chord arrived (Ctrl is the
			// joystick "fire" key: Ctrl -> '0' in Cursor mode). The chord is
			// ours, not the Spectrum's - take Ctrl back, so that it can't
			// stay down if its key-up gets lost in the window/mode change
			// the shortcut itself may cause.
			if(!e.key.repeat && (e.key.keysym.mod&KMOD_CTRL))
			{
				ReleaseKey(SDL_SCANCODE_LCTRL);
				ReleaseKey(SDL_SCANCODE_RCTRL);
			}
			break;
		}
		// OS auto-repeat means nothing to the emulated keyboard (the ROM does
		// its own repeating), and a repeat of a key whose down we have
		// already released (the Ctrl above) must not press it again.
		if(e.key.repeat)
			break;
		{
			dword flags = KF_DOWN|OpJoyKeyFlags();
			if(e.key.keysym.mod&KMOD_ALT)
				flags |= KF_ALT;
			if(e.key.keysym.mod&KMOD_SHIFT)
				flags |= KF_SHIFT;
			byte key = TranslateKey(e.key.keysym.sym, flags);
			PressedKey pk;
			pk.key = key;
			pk.joy_flags = OpJoyKeyFlags();
			pressed_keys[e.key.keysym.scancode] = pk;
			Handler()->OnKey(key, flags);
		}
		break;
	case SDL_KEYUP:
		// No PreProcessKey() here: shortcuts act on key-down only, and a
		// key-up must always get through to whatever it is releasing.
		ReleaseKey(e.key.keysym.scancode);
		break;
	default:
		break;
	}
}

}
//namespace xPlatform

#endif//USE_SDL2_DESKTOP
