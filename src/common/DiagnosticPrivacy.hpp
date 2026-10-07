#pragma once

#include <QUrl>

namespace chatterino::diagnostics {

inline QString presenceHost()
{
    static const auto host =
        QUrl(qEnvironmentVariable("MOLTORINO_HEARTBEAT_URL")).host();
    return host;
}

inline bool mayLogUrl(const QUrl &url)
{
    static const auto badgeHost =
        QUrl(qEnvironmentVariable("MOLTORINO_BADGE_SOCKET_URL")).host();
    const auto host = url.host();
    return host.compare(presenceHost(), Qt::CaseInsensitive) != 0 &&
           host.compare(badgeHost, Qt::CaseInsensitive) != 0 &&
           host.compare(QLatin1StringView("api.giphy.com"),
                        Qt::CaseInsensitive) != 0;
}

}
