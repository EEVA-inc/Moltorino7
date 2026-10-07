#include "providers/youtube/YouTubeCredentialStore.hpp"

#include "common/AccountCredentials.hpp"

#include <utility>

namespace chatterino {

QString youtubeCredentialKey(const QString &channelID)
{
    return QStringLiteral("youtube:%1:refresh-token").arg(channelID);
}

ExpectedStr<QString> parseYouTubeCredential(const QString &stored)
{
    if (stored.isEmpty())
    {
        return makeUnexpected(QStringLiteral("Credential was empty."));
    }
    if (stored.trimmed().startsWith(u'{'))
    {
        return makeUnexpected(QStringLiteral(
            "This saved YouTube login is no longer supported. "
            "Reconnect the account with Google."));
    }
    return stored;
}

void writeYouTubeCredential(const QString &channelID,
                            const QString &refreshToken,
                            std::function<void(ExpectedStr<void>)> callback)
{
    accountCredentials().write(youtubeCredentialKey(channelID), refreshToken,
                               std::move(callback));
}

}
