// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "pajlada/settings/settinglistener.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QFont>
#include <QFontMetrics>

#include <unordered_map>
#include <vector>

namespace chatterino {

class Settings;
class Paths;

bool registerBundledFonts();
int getUsernameBoldness();
QFont makeResolvedFont(const QString &family, qreal pointSize, int weight,
                       bool italic = false);
QFont makeResolvedFont(const QFont &base, int weight);

enum class FontStyle : uint8_t {
    Tiny,
    ChatSmall,
    ChatMediumSmall,
    ChatMedium,
    ChatMediumBold,
    ChatMediumItalic,
    ChatLarge,
    ChatVeryLarge,

    TimestampMedium,

    UiMedium,
    UiMediumBold,
    UiTabs,

    ChatUsername,
    EndType,

    ChatStart = ChatSmall,
    ChatEnd = ChatVeryLarge,
};

struct FontAlignmentMetrics {
    qreal uppercaseCenterAboveBottom = 0;
    qreal lowercaseCenterAboveBottom = 0;
};

class Fonts final
{
public:
    explicit Fonts(Settings &settings);

    QFont getFont(FontStyle type, float scale);
    QFontMetricsF getFontMetrics(FontStyle type, float scale);

    const FontAlignmentMetrics &getUsernameAlignmentMetrics(float scale);

    pajlada::Signals::NoArgSignal fontChanged;

private:
    struct FontData {
        FontData(const QFont &_font)
            : font(_font)
            , metrics(_font)
        {
        }

        const QFont font;
        const QFontMetricsF metrics;
    };

    struct ChatFontData {
        float scale;
        bool italic;
    };

    struct UiFontData {
        float size;
        const char *name;
        bool italic;
        int weight;
    };

    FontData &getOrCreateFontData(FontStyle type, float scale);
    static FontData createFontData(FontStyle type, float scale);

    std::vector<std::unordered_map<float, FontData>> fontsByType_;

    std::unordered_map<float, FontAlignmentMetrics> usernameAlignmentsByScale_;
    pajlada::SettingListener fontChangedListener;
    pajlada::Signals::SignalHolder themeConnections_;
};

}
