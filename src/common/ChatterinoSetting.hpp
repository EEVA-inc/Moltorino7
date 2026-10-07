// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QMagicEnum.hpp"
#include "util/RapidJsonSerializeQSize.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/settings.hpp>
#include <QSize>
#include <QString>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

namespace chatterino {

using SettingResetter = std::function<void(const std::string &)>;
using SettingValidator = bool (*)(const rapidjson::Value &);

bool validStructuredSettingNumbers(const rapidjson::Value &value);

template <typename Type>
bool validateSettingValue(const rapidjson::Value &value)
{
    if constexpr (std::is_floating_point_v<Type>)
    {
        return value.IsNumber() && std::isfinite(value.GetDouble()) &&
               std::abs(value.GetDouble()) <= std::numeric_limits<Type>::max();
    }
    else
    {
        if (!validStructuredSettingNumbers(value))
        {
            return false;
        }
        bool error = false;
        const auto decoded = pajlada::Deserialize<Type>::get(value, &error);
        if (error)
        {
            return false;
        }
        rapidjson::Document document;
        return pajlada::Serialize<Type>::get(decoded,
                                             document.GetAllocator()) == value;
    }
}

void _registerSetting(std::weak_ptr<pajlada::Settings::SettingData> setting,
                      SettingResetter resetToDefault,
                      SettingValidator validate);

template <typename Type>
SettingResetter settingResetter(Type defaultValue)
{
    return [defaultValue = std::move(defaultValue)](const std::string &path) {
        pajlada::Settings::Setting<Type> setting(path, defaultValue);
        setting.resetToDefaultValue();
    };
}

template <typename Type>
class ChatterinoSetting : public pajlada::Settings::Setting<Type>
{
public:
    ChatterinoSetting(const std::string &path)
        : pajlada::Settings::Setting<Type>(
              path, pajlada::Settings::SettingOption::CompareBeforeSet)
    {
        _registerSetting(this->getData(), settingResetter(Type{}),
                         validateSettingValue<Type>);
    }

    ChatterinoSetting(const std::string &path, const Type &defaultValue)
        : pajlada::Settings::Setting<Type>(
              path, defaultValue,
              pajlada::Settings::SettingOption::CompareBeforeSet)
    {
        _registerSetting(this->getData(), settingResetter(defaultValue),
                         validateSettingValue<Type>);
    }

    template <typename T2>
    ChatterinoSetting &operator=(const T2 &newValue)
    {
        this->setValue(newValue);

        return *this;
    }

    ChatterinoSetting &operator=(Type &&newValue) noexcept
    {
        pajlada::Settings::Setting<Type>::operator=(newValue);

        return *this;
    }

    using pajlada::Settings::Setting<Type>::operator==;
    using pajlada::Settings::Setting<Type>::operator!=;

    using pajlada::Settings::Setting<Type>::operator Type;
};

using BoolSetting = ChatterinoSetting<bool>;
using FloatSetting = ChatterinoSetting<float>;
using DoubleSetting = ChatterinoSetting<double>;
using IntSetting = ChatterinoSetting<int>;
using UInt64Setting = ChatterinoSetting<uint64_t>;
using StringSetting = ChatterinoSetting<std::string>;
using QStringSetting = ChatterinoSetting<QString>;
using QSizeSetting = ChatterinoSetting<QSize>;

template <typename Enum>
class EnumSetting : public ChatterinoSetting<std::underlying_type_t<Enum>>
{
    using Underlying = std::underlying_type_t<Enum>;

public:
    using ChatterinoSetting<Underlying>::ChatterinoSetting;

    EnumSetting(const std::string &path, const Enum &defaultValue)
        : ChatterinoSetting<Underlying>(path, Underlying(defaultValue))
    {
    }

    EnumSetting<Enum> &operator=(Enum newValue)
    {
        this->setValue(Underlying(newValue));

        return *this;
    }

    operator Enum()
    {
        return Enum(this->getValue());
    }

    Enum getEnum()
    {
        return Enum(this->getValue());
    }
};

template <typename Enum>
class EnumStringSetting : public pajlada::Settings::Setting<QString>
{
public:
    EnumStringSetting(const std::string &path, const Enum &defaultValue_)
        : pajlada::Settings::Setting<QString>(path)
        , defaultValue(defaultValue_)
    {
        _registerSetting(this->getData(), settingResetter(QString{}),
                         validateSettingValue<QString>);
    }

    template <typename T2>
    EnumStringSetting<Enum> &operator=(Enum newValue)
    {
        this->setValue(qmagicenum::enumNameString(newValue).toLower());

        return *this;
    }

    EnumStringSetting<Enum> &operator=(QString newValue)
    {
        this->setValue(newValue.toLower());

        return *this;
    }

    operator Enum()
    {
        return this->getEnum();
    }

    Enum getEnum() const
    {
        return qmagicenum::enumCast<Enum>(this->getValue(),
                                          qmagicenum::CASE_INSENSITIVE)
            .value_or(this->defaultValue);
    }

    Enum defaultValue;

    using pajlada::Settings::Setting<QString>::operator==;
    using pajlada::Settings::Setting<QString>::operator!=;

    using pajlada::Settings::Setting<QString>::operator QString;
};

template <typename T>
struct IsChatterinoSettingT : std::false_type {
};
template <typename T>
struct IsChatterinoSettingT<ChatterinoSetting<T>> : std::true_type {
};
template <typename T>
struct IsChatterinoSettingT<EnumStringSetting<T>> : std::true_type {
};

template <typename T>
concept IsChatterinoSetting = IsChatterinoSettingT<T>::value;

}
