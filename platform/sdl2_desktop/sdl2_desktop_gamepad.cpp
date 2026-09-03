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

// Port of platform/wxwidgets/wx_gamepad.cpp + joystick_mapper.cpp - see the
// header for why this is a copy rather than a shared #include.

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP
#ifdef SDL_USE_JOYSTICK

#include "sdl2_desktop_gamepad.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <SDL_log.h>

namespace xPlatform {

// ---------------------------------------------------------------------------
// WxGamepadBackend
// ---------------------------------------------------------------------------

void WxGamepadBackend::Initialize() {
    // SDL_INIT_GAMECONTROLLER is initialized by sdl2_desktop.cpp::Init().

    if (!(SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL2 GameController subsystem not initialized");
        return;
    }

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

int WxGamepadBackend::SlotForInstanceId(SDL_JoystickID instance_id) const {
    for (int i = 0; i < 16; ++i) {
        if (m_connected[i] && m_instance_ids[i] == instance_id)
            return i;
    }
    return -1;
}

void WxGamepadBackend::HandleControllerEvent(const SDL_Event& event,
    std::function<void(int)> on_device_added,
    std::function<void(int)> on_device_removed) {

    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            int which = event.cdevice.which; // device index for ADDED

            if (which >= 0 && which < 16 && m_connected[which])
                break;

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
            int slot = SlotForInstanceId(event.caxis.which);
            if (slot >= 0) UpdateDevice(slot);
            break;
        }

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            int slot = SlotForInstanceId(event.cbutton.which);
            if (slot >= 0) UpdateDevice(slot);
            break;
        }

        default:
            break;
    }
}

void WxGamepadBackend::PollEvents(
    std::function<void(int)> on_device_added,
    std::function<void(int)> on_device_removed) {
    // Not used by sdl2_desktop.cpp (it owns the single SDL_PollEvent() loop
    // and calls HandleControllerEvent() per-event instead), kept for parity
    // with the ported source and in case some future caller wants it.
    SDL_Event event;
    while (SDL_PollEvent(&event))
        HandleControllerEvent(event, on_device_added, on_device_removed);
}

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

const GamepadState& WxGamepadBackend::GetState(int device_index) const {
    static const GamepadState empty_state{};

    if (device_index >= 0 && device_index < 16 && m_connected[device_index])
        return m_states[device_index];

    return empty_state;
}

void WxGamepadBackend::UpdateDevice(int device_index) {
    if (!m_controllers[device_index] || !m_connected[device_index]) return;

    SDL_GameController* gc = m_controllers[device_index];
    GamepadState& state = m_states[device_index];

    SDL_GameControllerUpdate();

    if (!SDL_GameControllerGetAttached(gc)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Gamepad %d disconnected during update", device_index);
        m_connected[device_index] = false;
        return;
    }

    state.a = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0;
    state.b = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B) != 0;
    state.x = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X) != 0;
    state.y = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y) != 0;

    state.back = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK) != 0;
    state.start = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START) != 0;

    state.leftstick = SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK)
        ? SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0 : false;
    state.rightstick = SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)
        ? SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0 : false;
    state.leftshoulder = SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        ? SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0 : false;
    state.rightshoulder = SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        ? SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0 : false;

    Uint8 hat = 0;
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) &&
        SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))
        hat |= SDL_HAT_UP;
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) &&
        SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        hat |= SDL_HAT_DOWN;
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) &&
        SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        hat |= SDL_HAT_LEFT;
    if (SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) &&
        SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        hat |= SDL_HAT_RIGHT;
    state.hat = hat;

    state.leftX = SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)
        ? static_cast<float>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)) / 32768.0f : 0.0f;
    state.leftY = SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)
        ? static_cast<float>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)) / 32768.0f : 0.0f;
    state.rightX = SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX)
        ? static_cast<float>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX)) / 32768.0f : 0.0f;
    state.rightY = SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY)
        ? static_cast<float>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY)) / 32768.0f : 0.0f;

    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT))
        state.triggerLeft = static_cast<float>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) / 32767.0f;
    else
        state.triggerLeft = SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            ? (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 1.0f : 0.0f) : 0.0f;

    if (SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT))
        state.triggerRight = static_cast<float>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) / 32767.0f;
    else
        state.triggerRight = SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            ? (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1.0f : 0.0f) : 0.0f;

    if (std::abs(state.leftX) < state.deadzone) state.leftX = 0.0f;
    if (std::abs(state.leftY) < state.deadzone) state.leftY = 0.0f;
    if (std::abs(state.rightX) < state.deadzone) state.rightX = 0.0f;
    if (std::abs(state.rightY) < state.deadzone) state.rightY = 0.0f;
}

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

WxGamepadBackend& GamepadBackend() {
    static WxGamepadBackend instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string SerializeMapping(const std::map<EEmulatedJoystickInput, JoystickMappingEntry>& mapping) {
    if (mapping.empty()) return "";
    std::ostringstream oss;
    for (const auto& pair : mapping) {
        const char* input_name = nullptr;
        switch (pair.first) {
            case EEmulatedJoystickInput::UP:    input_name = "UP"; break;
            case EEmulatedJoystickInput::DOWN:  input_name = "DOWN"; break;
            case EEmulatedJoystickInput::LEFT:  input_name = "LEFT"; break;
            case EEmulatedJoystickInput::RIGHT: input_name = "RIGHT"; break;
            case EEmulatedJoystickInput::FIRE1: input_name = "FIRE1"; break;
            case EEmulatedJoystickInput::FIRE2: input_name = "FIRE2"; break;
        }
        if (input_name) {
            oss << input_name << ":" << SourceTypeToString(pair.second.source_type);
            if (pair.second.source_type >= EHostSourceType::AXIS_LEFT_X_POS &&
                pair.second.source_type <= EHostSourceType::TRIGGER_RIGHT) {
                oss << "@" << std::fixed << std::setprecision(2) << pair.second.threshold;
            }
            oss << ";";
        }
    }
    return oss.str();
}

bool DeserializeMapping(const std::string& data,
                        std::map<EEmulatedJoystickInput, JoystickMappingEntry>& out_mapping) {
    out_mapping.clear();
    if (data.empty()) return true;

    size_t pos = 0;
    while ((pos = data.find_first_not_of(';', pos)) != std::string::npos) {
        size_t end = data.find(';', pos);
        if (end == std::string::npos) end = data.length();

        std::string entry_str = data.substr(pos, end - pos);
        pos = end;

        size_t colon_pos = entry_str.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string input_name = entry_str.substr(0, colon_pos);
        std::string source_part = entry_str.substr(colon_pos + 1);

        float threshold = 0.5f;
        size_t at_pos = source_part.find('@');
        if (at_pos != std::string::npos) {
            try { threshold = std::stof(source_part.substr(at_pos + 1)); }
            catch (...) { threshold = 0.5f; }
            source_part = source_part.substr(0, at_pos);
        }

        EEmulatedJoystickInput input;
        if (input_name == "UP")       input = EEmulatedJoystickInput::UP;
        else if (input_name == "DOWN")  input = EEmulatedJoystickInput::DOWN;
        else if (input_name == "LEFT")  input = EEmulatedJoystickInput::LEFT;
        else if (input_name == "RIGHT") input = EEmulatedJoystickInput::RIGHT;
        else if (input_name == "FIRE1") input = EEmulatedJoystickInput::FIRE1;
        else if (input_name == "FIRE2") input = EEmulatedJoystickInput::FIRE2;
        else continue;

        JoystickMappingEntry entry;
        entry.source_type = StringToSourceType(source_part);
        entry.threshold = threshold;
        out_mapping[input] = entry;
    }
    return true;
}

std::string SerializeProfile(const JoystickProfile& profile) {
    std::ostringstream oss;
    if (!profile.device_guid.empty())
        oss << "GUID:" << profile.device_guid << ";";
    oss << SerializeMapping(profile.input_map);
    return oss.str();
}

bool DeserializeProfile(const std::string& data, JoystickProfile& out_profile) {
    out_profile.input_map.clear();
    out_profile.device_guid.clear();

    if (data.empty()) return true;

    std::string remainder = data;
    if (remainder.rfind("GUID:", 0) == 0) {
        size_t semi = remainder.find(';');
        if (semi == std::string::npos) {
            out_profile.device_guid = remainder.substr(5);
            remainder.clear();
        } else {
            out_profile.device_guid = remainder.substr(5, semi - 5);
            remainder = remainder.substr(semi + 1);
        }
    }
    return DeserializeMapping(remainder, out_profile.input_map);
}

std::string SourceTypeToString(EHostSourceType type) {
    switch (type) {
        case EHostSourceType::BUTTON_A:            return "A";
        case EHostSourceType::BUTTON_B:            return "B";
        case EHostSourceType::BUTTON_X:            return "X";
        case EHostSourceType::BUTTON_Y:            return "Y";
        case EHostSourceType::BUTTON_BACK:         return "BACK";
        case EHostSourceType::BUTTON_START:        return "START";
        case EHostSourceType::BUTTON_LEFTSTICK:    return "LS";
        case EHostSourceType::BUTTON_RIGHTSTICK:   return "RS";
        case EHostSourceType::BUTTON_LEFTSHOULDER: return "LB";
        case EHostSourceType::BUTTON_RIGHTSHOULDER: return "RB";
        case EHostSourceType::HAT_UP:    return "DPAD_UP";
        case EHostSourceType::HAT_DOWN:  return "DPAD_DOWN";
        case EHostSourceType::HAT_LEFT:  return "DPAD_LEFT";
        case EHostSourceType::HAT_RIGHT: return "DPAD_RIGHT";
        case EHostSourceType::AXIS_LEFT_X_POS:   return "LEFT_STICK_RIGHT";
        case EHostSourceType::AXIS_LEFT_X_NEG:   return "LEFT_STICK_LEFT";
        case EHostSourceType::AXIS_LEFT_Y_POS:   return "LEFT_STICK_DOWN";
        case EHostSourceType::AXIS_LEFT_Y_NEG:   return "LEFT_STICK_UP";
        case EHostSourceType::AXIS_RIGHT_X_POS:  return "RIGHT_STICK_RIGHT";
        case EHostSourceType::AXIS_RIGHT_X_NEG:  return "RIGHT_STICK_LEFT";
        case EHostSourceType::AXIS_RIGHT_Y_POS:  return "RIGHT_STICK_DOWN";
        case EHostSourceType::AXIS_RIGHT_Y_NEG:  return "RIGHT_STICK_UP";
        case EHostSourceType::TRIGGER_LEFT:   return "LT";
        case EHostSourceType::TRIGGER_RIGHT:  return "RT";
        default: return "NONE";
    }
}

const char* SourceTypeDisplayString(EHostSourceType type) {
    switch (type) {
        case EHostSourceType::BUTTON_A:            return "A";
        case EHostSourceType::BUTTON_B:            return "B";
        case EHostSourceType::BUTTON_X:            return "X";
        case EHostSourceType::BUTTON_Y:            return "Y";
        case EHostSourceType::BUTTON_BACK:         return "Back";
        case EHostSourceType::BUTTON_START:        return "Start";
        case EHostSourceType::BUTTON_LEFTSTICK:    return "L Stick";
        case EHostSourceType::BUTTON_RIGHTSTICK:   return "R Stick";
        case EHostSourceType::BUTTON_LEFTSHOULDER: return "L Bumper";
        case EHostSourceType::BUTTON_RIGHTSHOULDER: return "R Bumper";
        case EHostSourceType::HAT_UP:    return "DPad Up";
        case EHostSourceType::HAT_DOWN:  return "DPad Down";
        case EHostSourceType::HAT_LEFT:  return "DPad Left";
        case EHostSourceType::HAT_RIGHT: return "DPad Right";
        case EHostSourceType::AXIS_LEFT_X_POS:   return "L Stick Right";
        case EHostSourceType::AXIS_LEFT_X_NEG:   return "L Stick Left";
        case EHostSourceType::AXIS_LEFT_Y_POS:   return "L Stick Down";
        case EHostSourceType::AXIS_LEFT_Y_NEG:   return "L Stick Up";
        case EHostSourceType::AXIS_RIGHT_X_POS:  return "R Stick Right";
        case EHostSourceType::AXIS_RIGHT_X_NEG:  return "R Stick Left";
        case EHostSourceType::AXIS_RIGHT_Y_POS:  return "R Stick Down";
        case EHostSourceType::AXIS_RIGHT_Y_NEG:  return "R Stick Up";
        case EHostSourceType::TRIGGER_LEFT:   return "L Trigger";
        case EHostSourceType::TRIGGER_RIGHT:  return "R Trigger";
        default: return "None";
    }
}

EHostSourceType StringToSourceType(const std::string& str) {
    if (str == "A")           return EHostSourceType::BUTTON_A;
    if (str == "B")           return EHostSourceType::BUTTON_B;
    if (str == "X")           return EHostSourceType::BUTTON_X;
    if (str == "Y")           return EHostSourceType::BUTTON_Y;
    if (str == "BACK")        return EHostSourceType::BUTTON_BACK;
    if (str == "START")       return EHostSourceType::BUTTON_START;
    if (str == "LS")          return EHostSourceType::BUTTON_LEFTSTICK;
    if (str == "RS")          return EHostSourceType::BUTTON_RIGHTSTICK;
    if (str == "LB")          return EHostSourceType::BUTTON_LEFTSHOULDER;
    if (str == "RB")          return EHostSourceType::BUTTON_RIGHTSHOULDER;
    if (str == "DPAD_UP")     return EHostSourceType::HAT_UP;
    if (str == "DPAD_DOWN")   return EHostSourceType::HAT_DOWN;
    if (str == "DPAD_LEFT")   return EHostSourceType::HAT_LEFT;
    if (str == "DPAD_RIGHT")  return EHostSourceType::HAT_RIGHT;
    if (str == "LEFT_STICK_RIGHT")  return EHostSourceType::AXIS_LEFT_X_POS;
    if (str == "LEFT_STICK_LEFT")   return EHostSourceType::AXIS_LEFT_X_NEG;
    if (str == "LEFT_STICK_DOWN")   return EHostSourceType::AXIS_LEFT_Y_POS;
    if (str == "LEFT_STICK_UP")     return EHostSourceType::AXIS_LEFT_Y_NEG;
    if (str == "RIGHT_STICK_RIGHT") return EHostSourceType::AXIS_RIGHT_X_POS;
    if (str == "RIGHT_STICK_LEFT")  return EHostSourceType::AXIS_RIGHT_X_NEG;
    if (str == "RIGHT_STICK_DOWN")  return EHostSourceType::AXIS_RIGHT_Y_POS;
    if (str == "RIGHT_STICK_UP")    return EHostSourceType::AXIS_RIGHT_Y_NEG;
    if (str == "LT")          return EHostSourceType::TRIGGER_LEFT;
    if (str == "RT")          return EHostSourceType::TRIGGER_RIGHT;
    return EHostSourceType::NONE;
}

// ---------------------------------------------------------------------------
// JoystickMapper
// ---------------------------------------------------------------------------

bool JoystickMapper::IsSourceActive(const JoystickMappingEntry& entry, const GamepadState& state) const {
    switch (entry.source_type) {
        case EHostSourceType::BUTTON_A:            return state.a;
        case EHostSourceType::BUTTON_B:            return state.b;
        case EHostSourceType::BUTTON_X:            return state.x;
        case EHostSourceType::BUTTON_Y:            return state.y;
        case EHostSourceType::BUTTON_BACK:         return state.back;
        case EHostSourceType::BUTTON_START:        return state.start;
        case EHostSourceType::BUTTON_LEFTSTICK:    return state.leftstick;
        case EHostSourceType::BUTTON_RIGHTSTICK:   return state.rightstick;
        case EHostSourceType::BUTTON_LEFTSHOULDER: return state.leftshoulder;
        case EHostSourceType::BUTTON_RIGHTSHOULDER: return state.rightshoulder;
        case EHostSourceType::HAT_UP:    return (state.hat & SDL_HAT_UP) != 0;
        case EHostSourceType::HAT_DOWN:  return (state.hat & SDL_HAT_DOWN) != 0;
        case EHostSourceType::HAT_LEFT:  return (state.hat & SDL_HAT_LEFT) != 0;
        case EHostSourceType::HAT_RIGHT: return (state.hat & SDL_HAT_RIGHT) != 0;
        case EHostSourceType::AXIS_LEFT_X_POS:  return state.GetAxisWithDeadzone(state.leftX) > entry.threshold;
        case EHostSourceType::AXIS_LEFT_X_NEG:  return state.GetAxisWithDeadzone(state.leftX) < -entry.threshold;
        case EHostSourceType::AXIS_LEFT_Y_POS:  return state.GetAxisWithDeadzone(state.leftY) > entry.threshold;
        case EHostSourceType::AXIS_LEFT_Y_NEG:  return state.GetAxisWithDeadzone(state.leftY) < -entry.threshold;
        case EHostSourceType::AXIS_RIGHT_X_POS: return state.GetAxisWithDeadzone(state.rightX) > entry.threshold;
        case EHostSourceType::AXIS_RIGHT_X_NEG: return state.GetAxisWithDeadzone(state.rightX) < -entry.threshold;
        case EHostSourceType::AXIS_RIGHT_Y_POS: return state.GetAxisWithDeadzone(state.rightY) > entry.threshold;
        case EHostSourceType::AXIS_RIGHT_Y_NEG: return state.GetAxisWithDeadzone(state.rightY) < -entry.threshold;
        case EHostSourceType::TRIGGER_LEFT:  return state.triggerLeft > entry.threshold;
        case EHostSourceType::TRIGGER_RIGHT: return state.triggerRight > entry.threshold;
        default: return false;
    }
}

std::vector<JoystickMapper::EmulatedKeyEvent> JoystickMapper::ProcessEvent(
    const JoystickProfile& profile,
    int player_index,
    const GamepadState& current_state,
    int device_index) {

    std::vector<EmulatedKeyEvent> result;

    if (!profile.IsEnabled() || profile.host_device_index != device_index)
        return result;
    if (player_index < 0 || player_index >= 2)
        return result;

    auto& internal_state = m_player_states[player_index];

    struct InputCheck {
        EEmulatedJoystickInput input;
        bool PlayerInternalState::* state_ptr;
        char key;
    };

    static const std::array<InputCheck, 6> checks = {{
        {EEmulatedJoystickInput::UP,    &PlayerInternalState::up,    'u'},
        {EEmulatedJoystickInput::DOWN,  &PlayerInternalState::down,  'd'},
        {EEmulatedJoystickInput::LEFT,  &PlayerInternalState::left,  'l'},
        {EEmulatedJoystickInput::RIGHT, &PlayerInternalState::right, 'r'},
        {EEmulatedJoystickInput::FIRE1, &PlayerInternalState::fire1, 'f'},
        {EEmulatedJoystickInput::FIRE2, &PlayerInternalState::fire2, 'e'}
    }};

    for (const auto& check : checks) {
        auto it = profile.input_map.find(check.input);
        if (it == profile.input_map.end()) continue;

        bool new_state = IsSourceActive(it->second, current_state);
        bool old_state = internal_state.*(check.state_ptr);

        if (new_state != old_state) {
            result.push_back({check.key, new_state});
            internal_state.*(check.state_ptr) = new_state;
        }
    }
    return result;
}

std::vector<JoystickMapper::EmulatedKeyEvent> JoystickMapper::ReleaseAll(int player_index) {
    std::vector<EmulatedKeyEvent> result;

    if (player_index < 0 || player_index >= 2)
        return result;

    auto& internal_state = m_player_states[player_index];

    struct HeldKey {
        bool PlayerInternalState::* state_ptr;
        char key;
    };

    static const std::array<HeldKey, 6> keys = {{
        {&PlayerInternalState::up,    'u'},
        {&PlayerInternalState::down,  'd'},
        {&PlayerInternalState::left,  'l'},
        {&PlayerInternalState::right, 'r'},
        {&PlayerInternalState::fire1, 'f'},
        {&PlayerInternalState::fire2, 'e'}
    }};

    for (const auto& k : keys) {
        if (internal_state.*(k.state_ptr)) {
            result.push_back({k.key, false});
            internal_state.*(k.state_ptr) = false;
        }
    }
    return result;
}

int ResolveDeviceIndexForGuid(const std::string& guid, int hinted_index, std::string* out_resolved_guid) {
    auto devices = GamepadBackend().EnumerateDevices();

    if (!guid.empty()) {
        for (const auto& dev : devices) {
            if (dev.guid == guid) {
                if (out_resolved_guid) *out_resolved_guid = guid;
                return dev.index;
            }
        }
        if (out_resolved_guid) *out_resolved_guid = guid;
        return -1;
    }

    if (hinted_index >= 0) {
        for (const auto& dev : devices) {
            if (dev.index == hinted_index) {
                if (out_resolved_guid) *out_resolved_guid = dev.guid;
                return dev.index;
            }
        }
    }

    if (out_resolved_guid) out_resolved_guid->clear();
    return -1;
}

} // namespace xPlatform

#endif // SDL_USE_JOYSTICK
#endif // USE_SDL2_DESKTOP
