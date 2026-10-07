#pragma once

#include "util/Expected.hpp"

#include <QString>

#include <functional>

namespace chatterino {

[[nodiscard]] QString youtubeCredentialKey(const QString &channelID);
[[nodiscard]] ExpectedStr<QString> parseYouTubeCredential(
    const QString &stored);

void writeYouTubeCredential(const QString &channelID,
                            const QString &refreshToken,
                            std::function<void(ExpectedStr<void>)> callback);

}
