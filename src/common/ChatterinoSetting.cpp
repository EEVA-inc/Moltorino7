// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/ChatterinoSetting.hpp"

#include "singletons/Settings.hpp"

namespace chatterino {

bool validStructuredSettingNumbers(const rapidjson::Value &value)
{
    if (value.IsNumber())
    {
        return value.IsInt();
    }
    if (value.IsArray())
    {
        for (const auto &item : value.GetArray())
        {
            if (!validStructuredSettingNumbers(item))
            {
                return false;
            }
        }
    }
    if (value.IsObject())
    {
        for (const auto &item : value.GetObject())
        {
            if (!validStructuredSettingNumbers(item.value))
            {
                return false;
            }
        }
    }
    return true;
}

void _registerSetting(std::weak_ptr<pajlada::Settings::SettingData> setting,
                      SettingResetter resetToDefault, SettingValidator validate)
{
    _actuallyRegisterSetting(std::move(setting), std::move(resetToDefault),
                             validate);
}

}
