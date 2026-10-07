#pragma once

#include "providers/youtube/YouTubeTypes.hpp"

#include <QString>
#include <QStringView>

#include <chrono>
#include <optional>
#include <vector>

class QJsonObject;

namespace chatterino {

struct YouTubeInnertubeParseResult {
    std::vector<YouTubeMessage> messages;
    QString nextContinuation;
    QString clickTrackingParams;
    std::optional<std::chrono::milliseconds> serverTimeout;
    QString error;
    bool ended = false;
    bool valid = false;
};

[[nodiscard]] YouTubeInnertubeParseResult parseYouTubeInnertubeChat(
    const QJsonObject &response, QStringView liveChatID);

}
