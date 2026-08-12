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
#include "joystick_mapper.h"
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <map>
#include <functional>

namespace xPlatform {

// --- Serialize mapping to string ---
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
            oss << input_name << ":"
                << SourceTypeToString(pair.second.source_type);

            // Add threshold for axes and triggers
            if (pair.second.source_type >= EHostSourceType::AXIS_LEFT_X_POS &&
                pair.second.source_type <= EHostSourceType::TRIGGER_RIGHT) {
                oss << "@" << std::fixed << std::setprecision(2) << pair.second.threshold;
            }

            oss << ";";
        }
    }

    return oss.str();
}

// --- Deserialize mapping from string ---
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

        // Split into "INPUT:SOURCE[@threshold]"
        size_t colon_pos = entry_str.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string input_name = entry_str.substr(0, colon_pos);
        std::string source_part = entry_str.substr(colon_pos + 1);

        // Split SOURCE and @threshold
        float threshold = 0.5f;
        size_t at_pos = source_part.find('@');
        if (at_pos != std::string::npos) {
            try {
                threshold = std::stof(source_part.substr(at_pos + 1));
            } catch (...) {
                threshold = 0.5f;
            }
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

// --- Сериализация/десериализация профиля целиком (мэппинг + GUID) ---
std::string SerializeProfile(const JoystickProfile& profile) {
    std::ostringstream oss;
    if (!profile.device_guid.empty()) {
        oss << "GUID:" << profile.device_guid << ";";
    }
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

    // Unrecognized "INPUT" tokens are silently skipped by DeserializeMapping,
    // so this only ever fails to parse (returns non-true) on malformed data.
    return DeserializeMapping(remainder, out_profile.input_map);
}

// --- Преобразование типа источника в строку ---
std::string SourceTypeToString(EHostSourceType type) {
    switch (type) {
        // Buttons
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

        // D-pad/hat
        case EHostSourceType::HAT_UP:    return "DPAD_UP";
        case EHostSourceType::HAT_DOWN:  return "DPAD_DOWN";
        case EHostSourceType::HAT_LEFT:  return "DPAD_LEFT";
        case EHostSourceType::HAT_RIGHT: return "DPAD_RIGHT";

        // Stick axes
        case EHostSourceType::AXIS_LEFT_X_POS:   return "LEFT_STICK_RIGHT";
        case EHostSourceType::AXIS_LEFT_X_NEG:   return "LEFT_STICK_LEFT";
        case EHostSourceType::AXIS_LEFT_Y_POS:   return "LEFT_STICK_DOWN";
        case EHostSourceType::AXIS_LEFT_Y_NEG:   return "LEFT_STICK_UP";
        case EHostSourceType::AXIS_RIGHT_X_POS:  return "RIGHT_STICK_RIGHT";
        case EHostSourceType::AXIS_RIGHT_X_NEG:  return "RIGHT_STICK_LEFT";
        case EHostSourceType::AXIS_RIGHT_Y_POS:  return "RIGHT_STICK_DOWN";
        case EHostSourceType::AXIS_RIGHT_Y_NEG:  return "RIGHT_STICK_UP";

        // Triggers
        case EHostSourceType::TRIGGER_LEFT:   return "LT";
        case EHostSourceType::TRIGGER_RIGHT:  return "RT";

        default: return "NONE";
    }
}

// --- Преобразование строки в тип источника ---
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

// --- Реализация ProcessEvent с player_index ---
bool JoystickMapper::IsSourceActive(const JoystickMappingEntry& entry, const GamepadState& state) const {
    switch (entry.source_type) {
        // Buttons
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

        // D-pad (hat)
        case EHostSourceType::HAT_UP:    return (state.hat & SDL_HAT_UP) != 0;
        case EHostSourceType::HAT_DOWN:  return (state.hat & SDL_HAT_DOWN) != 0;
        case EHostSourceType::HAT_LEFT:  return (state.hat & SDL_HAT_LEFT) != 0;
        case EHostSourceType::HAT_RIGHT: return (state.hat & SDL_HAT_RIGHT) != 0;

        // Axes with activation threshold
        case EHostSourceType::AXIS_LEFT_X_POS:
            return state.GetAxisWithDeadzone(state.leftX) > entry.threshold;
        case EHostSourceType::AXIS_LEFT_X_NEG:
            return state.GetAxisWithDeadzone(state.leftX) < -entry.threshold;
        case EHostSourceType::AXIS_LEFT_Y_POS:
            return state.GetAxisWithDeadzone(state.leftY) > entry.threshold;
        case EHostSourceType::AXIS_LEFT_Y_NEG:
            return state.GetAxisWithDeadzone(state.leftY) < -entry.threshold;
        case EHostSourceType::AXIS_RIGHT_X_POS:
            return state.GetAxisWithDeadzone(state.rightX) > entry.threshold;
        case EHostSourceType::AXIS_RIGHT_X_NEG:
            return state.GetAxisWithDeadzone(state.rightX) < -entry.threshold;
        case EHostSourceType::AXIS_RIGHT_Y_POS:
            return state.GetAxisWithDeadzone(state.rightY) > entry.threshold;
        case EHostSourceType::AXIS_RIGHT_Y_NEG:
            return state.GetAxisWithDeadzone(state.rightY) < -entry.threshold;

        // Triggers
        case EHostSourceType::TRIGGER_LEFT:
            return state.triggerLeft > entry.threshold;
        case EHostSourceType::TRIGGER_RIGHT:
            return state.triggerRight > entry.threshold;

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

// --- Stable device identification by GUID (see comment in .h) ---
int ResolveDeviceIndexForGuid(const std::string& guid, int hinted_index, std::string* out_resolved_guid) {
    auto devices = GamepadBackend().EnumerateDevices();

    if (!guid.empty()) {
        for (const auto& dev : devices) {
            if (dev.guid == guid) {
                if (out_resolved_guid) *out_resolved_guid = guid;
                return dev.index;
            }
        }
        // GUID is known but the device with it is not currently connected — do
        // not lose the binding, just nothing to poll right now.
        if (out_resolved_guid) *out_resolved_guid = guid;
        return -1;
    }

    // Profile without GUID — saved before GUID-based binding appeared. One-time
    // migration: if the hinted index currently points to a real
    // device, remember its GUID for the future.
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
