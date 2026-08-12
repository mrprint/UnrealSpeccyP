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

#ifndef __JOYSTICK_MAPPER_H__
#define __JOYSTICK_MAPPER_H__

#pragma once

#ifdef USE_WXWIDGETS
#ifdef USE_SDL2_GAMEPAD

#include "wx_gamepad.h"
#include <string>
#include <map>
#include <vector>
#include <array>

namespace xPlatform {

class JoystickMapper {
public:
    struct EmulatedKeyEvent {
        char key;
        bool is_down;
    };

    // Преобразовать событие геймпада → эмулируемые клавиши по профилю игрока
    std::vector<EmulatedKeyEvent> ProcessEvent(
        const JoystickProfile& profile,
        int player_index,
        const GamepadState& current_state,
        int device_index);

private:
    struct PlayerInternalState {
        bool up = false, down = false, left = false, right = false;
        bool fire1 = false, fire2 = false;
    };

    std::array<PlayerInternalState, 2> m_player_states;

    bool IsSourceActive(const JoystickMappingEntry& entry, const GamepadState& state) const;
};

// --- Функции сериализации/десериализации мэппинга ---
std::string SerializeMapping(const std::map<EEmulatedJoystickInput, JoystickMappingEntry>& mapping);
bool DeserializeMapping(const std::string& data, std::map<EEmulatedJoystickInput, JoystickMappingEntry>& out_mapping);

// --- Сериализация/десериализация всего профиля игрока (мэппинг + GUID
// устройства). GUID хранится в самой строке мэппинга (префикс "GUID:...;"),
// чтобы не заводить отдельную опцию в xOptions под него — этот код и так уже
// единственный владелец формата этой строки. host_device_index сюда не
// входит: это чисто рантайм-значение, которое каждый раз заново
// разрешается из GUID через ResolveDeviceIndexForGuid().
std::string SerializeProfile(const JoystickProfile& profile);
bool DeserializeProfile(const std::string& data, JoystickProfile& out_profile);

// --- Преобразование типа источника в строку и обратно ---
std::string SourceTypeToString(EHostSourceType type);
EHostSourceType StringToSourceType(const std::string& str);

// --- Устойчивая идентификация устройства ---
//
// SDL присваивает устройствам числовой device_index по порядку подключения,
// поэтому один и тот же физический геймпад может получить другой индекс
// после переподключения, перезапуска эмулятора или просто при подключении
// другого устройства раньше него. GUID (модель/vendor/product ID) остаётся
// стабильным для одной и той же модели устройства между сессиями — это то,
// к чему в реальности должен быть привязан профиль, а не к сиюминутному
// индексу в списке.
//
// Возвращает device_index устройства с данным guid среди сейчас подключённых,
// либо -1, если такого устройства сейчас нет. Если guid пуст (профиль был
// сохранён до появления привязки по GUID), выполняет одноразовую миграцию:
// если hinted_index сейчас указывает на реальное подключённое устройство,
// использует его и возвращает его GUID через out_resolved_guid, чтобы вызывающая
// сторона могла сохранить эту привязку на будущее.
//
// ВАЖНО: GUID идентифицирует МОДЕЛЬ устройства, а не конкретный физический
// экземпляр — SDL не даёт более точного стабильного идентификатора. Если
// подключено два одинаковых контроллера, различить их по GUID нельзя.
int ResolveDeviceIndexForGuid(const std::string& guid, int hinted_index, std::string* out_resolved_guid = nullptr);

} // namespace xPlatform

#endif // USE_SDL2_GAMEPAD
#endif // USE_WXWIDGETS

#endif // __JOYSTICK_MAPPER_H__
