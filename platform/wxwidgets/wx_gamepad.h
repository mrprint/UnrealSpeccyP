/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2013 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

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

#ifndef __WX_GAMEPAD_H__
#define __WX_GAMEPAD_H__

#pragma once

#ifdef USE_WXWIDGETS
#ifdef USE_SDL2_GAMEPAD

#include <SDL.h>
#include <array>
#include <functional>
#include <string>
#include <vector>
#include <map>

namespace xPlatform {

// --- State of a single physical gamepad ---
struct GamepadState {
    // Buttons (standard for SDL_GameController)
    bool a = false, b = false, x = false, y = false;
    bool back = false, start = false;
    bool leftstick = false, rightstick = false;
    bool leftshoulder = false, rightshoulder = false;

    // D-pad (hat) — SDL_HAT_* bitmask
    uint8_t hat = 0;

    // Analog axes (-32768..32767 → normalized to -1.0f..1.0f)
    float leftX = 0.0f, leftY = 0.0f;
    float rightX = 0.0f, rightY = 0.0f;
    float triggerLeft = 0.0f, triggerRight = 0.0f;

    // Deadzone (applied on read)
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

// --- Host input sources for mapping ---
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

// --- Emulated ZX Spectrum joystick inputs ---
enum class EEmulatedJoystickInput {
    UP, DOWN, LEFT, RIGHT, FIRE1, FIRE2
};

// --- One mapping entry ---
struct JoystickMappingEntry {
    EHostSourceType source_type = EHostSourceType::NONE;
    float threshold = 0.5f;
};

// --- Player profile ---
struct JoystickProfile {
    int host_device_index = -1;
    std::string device_guid;

    // Мэппинг: эмулируемый ввод → источник хоста
    std::map<EEmulatedJoystickInput, JoystickMappingEntry> input_map;

    bool IsEnabled() const { return host_device_index >= 0; }
};

// --- SDL2 backend interface for wxwidgets ---
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

    // Event polling — call from wxTimer on the main thread
    void PollEvents(std::function<void(int device_index)> on_device_added,
                    std::function<void(int device_index)> on_device_removed);

    std::vector<DeviceInfo> EnumerateDevices();

    // Get the state of a specific device
    const GamepadState& GetState(int device_index) const;

    // Force-update state (for initialization after profile assignment)
    void RefreshDeviceState(int device_index);

private:
    std::array<SDL_GameController*, 16> m_controllers{};
    std::array<GamepadState, 16> m_states;
    std::array<bool, 16> m_connected{false};
    // SDL_CONTROLLERBUTTONDOWN/UP/AXISMOTION/DEVICEREMOVED events carry the
    // joystick *instance id* in `which`, not the device index used to open
    // the controller and to index the arrays above (only DEVICEADDED's
    // `which` is a device index). Instance ids are stable, ever-increasing
    // ids assigned per physical connection and can diverge from device
    // index - e.g. when a controller is picked up via a runtime hotplug
    // event rather than at startup. Track them so live input events can be
    // routed to the correct slot instead of silently updating nothing (or
    // the wrong slot).
    std::array<SDL_JoystickID, 16> m_instance_ids{};

    void UpdateDevice(int device_index);
    int SlotForInstanceId(SDL_JoystickID instance_id) const;
};

// Global backend instance
WxGamepadBackend& GamepadBackend();

} // namespace xPlatform

#endif // USE_SDL2_GAMEPAD
#endif // USE_WXWIDGETS

#endif // __WX_GAMEPAD_H__
