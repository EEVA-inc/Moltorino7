// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <cstdint>
#include <optional>

namespace chatterino {

namespace env {
inline constexpr const char *LOG_TO_FILE = "CHATTERINO_LOG_TO_FILE";
}

class Env
{
    Env();

public:
    static const Env &get();

    const QString recentMessagesApiUrl;
    const QString linkResolverUrl;
    const QString twitchServerHost;
    const uint16_t twitchServerPort;
    const bool twitchServerSecure;
    const std::optional<QString> proxyUrl;
    const QString logToFile;
};

}
