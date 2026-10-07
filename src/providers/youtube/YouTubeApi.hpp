#pragma once

#include "providers/youtube/YouTubeTypes.hpp"
#include "util/Expected.hpp"

#include <QByteArrayView>
#include <QDateTime>
#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace chatterino {

enum class YouTubeReadTransport : std::uint8_t {
    Innertube,
    Relay,
};

struct YouTubeResolvedChannel {
    QString channelID;
    QString displayName;
    QString videoID;
    QString liveChatID;
    QString streamTitle;
    QString readTicket;
    QDateTime scheduledStartTime;
    YouTubeReadTransport readTransport = YouTubeReadTransport::Relay;
    bool isLive = false;
    bool isUpcoming = false;
};

struct YouTubeLiveProbe {
    QString videoID;
    bool isLive = false;
};

namespace youtube::detail {

struct PublicLivePageState {
    QString videoID;
    bool valid = false;
    bool isLiveNow = false;
    bool hasLiveContent = false;
};

PublicLivePageState parsePublicLivePage(QByteArrayView page);

}

struct YouTubeOwnChannel {
    QString channelID;
    QString handle;
    QString displayName;
    QString avatarUrl;
};

struct YouTubeTokenResponse {
    QString accessToken;
    QString refreshToken;
    QDateTime expiresAt;
    QString grantedScope;
};

struct YouTubeApiError {
    QString message;
    QString code;
    QString reason;
    int httpStatus = 0;

    bool isUnauthorized() const
    {
        return this->httpStatus == 401;
    }

    bool isInvalidGrant() const
    {
        return this->code.compare(QStringLiteral("invalid_grant"),
                                  Qt::CaseInsensitive) == 0;
    }
};

namespace youtube::detail {

bool shouldRetryLiveChatBanDelete(const YouTubeApiError &error);

std::optional<QString> findMatchingLiveChatMessageID(
    const std::vector<YouTubeMessage> &candidates, QStringView authorChannelID,
    QStringView messageText, const QDateTime &publishedAt);

bool shouldResolveLiveChatMessageDelete(const YouTubeApiError &error);

}

class YouTubeApi
{
public:
    template <typename T>
    using Callback = std::function<void(ExpectedStr<T>)>;

    template <typename T>
    using AuthenticatedCallback =
        std::function<void(Expected<T, YouTubeApiError>)>;

    static bool isConfigured();
    static bool isOAuthConfigured();
    static QString normalizeSource(const QString &source);

    static void resolveSource(const QString &source,
                              Callback<YouTubeResolvedChannel> callback);
    static void probeLiveSource(const QString &source,
                                Callback<YouTubeLiveProbe> callback);

    static void getOwnChannelAuthenticated(
        const QString &accessToken,
        AuthenticatedCallback<YouTubeOwnChannel> callback);
    static void exchangeCode(
        const QString &code, const QString &verifier,
        const QString &redirectUri,
        AuthenticatedCallback<YouTubeTokenResponse> callback);
    static void refreshAccessToken(
        const QString &refreshToken,
        AuthenticatedCallback<YouTubeTokenResponse> callback);
    static void revokeToken(const QString &token,
                            AuthenticatedCallback<void> callback);

    static void sendMessage(const QString &liveChatID, const QString &message,
                            const QString &accessToken,
                            AuthenticatedCallback<YouTubeMessage> callback);
    static void getRecentLiveChatMessages(
        const QString &liveChatID, const QString &accessToken,
        AuthenticatedCallback<std::vector<YouTubeMessage>> callback);
    static void deleteMessage(const QString &messageID,
                              const QString &accessToken,
                              AuthenticatedCallback<void> callback);
    static void banUser(const QString &liveChatID, const QString &channelID,
                        std::optional<std::chrono::seconds> duration,
                        const QString &accessToken,
                        AuthenticatedCallback<QString> callback);
    static void unbanUser(const QString &banID, const QString &accessToken,
                          AuthenticatedCallback<void> callback);
};

}
