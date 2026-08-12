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

#include "../platform.h"
#include "wx_gamepad.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#include <cstring>
#include <SDL_log.h>

namespace xPlatform {

// --- Initialization ---
void WxGamepadBackend::Initialize() {
    // SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) is called in main_wx.cpp

    // Check subsystem initialization
    if (!(SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL2 GameController subsystem not initialized");
        return;
    }

    // Automatically open all connected gamepads during initialization
    int num_joysticks = SDL_NumJoysticks();
    if (num_joysticks < 0) {
        const char* error = SDL_GetError();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL_NumJoysticks failed during initialization: %s",
            error ? error : "Unknown error");
        SDL_ClearError();
        return;
    }

    for (int i = 0; i < num_joysticks && i < 16; ++i) {
        if (SDL_IsGameController(i)) {
            m_controllers[i] = SDL_GameControllerOpen(i);
            if (m_controllers[i]) {
                m_connected[i] = true;
                m_instance_ids[i] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(m_controllers[i]));
                UpdateDevice(i);
            } else {
                const char* error = SDL_GetError();
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to auto-open game controller %d: %s", i,
                    error ? error : "Unknown error");
                SDL_ClearError();
            }
        }
    }
}

// --- Cleanup ---
void WxGamepadBackend::Shutdown() {
    for (int i = 0; i < 16; ++i) {
        if (m_controllers[i]) {
            SDL_GameControllerClose(m_controllers[i]);
            m_controllers[i] = nullptr;
            m_connected[i] = false;
            m_instance_ids[i] = -1;
        }
    }
}

// --- Find our internal slot (device index) for an SDL joystick instance id ---
// SDL_CONTROLLERBUTTONDOWN/UP, SDL_CONTROLLERAXISMOTION and
// SDL_CONTROLLERDEVICEREMOVED all report the joystick *instance id* in
// `which` - only SDL_CONTROLLERDEVICEADDED's `which` is a device index.
// Instance ids are not the same numbering space as device index and can
// diverge (e.g. a controller picked up via a runtime hotplug event rather
// than at startup), so events must be routed through this lookup rather
// than using `which` directly as an array index.
int WxGamepadBackend::SlotForInstanceId(SDL_JoystickID instance_id) const {
    for (int i = 0; i < 16; ++i) {
        if (m_connected[i] && m_instance_ids[i] == instance_id)
            return i;
    }
    return -1;
}

// --- Event polling ---
void WxGamepadBackend::PollEvents(
    std::function<void(int)> on_device_added,
    std::function<void(int)> on_device_removed) {

    SDL_Event event;

    const char* error = SDL_GetError();
    if (error && strlen(error) > 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL error before polling: %s", error);
        SDL_ClearError();
    }

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_CONTROLLERDEVICEADDED: {
                int which = event.cdevice.which; // device index for ADDED

                if (which >= 0 && which < 16 && m_connected[which]) {
                    break;
                }

                SDL_GameController* controller = SDL_GameControllerOpen(which);
                if (!controller) {
                    const char* err = SDL_GetError();
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to open game controller %d: %s", which,
                        err ? err : "Unknown error");
                    SDL_ClearError();
                    break;
                }

                if (which >= 0 && which < 16) {
                    m_controllers[which] = controller;
                    m_connected[which] = true;
                    m_instance_ids[which] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
                    UpdateDevice(which);

                    if (on_device_added) on_device_added(which);
                } else {
                    SDL_GameControllerClose(controller);
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Game controller index %d out of range", which);
                }
                break;
            }

            case SDL_CONTROLLERDEVICEREMOVED: {
                SDL_JoystickID instance_id = event.cdevice.which; // instance id for REMOVED
                int slot = SlotForInstanceId(instance_id);

                if (slot >= 0 && m_controllers[slot]) {
                    m_connected[slot] = false;
                    SDL_GameControllerClose(m_controllers[slot]);
                    m_controllers[slot] = nullptr;
                    m_states[slot] = GamepadState();

                    if (on_device_removed) on_device_removed(slot);
                }
                break;
            }

            case SDL_CONTROLLERAXISMOTION: {
                int slot = SlotForInstanceId(event.caxis.which); // instance id
                if (slot >= 0) {
                    UpdateDevice(slot);
                }
                break;
            }

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                int slot = SlotForInstanceId(event.cbutton.which); // instance id
                if (slot >= 0) {
                    UpdateDevice(slot);
                }
                break;
            }

            default:
                break;
        }
    }
}

// --- Device enumeration ---
std::vector<WxGamepadBackend::DeviceInfo> WxGamepadBackend::EnumerateDevices() {
    std::vector<DeviceInfo> devices;

    int num_joysticks = SDL_NumJoysticks();
    if (num_joysticks < 0) {
        const char* error = SDL_GetError();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL_NumJoysticks failed: %s", error ? error : "Unknown error");
        SDL_ClearError();
        return devices;
    }

    for (int i = 0; i < num_joysticks; ++i) {
        if (!SDL_IsGameController(i)) continue;

        DeviceInfo info;
        info.index = i;

        const char* name = SDL_JoystickNameForIndex(i);
        info.name = name ? std::string(name) : "Unknown Controller";

        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_str[33];
        SDL_GUIDToString(guid, guid_str, sizeof(guid_str));
        info.guid = std::string(guid_str);

        info.is_gamepad = true;

        devices.push_back(info);
    }

    return devices;
}

// --- State retrieval ---
const GamepadState& WxGamepadBackend::GetState(int device_index) const {
    static GamepadState empty_state;

    if (device_index >= 0 && device_index < 16 && m_connected[device_index]) {
        return m_states[device_index];
    }

    empty_state = GamepadState();
    return empty_state;
}

// --- Device state update ---
void WxGamepadBackend::UpdateDevice(int device_index) {
    if (!m_controllers[device_index] || !m_connected[device_index]) return;

    SDL_GameController* gc = m_controllers[device_index];
    GamepadState& state = m_states[device_index];

    // SDL2 requires explicit update before reading button/axis state.
    // PollEvents() calls this after processing events, but direct reads
    // (e.g. from the capture timer) need it too.
    SDL_GameControllerUpdate();

    if (!SDL_GameControllerGetAttached(gc)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Gamepad %d disconnected during update", device_index);
        m_connected[device_index] = false;
        return;
    }

    // Buttons
    state.a = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0;
    state.b = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B) != 0;
    state.x = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X) != 0;
    state.y = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y) != 0;

    state.back = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK) != 0;
    state.start = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START) != 0;

    // Stick buttons (with support check)
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
        state.leftstick = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
    } else {
        state.leftstick = false;
    }

    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
        state.rightstick = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    } else {
        state.rightstick = false;
    }

    // Shoulder buttons (with support check)
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
        state.leftshoulder = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    } else {
        state.leftshoulder = false;
    }

    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
        state.rightshoulder = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    } else {
        state.rightshoulder = false;
    }

    // D-pad — individual buttons with support check
    Uint8 hat = 0;

    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))
            hat |= SDL_HAT_UP;
    }
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
            hat |= SDL_HAT_DOWN;
    }
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
            hat |= SDL_HAT_LEFT;
    }
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            hat |= SDL_HAT_RIGHT;
    }

    state.hat = hat;

    // Analog stick axes with support check
    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)) {
        Sint16 axis_val = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        state.leftX = static_cast<float>(axis_val) / 32768.0f;
    } else {
        state.leftX = 0.0f;
    }

    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)) {
        Sint16 axis_val = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        state.leftY = static_cast<float>(axis_val) / 32768.0f;
    } else {
        state.leftY = 0.0f;
    }

    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX)) {
        Sint16 axis_val = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        state.rightX = static_cast<float>(axis_val) / 32768.0f;
    } else {
        state.rightX = 0.0f;
    }

    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY)) {
        Sint16 axis_val = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        state.rightY = static_cast<float>(axis_val) / 32768.0f;
    } else {
        state.rightY = 0.0f;
    }

    // Triggers — check support via SDL_GameControllerHasAxis
    // SDL2 trigger axes return 0 (released) .. 32767 (fully pressed), NOT centered like stick axes
    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) {
        Sint16 axis_val = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        state.triggerLeft = static_cast<float>(axis_val) / 32767.0f;
    } else {
        if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
            state.triggerLeft = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 1.0f : 0.0f;
        } else {
            state.triggerLeft = 0.0f;
        }
    }

    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        Sint16 axis_val = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        state.triggerRight = static_cast<float>(axis_val) / 32767.0f;
    } else {
        if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            state.triggerRight = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1.0f : 0.0f;
        } else {
            state.triggerRight = 0.0f;
        }
    }

    // Apply deadzone to stick axes (but not to triggers)
    if (std::abs(state.leftX) < state.deadzone) state.leftX = 0.0f;
    if (std::abs(state.leftY) < state.deadzone) state.leftY = 0.0f;
    if (std::abs(state.rightX) < state.deadzone) state.rightX = 0.0f;
    if (std::abs(state.rightY) < state.deadzone) state.rightY = 0.0f;
}

// --- RefreshDeviceState with error handling ---
void WxGamepadBackend::RefreshDeviceState(int device_index) {
    if (device_index < 0 || device_index >= 16) return;

    if (m_connected[device_index] && m_controllers[device_index]) {
        UpdateDevice(device_index);
        return;
    }

    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (!controller) {
        const char* error = SDL_GetError();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to open game controller %d: %s", device_index,
            error ? error : "Unknown error");
        SDL_ClearError();
        return;
    }

    m_controllers[device_index] = controller;
    m_connected[device_index] = true;
    m_instance_ids[device_index] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    UpdateDevice(device_index);
}

// --- Global instance ---
WxGamepadBackend& GamepadBackend() {
    static WxGamepadBackend instance;
    return instance;
}

} // namespace xPlatform
