// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/hotkeys/HotkeyHelpers.hpp"

#include "controllers/hotkeys/ActionNames.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"

#include <QKeyCombination>
#include <QStringList>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace chatterino {

std::vector<QString> parseHotkeyArguments(QString argumentString)
{
    std::vector<QString> arguments;

    argumentString = argumentString.trimmed();

    if (argumentString.isEmpty())
    {

        return arguments;
    }

    auto argList = argumentString.split("\n");

    for (const auto &arg : argList)
    {
        arguments.push_back(arg.trimmed());
    }

    return arguments;
}

std::optional<ActionDefinition> findHotkeyActionDefinition(
    HotkeyCategory category, const QString &action)
{
    auto allActions = actionNames.find(category);
    if (allActions != actionNames.end())
    {
        const auto &actionsMap = allActions->second;
        auto definition = actionsMap.find(action);
        if (definition != actionsMap.end())
        {
            return {definition->second};
        }
    }
    return {};
}

QKeySequence normalizeKeySequence(const QKeySequence &seq)
{
    if (seq.isEmpty())
    {
        return seq;
    }

    bool needsNormalization = false;
    for (int i = 0; i < seq.count(); i++)
    {
        if (seq[i].key() == Qt::Key_Enter)
        {
            needsNormalization = true;
            break;
        }
    }

    if (!needsNormalization)
    {
        return seq;
    }

    std::array<QKeyCombination, 4> combos{};
    int count = seq.count();

    for (int i = 0; i < count; i++)
    {
        auto combo = seq[i];
        if (combo.key() == Qt::Key_Enter)
        {
            combos.at(i) =
                QKeyCombination(combo.keyboardModifiers(), Qt::Key_Return);
        }
        else
        {
            combos.at(i) = combo;
        }
    }

    switch (count)
    {
        case 1:
            return {combos.at(0)};
        case 2:
            return {combos.at(0), combos.at(1)};
        case 3:
            return {combos.at(0), combos.at(1), combos.at(2)};
        case 4:
        default:
            return {combos.at(0), combos.at(1), combos.at(2), combos.at(3)};
    }
}

std::optional<Qt::Key> physicalNumberRowKey(const QString &platformName,
                                         int logicalKey,
                                         quint32 nativeScanCode,
                                         quint32 nativeVirtualKey)
{
    if (logicalKey >= Qt::Key_0 && logicalKey <= Qt::Key_9)
    {
        return static_cast<Qt::Key>(logicalKey);
    }

    auto digitKey = [](int digit) {
        return static_cast<Qt::Key>(static_cast<int>(Qt::Key_0) + digit);
    };

    if (platformName.compare("windows", Qt::CaseInsensitive) == 0)
    {
        if (nativeVirtualKey >= '0' && nativeVirtualKey <= '9')
        {
            return digitKey(static_cast<int>(nativeVirtualKey - '0'));
        }

        const auto scanCode = nativeScanCode & 0xff;
        if (scanCode >= 0x02 && scanCode <= 0x0a)
        {
            return digitKey(static_cast<int>(scanCode - 0x01));
        }
        if (scanCode == 0x0b)
        {
            return Qt::Key_0;
        }
        return std::nullopt;
    }

    if (platformName.compare("cocoa", Qt::CaseInsensitive) == 0)
    {
        constexpr std::array<std::pair<quint32, int>, 10> COCOA_NUMBER_ROW{{
            {29, 0},
            {18, 1},
            {19, 2},
            {20, 3},
            {21, 4},
            {23, 5},
            {22, 6},
            {26, 7},
            {28, 8},
            {25, 9},
        }};
        const auto found =
            std::find_if(COCOA_NUMBER_ROW.begin(), COCOA_NUMBER_ROW.end(),
                         [nativeVirtualKey](const auto &entry) {
                             return entry.first == nativeVirtualKey;
                         });
        return found == COCOA_NUMBER_ROW.end()
                   ? std::nullopt
                   : std::optional<Qt::Key>{digitKey(found->second)};
    }

    if (platformName.compare("xcb", Qt::CaseInsensitive) == 0 ||
        platformName.startsWith("wayland", Qt::CaseInsensitive))
    {
        if (nativeScanCode >= 10 && nativeScanCode <= 18)
        {
            return digitKey(static_cast<int>(nativeScanCode - 9));
        }
        if (nativeScanCode == 19)
        {
            return Qt::Key_0;
        }
    }

    return std::nullopt;
}

std::vector<QString> remapIndexedHotkeyArguments(
    const std::vector<QString> &arguments, const std::vector<int> &oldToNew)
{
    if (arguments.size() != 1)
    {
        return arguments;
    }

    bool ok = false;
    const int oneBasedIndex = arguments.front().toInt(&ok);
    if (!ok || oneBasedIndex <= 0)
    {
        return arguments;
    }
    const int oldIndex = oneBasedIndex - 1;
    if (oldIndex >= static_cast<int>(oldToNew.size()))
    {
        return arguments;
    }

    const int newIndex = oldToNew.at(oldIndex);
    if (newIndex < 0 || newIndex >= static_cast<int>(oldToNew.size()))
    {
        return arguments;
    }

    return {QString::number(newIndex + 1)};
}

}
