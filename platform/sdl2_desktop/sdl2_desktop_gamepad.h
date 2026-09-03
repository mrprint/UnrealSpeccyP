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

#ifndef __SDL2_DESKTOP_GAMEPAD_H__
#define __SDL2_DESKTOP_GAMEPAD_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_gamepad.h
//
//  Per-player, remappable gamepad backend for the "sdl2_desktop" platform -
//  the same data model and logic as platform/wxwidgets/wx_gamepad.h +
//  joystick_mapper.h (GamepadState / EHostSourceType / EEmulatedJoystickInput
//  / JoystickProfile / JoystickMapper / (de)serialization / GUID resolution),
//  copied verbatim where possible; that code has no wxWidgets dependency at
//  all, only an `#ifdef USE_WXWIDGETS` guard around the whole file. This is
//  the same file with that guard swapped for USE_SDL2_DESKTOP, plus one
//  addition: HandleControllerEvent(), which lets a single, already-running
//  SDL_PollEvent() loop (this platform's own, in sdl2_desktop.cpp) feed
//  events in one at a time instead of PollEvents() draining the queue itself
//  - the wx build has SDL initialized *only* for the gamepad subsystem, so
//  wx_canvas.cpp's polling timer calling PollEvents() is the sole consumer
//  of SDL_PollEvent() there. Here SDL also owns the window/keyboard/mouse
//  event queue, so there can only be one place draining it.
//
// =============================================================================

#ifdef USE_SDL2_DESKTOP
#ifdef SDL_USE_JOYSTICK

#include <SDL.h>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cmath>

namespace xPlatform {

// --- State of a single physical gamepad ---
struct GamepadState {
    bool a = false, b = false, x = false, y = false;
    bool back = false, start = false;
    bool leftstick = false, rightstick = false;
    bool leftshoulder = false, rightshoulder = false;

    uint8_t hat = 0;

    float leftX = 0.0f, leftY = 0.0f;
    float rightX = 0.0f, rightY = 0.0f;
    float triggerLeft = 0.0f, triggerRight = 0.0f;

    float deadzone = 0.15f;

    bool IsHatUp() const   { return hat & SDL_HAT_UP; }
    bool IsHatDown() const { return hat & SDL_HAT_DOWN; }
    bool IsHatLeft() const { return hat & SDL_HAT_LEFT; }
    bool IsHatRight() const{ return hat & SDL_HAT_RIGHT; }

    float GetAxisWithDeadzone(float raw) const {
        if (std::abs(raw) < deadzone) return 0.0f;
        return raw;
    }
};

enum class EHostSourceType {
    NONE,
    BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y,
    BUTTON_BACK, BUTTON_START,
    BUTTON_LEFTSTICK, BUTTON_RIGHTSTICK,
    BUTTON_LEFTSHOULDER, BUTTON_RIGHTSHOULDER,
    HAT_UP, HAT_DOWN, HAT_LEFT, HAT_RIGHT,
    AXIS_LEFT_X_POS, AXIS_LEFT_X_NEG,
    AXIS_LEFT_Y_POS, AXIS_LEFT_Y_NEG,
    AXIS_RIGHT_X_POS, AXIS_RIGHT_X_NEG,
    AXIS_RIGHT_Y_POS, AXIS_RIGHT_Y_NEG,
    TRIGGER_LEFT, TRIGGER_RIGHT
};

enum class EEmulatedJoystickInput {
    UP, DOWN, LEFT, RIGHT, FIRE1, FIRE2
};

struct JoystickMappingEntry {
    EHostSourceType source_type = EHostSourceType::NONE;
    float threshold = 0.5f;
};

struct JoystickProfile {
    int host_device_index = -1;
    std::string device_guid;
    std::map<EEmulatedJoystickInput, JoystickMappingEntry> input_map;
    bool IsEnabled() const { return host_device_index >= 0; }
};

class WxGamepadBackend {
public:
    struct DeviceInfo {
        int index;
        std::string name;
        std::string guid;
        bool is_gamepad;
    };

    void Initialize();
    void Shutdown();

    // Event polling — call from wxTimer on the main thread (wx build only).
    void PollEvents(std::function<void(int device_index)> on_device_added,
                    std::function<void(int device_index)> on_device_removed);

    // Single-event equivalent of the switch inside PollEvents(), for a host
    // (this platform) that already owns THE SDL_PollEvent() loop and just
    // wants to forward one relevant event at a time. Handles
    // SDL_CONTROLLERDEVICEADDED/REMOVED/BUTTONDOWN/UP/AXISMOTION; anything
    // else is ignored. Callbacks mirror PollEvents()'s.
    void HandleControllerEvent(const SDL_Event& e,
                    std::function<void(int device_index)> on_device_added = nullptr,
                    std::function<void(int device_index)> on_device_removed = nullptr);

    std::vector<DeviceInfo> EnumerateDevices();

    const GamepadState& GetState(int device_index) const;

    void RefreshDeviceState(int device_index);

private:
    static constexpr int kMaxControllers = 16;

    // RAII wrapper for SDL_GameController*: each slot auto-closes on
    // destruction or reassignment via SDL_GameControllerClose, eliminating
    // the possibility of a forgotten close or double-close that the manual
    // open/close in Initialize()/Shutdown()/HandleControllerEvent() carried.
    // Stateful deleter: always callable regardless of how the unique_ptr was
    // constructed.
    struct ControllerDeleter {
        void operator()(SDL_GameController* gc) const noexcept { SDL_GameControllerClose(gc); }
    };
    using ControllerPtr = std::unique_ptr<SDL_GameController, ControllerDeleter>;
    std::array<ControllerPtr, kMaxControllers> m_controllers{};
    std::array<GamepadState, kMaxControllers> m_states;
    std::array<bool, kMaxControllers> m_connected{false};
    std::array<SDL_JoystickID, kMaxControllers> m_instance_ids{};

    void UpdateDevice(int device_index);
    int SlotForInstanceId(SDL_JoystickID instance_id) const;
};

WxGamepadBackend& GamepadBackend();

// --- Translates raw GamepadState + a player's profile into emulator key events ---
class JoystickMapper {
public:
    struct EmulatedKeyEvent {
        char key;
        bool is_down;
    };

    std::vector<EmulatedKeyEvent> ProcessEvent(
        const JoystickProfile& profile,
        int player_index,
        const GamepadState& current_state,
        int device_index);

    // Force-releases any keys still marked "held" for this player, without
    // needing a live GamepadState. Call this when a profile's device has
    // just gone from enabled to disabled (e.g. unplugged mid-press) -
    // ProcessEvent() is never invoked in that case (see the SDL_USE_JOYSTICK
    // block in sdl2_desktop.cpp), so without this the emulated key would
    // stay stuck down until the device reconnects and its state happens to
    // differ from what was last recorded here.
    std::vector<EmulatedKeyEvent> ReleaseAll(int player_index);

private:
    struct PlayerInternalState {
        bool up = false, down = false, left = false, right = false;
        bool fire1 = false, fire2 = false;
    };

    // Single source of truth for the emulated-input -> (per-player state bit,
    // ZX keyboard key) mapping, shared by ProcessEvent() and ReleaseAll().
    struct InputKey {
        EEmulatedJoystickInput input;
        bool PlayerInternalState::* state_ptr;
        char key;
    };
    static constexpr std::array<InputKey, 6> kInputs = {{
        {EEmulatedJoystickInput::UP,    &PlayerInternalState::up,    'u'},
        {EEmulatedJoystickInput::DOWN,  &PlayerInternalState::down,  'd'},
        {EEmulatedJoystickInput::LEFT,  &PlayerInternalState::left,  'l'},
        {EEmulatedJoystickInput::RIGHT, &PlayerInternalState::right, 'r'},
        {EEmulatedJoystickInput::FIRE1, &PlayerInternalState::fire1, 'f'},
        {EEmulatedJoystickInput::FIRE2, &PlayerInternalState::fire2, 'e'}
    }};

    std::array<PlayerInternalState, 2> m_player_states;

    bool IsSourceActive(const JoystickMappingEntry& entry, const GamepadState& state) const;
};

std::string SerializeMapping(const std::map<EEmulatedJoystickInput, JoystickMappingEntry>& mapping);
bool DeserializeMapping(const std::string& data, std::map<EEmulatedJoystickInput, JoystickMappingEntry>& out_mapping);

std::string SerializeProfile(const JoystickProfile& profile);
bool DeserializeProfile(const std::string& data, JoystickProfile& out_profile);

std::string SourceTypeToString(EHostSourceType type);
EHostSourceType StringToSourceType(const std::string& str);
const char* SourceTypeDisplayString(EHostSourceType type);

int ResolveDeviceIndexForGuid(const std::string& guid, int hinted_index, std::string* out_resolved_guid = nullptr);

} // namespace xPlatform

#endif // SDL_USE_JOYSTICK
#endif // USE_SDL2_DESKTOP

#endif // __SDL2_DESKTOP_GAMEPAD_H__
