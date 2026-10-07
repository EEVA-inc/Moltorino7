#include "providers/youtube/YouTubeApi.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/youtube/YouTubeCredentials.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;
using namespace std::chrono_literals;

constexpr auto API_ROOT = "https://www.googleapis.com/youtube/v3/";
constexpr auto TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr auto REVOKE_URL = "https://oauth2.googleapis.com/revoke";
constexpr auto YOUTUBE_ROOT = "https://www.youtube.com/";

YouTubeApiError googleApiError(const NetworkResult &result)
{
    const auto root = result.parseJson();
    const auto errorValue = root.value("error");
    const auto errorObject = errorValue.toObject();

    YouTubeApiError error{
        .httpStatus = result.status().value_or(0),
    };
    if (errorValue.isString())
    {
        error.code = errorValue.toString();
    }
    else if (!errorObject.isEmpty())
    {
        error.code = errorObject.value("status").toString();
        if (error.code.isEmpty() && errorObject.value("code").isString())
        {
            error.code = errorObject.value("code").toString();
        }
        if (error.code.isEmpty() && errorObject.value("code").isDouble())
        {
            error.code = QString::number(errorObject.value("code").toInt());
        }
        const auto reasons = errorObject.value("errors").toArray();
        if (!reasons.isEmpty())
        {
            error.reason =
                reasons.first().toObject().value("reason").toString();
        }
        error.message = errorObject.value("message").toString();
    }

    if (error.message.isEmpty())
    {
        error.message = root.value("error_description").toString();
    }
    if (error.message.isEmpty())
    {
        error.message = root.value("message").toString();
    }
    if (error.message.isEmpty())
    {
        error.message =
            !error.code.isEmpty() ? error.code : result.formatError();
    }
    error.message.remove(QRegularExpression(
        u"</?code>"_s, QRegularExpression::CaseInsensitiveOption));
    return error;
}

QDateTime parseApiDateTime(const QJsonValue &value)
{
    const auto text = value.toString();
    auto dateTime = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dateTime.isValid())
    {
        dateTime = QDateTime::fromString(text, Qt::ISODate);
    }
    return dateTime.isValid() ? dateTime.toUTC() : QDateTime{};
}

QUrl apiUrl(QStringView path, QUrlQuery query)
{
    QUrl url(QString::fromLatin1(API_ROOT).append(path));
    url.setQuery(query);
    return url;
}

QUrl relayUrl(QStringView path)
{
    auto root = youtube::credentials::readRoot().trimmed();
    if (!root.endsWith('/'))
    {
        root.append('/');
    }
    return QUrl(root.append(path));
}

using ApiJsonCallback =
    std::function<void(Expected<QJsonObject, YouTubeApiError>)>;
using RelayJsonCallback = std::function<void(ExpectedStr<QJsonObject>)>;

void jsonRequest(const QUrl &url, NetworkRequestType type,
                 const QString &accessToken, const QJsonObject *body,
                 bool hideBody, ApiJsonCallback callback)
{
    auto request = NetworkRequest(url, type)
                       .timeout(20000)
                       .maximumResponseSize(16 * 1024 * 1024);
    if (!accessToken.isEmpty())
    {
        request = std::move(request).header("Authorization",
                                            "Bearer " + accessToken.toUtf8());
    }
    if (body)
    {
        request = std::move(request).json(*body);
    }
    if (hideBody)
    {
        request = std::move(request).hideRequestBody();
    }
    std::move(request)
        .onSuccess([callback](const NetworkResult &result) {
            if (result.getData().isEmpty())
            {
                callback(QJsonObject{});
                return;
            }
            QJsonParseError error;
            const auto document =
                QJsonDocument::fromJson(result.getData(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
            {
                callback(makeUnexpected(YouTubeApiError{
                    .message = u"YouTube returned an invalid response."_s,
                }));
                return;
            }
            callback(document.object());
        })
        .onError([callback](const NetworkResult &result) {
            callback(makeUnexpected(googleApiError(result)));
        })
        .execute();
}

void deleteLiveChatBan(const QString &banID, const QString &accessToken,
                       YouTubeApi::AuthenticatedCallback<void> callback,
                       bool mayRetry)
{
    QUrlQuery query{{u"id"_s, banID}};
    jsonRequest(
        apiUrl(u"liveChat/bans"_s, query), NetworkRequestType::Delete,
        accessToken, nullptr, false,
        [banID, accessToken, callback = std::move(callback), mayRetry](
            Expected<QJsonObject, YouTubeApiError> result) mutable {
            if (!result && mayRetry &&
                youtube::detail::shouldRetryLiveChatBanDelete(result.error()))
            {
                QTimer::singleShot(
                    1250ms,
                    [banID, accessToken,
                     callback = std::move(callback)]() mutable {
                        deleteLiveChatBan(banID, accessToken,
                                          std::move(callback), false);
                    });
                return;
            }
            if (result)
            {
                callback({});
            }
            else
            {
                callback(makeUnexpected(result.error()));
            }
        });
}

void relayJsonRequest(const QUrl &url, const QString &readTicket,
                      RelayJsonCallback callback)
{
    auto request = NetworkRequest(url, NetworkRequestType::Get)
                       .timeout(20000)
                       .maximumResponseSize(16 * 1024 * 1024);
    if (!readTicket.isEmpty())
    {
        request = std::move(request).header("X-Moltorino-YouTube-Ticket",
                                            readTicket.toUtf8());
    }
    std::move(request)
        .onSuccess([callback](const NetworkResult &result) {
            QJsonParseError error;
            const auto document =
                QJsonDocument::fromJson(result.getData(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
            {
                callback(makeUnexpected(
                    u"Moltorino returned an invalid YouTube response."_s));
                return;
            }
            callback(document.object());
        })
        .onError([callback](const NetworkResult &result) {
            callback(makeUnexpected(googleApiError(result).message));
        })
        .execute();
}

QString bestThumbnail(const QJsonObject &snippet)
{
    const auto thumbs = snippet.value("thumbnails").toObject();
    for (const auto &name : {"high", "medium", "default"})
    {
        const auto url = thumbs.value(name).toObject().value("url").toString();
        if (!url.isEmpty())
        {
            return url;
        }
    }
    return {};
}

enum class SourceKind { Invalid, Video, Channel, Handle };

struct ParsedSource {
    SourceKind kind = SourceKind::Invalid;
    QString value;
};

bool isVideoID(QByteArrayView value)
{
    if (value.size() != 11)
    {
        return false;
    }
    return std::ranges::all_of(value, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

QString videoIDAfter(QByteArrayView page, QByteArrayView marker)
{
    const auto offset = page.indexOf(marker);
    if (offset < 0 || page.size() - offset - marker.size() < 11)
    {
        return {};
    }
    const auto value = page.sliced(offset + marker.size(), 11);
    return isVideoID(value) ? QString::fromLatin1(value) : QString{};
}

QUrl publicLiveUrl(const ParsedSource &source)
{
    switch (source.kind)
    {
        case SourceKind::Video: {
            QUrl url(QString::fromLatin1(YOUTUBE_ROOT) + u"watch"_s);
            url.setQuery(QUrlQuery{{u"v"_s, source.value}});
            return url;
        }
        case SourceKind::Channel:
            return QUrl(QString::fromLatin1(YOUTUBE_ROOT) + u"channel/"_s +
                        QString::fromUtf8(QUrl::toPercentEncoding(
                            source.value, QByteArray("_-"))) +
                        u"/live"_s);
        case SourceKind::Handle:
            return QUrl(QString::fromLatin1(YOUTUBE_ROOT) +
                        QString::fromUtf8(QUrl::toPercentEncoding(
                            source.value, QByteArray("@._-"))) +
                        u"/live"_s);
        case SourceKind::Invalid:
            return {};
    }
    return {};
}

using PublicPageCallback =
    std::function<void(ExpectedStr<youtube::detail::PublicLivePageState>)>;

void requestPublicLivePage(const QUrl &url, PublicPageCallback callback)
{
    constexpr qsizetype MAX_PUBLIC_LIVE_PAGE_BYTES = 16 * 1024 * 1024;
    auto request =
        NetworkRequest(url, NetworkRequestType::Get)
            .timeout(20000)
            .maximumResponseSize(MAX_PUBLIC_LIVE_PAGE_BYTES)
            .followRedirects(true)
            .attribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::AlwaysNetwork)
            .header("Accept-Language", "en-US,en;q=0.9")
            .header("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                  "AppleWebKit/537.36 (KHTML, like Gecko) "
                                  "Chrome/126.0.0.0 Safari/537.36");
    std::move(request)
        .onSuccess([callback](const NetworkResult &result) mutable {
            if (result.getData().size() > MAX_PUBLIC_LIVE_PAGE_BYTES)
            {
                callback(makeUnexpected(
                    u"YouTube returned an unexpectedly large live page."_s));
                return;
            }
            auto parsed = youtube::detail::parsePublicLivePage(
                QByteArrayView(result.getData()));
            if (!parsed.valid)
            {
                callback(makeUnexpected(
                    u"YouTube returned an unrecognized public live page."_s));
                return;
            }
            callback(std::move(parsed));
        })
        .onError([callback](const NetworkResult &result) mutable {
            callback(makeUnexpected(result.formatError()));
        })
        .execute();
}

using ResolveCallback =
    std::function<void(ExpectedStr<YouTubeResolvedChannel>)>;

ParsedSource parseSource(const QString &raw)
{
    auto source = raw.trimmed();
    if (source.startsWith(u":youtube:"_s, Qt::CaseInsensitive))
    {
        source.remove(0, 9);
    }
    if (source.startsWith(u"video:"_s, Qt::CaseInsensitive))
    {
        return {SourceKind::Video, source.sliced(6)};
    }
    if (source.startsWith(u"channel:"_s, Qt::CaseInsensitive))
    {
        return {SourceKind::Channel, source.sliced(8)};
    }
    if (source.startsWith(u"handle:"_s, Qt::CaseInsensitive))
    {
        return {SourceKind::Handle, source.sliced(7)};
    }

    QUrl url;
    if (source.contains(u"://"_s))
    {
        url = QUrl::fromUserInput(source);
    }
    else if (source.startsWith(u"www.youtube.com/"_s, Qt::CaseInsensitive) ||
             source.startsWith(u"youtube.com/"_s, Qt::CaseInsensitive) ||
             source.startsWith(u"youtu.be/"_s, Qt::CaseInsensitive))
    {
        url = QUrl::fromUserInput(u"https://"_s + source);
    }

    if (url.isValid() && !url.host().isEmpty())
    {
        auto host = url.host().toLower();
        if (host.startsWith(u"www."_s))
        {
            host.remove(0, 4);
        }
        const auto parts = url.path().split('/', Qt::SkipEmptyParts);
        if (host == u"youtu.be"_s && !parts.isEmpty())
        {
            return {SourceKind::Video, parts.front()};
        }
        if (host == u"youtube.com"_s || host.endsWith(u".youtube.com"_s))
        {
            const auto queryVideo = QUrlQuery(url).queryItemValue(u"v"_s);
            if (!queryVideo.isEmpty())
            {
                return {SourceKind::Video, queryVideo};
            }
            if (parts.size() >= 2 &&
                (parts.front() == u"live"_s || parts.front() == u"embed"_s ||
                 parts.front() == u"shorts"_s))
            {
                return {SourceKind::Video, parts.at(1)};
            }
            if (parts.size() >= 2 && parts.front() == u"channel"_s)
            {
                return {SourceKind::Channel, parts.at(1)};
            }
            if (!parts.isEmpty() && parts.front().startsWith('@'))
            {
                return {SourceKind::Handle, parts.front()};
            }
        }
    }

    static const QRegularExpression channelPattern(u"^UC[A-Za-z0-9_-]{22}$"_s);
    if (channelPattern.match(source).hasMatch())
    {
        return {SourceKind::Channel, source};
    }
    if (source.startsWith('@') && source.size() > 1)
    {
        return {SourceKind::Handle, source};
    }
    if (!source.isEmpty() && !source.contains(' '))
    {
        return {SourceKind::Handle, '@' + source};
    }
    return {};
}

ExpectedStr<YouTubeResolvedChannel> parseResolvedChannel(
    const QJsonObject &result)
{
    if (result.value("protocolVersion").toInt() != 1)
    {
        return makeUnexpected(
            u"Moltorino returned an unsupported YouTube protocol."_s);
    }
    YouTubeResolvedChannel resolved{
        .channelID = result.value("channelID").toString(),
        .displayName = result.value("displayName").toString(),
        .videoID = result.value("videoID").toString(),
        .liveChatID = result.value("liveChatID").toString(),
        .streamTitle = result.value("streamTitle").toString(),
        .readTicket = result.value("ticket").toString(),
        .scheduledStartTime =
            parseApiDateTime(result.value("scheduledStartTime")),
        .readTransport =
            youtube::credentials::directReadEnabled() &&
                    result.value("readTransport").toString() == u"innertube"_s
                ? YouTubeReadTransport::Innertube
                : YouTubeReadTransport::Relay,
        .isLive = result.value("isLive").toBool(),
        .isUpcoming = result.value("isUpcoming").toBool(),
    };
    if (!resolved.liveChatID.isEmpty() && resolved.readTicket.isEmpty())
    {
        return makeUnexpected(
            u"Moltorino did not authorize the YouTube chat stream."_s);
    }
    return resolved;
}

void resolveRelaySource(const QString &stableSource, ResolveCallback callback)
{
    auto url = relayUrl(u"resolve"_s);
    url.setQuery(QUrlQuery{{u"source"_s, stableSource}});
    relayJsonRequest(url, {},
                     [callback = std::move(callback)](
                         ExpectedStr<QJsonObject> result) mutable {
                         if (!result)
                         {
                             callback(makeUnexpected(result.error()));
                             return;
                         }
                         callback(parseResolvedChannel(*result));
                     });
}

bool shouldRetryVideoAsHandle(const ParsedSource &parsed, const QString &error)
{
    if (parsed.kind != SourceKind::Video || parsed.value.size() < 3 ||
        parsed.value.size() > 30 || parsed.value != parsed.value.toLower())
    {
        return false;
    }
    static const QRegularExpression handlePattern(u"^[a-z0-9._-]+$"_s);
    return handlePattern.match(parsed.value).hasMatch() &&
           error.contains(u"video"_s, Qt::CaseInsensitive) &&
           error.contains(u"not found"_s, Qt::CaseInsensitive);
}

void tokenRequest(
    QUrlQuery form,
    YouTubeApi::AuthenticatedCallback<YouTubeTokenResponse> callback)
{
    if (!YouTubeApi::isOAuthConfigured())
    {
        callback(makeUnexpected(YouTubeApiError{
            .message = u"YouTube login is not configured in this build. "
                       u"The builder must supply Google Desktop OAuth "
                       u"credentials."_s,
            .code = u"not_configured"_s,
        }));
        return;
    }
    auto request =
        NetworkRequest(QUrl(QString::fromLatin1(TOKEN_URL)),
                       NetworkRequestType::Post)
            .timeout(20000)
            .maximumResponseSize(1024 * 1024)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .payload(form.query(QUrl::FullyEncoded).toUtf8())
            .hideRequestBody();
    std::move(request)
        .onSuccess([callback](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto access = root.value("access_token").toString();
            if (access.isEmpty())
            {
                callback(makeUnexpected(YouTubeApiError{
                    .message = u"Google did not return an access token."_s,
                }));
                return;
            }
            callback(YouTubeTokenResponse{
                .accessToken = access,
                .refreshToken = root.value("refresh_token").toString(),
                .expiresAt = QDateTime::currentDateTimeUtc().addSecs(
                    std::max(60, root.value("expires_in").toInt(3600))),
                .grantedScope = root.value("scope").toString(),
            });
        })
        .onError([callback](const NetworkResult &result) {
            callback(makeUnexpected(googleApiError(result)));
        })
        .execute();
}

}

namespace chatterino {

youtube::detail::PublicLivePageState youtube::detail::parsePublicLivePage(
    QByteArrayView page)
{
    PublicLivePageState state;
    state.valid = page.contains("ytInitialData") ||
                  page.contains("ytInitialPlayerResponse");
    if (!state.valid)
    {
        return state;
    }

    state.videoID = videoIDAfter(
        page,
        "<link rel=\"canonical\" href=\"https://www.youtube.com/watch?v=");
    if (state.videoID.isEmpty())
    {
        state.videoID = videoIDAfter(page, "\"videoDetails\":{\"videoId\":\"");
    }

    state.isLiveNow =
        !state.videoID.isEmpty() && page.contains("\"isLiveNow\":true");
    state.hasLiveContent =
        !state.videoID.isEmpty() && page.contains("\"isLiveContent\":true");
    return state;
}

bool YouTubeApi::isConfigured()
{
    const QUrl url(youtube::credentials::readRoot().trimmed());
    const bool localTest =
        url.scheme() == u"http"_s &&
        (url.host() == u"127.0.0.1"_s || url.host() == u"localhost"_s);
    return url.isValid() && !url.host().isEmpty() &&
           url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
           (url.scheme() == u"https"_s || localTest);
}

bool YouTubeApi::isOAuthConfigured()
{
    return !youtube::credentials::oauthClientID().trimmed().isEmpty() &&
           !youtube::credentials::oauthClientSecret().trimmed().isEmpty();
}

QString YouTubeApi::normalizeSource(const QString &source)
{
    const auto parsed = parseSource(source);
    switch (parsed.kind)
    {
        case SourceKind::Video:
            return u"video:"_s + parsed.value;
        case SourceKind::Channel:
            return u"channel:"_s + parsed.value;
        case SourceKind::Handle:
            return u"handle:"_s + parsed.value.toLower();
        case SourceKind::Invalid:
            return source.trimmed();
    }
    return source.trimmed();
}

void YouTubeApi::resolveSource(const QString &source,
                               Callback<YouTubeResolvedChannel> callback)
{
    if (!isConfigured())
    {
        callback(makeUnexpected(
            u"YouTube chat is not configured in this build. "
            u"The builder must set a YouTube read relay."_s));
        return;
    }
    const auto parsed = parseSource(source);
    if (parsed.kind == SourceKind::Invalid)
    {
        callback(makeUnexpected(
            u"Enter a YouTube handle, channel URL, video URL, or video:<ID>."_s));
        return;
    }
    const auto stable = normalizeSource(source);
    resolveRelaySource(
        stable, [parsed, callback = std::move(callback)](
                    ExpectedStr<YouTubeResolvedChannel> result) mutable {
            if (result || !shouldRetryVideoAsHandle(parsed, result.error()))
            {
                callback(std::move(result));
                return;
            }

            const auto originalError = result.error();
            resolveRelaySource(
                u"handle:@"_s + parsed.value,
                [callback = std::move(callback), originalError](
                    ExpectedStr<YouTubeResolvedChannel> fallback) mutable {
                    if (fallback)
                    {
                        callback(std::move(fallback));
                    }
                    else
                    {
                        callback(makeUnexpected(originalError));
                    }
                });
        });
}

void YouTubeApi::probeLiveSource(const QString &source,
                                 Callback<YouTubeLiveProbe> callback)
{
    const auto parsed = parseSource(source);
    if (parsed.kind == SourceKind::Invalid)
    {
        callback(makeUnexpected(
            u"Enter a YouTube handle, channel URL, video URL, or video:<ID>."_s));
        return;
    }

    requestPublicLivePage(
        publicLiveUrl(parsed),
        [parsed, callback = std::move(callback)](
            ExpectedStr<youtube::detail::PublicLivePageState> result) mutable {
            if (!result)
            {
                callback(makeUnexpected(result.error()));
                return;
            }
            if (result->isLiveNow)
            {
                callback(YouTubeLiveProbe{
                    .videoID = result->videoID,
                    .isLive = true,
                });
                return;
            }

            if (parsed.kind == SourceKind::Video || !result->hasLiveContent ||
                result->videoID.isEmpty())
            {
                callback(YouTubeLiveProbe{
                    .videoID = result->videoID,
                    .isLive = false,
                });
                return;
            }

            const auto candidateVideoID = result->videoID;
            requestPublicLivePage(
                publicLiveUrl({SourceKind::Video, candidateVideoID}),
                [candidateVideoID, callback = std::move(callback)](
                    ExpectedStr<youtube::detail::PublicLivePageState>
                        verification) mutable {
                    if (!verification)
                    {
                        callback(makeUnexpected(verification.error()));
                        return;
                    }
                    callback(YouTubeLiveProbe{
                        .videoID = candidateVideoID,
                        .isLive = verification->isLiveNow,
                    });
                });
        });
}

void YouTubeApi::getOwnChannelAuthenticated(
    const QString &accessToken,
    AuthenticatedCallback<YouTubeOwnChannel> callback)
{
    QUrlQuery query{{u"part"_s, u"id,snippet"_s}, {u"mine"_s, u"true"_s}};
    jsonRequest(
        apiUrl(u"channels"_s, query), NetworkRequestType::Get, accessToken,
        nullptr, false,
        [callback = std::move(callback)](
            Expected<QJsonObject, YouTubeApiError> result) mutable {
            if (!result)
            {
                callback(makeUnexpected(result.error()));
                return;
            }
            const auto items = result->value("items").toArray();
            if (items.isEmpty())
            {
                callback(makeUnexpected(YouTubeApiError{
                    .message = u"This Google account has no YouTube channel."_s,
                }));
                return;
            }
            const auto item = items.first().toObject();
            const auto snippet = item.value("snippet").toObject();
            callback(YouTubeOwnChannel{
                .channelID = item.value("id").toString(),
                .handle =
                    visibleYouTubeName(snippet.value("customUrl").toString()),
                .displayName = snippet.value("title").toString(),
                .avatarUrl = bestThumbnail(snippet),
            });
        });
}

void YouTubeApi::exchangeCode(
    const QString &code, const QString &verifier, const QString &redirectUri,
    AuthenticatedCallback<YouTubeTokenResponse> callback)
{
    tokenRequest(
        QUrlQuery{
            {u"client_id"_s, youtube::credentials::oauthClientID().trimmed()},
            {u"client_secret"_s,
             youtube::credentials::oauthClientSecret().trimmed()},
            {u"code"_s, code},
            {u"code_verifier"_s, verifier},
            {u"redirect_uri"_s, redirectUri},
            {u"grant_type"_s, u"authorization_code"_s},
        },
        std::move(callback));
}

void YouTubeApi::refreshAccessToken(
    const QString &refreshToken,
    AuthenticatedCallback<YouTubeTokenResponse> callback)
{
    tokenRequest(
        QUrlQuery{
            {u"client_id"_s, youtube::credentials::oauthClientID().trimmed()},
            {u"client_secret"_s,
             youtube::credentials::oauthClientSecret().trimmed()},
            {u"refresh_token"_s, refreshToken},
            {u"grant_type"_s, u"refresh_token"_s},
        },
        std::move(callback));
}

void YouTubeApi::revokeToken(const QString &token,
                             AuthenticatedCallback<void> callback)
{
    if (token.isEmpty())
    {
        callback({});
        return;
    }

    const auto form = QUrlQuery{{u"token"_s, token}};
    auto request =
        NetworkRequest(QUrl(QString::fromLatin1(REVOKE_URL)),
                       NetworkRequestType::Post)
            .timeout(20000)
            .maximumResponseSize(1024 * 1024)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .payload(form.query(QUrl::FullyEncoded).toUtf8())
            .hideRequestBody();
    std::move(request)
        .onSuccess([callback](const NetworkResult &) {
            callback({});
        })
        .onError([callback](const NetworkResult &result) {
            callback(makeUnexpected(googleApiError(result)));
        })
        .execute();
}

void YouTubeApi::sendMessage(const QString &liveChatID, const QString &message,
                             const QString &accessToken,
                             AuthenticatedCallback<YouTubeMessage> callback)
{
    QUrlQuery query{{u"part"_s, u"snippet"_s}};
    const QJsonObject body{
        {"snippet",
         QJsonObject{
             {"liveChatId", liveChatID},
             {"type", "textMessageEvent"},
             {"textMessageDetails", QJsonObject{{"messageText", message}}},
         }}};
    jsonRequest(apiUrl(u"liveChat/messages"_s, query), NetworkRequestType::Post,
                accessToken, &body, true,
                [liveChatID, message, callback = std::move(callback)](
                    Expected<QJsonObject, YouTubeApiError> result) mutable {
                    if (!result)
                    {
                        callback(makeUnexpected(result.error()));
                        return;
                    }
                    auto created = parseYouTubeMessage(*result);

                    created.kind = YouTubeMessageKind::Text;
                    if (created.liveChatId.isEmpty())
                    {
                        created.liveChatId = liveChatID;
                    }
                    if (created.text.isEmpty())
                    {
                        created.text = message;
                    }
                    callback(std::move(created));
                });
}

void YouTubeApi::getRecentLiveChatMessages(
    const QString &liveChatID, const QString &accessToken,
    AuthenticatedCallback<std::vector<YouTubeMessage>> callback)
{
    QUrlQuery query{
        {u"liveChatId"_s, liveChatID},
        {u"part"_s, u"id,snippet,authorDetails"_s},
        {u"maxResults"_s, u"2000"_s},
    };
    jsonRequest(
        apiUrl(u"liveChat/messages"_s, query), NetworkRequestType::Get,
        accessToken, nullptr, false,
        [callback = std::move(callback)](
            Expected<QJsonObject, YouTubeApiError> result) mutable {
            if (!result)
            {
                callback(makeUnexpected(result.error()));
                return;
            }

            std::vector<YouTubeMessage> messages;
            const auto items = result->value(u"items"_s).toArray();
            messages.reserve(items.size());
            for (const auto &item : items)
            {
                if (!item.isObject())
                {
                    continue;
                }
                auto message = parseYouTubeMessage(item.toObject());
                if (!message.isIgnored())
                {
                    messages.emplace_back(std::move(message));
                }
            }
            callback(std::move(messages));
        });
}

void YouTubeApi::deleteMessage(const QString &messageID,
                               const QString &accessToken,
                               AuthenticatedCallback<void> callback)
{
    QUrlQuery query{{u"id"_s, messageID}};
    jsonRequest(apiUrl(u"liveChat/messages"_s, query),
                NetworkRequestType::Delete, accessToken, nullptr, false,
                [callback = std::move(callback)](
                    Expected<QJsonObject, YouTubeApiError> result) mutable {
                    if (result)
                    {
                        callback({});
                    }
                    else
                    {
                        callback(makeUnexpected(result.error()));
                    }
                });
}

void YouTubeApi::banUser(const QString &liveChatID, const QString &channelID,
                         std::optional<std::chrono::seconds> duration,
                         const QString &accessToken,
                         AuthenticatedCallback<QString> callback)
{
    QJsonObject snippet{
        {"liveChatId", liveChatID},
        {"type", duration ? "temporary" : "permanent"},
        {"bannedUserDetails", QJsonObject{{"channelId", channelID}}},
    };
    if (duration)
    {
        snippet.insert("banDurationSeconds",
                       QString::number(duration->count()));
    }
    const QJsonObject body{{"snippet", snippet}};
    QUrlQuery query{{u"part"_s, u"snippet"_s}};
    jsonRequest(apiUrl(u"liveChat/bans"_s, query), NetworkRequestType::Post,
                accessToken, &body, true,
                [callback = std::move(callback)](
                    Expected<QJsonObject, YouTubeApiError> result) mutable {
                    if (!result)
                    {
                        callback(makeUnexpected(result.error()));
                        return;
                    }
                    const auto id = result->value("id").toString();
                    if (id.isEmpty())
                    {
                        callback(makeUnexpected(YouTubeApiError{
                            .message = u"YouTube did not return a ban ID."_s,
                        }));
                        return;
                    }
                    callback(id);
                });
}

void YouTubeApi::unbanUser(const QString &banID, const QString &accessToken,
                           AuthenticatedCallback<void> callback)
{
    deleteLiveChatBan(banID, accessToken, std::move(callback), true);
}

bool youtube::detail::shouldRetryLiveChatBanDelete(
    const YouTubeApiError &error)
{
    const auto reason = error.reason.trimmed();
    return reason.compare(u"invalidLiveChatBanId"_s,
                          Qt::CaseInsensitive) == 0 ||
           reason.compare(u"liveChatBanNotFound"_s,
                          Qt::CaseInsensitive) == 0;
}

bool youtube::detail::shouldResolveLiveChatMessageDelete(
    const YouTubeApiError &error)
{
    return error.httpStatus == 404 &&
           (error.reason.isEmpty() ||
            error.reason.compare(u"liveChatMessageNotFound"_s,
                                 Qt::CaseInsensitive) == 0);
}

std::optional<QString> youtube::detail::findMatchingLiveChatMessageID(
    const std::vector<YouTubeMessage> &candidates, QStringView authorChannelID,
    QStringView messageText, const QDateTime &publishedAt)
{
    constexpr qint64 MATCH_WINDOW_MS = 60'000;
    const auto normalizedText =
        messageText.toString().normalized(QString::NormalizationForm_C).trimmed();
    if (authorChannelID.isEmpty() || normalizedText.isEmpty())
    {
        return std::nullopt;
    }

    const YouTubeMessage *best = nullptr;
    qint64 bestDifference = MATCH_WINDOW_MS + 1;
    bool ambiguous = false;
    for (const auto &candidate : candidates)
    {
        const auto candidateText =
            candidate.kind == YouTubeMessageKind::SuperChat &&
                    candidate.text.trimmed().isEmpty()
                ? candidate.amountDisplayString
                : candidate.text;
        if (!candidate.isUserChatMessage() ||
            candidate.id.isEmpty() ||
            candidate.author.channelId != authorChannelID ||
            candidateText.normalized(QString::NormalizationForm_C).trimmed() !=
                normalizedText)
        {
            continue;
        }

        if (!publishedAt.isValid())
        {
            if (best != nullptr)
            {
                return std::nullopt;
            }
            best = &candidate;
            continue;
        }
        if (!candidate.publishedAt.isValid())
        {
            continue;
        }

        const auto signedDifference =
            candidate.publishedAt.msecsTo(publishedAt);
        const auto difference = signedDifference < 0 ? -signedDifference
                                                      : signedDifference;
        if (difference > MATCH_WINDOW_MS)
        {
            continue;
        }
        if (difference < bestDifference)
        {
            best = &candidate;
            bestDifference = difference;
            ambiguous = false;
        }
        else if (difference == bestDifference && best != nullptr &&
                 best->id != candidate.id)
        {
            ambiguous = true;
        }
    }

    return best != nullptr && !ambiguous ? std::optional{best->id}
                                         : std::nullopt;
}

}
