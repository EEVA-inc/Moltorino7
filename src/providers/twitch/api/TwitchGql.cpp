// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchGql.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "util/Helpers.hpp"
#include "util/RapidjsonHelpers.hpp"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QUuid>
#include <QStringBuilder>

#include "common/QLogging.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace chatterino {

namespace {

    QString normalizeCustomTwitchAuthToken(QString token)
    {
        token = token.trimmed();

        while (!token.isEmpty())
        {
            const auto previous = token;

            if (token.size() >= 2 &&
                ((token.startsWith('"') && token.endsWith('"')) ||
                 (token.startsWith('\'') && token.endsWith('\''))))
            {
                token = token.mid(1, token.size() - 2).trimmed();
            }

            if (token.startsWith("Authorization:", Qt::CaseInsensitive))
            {
                token = token.mid(QString("Authorization:").size()).trimmed();
            }

            if (token.startsWith("OAuth ", Qt::CaseInsensitive))
            {
                token = token.mid(QString("OAuth ").size()).trimmed();
            }

            if (token.startsWith("Bearer ", Qt::CaseInsensitive))
            {
                token = token.mid(QString("Bearer ").size()).trimmed();
            }

            if (token.startsWith("oauth:", Qt::CaseInsensitive))
            {
                token = token.mid(QString("oauth:").size()).trimmed();
            }

            if (token == previous)
            {
                break;
            }
        }

        return token;
    }

    const QString &twitchGqlDeviceId()
    {
        static const QString deviceId = [] {
            auto uuid = generateUuid();
            uuid.remove('{').remove('}').remove('-');
            return uuid;
        }();
        return deviceId;
    }

    const QString &twitchGqlSessionId()
    {
        static const QString sessionId = [] {
            auto uuid = generateUuid();
            uuid.remove('{').remove('}').remove('-');
            return uuid;
        }();
        return sessionId;
    }

    QString predictionCreateOutcomeColor(int index, int outcomeCount)
    {
        if (outcomeCount == 2)
        {
            return index == 0 ? "BLUE" : "PINK";
        }

        return "BLUE";
    }

    constexpr auto TWITCH_GQL_BROWSER_CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko";

    constexpr auto TWITCH_GQL_BROWSER_CLIENT_VERSION =
        "ef928475-9403-42f2-8a34-55784bd08e16";
    constexpr auto TWITCH_GQL_BROWSER_USER_AGENT =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
    constexpr auto TWITCH_GQL_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";
    constexpr auto TWITCH_GQL_TV_USER_AGENT =
        "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
    constexpr auto TWITCH_GQL_TV_ORIGIN = "https://android.tv.twitch.tv";
    constexpr auto TWITCH_GQL_TV_REFERER = "https://android.tv.twitch.tv/";
    constexpr int TWITCH_GQL_TIMEOUT_MS = 15 * 1000;

    NetworkRequest makeGqlRequest(const char *query,
                                  const QJsonObject &variables,
                                  const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko")
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payload);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makePersistedGqlRequest(const QString &operationName,
                                           const QString &sha256Hash,
                                           const QJsonObject &variables,
                                           std::shared_ptr<TwitchAccount> account)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko") // Web client ID required for these endpoints
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        if (account)
        {
            const auto &token = account->getOAuthToken();
            if (!token.isEmpty())
            {
                request = std::move(request).header("Authorization", "OAuth " + token);
            }
        }

        return request;
    }

    NetworkRequest makePersistedGqlRequest(const QString &operationName,
                                           const QString &sha256Hash,
                                           const QJsonObject &variables,
                                           const QString &oauthToken,
                                           bool batchPayload = true)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko")
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId());

        if (batchPayload)
        {
            QJsonArray payloadArray;
            payloadArray.append(payload);
            request = std::move(request).json(payloadArray);
        }
        else
        {
            request = std::move(request).json(payload);
        }

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeTvPersistedGqlRequest(const QString &operationName,
                                             const QString &sha256Hash,
                                             const QJsonObject &variables,
                                             const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", TWITCH_GQL_TV_CLIENT_ID)
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("Origin", TWITCH_GQL_TV_ORIGIN)
                .header("Referer", TWITCH_GQL_TV_REFERER)
                .header("User-Agent", TWITCH_GQL_TV_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeInlineGqlRequest(const char *query,
                                        const QJsonObject &variables,
                                        const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko")
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeTvInlineGqlRequest(const char *query,
                                          const QJsonObject &variables,
                                          const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", TWITCH_GQL_TV_CLIENT_ID)
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("Origin", TWITCH_GQL_TV_ORIGIN)
                .header("Referer", TWITCH_GQL_TV_REFERER)
                .header("User-Agent", TWITCH_GQL_TV_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeAuthenticatedInlineGqlRequest(
        const char *query, const QJsonObject &variables, const TwitchGqlAuth &auth)
    {
        if (twitchgql::detail::authTransport(auth) == TwitchGqlAuthTransport::Tv)
        {
            return makeTvInlineGqlRequest(query, variables, auth.oauthToken);
        }
        return makeInlineGqlRequest(query, variables, auth.oauthToken)
            .header("Client-Id", twitchgql::detail::effectiveClientId(auth));
    }

    QString extractFirstGqlErrorMessage(const rapidjson::Document &doc)
    {
        const rapidjson::Value *payload = nullptr;

        if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject())
        {
            payload = &doc[0];
        }
        else if (doc.IsObject())
        {
            payload = &doc;
        }

        if (payload == nullptr || !payload->HasMember("errors") ||
            !(*payload)["errors"].IsArray() || (*payload)["errors"].Empty())
        {
            return {};
        }

        for (const auto &error : (*payload)["errors"].GetArray())
        {
            QString message;
            if (rj::getSafe(error, "message", message) && !message.isEmpty())
            {
                return message;
            }
        }

        return "Twitch rejected the request";
    }

    QJsonObject firstPayloadObject(const QJsonValue &value)
    {
        if (value.isArray())
        {
            const auto array = value.toArray();
            if (!array.isEmpty() && array.first().isObject())
            {
                return array.first().toObject();
            }
        }
        else if (value.isObject())
        {
            return value.toObject();
        }

        return {};
    }

    bool readInteger(const rapidjson::Value &value, qint64 &out)
    {
        if (value.IsInt64())
        {
            out = value.GetInt64();
            return true;
        }
        if (value.IsUint64())
        {
            const auto raw = value.GetUint64();
            if (raw > quint64(std::numeric_limits<qint64>::max()))
            {
                return false;
            }
            out = qint64(raw);
            return true;
        }
        if (value.IsDouble())
        {
            const auto raw = value.GetDouble();
            if (!std::isfinite(raw) || raw < 0 || std::trunc(raw) != raw ||
                raw >= double(std::numeric_limits<qint64>::max()))
            {
                return false;
            }
            out = qint64(raw);
            return true;
        }
        return false;
    }

    bool readInteger(const rapidjson::Value &obj, const char *key, qint64 &out)
    {
        if (!obj.IsObject() || !obj.HasMember(key))
        {
            return false;
        }

        return readInteger(obj[key], out);
    }

    qint64 jsonIntegerValue(const QJsonValue &value, qint64 fallback = -1)
    {
        if (value.isDouble())
        {
            return value.toInteger(fallback);
        }

        return fallback;
    }

    QString extractFirstGqlErrorMessageFromPayload(const QJsonObject &payload)
    {
        const auto errors = payload.value("errors").toArray();
        for (const auto &errorValue : errors)
        {
            const auto error = errorValue.toObject();
            const auto message = error.value("message").toString();
            if (!message.isEmpty())
            {
                return message;
            }
        }
        return errors.isEmpty() ? QString{}
                                : QStringLiteral("Twitch rejected the request");
    }

    QString extractFirstGqlErrorMessage(const QJsonValue &value)
    {
        if (value.isArray())
        {
            for (const auto &payloadValue : value.toArray())
            {
                const auto message =
                    extractFirstGqlErrorMessageFromPayload(
                        payloadValue.toObject());
                if (!message.isEmpty())
                {
                    return message;
                }
            }
            return {};
        }

        return extractFirstGqlErrorMessageFromPayload(firstPayloadObject(value));
    }

    QJsonObject payloadDataObject(const QJsonValue &value)
    {
        const auto payload = firstPayloadObject(value);
        return payload.value("data").toObject();
    }

    GqlVanityBadge vanityBadgeFromObject(const QJsonObject &object)
    {
        return GqlVanityBadge{
            .id = object.value("id").toString(),
            .setId = object.value("setID").toString(),
            .version = object.value("version").toString(),
            .title = object.value("title").toString(),
            .image1 = object.value("image1x").toString(),
            .image2 = object.value("image2x").toString(),
            .image4 = object.value("image4x").toString(),
        };
    }

    QVector<GqlVanityBadge> vanityBadgesFromArray(const QJsonArray &array)
    {
        QVector<GqlVanityBadge> badges;
        badges.reserve(array.size());
        for (const auto &value : array)
        {
            auto badge = vanityBadgeFromObject(value.toObject());
            if (!badge.setId.isEmpty())
            {
                badges.push_back(std::move(badge));
            }
        }
        return badges;
    }

    QJsonObject payloadDataObjectForOperation(const QJsonValue &value,
                                              const QString &operationName)
    {
        if (value.isArray())
        {
            const auto array = value.toArray();
            for (const auto &payloadValue : array)
            {
                const auto payload = payloadValue.toObject();
                const auto payloadOperation = payload.value("extensions")
                                                  .toObject()
                                                  .value("operationName")
                                                  .toString();
                if (payloadOperation.compare(operationName,
                                             Qt::CaseInsensitive) == 0)
                {
                    return payload.value("data").toObject();
                }
            }
        }

        return payloadDataObject(value);
    }

    QJsonObject persistedPayload(const QString &operationName,
                                 const QJsonObject &variables,
                                 const QString &sha256Hash)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);
        return payload;
    }

    NetworkRequest makeTvPersistedGqlBatchRequest(
        const QJsonArray &payloadArray, const QString &oauthToken)
    {
        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .maximumResponseSize(4 * 1024 * 1024)
                .followRedirects(false)
                .header("Client-Id", TWITCH_GQL_TV_CLIENT_ID)
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("Origin", TWITCH_GQL_TV_ORIGIN)
                .header("Referer", TWITCH_GQL_TV_REFERER)
                .header("User-Agent", TWITCH_GQL_TV_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    void sendTerminatePollRequest(
        const QString &pollId, const QString &currentUserId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("pollID", pollId);

        QJsonObject variables;
        variables.insert("input", input);

        QJsonArray payloadArray;
        payloadArray.append(persistedPayload(
            "TerminatePoll", variables,
            "2701ef0594dae5f532ce68e58cc3036a6d020755eef49927f98c14017fd819b2"));
        if (!currentUserId.trimmed().isEmpty())
        {
            QJsonObject spadeVariables;
            spadeVariables.insert("id", currentUserId.trimmed());
            payloadArray.append(persistedPayload(
                "Core_Services_Spade_ChatEvent_User", spadeVariables,
                "9cb0f182474382a0e72e817318460eeefc7c1cab0d163ac064a603d850b085ea"));
        }

        makeTvPersistedGqlBatchRequest(payloadArray, oauthToken)
            .onSuccess([successCallback, failureCallback, pollId](
                           const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto data =
                    payloadDataObjectForOperation(root, "TerminatePoll");
                const auto returnedPollId = data.value("terminatePoll")
                                                .toObject()
                                                .value("poll")
                                                .toObject()
                                                .value("id")
                                                .toString();
                if (returnedPollId.isEmpty() || returnedPollId != pollId)
                {
                    failureCallback("Twitch API Error: Failed to end poll");
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    GqlChannelSelfData channelSelfDataFromObject(const QJsonObject &self)
    {
        GqlChannelSelfData data;

        const auto badges = self.value("displayBadges").toArray();
        for (const auto &badgeValue : badges)
        {
            const auto badge = badgeValue.toObject();
            const auto setId = badge.value("setID").toString();
            const auto title = badge.value("title").toString();
            if (setId.compare("lead_moderator", Qt::CaseInsensitive) == 0 ||
                title.compare("Lead Moderator", Qt::CaseInsensitive) == 0)
            {
                data.isLeadModerator = true;
                break;
            }
        }

        return data;
    }

    QString gqlPayloadErrorMessage(const QJsonValue &value,
                                   const QString &fallback)
    {
        if (value.isUndefined() || value.isNull())
        {
            return {};
        }
        if (value.isString())
        {
            return value.toString().trimmed();
        }
        if (!value.isObject())
        {
            return fallback;
        }

        const auto obj = value.toObject();
        for (const auto &key : {
                 QStringLiteral("code"),
                 QStringLiteral("reason"),
                 QStringLiteral("message"),
             })
        {
            const auto text = obj.value(key).toString().trimmed();
            if (!text.isEmpty())
            {
                return text;
            }
        }

        return fallback;
    }

    QString predictionMutationError(const NetworkResult &result,
                                    const QString &fallback)
    {
        const auto root = result.parseJsonValue();
        const auto gqlError = extractFirstGqlErrorMessage(root);
        if (!gqlError.isEmpty())
        {
            return gqlError;
        }

        const auto data = payloadDataObject(root);
        if (data.size() != 1)
        {
            return fallback;
        }
        const auto payload = data.begin().value().toObject();
        if (!payload.contains("error"))
        {
            return fallback;
        }
        return gqlPayloadErrorMessage(payload.value("error"), fallback);
    }

    QString gqlMutationDataError(const QJsonValue &root,
                                const QString &fallback)
    {
        const auto data = payloadDataObject(root);
        if (data.isEmpty())
        {
            return fallback;
        }


        for (const auto &value : data)
        {
            const auto payload = value.toObject();
            if (payload.contains("error"))
            {
                const auto error =
                    gqlPayloadErrorMessage(payload.value("error"), fallback);
                if (!error.isEmpty())
                {
                    return error;
                }
            }
        }
        return {};
    }

    void runRoleMutation(const QString &operationName, const QString &hash,
                         const QString &payloadName,
                         const QString &targetInputName,
                         const QString &channelId,
                         const QString &targetLogin,
                         const QString &oauthToken,
                         const QString &fallbackError,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("channelID", channelId);
        input.insert(targetInputName, targetLogin);

        QJsonObject variables;
        variables.insert("input", input);

        makePersistedGqlRequest(operationName, hash, variables, oauthToken)
            .onSuccess([payloadName, fallbackError, successCallback,
                        failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto payload =
                    payloadDataObject(root).value(payloadName).toObject();
                if (payload.isEmpty())
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                const auto payloadError =
                    gqlPayloadErrorMessage(payload.value("error"), fallbackError);
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    void runTvRoleMutation(const QString &operationName, const QString &hash,
                           const QString &payloadName,
                           const QString &targetInputName,
                           const QString &channelId,
                           const QString &targetValue,
                           const QString &oauthToken,
                           const QString &fallbackError,
                           const QString &roleId,
                           const QString &successFlagName,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("channelID", channelId);
        input.insert(targetInputName, targetValue);
        if (!roleId.isEmpty())
        {
            input.insert("roleID", roleId);
        }

        QJsonObject variables;
        variables.insert("input", input);

        makeTvPersistedGqlRequest(operationName, hash, variables, oauthToken)
            .onSuccess([operationName, payloadName, fallbackError,
                        successFlagName, successCallback,
                        failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto payload =
                    payloadDataObjectForOperation(root, operationName)
                        .value(payloadName)
                        .toObject();
                if (payload.isEmpty())
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                const auto payloadError = gqlPayloadErrorMessage(
                    payload.value("error"), fallbackError);
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }

                if (!successFlagName.isEmpty() &&
                    !payload.value(successFlagName).toBool(false))
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    void runFollowMutation(const QString &operationName, const QString &hash,
                           const QString &payloadName, const QString &targetId,
                           bool disableNotifications,
                           const QString &oauthToken,
                           const QString &fallbackError,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("targetID", targetId);
        if (disableNotifications)
        {
            input.insert("disableNotifications", true);
        }
        else if (operationName == "FollowButton_FollowUser")
        {
            input.insert("disableNotifications", false);
        }

        QJsonObject variables;
        variables.insert("input", input);

        makeTvPersistedGqlRequest(operationName, hash, variables, oauthToken)
            .onSuccess([payloadName, fallbackError, successCallback,
                        failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto payload =
                    payloadDataObject(root).value(payloadName).toObject();
                if (payload.isEmpty())
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                const auto payloadError =
                    gqlPayloadErrorMessage(payload.value("error"), fallbackError);
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    GqlBlockedTerm blockedTermFromObject(const QJsonObject &obj)
    {
        GqlBlockedTerm term;
        term.id = obj.value("id").toString().trimmed();
        term.phrase = obj.value("phrase").toString().trimmed();
        term.expiresAt = obj.value("expiresAt").toString().trimmed();
        term.isModEditable = obj.value("isModEditable").toBool(false);
        term.hitCount = obj.value("hitCount").toInt(0);
        return term;
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    QString makeTransactionId()
    {
        auto uuid = generateUuid();
        uuid.remove('{').remove('}').remove('-');
        return uuid;
    }

    QString imageUrlFromRewardObject(const QJsonObject &obj)
    {
        auto image = obj.value("image").toObject();
        if (image.isEmpty())
        {
            image = obj.value("defaultImage").toObject();
        }

        auto url = image.value("url2x").toString();
        if (url.isEmpty())
        {
            url = image.value("url").toString();
        }
        if (url.isEmpty())
        {
            url = image.value("url_2x").toString();
        }
        if (url.isEmpty())
        {
            url = image.value("url_1x").toString();
        }
        return url;
    }

    int rewardCostFromObject(const QJsonObject &obj)
    {
        const auto pricingType = obj.value("pricingType").toString();
        const auto rewardType = obj.value("type").toString(
            obj.value("rewardType").toString());
        const bool isBitsReward =
            pricingType.compare(QStringLiteral("BITS"),
                                Qt::CaseInsensitive) == 0 ||
            rewardType == QStringLiteral("SEND_GIGANTIFIED_EMOTE");
        if (isBitsReward)
        {
            auto cost = obj.value("bitsCost").toInt(0);
            if (cost <= 0)
            {
                cost = obj.value("defaultBitsCost").toInt(0);
            }
            if (cost > 0)
            {
                return cost;
            }
        }

        auto cost = obj.value("cost").toInt(-1);
        if (cost < 0 || obj.value("cost").isNull())
        {
            cost = obj.value("defaultCost").toInt(0);
        }
        if (cost <= 0)
        {
            cost = obj.value("bitsCost").toInt(0);
        }
        if (cost <= 0)
        {
            cost = obj.value("defaultBitsCost").toInt(0);
        }
        return cost;
    }

    QString automaticRewardTitle(const QString &type)
    {
        if (type == "RANDOM_SUB_EMOTE_UNLOCK")
        {
            return "Unlock a Random Emote";
        }
        if (type == "CHOSEN_SUB_EMOTE_UNLOCK")
        {
            return "Choose an Emote to Unlock";
        }
        if (type == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK")
        {
            return "Modify a Single Emote";
        }
        if (type == "SINGLE_MESSAGE_BYPASS_SUB_MODE")
        {
            return "Send a Message in Sub-Only";
        }
        if (type == "SEND_HIGHLIGHTED_MESSAGE")
        {
            return "Highlight My Message";
        }
        if (type == "SEND_ANIMATED_MESSAGE")
        {
            return "Message Effects";
        }
        if (type == "SEND_GIGANTIFIED_EMOTE")
        {
            return "Gigantify an Emote";
        }
        if (type == "CELEBRATION")
        {
            return "On-Screen Celebration";
        }
        return type;
    }

    QString automaticRewardPrompt(const QString &type)
    {
        if (type == "RANDOM_SUB_EMOTE_UNLOCK")
        {
            return "Unlock a random subscriber emote for 24 hours.";
        }
        if (type == "CHOSEN_SUB_EMOTE_UNLOCK")
        {
            return "Pick a subscriber emote to unlock for 24 hours.";
        }
        if (type == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK")
        {
            return "Pick an emote and modifier to unlock for 24 hours.";
        }
        if (type == "SINGLE_MESSAGE_BYPASS_SUB_MODE")
        {
            return "Send one message while sub-only mode is active.";
        }
        if (type == "SEND_HIGHLIGHTED_MESSAGE")
        {
            return "Send one highlighted message.";
        }
        if (type == "SEND_GIGANTIFIED_EMOTE")
        {
            return "Pick a Twitch emote to send enlarged in chat.";
        }
        return {};
    }

    GqlChannelPointReward channelPointRewardFromObject(const QJsonObject &obj,
                                                       bool automatic)
    {
        GqlChannelPointReward reward;
        reward.isAutomatic = automatic;
        reward.id = obj.value("id").toString();
        reward.rewardType = automatic ? obj.value("type").toString()
                                      : QStringLiteral("CUSTOM_REWARD");
        reward.title = automatic ? automaticRewardTitle(reward.rewardType)
                                 : obj.value("title").toString();
        reward.prompt = automatic ? automaticRewardPrompt(reward.rewardType)
                                  : obj.value("prompt").toString();
        reward.pricingType = obj.value("pricingType").toString("POINTS");
        reward.backgroundColor =
            obj.value("backgroundColor")
                .toString(obj.value("defaultBackgroundColor").toString());
        reward.imageUrl = imageUrlFromRewardObject(obj);
        reward.cost = rewardCostFromObject(obj);
        reward.isEnabled = obj.value("isEnabled").toBool(false) &&
                           !obj.value("isPaused").toBool(false);
        reward.isInStock = obj.value("isInStock").toBool(true);
        reward.isUserInputRequired =
            obj.value("isUserInputRequired").toBool(false);
        return reward;
    }

    GqlChannelPointRedeemResult redeemResultFromPayload(
        const QJsonObject &payload)
    {
        GqlChannelPointRedeemResult result;
        result.balance = jsonIntegerValue(payload.value("balance"));
        const auto emote = payload.value("emote").toObject();
        result.emoteId = emote.value("id").toString();
        result.emoteToken = emote.value("token").toString();
        return result;
    }

#endif

    bool rejectGqlOrPayloadError(
        const QJsonValue &root, const QJsonObject &payload,
        const QString &fallback,
        const std::function<void(const QString &)> &failureCallback)
    {
        const auto gqlError = extractFirstGqlErrorMessage(root);
        if (!gqlError.isEmpty())
        {
            failureCallback("Twitch API Error: " + gqlError);
            return true;
        }

        const auto payloadError =
            gqlPayloadErrorMessage(payload.value("error"), fallback);
        if (!payloadError.isEmpty())
        {
            failureCallback("Twitch API Error: " + payloadError);
            return true;
        }

        return false;
    }

    GqlModeratorQueueUser moderatorQueueUserFromObject(
        const QJsonObject &object)
    {
        GqlModeratorQueueUser user;
        user.id = object.value(QStringLiteral("id")).toString();
        user.login = object.value(QStringLiteral("login")).toString();
        user.displayName =
            object.value(QStringLiteral("displayName")).toString(user.login);
        user.profileImageUrl =
            object.value(QStringLiteral("profileImageURL")).toString();
        user.createdAt =
            object.value(QStringLiteral("createdAt")).toString();
        user.chatColor = object.value(QStringLiteral("chatColor")).toString();
        return user;
    }

    GqlUnbanRequest unbanRequestFromEdge(const QJsonObject &edge)
    {
        const auto node = edge.value(QStringLiteral("node")).toObject();
        GqlUnbanRequest request;
        request.id = node.value(QStringLiteral("id")).toString();
        request.cursor = edge.value(QStringLiteral("cursor")).toString();
        request.createdAt =
            node.value(QStringLiteral("createdAt")).toString();
        request.status = node.value(QStringLiteral("status")).toString();
        request.requester = moderatorQueueUserFromObject(
            node.value(QStringLiteral("requester")).toObject());
        request.requesterMessage =
            node.value(QStringLiteral("requesterMessage")).toString();
        request.resolvedAt =
            node.value(QStringLiteral("resolvedAt")).toString();
        request.resolverMessage =
            node.value(QStringLiteral("resolverMessage")).toString();
        request.resolvedBy = moderatorQueueUserFromObject(
            node.value(QStringLiteral("resolvedBy")).toObject());
        return request;
    }

    GqlModeratorComment moderatorCommentFromObject(const QJsonObject &node,
                                                  bool shared)
    {
        const auto author = node.value(QStringLiteral("author")).toObject();
        GqlModeratorComment comment;
        comment.id = node.value(QStringLiteral("id")).toString();
        comment.timestamp =
            node.value(QStringLiteral("timestamp")).toString();
        comment.text = node.value(QStringLiteral("text")).toString();
        comment.channelLogin = node.value(QStringLiteral("channel"))
                                   .toObject()
                                   .value(QStringLiteral("login"))
                                   .toString();
        comment.authorLogin =
            author.value(QStringLiteral("login")).toString();
        comment.authorDisplayName =
            author.value(QStringLiteral("displayName"))
                .toString(comment.authorLogin);
        comment.authorColor =
            author.value(QStringLiteral("chatColor")).toString();
        comment.shareable =
            node.value(QStringLiteral("isShareable")).toBool(false);
        comment.shared = shared;
        return comment;
    }

    GqlModeratorComment moderatorCommentFromEdge(const QJsonObject &edge,
                                                bool shared)
    {
        auto comment = moderatorCommentFromObject(
            edge.value(QStringLiteral("node")).toObject(), shared);
        comment.cursor = edge.value(QStringLiteral("cursor")).toString();
        return comment;
    }

    GqlModeratorCommentPage moderatorCommentPage(
        const QJsonObject &connection, bool shared)
    {
        GqlModeratorCommentPage page;
        const auto edges = connection.value("edges").toArray();
        page.comments.reserve(edges.size());
        for (const auto &value : edges)
        {
            auto comment = moderatorCommentFromEdge(value.toObject(), shared);
            if (!comment.cursor.isEmpty())
            {
                page.nextCursor = comment.cursor;
            }
            if (!comment.id.isEmpty())
            {
                page.comments.push_back(std::move(comment));
            }
        }
        page.hasNextPage = connection.value("pageInfo")
                               .toObject()
                               .value("hasNextPage")
                               .toBool() &&
                           !page.nextCursor.isEmpty();
        return page;
    }

    QString userDisplayNameFromValue(const QJsonValue &value)
    {
        const auto obj = value.toObject();
        auto name = obj.value("displayName").toString();
        if (name.isEmpty())
        {
            name = obj.value("display_name").toString();
        }
        if (name.isEmpty())
        {
            name = obj.value("login").toString();
        }
        if (name.isEmpty())
        {
            name = obj.value("name").toString();
        }
        return name;
    }

    QDateTime parseGqlDateTime(const QJsonValue &value)
    {
        const auto str = value.toString();
        if (str.isEmpty())
        {
            return {};
        }

        auto dt = QDateTime::fromString(str, Qt::ISODate);
        if (!dt.isValid())
        {
            dt = QDateTime::fromString(str, Qt::ISODateWithMs);
        }
        return dt;
    }

    struct GqlFragmentUser {
        QString id;
        QString login;
        QString displayName;
        int fragmentIndex = -1;
    };

    QString tokenText(const QJsonObject &token)
    {
        auto text = token.value("text").toString();
        if (text.isEmpty())
        {
            text = token.value("localizedText").toString();
        }
        if (text.isEmpty())
        {
            text = token.value("displayName").toString();
        }
        if (text.isEmpty())
        {
            text = token.value("login").toString();
        }
        return text;
    }

    QJsonArray localizedFragments(const QJsonObject &obj)
    {
        auto fragments = obj.value("localizedStringFragments").toArray();
        if (fragments.isEmpty())
        {
            fragments = obj.value("fragments").toArray();
        }
        return fragments;
    }

    QJsonArray actionFragments(const QJsonObject &node,
                               const QString &fieldName)
    {
        return localizedFragments(node.value(fieldName).toObject());
    }

    QVector<GqlFragmentUser> usersFromFragments(const QJsonArray &fragments)
    {
        QVector<GqlFragmentUser> users;
        for (int i = 0; i < fragments.size(); ++i)
        {
            const auto fragment = fragments.at(i).toObject();
            const auto token = fragment.value("token").toObject();
            auto displayName = token.value("displayName").toString();
            const auto login = token.value("login").toString();
            const auto id = token.value("id").toString();
            const auto type = token.value("__typename").toString();
            if (displayName.isEmpty() && type == "User")
            {
                displayName = tokenText(token);
            }
            if (displayName.isEmpty() && login.isEmpty() && id.isEmpty())
            {
                continue;
            }

            users.push_back({
                .id = id,
                .login = login,
                .displayName = displayName.isEmpty() ? login : displayName,
                .fragmentIndex = i,
            });
        }
        return users;
    }

    QString textFromFragments(const QJsonArray &fragments)
    {
        QString text;
        for (const auto &fragmentValue : fragments)
        {
            const auto fragment = fragmentValue.toObject();
            const auto token = fragment.value("token").toObject();
            auto part = tokenText(token);
            if (part.isEmpty())
            {
                part = fragment.value("text").toString();
            }
            text += part;
        }
        return text.simplified();
    }

    QString textBeforeFragment(const QJsonArray &fragments, int index,
                               int lookBehind = 2)
    {
        QString text;
        const int begin = std::max(0, index - lookBehind);
        for (int i = begin; i < index; ++i)
        {
            const auto fragment = fragments.at(i).toObject();
            auto part = tokenText(fragment.value("token").toObject());
            if (part.isEmpty())
            {
                part = fragment.value("text").toString();
            }
            text += part;
        }
        return text;
    }

    std::optional<GqlFragmentUser> userAfterByText(
        const QJsonArray &fragments)
    {
        const auto users = usersFromFragments(fragments);
        for (const auto &user : users)
        {
            const auto before = textBeforeFragment(fragments, user.fragmentIndex)
                                    .toLower();
            if (before.contains(" by ") || before.endsWith("by ") ||
                before.contains("automated by "))
            {
                return user;
            }
        }
        return std::nullopt;
    }

    void assignUser(GqlFragmentUser user, QString &id, QString &login,
                    QString &displayName)
    {
        id = std::move(user.id);
        login = std::move(user.login);
        displayName = std::move(user.displayName);
    }

    GqlModerationActionKind moderationActionKind(const QString &category,
                                                 const QString &icon,
                                                 const QString &text)
    {
        const auto cat = category.toUpper();
        const auto ico = icon.toUpper();
        const auto lowerText = text.toLower();

        if (ico == "BAN")
        {
            return GqlModerationActionKind::Ban;
        }
        if (ico == "UNBAN")
        {
            return GqlModerationActionKind::Unban;
        }
        if (ico == "TIMEOUT")
        {
            return GqlModerationActionKind::Timeout;
        }
        if (ico == "UNTIMEOUT")
        {
            return GqlModerationActionKind::Untimeout;
        }
        if (cat.contains("BANS_AND_UNBANS"))
        {
            return lowerText.contains("unban") ? GqlModerationActionKind::Unban
                                               : GqlModerationActionKind::Ban;
        }
        if (cat.contains("TIMEOUTS_AND_UNTIMEOUTS"))
        {
            return lowerText.contains("untimeout") ||
                           lowerText.contains("timeout removed")
                       ? GqlModerationActionKind::Untimeout
                       : GqlModerationActionKind::Timeout;
        }
        if (cat.contains("DELETE") || ico.contains("DELETE") ||
            lowerText.contains("message deleted") ||
            lowerText.contains("deleted message") ||
            lowerText.contains("was deleted"))
        {
            return GqlModerationActionKind::Delete;
        }
        if (cat.contains("MESSAGE") || ico.contains("MESSAGE"))
        {
            return GqlModerationActionKind::Message;
        }
        return GqlModerationActionKind::Other;
    }

    GqlModerationActionLogEntry moderationActionFromNode(
        const QJsonObject &edge)
    {
        const auto node = edge.value("node").toObject();
        GqlModerationActionLogEntry action;
        action.cursor = edge.value("cursor").toString();
        action.id = node.value("id").toString();
        action.category = node.value("filterCategoryID").toString();
        action.icon = node.value("icon").toString();
        action.createdAt = parseGqlDateTime(node.value("createdAt"));

        const auto contentFragments = actionFragments(node, "content");
        const auto bodyFragments = actionFragments(node, "contentBody");
        const auto titleFragments = actionFragments(node, "title");

        action.text = textFromFragments(contentFragments);
        if (action.text.isEmpty())
        {
            action.text = textFromFragments(bodyFragments);
        }
        if (action.text.isEmpty())
        {
            action.text = textFromFragments(titleFragments);
        }

        action.kind =
            moderationActionKind(action.category, action.icon, action.text);

        if (auto moderator = userAfterByText(contentFragments))
        {
            assignUser(std::move(*moderator), action.moderatorId,
                       action.moderatorLogin, action.moderatorDisplayName);
        }
        else if (auto bodyModerator = userAfterByText(bodyFragments))
        {
            assignUser(std::move(*bodyModerator), action.moderatorId,
                       action.moderatorLogin, action.moderatorDisplayName);
        }

        const auto titleUsers = usersFromFragments(titleFragments);
        if (!titleUsers.empty())
        {
            const auto &target = titleUsers.front();
            action.targetId = target.id;
            action.targetLogin = target.login;
            action.targetDisplayName = target.displayName;
        }

        if (action.moderatorId.isEmpty() && action.moderatorLogin.isEmpty())
        {
            const auto contentUsers = usersFromFragments(contentFragments);
            if (!contentUsers.empty())
            {
                const auto &candidate = contentUsers.front();
                const bool sameAsTarget =
                    (!candidate.id.isEmpty() &&
                     candidate.id == action.targetId) ||
                    (!candidate.login.isEmpty() &&
                     candidate.login.compare(action.targetLogin,
                                             Qt::CaseInsensitive) == 0);
                if (!sameAsTarget)
                {
                    action.moderatorId = candidate.id;
                    action.moderatorLogin = candidate.login;
                    action.moderatorDisplayName = candidate.displayName;
                }
            }
        }

        const auto user = node.value("user").toObject();
        if (action.moderatorId.isEmpty() && action.moderatorLogin.isEmpty() &&
            !user.isEmpty())
        {
            const auto id = user.value("id").toString();
            const auto login = user.value("login").toString();
            const bool sameAsTarget =
                (!id.isEmpty() && id == action.targetId) ||
                (!login.isEmpty() &&
                 login.compare(action.targetLogin, Qt::CaseInsensitive) == 0);
            if (!sameAsTarget)
            {
                action.moderatorId = id;
                action.moderatorLogin = login;
                action.moderatorDisplayName =
                    user.value("displayName").toString(login);
            }
        }

        if (action.targetId.isEmpty() && action.targetLogin.isEmpty())
        {
            const auto contentUsers = usersFromFragments(contentFragments);
            for (const auto &userCandidate : contentUsers)
            {
                const bool isModerator =
                    (!userCandidate.id.isEmpty() &&
                     userCandidate.id == action.moderatorId) ||
                    (!userCandidate.login.isEmpty() &&
                     userCandidate.login.compare(action.moderatorLogin,
                                                 Qt::CaseInsensitive) == 0);
                if (!isModerator)
                {
                    action.targetId = userCandidate.id;
                    action.targetLogin = userCandidate.login;
                    action.targetDisplayName = userCandidate.displayName;
                    break;
                }
            }
        }

        return action;
    }

    bool isConnectionObject(const QJsonObject &obj)
    {
        return obj.value("edges").isArray() || obj.value("nodes").isArray();
    }

    QJsonObject findModeratedChannelsConnection(const QJsonValue &value)
    {
        if (value.isArray())
        {
            const auto array = value.toArray();
            for (const auto &item : array)
            {
                auto found = findModeratedChannelsConnection(item);
                if (!found.isEmpty())
                {
                    return found;
                }
            }
            return {};
        }

        if (!value.isObject())
        {
            return {};
        }

        const auto obj = value.toObject();
        const auto direct = obj.value("moderatedChannels");
        if (direct.isObject())
        {
            const auto connection = direct.toObject();
            if (isConnectionObject(connection))
            {
                return connection;
            }
        }

        for (auto it = obj.begin(); it != obj.end(); ++it)
        {
            auto found = findModeratedChannelsConnection(it.value());
            if (!found.isEmpty())
            {
                return found;
            }
        }

        return {};
    }

    QString raidObjectString(const QJsonObject &obj,
                             std::initializer_list<QString> keys)
    {
        for (const auto &key : keys)
        {
            const auto value = obj.value(key);
            if (value.isString())
            {
                const auto text = value.toString().trimmed();
                if (!text.isEmpty())
                {
                    return text;
                }
            }
            if (value.isDouble())
            {
                return QString::number(qint64(value.toDouble()));
            }
        }

        return {};
    }

    QString raidUserIdFromObject(const QJsonObject &obj)
    {
        return raidObjectString(obj,
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("userID"),
                                    QStringLiteral("userId"),
                                    QStringLiteral("user_id"),
                                });
    }

    QString normalizedRaidLogin(QString value)
    {
        value = value.trimmed().toLower();
        while (value.startsWith(QLatin1Char('@')) ||
               value.startsWith(QLatin1Char('#')))
        {
            value = value.mid(1).trimmed();
        }
        return value;
    }

    QString raidErrorMessage(const QJsonValue &value)
    {
        if (value.isUndefined() || value.isNull())
        {
            return {};
        }
        if (value.isString())
        {
            return value.toString().trimmed();
        }
        if (!value.isObject())
        {
            return QStringLiteral("Twitch rejected the raid action");
        }

        const auto obj = value.toObject();
        const auto message =
            raidObjectString(obj,
                             {
                                 QStringLiteral("code"),
                                 QStringLiteral("reason"),
                                 QStringLiteral("message"),
                             });
        return message.isEmpty() ? QStringLiteral("Twitch rejected the raid action")
                                 : message;
    }

    QString raidFailureMessage(QString error)
    {
        error = error.trimmed();
        if (error.startsWith(QStringLiteral("Twitch API Error:"),
                             Qt::CaseInsensitive))
        {
            error = error.mid(QStringLiteral("Twitch API Error:").size())
                        .trimmed();
        }

        if (error.isEmpty())
        {
            return QStringLiteral("Twitch rejected the raid action");
        }

        const auto upper = error.toUpper();
        if (upper.contains(QStringLiteral("TARGET_SETTINGS_DO_NOT_ALLOW")) ||
            (upper.contains(QStringLiteral("TARGET")) &&
             upper.contains(QStringLiteral("SETTING"))))
        {
            return QStringLiteral(
                "That channel's raid settings do not allow this raid. "
                "They may require more viewers than you currently have.");
        }

        if (upper.contains(QStringLiteral("EDITOR")) ||
            upper.contains(QStringLiteral("BROADCASTER")) ||
            upper.contains(QStringLiteral("NOT_AUTHORIZED")) ||
            upper.contains(QStringLiteral("NOT AUTHORIZED")) ||
            upper.contains(QStringLiteral("FORBIDDEN")) ||
            upper.contains(QStringLiteral("PERMISSION")) ||
            upper == QStringLiteral("SERVICE ERROR"))
        {
            return QStringLiteral(
                "You need broadcaster or editor raid permission in this "
                "channel.");
        }

        if (upper.contains(QStringLiteral("NO_ACTIVE_RAID")) ||
            upper.contains(QStringLiteral("NO_RAID")) ||
            upper.contains(QStringLiteral("NO RAID")))
        {
            return QStringLiteral("There is no active raid in this channel.");
        }

        if (upper.contains(QStringLiteral("CANT_RAID_YOURSELF")) ||
            upper.contains(QStringLiteral("CAN'T RAID YOURSELF")) ||
            upper.contains(QStringLiteral("CANNOT RAID YOURSELF")))
        {
            return QStringLiteral("A channel cannot raid itself.");
        }

        return QStringLiteral("Twitch API Error: ") + error;
    }

    std::optional<TwitchChannel::PollEvent> parsePollEventFromGql(
        const QJsonObject &viewablePoll, const QString &currentUserId = {})
    {
        if (viewablePoll.isEmpty())
        {
            return std::nullopt;
        }

        TwitchChannel::PollEvent poll;
        poll.id = viewablePoll.value("id").toString();
        poll.title = viewablePoll.value("title").toString();
        poll.status = viewablePoll.value("status").toString();
        poll.remainingDurationMilliseconds =
            viewablePoll.value("remainingDurationMilliseconds").toInt();
        poll.createdAt = parseGqlDateTime(viewablePoll.value("createdAt"));
        auto endsAt = parseGqlDateTime(viewablePoll.value("endsAt"));
        if (endsAt.isValid())
        {
            poll.endsAt = endsAt;
            if (poll.createdAt.isValid())
            {
                poll.durationSeconds =
                    std::max(0, int(poll.createdAt.secsTo(*poll.endsAt)));
            }
        }

        poll.createdByName =
            userDisplayNameFromValue(viewablePoll.value("createdBy"));
        poll.currentUserId = currentUserId;

        const auto settings = viewablePoll.value("settings").toObject();
        const auto cpVotes =
            settings.value("communityPointsVotes").toObject();
        poll.channelPointsVotingEnabled =
            cpVotes.value("isEnabled").toBool();
        poll.pointsPerVote = cpVotes.value("cost").toInt();

        const auto topContributor =
            viewablePoll.value("topChannelPointsContributor").toObject();
        const auto topContributorName =
            userDisplayNameFromValue(topContributor);
        const int topContributorAmount =
            topContributor.value("contribution").toInt(
                topContributor.value("amount").toInt());

        const auto choices = viewablePoll.value("choices").toArray();
        poll.choices.reserve(size_t(choices.size()));
        for (const auto &choiceValue : choices)
        {
            const auto choiceObj = choiceValue.toObject();
            TwitchChannel::PollChoice choice;
            choice.id = choiceObj.value("id").toString();
            choice.title = choiceObj.value("title").toString();
            const auto votes = choiceObj.value("votes").toObject();
            choice.totalVotes = votes.value("total").toInt();
            choice.freeVotes = votes.value("base").toInt();
            choice.channelPointsVotes =
                votes.value("communityPoints").toInt();
            choice.totalVoters = choiceObj.value("totalVoters").toInt();
            if (!topContributorName.isEmpty() && choice.channelPointsVotes > 0)
            {
                choice.topChannelPointsContribution = topContributorAmount;
                choice.topChannelPointsContributorName = topContributorName;
            }
            poll.totalVotes += choice.totalVotes;
            poll.choices.push_back(std::move(choice));
        }

        const auto voter = viewablePoll.value("self").toObject().value("voter").toObject();
        const auto voterChoices = voter.value("choices").toArray();
        poll.selfVotes.reserve(size_t(voterChoices.size()));
        for (const auto &voterChoiceValue : voterChoices)
        {
            const auto voterChoice = voterChoiceValue.toObject();
            TwitchChannel::PollSelfVote selfVote;
            selfVote.choiceId =
                voterChoice.value("pollChoice").toObject().value("id").toString();
            const auto votes = voterChoice.value("votes").toObject();
            selfVote.freeVotes = votes.value("base").toInt();
            selfVote.channelPointsVotes =
                votes.value("communityPoints").toInt();
            if (!selfVote.choiceId.isEmpty())
            {
                poll.selfVotes.push_back(std::move(selfVote));
            }
        }

        if (poll.id.isEmpty() || poll.title.isEmpty() || poll.choices.empty())
        {
            return std::nullopt;
        }

        return poll;
    }

    GqlBroadcastSettings parseBroadcastSettings(
        const QString &userId, const QJsonObject &settings)
    {
        GqlBroadcastSettings result;
        result.userId = userId.trimmed();
        result.title = settings.value("title").toString();
        result.language = settings.value("language").toString().trimmed();

        const auto game = settings.value("game").toObject();
        result.category.id = game.value("id").toString().trimmed();
        result.category.name = game.value("name").toString().trimmed();
        result.category.displayName =
            game.value("displayName").toString().trimmed();
        if (result.category.displayName.isEmpty())
        {
            result.category.displayName = result.category.name;
        }

        return result;
    }

    std::optional<GqlContentClassificationLabel>
        parseContentClassificationLabel(const QJsonValue &value)
    {
        if (!value.isObject())
        {
            return std::nullopt;
        }

        const auto object = value.toObject();
        GqlContentClassificationLabel label;
        label.id = object.value("id").toString().trimmed();
        if (label.id.isEmpty())
        {
            return std::nullopt;
        }

        label.name = object.value("localizedName").toString().trimmed();
        if (label.name.isEmpty())
        {
            label.name = object.value("name").toString().trimmed();
        }
        if (label.name.isEmpty())
        {
            label.name = label.id;
        }
        label.description =
            object.value("description").toString().trimmed();
        label.lockedUntil =
            object.value("lockedUntil").toString().trimmed();
        label.isEnabled = object.value("isEnabled").toBool(false);
        label.isLocked = object.value("isLocked").toBool(false);
        label.isSelectable = object.value("isSelectable").toBool(false);
        return label;
    }

    std::optional<QVector<GqlContentClassificationLabel>>
        parseContentClassificationLabels(const QJsonValue &value)
    {
        if (!value.isArray())
        {
            return std::nullopt;
        }

        QVector<GqlContentClassificationLabel> labels;
        for (const auto &labelValue : value.toArray())
        {
            auto label = parseContentClassificationLabel(labelValue);
            if (!label)
            {
                return std::nullopt;
            }
            labels.push_back(std::move(*label));
        }
        return labels;
    }

    QStringList parseStringArray(const QJsonValue &value)
    {
        QStringList result;
        if (!value.isArray())
        {
            return result;
        }

        for (const auto &entry : value.toArray())
        {
            const auto text = entry.toString().trimmed();
            if (!text.isEmpty())
            {
                result.push_back(text);
            }
        }
        return result;
    }

    struct BroadcastManagementRequestState {
        std::mutex mutex;
        bool completed = false;
        bool contextReady = false;
        bool tagsReady = false;
        GqlBroadcastSettings settings;
        QStringList tags;
        std::function<void(GqlBroadcastSettings)> successCallback;
        std::function<void(const QString &)> failureCallback;
    };

    void failBroadcastManagementRequest(
        const std::shared_ptr<BroadcastManagementRequestState> &state,
        const QString &error)
    {
        std::function<void(const QString &)> callback;
        {
            const std::lock_guard guard(state->mutex);
            if (state->completed)
            {
                return;
            }
            state->completed = true;
            callback = std::move(state->failureCallback);
        }
        callback(error);
    }

    void finishBroadcastManagementRequestIfReady(
        const std::shared_ptr<BroadcastManagementRequestState> &state)
    {
        std::function<void(GqlBroadcastSettings)> callback;
        GqlBroadcastSettings settings;
        {
            const std::lock_guard guard(state->mutex);
            if (state->completed || !state->contextReady || !state->tagsReady)
            {
                return;
            }
            state->completed = true;
            settings = std::move(state->settings);
            settings.tags = std::move(state->tags);
            callback = std::move(state->successCallback);
        }
        callback(std::move(settings));
    }


}  // namespace

TwitchGqlAuthTransport twitchgql::detail::authTransport(
    const TwitchGqlAuth &auth)
{
    return auth.clientId.trimmed().compare(
               QString::fromUtf8(TWITCH_GQL_TV_CLIENT_ID),
               Qt::CaseInsensitive) == 0
               ? TwitchGqlAuthTransport::Tv
               : TwitchGqlAuthTransport::Browser;
}

QString twitchgql::detail::effectiveClientId(const TwitchGqlAuth &auth)
{
    const auto clientId = auth.clientId.trimmed();
    return clientId.isEmpty() ? QString::fromUtf8(TWITCH_GQL_BROWSER_CLIENT_ID)
                              : clientId;
}

QString twitchgql::detail::tvClientId()
{
    return QString::fromUtf8(TWITCH_GQL_TV_CLIENT_ID);
}

QHash<QString, bool> twitchgql::detail::parseChatRoomBanStatuses(
    const QJsonValue &response, const QVector<QString> &channelIds)
{
    const auto payload = firstPayloadObject(response);
    const auto data = payload.value(QStringLiteral("data")).toObject();

    QSet<QString> erroredAliases;
    for (const auto &errorValue :
         payload.value(QStringLiteral("errors")).toArray())
    {
        const auto path = errorValue.toObject()
                              .value(QStringLiteral("path"))
                              .toArray();
        if (!path.isEmpty() && path.first().isString())
        {
            erroredAliases.insert(path.first().toString());
        }
    }

    QHash<QString, bool> statuses;
    for (int index = 0; index < channelIds.size(); ++index)
    {
        const auto alias = QStringLiteral("status%1").arg(index);
        if (!data.contains(alias) || erroredAliases.contains(alias))
        {
            continue;
        }

        const auto status = data.value(alias).toObject();
        statuses.insert(channelIds.at(index),
                        !status.value(QStringLiteral("createdAt"))
                             .toString()
                             .isEmpty());
    }
    return statuses;
}


void TwitchGql::sendChatMessageWithNonce(
    const QString &channelId, const QString &message, const QString &nonce,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("message", message);
    input.insert("nonce", nonce);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "sendChatMessage",
        "0435464292cf380ed4b3d905e4edcb73078362e82c06367a5b2181c76c822fa2",
        variables, oauthToken, false)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch's response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                qCDebug(chatterinoTwitch)
                    << "Twitch API Error in sendChatMessage:" << gqlError;
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payloadValue =
                payloadDataObject(root).value("sendChatMessage");
            if (!payloadValue.isObject())
            {
                failureCallback(
                    "Twitch returned an empty response while sending the message");
                return;
            }

            const auto payload = payloadValue.toObject();
            for (const auto &errorKey : {
                     QStringLiteral("dropReason"),
                     QStringLiteral("error"),
                 })
            {
                const auto payloadError = gqlPayloadErrorMessage(
                    payload.value(errorKey),
                    QStringLiteral("Twitch rejected the message"));
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getVanityState(
    const QString &channelLogin, const QString &expectedChannelId,
    const TwitchGqlAuth &auth,
    std::function<void(GqlVanityState)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto QUERY = R"(
query ChatSettings_Badges($channelLogin: String!) {
  currentUser {
    id
    login
    displayName
    selectedBadge { id setID version title image1x: imageURL(size: NORMAL) image2x: imageURL(size: DOUBLE) image4x: imageURL(size: QUADRUPLE) }
    availableBadges { id setID version title image1x: imageURL(size: NORMAL) image2x: imageURL(size: DOUBLE) image4x: imageURL(size: QUADRUPLE) }
  }
  user(login: $channelLogin) {
    id
    self {
      selectedBadge { id setID version title image1x: imageURL(size: NORMAL) image2x: imageURL(size: DOUBLE) image4x: imageURL(size: QUADRUPLE) }
      availableBadges { id setID version title image1x: imageURL(size: NORMAL) image2x: imageURL(size: DOUBLE) image4x: imageURL(size: QUADRUPLE) }
    }
  }
}
)";

    makeAuthenticatedInlineGqlRequest(
        QUERY, QJsonObject{{"channelLogin", channelLogin}}, auth)
        .header("Accept-Language", "en-US")
        .onSuccess([expectedChannelId, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto currentUser = data.value("currentUser").toObject();
            const auto channelSelf =
                data.value("user").toObject().value("self").toObject();
            const auto channelId =
                data.value("user").toObject().value("id").toString();
            if (currentUser.isEmpty())
            {
                failureCallback("Twitch did not return badge settings");
                return;
            }
            if (!expectedChannelId.isEmpty() &&
                channelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }

            GqlVanityState state;
            state.currentUserId = currentUser.value("id").toString();
            state.currentUserLogin = currentUser.value("login").toString();
            state.currentUserDisplayName =
                currentUser.value("displayName").toString();
            state.channelId = channelId;
            state.globalBadges = vanityBadgesFromArray(
                currentUser.value("availableBadges").toArray());
            state.globalBadges.erase(
                std::remove_if(state.globalBadges.begin(),
                               state.globalBadges.end(),
                               [](const auto &badge) {
                                   return TwitchBadge::vanitySlotKeyForSet(
                                              badge.setId) !=
                                          QStringLiteral("tv");
                               }),
                state.globalBadges.end());
            const auto selectedGlobal = currentUser.value("selectedBadge");
            if (selectedGlobal.isObject())
            {
                auto badge = vanityBadgeFromObject(selectedGlobal.toObject());
                if (!badge.setId.isEmpty() &&
                    TwitchBadge::vanitySlotKeyForSet(badge.setId) ==
                        QStringLiteral("tv"))
                {
                    state.selectedGlobalBadge = std::move(badge);
                    const auto alreadyAvailable = std::ranges::any_of(
                        state.globalBadges, [&](const auto &available) {
                            return available.setId.compare(
                                       state.selectedGlobalBadge->setId,
                                       Qt::CaseInsensitive) == 0;
                        });
                    if (!alreadyAvailable)
                    {
                        state.globalBadges.push_back(*state.selectedGlobalBadge);
                    }
                }
            }

            state.channelBadges = vanityBadgesFromArray(
                channelSelf.value("availableBadges").toArray());
            state.channelBadges.erase(
                std::remove_if(state.channelBadges.begin(),
                               state.channelBadges.end(),
                               [](const auto &badge) {
                                   return TwitchBadge::vanitySlotKeyForSet(
                                              badge.setId) !=
                                          QStringLiteral("tv");
                               }),
                state.channelBadges.end());
            const auto selectedChannel = channelSelf.value("selectedBadge");
            if (selectedChannel.isObject())
            {
                auto badge = vanityBadgeFromObject(selectedChannel.toObject());
                if (!badge.setId.isEmpty() &&
                    TwitchBadge::vanitySlotKeyForSet(badge.setId) ==
                        QStringLiteral("tv"))
                {
                    state.selectedChannelBadge = std::move(badge);
                    const auto alreadyAvailable = std::ranges::any_of(
                        state.channelBadges, [&](const auto &available) {
                            return available.setId.compare(
                                       state.selectedChannelBadge->setId,
                                       Qt::CaseInsensitive) == 0;
                        });
                    if (!alreadyAvailable)
                    {
                        state.channelBadges.push_back(
                            *state.selectedChannelBadge);
                    }
                }
            }
            successCallback(std::move(state));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::selectGlobalBadge(
    const QString &setId, const QString &version, const TwitchGqlAuth &auth,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto MUTATION = R"(
mutation ChatSettings_SelectGlobalBadge($input: SelectGlobalBadgeInput!) {
  selectGlobalBadge(input: $input) { user { id } }
}
)";
    QJsonObject variables{{"input", QJsonObject{{"badgeSetID", setId},
                                                {"badgeSetVersion", version}}}};
    makeAuthenticatedInlineGqlRequest(MUTATION, variables, auth)
        .onSuccess(
            [successCallback, failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                const auto error = extractFirstGqlErrorMessage(root);
                if (!error.isEmpty())
                {
                    failureCallback("Twitch API Error: " + error);
                    return;
                }
                if (payloadDataObject(root)
                        .value("selectGlobalBadge")
                        .toObject()
                        .isEmpty())
                {
                    failureCallback("Twitch did not confirm the global badge");
                    return;
                }
                successCallback();
            })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::deselectGlobalBadge(
    const TwitchGqlAuth &auth, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto MUTATION = R"(
mutation ChatSettings_DeselectGlobalBadge {
  deselectGlobalBadge { user { id } }
}
)";
    makeAuthenticatedInlineGqlRequest(MUTATION, QJsonObject{}, auth)
        .onSuccess(
            [successCallback, failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                const auto error = extractFirstGqlErrorMessage(root);
                if (!error.isEmpty())
                {
                    failureCallback("Twitch API Error: " + error);
                    return;
                }
                if (payloadDataObject(root)
                        .value("deselectGlobalBadge")
                        .toObject()
                        .isEmpty())
                {
                    failureCallback("Twitch did not confirm the global badge");
                    return;
                }
                successCallback();
            })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::selectChannelBadge(
    const QString &channelId, const QString &setId, const QString &version,
    const TwitchGqlAuth &auth, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto MUTATION = R"(
mutation ChatSettings_SelectChannelBadge($input: SelectChannelBadgeInput!) {
  selectChannelBadge(input: $input) { isSuccessful user { id } }
}
)";
    QJsonObject variables{{"input", QJsonObject{{"badgeSetID", setId},
                                                {"badgeSetVersion", version},
                                                {"channelID", channelId}}}};
    makeAuthenticatedInlineGqlRequest(MUTATION, variables, auth)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            const auto error = extractFirstGqlErrorMessage(root);
            if (!error.isEmpty())
            {
                failureCallback("Twitch API Error: " + error);
                return;
            }
            const auto payload =
                payloadDataObject(root).value("selectChannelBadge").toObject();
            if (payload.isEmpty() || !payload.value("isSuccessful").toBool())
            {
                failureCallback("Twitch did not confirm the channel badge");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::deselectChannelBadge(
    const QString &channelId, const TwitchGqlAuth &auth,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto MUTATION = R"(
mutation ChatSettings_DeselectChannelBadge($input: DeselectChannelBadgeInput!) {
  deselectChannelBadge(input: $input) { user { id } }
}
)";
    QJsonObject variables{{"input", QJsonObject{{"channelID", channelId}}}};
    makeAuthenticatedInlineGqlRequest(MUTATION, variables, auth)
        .onSuccess(
            [successCallback, failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                const auto error = extractFirstGqlErrorMessage(root);
                if (!error.isEmpty())
                {
                    failureCallback("Twitch API Error: " + error);
                    return;
                }
                if (payloadDataObject(root)
                        .value("deselectChannelBadge")
                        .toObject()
                        .isEmpty())
                {
                    failureCallback("Twitch did not confirm the channel badge");
                    return;
                }
                successCallback();
            })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::banUserFromChatRoom(
    const QString &channelId, const QString &targetLogin, const QString &reason,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedChannelId = channelId.trimmed();
    const auto normalizedTarget = targetLogin.trimmed().toLower();
    if (normalizedChannelId.isEmpty() || normalizedTarget.isEmpty())
    {
        failureCallback("The channel or username is missing");
        return;
    }

    static constexpr auto MUTATION = R"(
mutation MoltorinoBanUserFromChatRoom($input: BanUserFromChatRoomInput!) {
  banUserFromChatRoom(input: $input) {
    ban {
      isPermanent
    }
    error {
      code
    }
  }
}
)";

    QJsonObject input;
    input.insert("channelID", normalizedChannelId);
    input.insert("bannedUserLogin", normalizedTarget);
    input.insert("expiresIn", QJsonValue::Null);
    if (!reason.trimmed().isEmpty())
    {
        input.insert("reason", reason.trimmed());
    }

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .hideRequestBody()
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch's ban response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("banUserFromChatRoom").toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return a ban result");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"), QStringLiteral("Failed to ban user"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            if (payload.value("ban").toObject().isEmpty())
            {
                failureCallback("Twitch did not confirm the ban");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unbanUserFromChatRoom(
    const QString &channelId, const QString &targetLogin,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedChannelId = channelId.trimmed();
    const auto normalizedTarget = targetLogin.trimmed().toLower();
    if (normalizedChannelId.isEmpty() || normalizedTarget.isEmpty())
    {
        failureCallback("The channel or username is missing");
        return;
    }

    static constexpr auto MUTATION = R"(
mutation MoltorinoUnbanUserFromChatRoom($input: UnbanUserFromChatRoomInput!) {
  unbanUserFromChatRoom(input: $input) {
    ban {
      isPermanent
    }
    error {
      code
    }
  }
}
)";

    QJsonObject input;
    input.insert("channelID", normalizedChannelId);
    input.insert("bannedUserLogin", normalizedTarget);

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch's unban response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload = payloadDataObject(root)
                                     .value("unbanUserFromChatRoom")
                                     .toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return an unban result");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"), QStringLiteral("Failed to unban user"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChatRoomBanStatuses(
    const QString &targetUserId, const QVector<QString> &channelIds,
    const QString &oauthToken,
    std::function<void(QHash<QString, bool>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedTarget = targetUserId.trimmed();
    if (normalizedTarget.isEmpty())
    {
        failureCallback("The target user ID is missing");
        return;
    }

    QVector<QString> normalizedChannels;
    normalizedChannels.reserve(channelIds.size());
    QSet<QString> seen;
    for (const auto &channelId : channelIds)
    {
        const auto normalized = channelId.trimmed();
        if (!normalized.isEmpty() && !seen.contains(normalized))
        {
            seen.insert(normalized);
            normalizedChannels.push_back(normalized);
        }
    }
    if (normalizedChannels.isEmpty())
    {
        successCallback({});
        return;
    }
    if (normalizedChannels.size() > 25)
    {
        failureCallback("Too many channels in one ban status request");
        return;
    }

    QStringList definitions{QStringLiteral("$targetID: ID!")};
    QStringList selections;
    QJsonObject variables;
    variables.insert(QStringLiteral("targetID"), normalizedTarget);
    for (int index = 0; index < normalizedChannels.size(); ++index)
    {
        const auto variable = QStringLiteral("channel%1").arg(index);
        const auto alias = QStringLiteral("status%1").arg(index);
        definitions.push_back(QStringLiteral("$%1: ID!").arg(variable));
        selections.push_back(
            QStringLiteral(
                "%1: chatRoomBanStatus(channelID: $%2, userID: $targetID) "
                "{ createdAt }")
                .arg(alias, variable));
        variables.insert(variable, normalizedChannels.at(index));
    }

    const auto query =
        QStringLiteral("query MoltorinoCrossChannelBanStatus(%1) { %2 }")
            .arg(definitions.join(QStringLiteral(", ")),
                 selections.join(QLatin1Char('\n')))
            .toUtf8();

    makeInlineGqlRequest(query.constData(), variables, oauthToken)
        .onSuccess([normalizedChannels, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch's ban status response");
                return;
            }

            const auto data = payloadDataObject(root);
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (data.isEmpty() && !gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }
            auto statuses = twitchgql::detail::parseChatRoomBanStatuses(
                root, normalizedChannels);
            successCallback(std::move(statuses));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::pinMessage(const QString &channelId, const QString &messageId,
                           int durationSeconds, const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("messageID", messageId);
    if (durationSeconds > 0)
    {
        input.insert("durationSeconds", durationSeconds);
    }
    input.insert("type", "MOD");
    variables.insert("input", input);

    makePersistedGqlRequest("PinChatMessage", "214191369c21f1ad67ac074795d53832329c70e4088c979040c9f86334a7d736", variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                qCDebug(chatterinoTwitch)
                    << "Twitch API Error in PinChatMessage:" << gqlError;
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("pinChatMessage").toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return a pin result");
                return;
            }
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to pin message"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getUserByLogin(
    const QString &login, const QString &oauthToken,
    std::function<void(std::optional<GqlUser>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr char QUERY[] = R"(
        query MoltorinoUserByLogin($login: String!) {
            user(login: $login) {
                id
                login
                displayName
            }
        }
    )";

    QJsonObject variables;
    variables.insert("login", login);

    makeGqlRequest(QUERY, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto userObject =
                payloadDataObject(root).value("user").toObject();
            GqlUser user;
            user.id = userObject.value("id").toString();
            user.login = userObject.value("login").toString();
            user.displayName = userObject.value("displayName").toString();

            if (user.id.isEmpty())
            {
                successCallback(std::nullopt);
                return;
            }

            successCallback(std::move(user));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::followUser(
    const QString &targetId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runFollowMutation(
        "FollowButton_FollowUser",
        "800e7346bdf7e5278a3c1d3f21b2b56e2639928f86815677a7126b093b2fdd08",
        "followUser", targetId, false, oauthToken,
        "Twitch did not follow the user", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::unfollowUser(
    const QString &targetId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runFollowMutation(
        "FollowButton_UnfollowUser",
        "f7dae976ebf41c755ae2d758546bfd176b4eeb856656098bb40e0a672ca0d880",
        "unfollowUser", targetId, false, oauthToken,
        "Twitch did not unfollow the user", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::getLatestModLogMessageBySender(
    const QString &channelId, const QString &senderId,
    const QString &oauthToken,
    std::function<void(std::optional<GqlModLogMessage>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("senderID", senderId);

    makePersistedGqlRequest(
        "ViewerCardModLogsMessagesBySender",
        "eb4e9869e1bb0b3ed553e1ed657fa09f8553781093569c3a5813ad09ee9c0776",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto messages = data.value("viewerCardModLogs")
                                      .toObject()
                                      .value("messages")
                                      .toObject();
            const auto edges = messages.value("edges").toArray();
            for (const auto &edgeValue : edges)
            {
                const auto node =
                    edgeValue.toObject().value("node").toObject();
                if (node.value("isDeleted").toBool(false))
                {
                    continue;
                }

                GqlModLogMessage message;
                message.id = node.value("id").toString();
                message.sentAt = node.value("sentAt").toString();
                message.text =
                    node.value("content").toObject().value("text").toString();
                if (!message.id.isEmpty())
                {
                    successCallback(std::move(message));
                    return;
                }
            }

            successCallback(std::nullopt);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getUsercardMessagesBySender(
    const QString &channelId, const QString &senderId, const QString &cursor,
    const QString &oauthToken,
    std::function<void(GqlUsercardMessagePage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("senderID", senderId);
    if (!cursor.isEmpty())
    {
        variables.insert("cursor", cursor);
    }

    makeTvPersistedGqlRequest(
        "ViewerCardModLogsMessagesBySender",
        "eb4e9869e1bb0b3ed553e1ed657fa09f8553781093569c3a5813ad09ee9c0776",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto messages = data.value("viewerCardModLogs")
                                      .toObject()
                                      .value("messages")
                                      .toObject();
            const auto edges = messages.value("edges").toArray();

            GqlUsercardMessagePage page;
            page.hasNextPage = messages.value("pageInfo")
                                   .toObject()
                                   .value("hasNextPage")
                                   .toBool(false);
            page.messages.reserve(edges.size());

            for (const auto &edgeValue : edges)
            {
                const auto edge = edgeValue.toObject();
                const auto node = edge.value("node").toObject();
                const auto content = node.value("content").toObject();
                const auto sender = node.value("sender").toObject();

                GqlUsercardMessage message;
                message.id = node.value("id").toString();
                message.sentAt = node.value("sentAt").toString();
                message.text = content.value("text").toString();
                message.cursor = edge.value("cursor").toString();
                message.isDeleted = node.value("isDeleted").toBool(false);
                message.deletedBy = node.value("lastUpdatedBy")
                                        .toObject()
                                        .value("displayName")
                                        .toString();
                message.senderId = sender.value("id").toString();
                message.senderLogin = sender.value("login").toString();
                message.senderDisplayName =
                    sender.value("displayName").toString();
                message.senderColor = sender.value("chatColor").toString();
                QStringList badges;
                const auto displayBadges =
                    sender.value("displayBadges").toArray();
                badges.reserve(displayBadges.size());
                for (const auto &badgeValue : displayBadges)
                {
                    const auto badge = badgeValue.toObject();
                    const auto setId = badge.value("setID").toString();
                    const auto version = badge.value("version").toString();
                    if (!setId.isEmpty() && !version.isEmpty())
                    {
                        badges.push_back(
                            QStringLiteral("%1/%2").arg(setId, version));
                    }
                }
                message.senderBadges = badges.join(u',');

                if (!message.cursor.isEmpty())
                {
                    page.nextCursor = message.cursor;
                }
                if (!message.id.isEmpty() && !message.text.isEmpty() &&
                    !message.senderLogin.isEmpty())
                {
                    page.messages.push_back(std::move(message));
                }
            }

            successCallback(std::move(page));
        })
        .onError([failureCallback](const NetworkResult &result) {
            auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                failureCallback(QString("Network Error: %1 | %2")
                                    .arg(result.formatError(), body.left(200)));
                return;
            }

            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModerationActionLogs(
    const QString &channelId, const QString &cursor, const QString &oauthToken,
    std::function<void(GqlModerationActionLogPage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("after", cursor.isEmpty() ? QJsonValue() : cursor);

    makeTvPersistedGqlRequest(
        "ModActionsList",
        "f09041ba19fdd3d0ceb6e9b9163d5d903eddd625e1b156e8af6deb207ed3e77e",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto logs = payloadDataObject(root)
                                  .value("channel")
                                  .toObject()
                                  .value("moderationActionLogs")
                                  .toObject();
            if (logs.isEmpty())
            {
                failureCallback("Twitch did not return moderation action logs");
                return;
            }

            GqlModerationActionLogPage page;
            const auto edges = logs.value("edges").toArray();
            page.actions.reserve(edges.size());
            for (const auto &edgeValue : edges)
            {
                const auto edge = edgeValue.toObject();
                auto action = moderationActionFromNode(edge);
                if (action.cursor.isEmpty())
                {
                    action.cursor = edge.value("cursor").toString();
                }
                page.nextCursor = action.cursor;
                page.actions.push_back(std::move(action));
            }
            page.hasNextPage = logs.value("pageInfo")
                                   .toObject()
                                   .value("hasNextPage")
                                   .toBool(!page.nextCursor.isEmpty());

            successCallback(std::move(page));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getUnbanRequests(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &cursor, bool newestFirst, const QString &oauthToken,
    std::function<void(GqlUnbanRequestPage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoUnbanRequests(
            $channelLogin: String!
            $cursor: Cursor
            $order: UnbanRequestsSortOrder!
        ) {
            channel(name: $channelLogin) {
                id
                unbanRequests(
                    first: 25
                    after: $cursor
                    options: { order: $order, status: PENDING }
                ) {
                    edges {
                        cursor
                        node {
                            id
                            createdAt
                            status
                            requester {
                                id
                                login
                                displayName
                                profileImageURL(width: 50)
                            }
                            requesterMessage
                            resolvedAt
                            resolverMessage
                            resolvedBy {
                                id
                                login
                                displayName
                                chatColor
                            }
                        }
                    }
                    pageInfo {
                        hasNextPage
                    }
                    totalCount(status: PENDING)
                }
                unbanRequestsSettings {
                    isEnabled
                    cooldownMinutes
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelLogin"), channelLogin);
    variables.insert(
        QStringLiteral("cursor"),
        cursor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cursor));
    variables.insert(QStringLiteral("order"), newestFirst
                                                  ? QStringLiteral("NEWEST")
                                                  : QStringLiteral("OLDEST"));

    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            if (channel.isEmpty())
            {
                failureCallback("Twitch did not return this channel");
                return;
            }
            if (!expectedChannelId.isEmpty() &&
                channel.value("id").toString() != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }

            const auto connection = channel.value("unbanRequests").toObject();
            if (connection.isEmpty())
            {
                failureCallback("Unban requests are unavailable");
                return;
            }
            GqlUnbanRequestPage page;
            page.totalCount = connection.value("totalCount").toInt(0);
            const auto settings =
                channel.value("unbanRequestsSettings").toObject();
            page.isEnabled = settings.value("isEnabled").toBool(true);
            page.cooldownMinutes = settings.value("cooldownMinutes").toInt(0);

            const auto edges = connection.value("edges").toArray();
            page.requests.reserve(edges.size());
            for (const auto &edgeValue : edges)
            {
                const auto edge = edgeValue.toObject();
                const auto edgeCursor =
                    edge.value(QStringLiteral("cursor")).toString();
                if (!edgeCursor.isEmpty())
                {
                    page.nextCursor = edgeCursor;
                }
                auto request = unbanRequestFromEdge(edge);
                if (request.id.isEmpty() || request.requester.id.isEmpty())
                {
                    continue;
                }
                page.requests.push_back(std::move(request));
            }

            page.hasNextPage = connection.value("pageInfo")
                                   .toObject()
                                   .value("hasNextPage")
                                   .toBool(false) &&
                               !page.nextCursor.isEmpty();
            successCallback(std::move(page));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getUnbanRequestUserContext(
    const QString &channelId, const QString &userId, const QString &oauthToken,
    std::function<void(GqlUnbanRequestUserContext)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoUnbanRequestUserContext(
            $channelID: ID!
            $targetID: ID!
        ) {
            user(id: $targetID) {
                id
                login
                displayName
                createdAt
                profileImageURL(width: 96)
                chatColor
            }
            viewerCardModLogs(channelID: $channelID, targetID: $targetID) {
                bans: targetedActions(first: 1, type: BAN) {
                    ... on ModLogsTargetedActionsConnection {
                        count
                    }
                }
                timeouts: targetedActions(first: 1, type: TIMEOUT) {
                    ... on ModLogsTargetedActionsConnection {
                        count
                    }
                }
            }
            chatRoomBanStatus(channelID: $channelID, userID: $targetID) {
                createdAt
                moderator {
                    id
                    login
                    displayName
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelID"), channelId);
    variables.insert(QStringLiteral("targetID"), userId);

    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess(
            [userId, successCallback, failureCallback](
                const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }
                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto data = payloadDataObject(root);
                GqlUnbanRequestUserContext context;
                context.user =
                    moderatorQueueUserFromObject(data.value("user").toObject());
                if (context.user.id.isEmpty() || context.user.id != userId)
                {
                    failureCallback("Twitch did not return this user");
                    return;
                }

                const auto logs = data.value("viewerCardModLogs").toObject();
                context.banCount =
                    logs.value("bans").toObject().value("count").toInt(0);
                context.timeoutCount =
                    logs.value("timeouts").toObject().value("count").toInt(0);

                const auto ban = data.value("chatRoomBanStatus").toObject();
                context.bannedAt = ban.value("createdAt").toString();
                context.bannedByLogin =
                    ban.value("moderator").toObject().value("login").toString();
                context.currentlyBanned = !context.bannedAt.isEmpty();
                successCallback(std::move(context));
            })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModeratorComments(
    const QString &channelId, const QString &userId, const QString &cursor,
    const QString &oauthToken,
    std::function<void(GqlModeratorCommentPage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoModeratorComments(
            $channelID: ID!
            $targetID: ID!
            $cursor: Cursor
        ) {
            viewerCardModLogs(channelID: $channelID, targetID: $targetID) {
                comments(first: 50, after: $cursor) {
                    ... on ModLogsCommentConnection {
                        edges {
                            cursor
                            node {
                                id
                                timestamp
                                text
                                isShareable
                                channel {
                                    id
                                    login
                                }
                                author {
                                    id
                                    login
                                    displayName
                                    chatColor
                                }
                            }
                        }
                        pageInfo {
                            hasNextPage
                        }
                    }
                    ... on ModLogsCommentsError {
                        code
                    }
                    __typename
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelID"), channelId);
    variables.insert(QStringLiteral("targetID"), userId);
    variables.insert(
        QStringLiteral("cursor"),
        cursor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cursor));

    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto comments = payloadDataObject(root)
                                      .value("viewerCardModLogs")
                                      .toObject()
                                      .value("comments")
                                      .toObject();
            const auto type = comments.value("__typename").toString();
            if (type.endsWith(QStringLiteral("Error")))
            {
                const auto code = comments.value("code").toString();
                failureCallback(
                    code.isEmpty()
                        ? QStringLiteral("Moderator comments are unavailable")
                        : QStringLiteral("Twitch API Error: ") + code);
                return;
            }
            if (type != QStringLiteral("ModLogsCommentConnection"))
            {
                failureCallback("Moderator comments are unavailable");
                return;
            }

            successCallback(moderatorCommentPage(comments, false));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getSharedModeratorComments(
    const QString &channelId, const QString &userId, const QString &cursor,
    const QString &oauthToken,
    std::function<void(GqlModeratorCommentPage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoSharedModeratorComments(
            $channelID: ID!
            $targetID: ID!
            $cursor: Cursor
        ) {
            viewerCardModLogs(channelID: $channelID, targetID: $targetID) {
                sharedComments(first: 50, after: $cursor) {
                    ... on SharedModLogsCommentConnection {
                        edges {
                            cursor
                            node {
                                id
                                timestamp
                                text
                                isShareable
                                channel {
                                    id
                                    login
                                }
                                author {
                                    id
                                    login
                                    displayName
                                    chatColor
                                }
                            }
                        }
                        pageInfo {
                            hasNextPage
                        }
                    }
                    ... on SharedModLogsCommentsError {
                        code
                    }
                    __typename
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelID"), channelId);
    variables.insert(QStringLiteral("targetID"), userId);
    variables.insert(
        QStringLiteral("cursor"),
        cursor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cursor));

    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess(
            [successCallback, failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }
                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto comments = payloadDataObject(root)
                                          .value("viewerCardModLogs")
                                          .toObject()
                                          .value("sharedComments")
                                          .toObject();
                const auto type = comments.value("__typename").toString();
                if (type.endsWith(QStringLiteral("Error")))
                {
                    const auto code = comments.value("code").toString();
                    failureCallback(
                        code.isEmpty()
                            ? QStringLiteral(
                                  "Shared moderator comments are unavailable")
                            : QStringLiteral("Twitch API Error: ") + code);
                    return;
                }
                if (type !=
                    QStringLiteral("SharedModLogsCommentConnection"))
                {
                    failureCallback(
                        "Shared moderator comments are unavailable");
                    return;
                }

                successCallback(moderatorCommentPage(comments, true));
            })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModeratorCommentSharingSetting(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(bool)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoModeratorCommentSharing($channelLogin: String!) {
            channel(name: $channelLogin) {
                id
                moderationSettings {
                    bansSharingSettings {
                        ... on BansSharingSettings {
                            isModCommentsSharingDisabled
                        }
                        ... on BansSharingSettingsError {
                            code
                        }
                        __typename
                    }
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelLogin"), channelLogin);
    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }
            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            if (channel.isEmpty() ||
                (!expectedChannelId.isEmpty() &&
                 channel.value("id").toString() != expectedChannelId))
            {
                failureCallback("Twitch returned a different channel");
                return;
            }
            const auto settings = channel.value("moderationSettings")
                                      .toObject()
                                      .value("bansSharingSettings")
                                      .toObject();
            const auto type = settings.value("__typename").toString();
            if (type == QStringLiteral("BansSharingSettingsError"))
            {
                failureCallback("Moderator comment sharing is unavailable");
                return;
            }
            if (type != QStringLiteral("BansSharingSettings") ||
                !settings.contains("isModCommentsSharingDisabled"))
            {
                failureCallback("Moderator comment sharing is unavailable");
                return;
            }
            successCallback(
                settings.value("isModCommentsSharingDisabled").toBool(true));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::createModeratorComment(
    const QString &channelId, const QString &userId, const QString &text,
    bool shareable, const QString &oauthToken,
    std::function<void(GqlModeratorComment)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        mutation MoltorinoCreateModeratorComment(
            $input: CreateModeratorCommentInput!
        ) {
            createModeratorComment(input: $input) {
                comment {
                    id
                    timestamp
                    text
                    isShareable
                    channel { id login }
                    author { id login displayName chatColor }
                }
            }
        }
    )";

    QJsonObject input;
    input.insert(QStringLiteral("channelID"), channelId);
    input.insert(QStringLiteral("targetID"), userId);
    input.insert(QStringLiteral("text"), text);
    input.insert(QStringLiteral("isShareable"), shareable);
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    makeInlineGqlRequest(query, variables, oauthToken)
        .hideRequestBody()
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }
            auto comment =
                moderatorCommentFromObject(payloadDataObject(root)
                                               .value("createModeratorComment")
                                               .toObject()
                                               .value("comment")
                                               .toObject(),
                                           false);
            if (comment.id.isEmpty())
            {
                failureCallback("Twitch did not return the new comment");
                return;
            }
            successCallback(std::move(comment));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::deleteModeratorComment(
    const QString &commentId, const QString &channelId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        mutation MoltorinoDeleteModeratorComment(
            $input: DeleteModeratorCommentInput!
        ) {
            deleteModeratorComment(input: $input) {
                comment { id }
            }
        }
    )";

    QJsonObject input;
    input.insert(QStringLiteral("ID"), commentId);
    input.insert(QStringLiteral("channelID"), channelId);
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess(
            [successCallback, failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }
                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }
                const auto id = payloadDataObject(root)
                                    .value("deleteModeratorComment")
                                    .toObject()
                                    .value("comment")
                                    .toObject()
                                    .value("id")
                                    .toString();
                if (id.isEmpty())
                {
                    failureCallback("Twitch did not confirm the deletion");
                    return;
                }
                successCallback();
            })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::resolveUnbanRequest(
    const QString &requestId, bool approve, const QString &moderatorNote,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedRequestId = requestId.trimmed();
    if (normalizedRequestId.isEmpty())
    {
        failureCallback("The unban request ID is missing");
        return;
    }
    static const char *approveMutation = R"(
        mutation MoltorinoApproveUnbanRequest(
            $input: ApproveUnbanRequestInput!
        ) {
            approveUnbanRequest(input: $input) {
                unbanRequest {
                    id
                    status
                    resolvedAt
                }
                error {
                    code
                }
            }
        }
    )";
    static const char *denyMutation = R"(
        mutation MoltorinoDenyUnbanRequest(
            $input: DenyUnbanRequestInput!
        ) {
            denyUnbanRequest(input: $input) {
                unbanRequest {
                    id
                    status
                    resolvedAt
                }
                error {
                    code
                }
            }
        }
    )";

    QJsonObject input;
    input.insert(QStringLiteral("id"), normalizedRequestId);
    input.insert(QStringLiteral("resolverMessage"),
                 moderatorNote.trimmed().isEmpty()
                     ? QJsonValue(QJsonValue::Null)
                     : QJsonValue(moderatorNote.trimmed()));
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto payloadName = approve ? QStringLiteral("approveUnbanRequest")
                                     : QStringLiteral("denyUnbanRequest");
    makeInlineGqlRequest(approve ? approveMutation : denyMutation, variables,
                         oauthToken)
        .hideRequestBody()
        .onSuccess([successCallback, failureCallback, payloadName,
                    normalizedRequestId](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto payload =
                payloadDataObject(root).value(payloadName).toObject();
            if (rejectGqlOrPayloadError(
                    root, payload, "Failed to resolve unban request",
                    failureCallback))
            {
                return;
            }
            const auto resolvedId =
                payload.value("unbanRequest").toObject().value("id").toString();
            if (resolvedId != normalizedRequestId)
            {
                failureCallback("Twitch API Error: Twitch returned a different "
                                "unban request");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unpinMessage(const QString &pinId,
                             const QString &oauthToken,
                             std::function<void()> successCallback,
                             std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", pinId);
    input.insert("reason", "UNPIN");
    variables.insert("input", input);

    makePersistedGqlRequest(
        "unpinChatMessage",
        "86409b9c86510bdc9f2c6d8e58fdc4041963c001de53577160ab649e03334511",
        variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("unpinChatMessage").toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return an unpin result");
                return;
            }
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to unpin message"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::updatePinnedMessage(const QString &pinId,
                                    std::optional<int> durationSeconds,
                                    const QString &oauthToken,
                                    std::function<void()> successCallback,
                                    std::function<void(const QString &)>
                                        failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", pinId);
    if (durationSeconds.has_value())
    {
        input.insert("durationSeconds", *durationSeconds);
    }
    variables.insert("input", input);

    makePersistedGqlRequest(
        "UpdatePinnedChatMessage",
        "e69a15a7aaa412857a066fc98f52e74ccefd5b82429c7d2bf747559ab78f6af9",
        variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload = payloadDataObject(root)
                                     .value("updatePinnedChatMessage")
                                     .toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return a pin update result");
                return;
            }
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to update pinned message"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getCurrentPin(
    const QString &channelId, std::shared_ptr<TwitchAccount> account,
    std::function<void(std::optional<TwitchChannel::PinnedMessage>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("count", 1);

    makePersistedGqlRequest(
        "GetPinnedChat",
        "2d099d4c9b6af80a07d8440140c4f3dbb04d516b35c401aab7ce8f60765308d5",
        variables, std::shared_ptr<TwitchAccount>(nullptr))
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto error = extractFirstGqlErrorMessage(doc);
            if (!error.isEmpty())
            {
                failureCallback("Twitch API Error: " + error);
                return;
            }

            const rapidjson::Value *dataVal = nullptr;

            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") &&
                     doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (!dataVal || !dataVal->HasMember("channel") ||
                !(*dataVal)["channel"].IsObject())
            {
                successCallback(std::nullopt);
                return;
            }

            const auto &channel = (*dataVal)["channel"];
            if (!channel.HasMember("pinnedChatMessages") ||
                !channel["pinnedChatMessages"].IsObject())
            {
                successCallback(std::nullopt);
                return;
            }

            const auto &pinnedChatMessages = channel["pinnedChatMessages"];
            if (!pinnedChatMessages.HasMember("edges") ||
                !pinnedChatMessages["edges"].IsArray())
            {
                successCallback(std::nullopt);
                return;
            }

            const auto &edges = pinnedChatMessages["edges"];
            if (edges.Empty() || !edges[0].IsObject() ||
                !edges[0].HasMember("node") || !edges[0]["node"].IsObject())
            {
                successCallback(std::nullopt);
                return;
            }

            const auto &node = edges[0]["node"];
            TwitchChannel::PinnedMessage pin;
            rj::getSafe(node, "id", pin.pinId);

            if (node.HasMember("pinnedMessage") &&
                node["pinnedMessage"].IsObject())
            {
                const auto &pinnedMessage = node["pinnedMessage"];
                rj::getSafe(pinnedMessage, "id", pin.messageId);

                if (pinnedMessage.HasMember("content") &&
                    pinnedMessage["content"].IsObject())
                {
                    const auto &content = pinnedMessage["content"];
                    rj::getSafe(content, "text", pin.text);
                    if (content.HasMember("fragments") &&
                        content["fragments"].IsArray() &&
                        content["fragments"].Size() <= 1000 &&
                        pin.text.size() <= 10000)
                    {
                        QString assembled;
                        QStringList emotes;
                        int offset = 0;
                        for (const auto &fragment :
                             content["fragments"].GetArray())
                        {
                            if (!fragment.IsObject())
                            {
                                break;
                            }
                            QString text;
                            rj::getSafe(fragment, "text", text);
                            const auto length = text.toUcs4().size();
                            if (fragment.HasMember("emoticon") &&
                                fragment["emoticon"].IsObject())
                            {
                                QString id;
                                rj::getSafe(fragment["emoticon"], "emoticonID",
                                            id);
                                static const QRegularExpression safeId(
                                    QStringLiteral("^[A-Za-z0-9_]{1,128}$"));
                                if (length > 0 && safeId.match(id).hasMatch())
                                {
                                    emotes.push_back(
                                        QString("%1:%2-%3")
                                            .arg(id)
                                            .arg(offset)
                                            .arg(offset + length - 1));
                                }
                            }
                            offset += length;
                            assembled += text;
                            if (assembled.size() > pin.text.size())
                            {
                                break;
                            }
                        }
                        if (assembled == pin.text)
                        {
                            pin.emotes = emotes.join(u'/');
                        }
                    }
                }
                if (pinnedMessage.HasMember("sender") &&
                    pinnedMessage["sender"].IsObject())
                {
                    const auto &sender = pinnedMessage["sender"];
                    rj::getSafe(sender, "displayName", pin.authorName);
                    if (!rj::getSafe(sender, "login", pin.authorLogin) ||
                        pin.authorLogin.isEmpty())
                    {
                        pin.authorLogin = pin.authorName;
                    }
                    rj::getSafe(sender, "id", pin.authorId);
                    rj::getSafe(sender, "chatColor", pin.authorColor);

                    if (sender.HasMember("displayBadges") &&
                        sender["displayBadges"].IsArray())
                    {
                        QStringList badgeList;
                        for (const auto &badge :
                             sender["displayBadges"].GetArray())
                        {
                            if (badge.IsObject())
                            {
                                QString setID, version;
                                rj::getSafe(badge, "setID", setID);
                                rj::getSafe(badge, "version", version);
                                if (!setID.isEmpty())
                                {
                                    badgeList
                                        << QString("%1/%2").arg(setID, version);
                                }
                            }
                        }
                        pin.authorBadges = badgeList.join(",");
                    }
                }
            }

            QString endsAtStr;
            if (rj::getSafe(node, "endsAt", endsAtStr) && !endsAtStr.isEmpty())
            {
                pin.endsAt = QDateTime::fromString(endsAtStr, Qt::ISODate);
            }

            QString updatedAtStr;
            if (rj::getSafe(node, "updatedAt", updatedAtStr) &&
                !updatedAtStr.isEmpty())
            {
                pin.pinnedAt = QDateTime::fromString(updatedAtStr, Qt::ISODate);
            }
            else
            {
                pin.pinnedAt = QDateTime::currentDateTimeUtc();
            }

            if (node.HasMember("pinnedBy") && node["pinnedBy"].IsObject())
            {
                const auto &pinnedBy = node["pinnedBy"];
                rj::getSafe(pinnedBy, "displayName", pin.pinnerName);
                if (!rj::getSafe(pinnedBy, "login", pin.pinnerLogin) ||
                    pin.pinnerLogin.isEmpty())
                {
                    pin.pinnerLogin = pin.pinnerName;
                }
            }

            if (pin.pinId.isEmpty() && pin.messageId.isEmpty())
            {
                failureCallback("Pinned message payload was incomplete");
                return;
            }

            successCallback(pin);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " +
                            QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::getActivePrediction(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::PredictionEvent>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin);

    static const char *predictionQuery = R"(
        query ChannelPointsPredictionContext($channelLogin: String!) {
            channel(name: $channelLogin) {
                id
                activePredictionEvents {
                    id
                    title
                    status
                    predictionWindowSeconds
                    createdAt
                    lockedAt
                    outcomes {
                        id
                        title
                        totalUsers
                        totalPoints
                        color
                        topPredictors {
                            points
                            user {
                                displayName
                                login
                            }
                        }
                    }
                    createdBy { ... on User { displayName login } }
                    lockedBy { ... on User { displayName login } }
                    endedBy { ... on User { displayName login } }
                }
                lockedPredictionEvents {
                    id
                    title
                    status
                    predictionWindowSeconds
                    createdAt
                    lockedAt
                    outcomes {
                        id
                        title
                        totalUsers
                        totalPoints
                        color
                        topPredictors {
                            points
                            user {
                                displayName
                                login
                            }
                        }
                    }
                    createdBy { ... on User { displayName login } }
                    lockedBy { ... on User { displayName login } }
                    endedBy { ... on User { displayName login } }
                }
            }
        }
    )";

    makeInlineGqlRequest(predictionQuery, variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback,
                    failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(doc);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") &&
                     doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (!dataVal || !dataVal->HasMember("channel") ||
                !(*dataVal)["channel"].IsObject())
            {
                if (!expectedChannelId.isEmpty())
                {
                    failureCallback("Twitch did not return this channel");
                    return;
                }
                successCallback(std::nullopt);
                return;
            }

            const auto &channel = (*dataVal)["channel"];
            QString returnedChannelId;
            rj::getSafe(channel, "id", returnedChannelId);
            if (!expectedChannelId.isEmpty() &&
                returnedChannelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }
            const rapidjson::Value *nodePtr = nullptr;

            if (channel.HasMember("activePredictionEvents") &&
                channel["activePredictionEvents"].IsArray())
            {
                const auto &activeEvents = channel["activePredictionEvents"];
                if (!activeEvents.Empty() && activeEvents[0].IsObject())
                {
                    nodePtr = &activeEvents[0];
                }
            }

            if (nodePtr == nullptr &&
                channel.HasMember("lockedPredictionEvents") &&
                channel["lockedPredictionEvents"].IsArray())
            {
                const auto &lockedEvents = channel["lockedPredictionEvents"];
                if (!lockedEvents.Empty() && lockedEvents[0].IsObject())
                {
                    nodePtr = &lockedEvents[0];
                }
            }

            if (nodePtr == nullptr && channel.HasMember("predictionEvents") &&
                channel["predictionEvents"].IsObject())
            {
                const auto &predictionEvents = channel["predictionEvents"];
                if (predictionEvents.HasMember("edges") &&
                    predictionEvents["edges"].IsArray())
                {
                    const auto &edges = predictionEvents["edges"];
                    if (!edges.Empty() && edges[0].IsObject() &&
                        edges[0].HasMember("node") &&
                        edges[0]["node"].IsObject())
                    {
                        nodePtr = &edges[0]["node"];
                    }
                }
            }

            if (nodePtr == nullptr)
            {
                successCallback(std::nullopt);
                return;
            }

            const auto &node = *nodePtr;
            TwitchChannel::PredictionEvent prediction;
            rj::getSafe(node, "id", prediction.id);
            rj::getSafe(node, "title", prediction.title);
            rj::getSafe(node, "status", prediction.status);
            rj::getSafe(node, "predictionWindowSeconds",
                        prediction.predictionWindowSeconds);

            if (prediction.status.compare("ACTIVE", Qt::CaseInsensitive) != 0 &&
                prediction.status.compare("LOCKED", Qt::CaseInsensitive) != 0)
            {
                successCallback(std::nullopt);
                return;
            }

            QString createdAtStr;
            if (rj::getSafe(node, "createdAt", createdAtStr) &&
                !createdAtStr.isEmpty())
            {
                prediction.createdAt =
                    QDateTime::fromString(createdAtStr, Qt::ISODate);
            }

            QString lockedAtStr;
            if (rj::getSafe(node, "lockedAt", lockedAtStr) &&
                !lockedAtStr.isEmpty())
            {
                prediction.lockedAt =
                    QDateTime::fromString(lockedAtStr, Qt::ISODate);
            }

            if (node.HasMember("createdBy") && node["createdBy"].IsObject())
            {
                rj::getSafe(node["createdBy"], "displayName",
                            prediction.createdByName);
                if (prediction.createdByName.isEmpty())
                {
                    rj::getSafe(node["createdBy"], "login",
                                prediction.createdByName);
                }
            }
            if (node.HasMember("lockedBy") && node["lockedBy"].IsObject())
            {
                rj::getSafe(node["lockedBy"], "displayName",
                            prediction.lockedByName);
                if (prediction.lockedByName.isEmpty())
                {
                    rj::getSafe(node["lockedBy"], "login",
                                prediction.lockedByName);
                }
            }
            if (node.HasMember("endedBy") && node["endedBy"].IsObject())
            {
                rj::getSafe(node["endedBy"], "displayName",
                            prediction.endedByName);
                if (prediction.endedByName.isEmpty())
                {
                    rj::getSafe(node["endedBy"], "login",
                                prediction.endedByName);
                }
            }

            if (node.HasMember("self") && node["self"].IsObject())
            {
                const auto &self = node["self"];
                rj::getSafe(self, "pointsParticipated", prediction.selfPoints);
                if (self.HasMember("outcome") && self["outcome"].IsObject())
                {
                    rj::getSafe(self["outcome"], "id",
                                prediction.selfOutcomeId);
                }
            }

            if (node.HasMember("outcomes") && node["outcomes"].IsArray())
            {
                const auto &outcomesArr = node["outcomes"];
                int outcomeCount = outcomesArr.Size();
                for (int i = 0; i < outcomeCount; ++i)
                {
                    if (!outcomesArr[i].IsObject())
                        continue;
                    const auto &oObj = outcomesArr[i];
                    TwitchChannel::PredictionOutcome outcome;
                    rj::getSafe(oObj, "id", outcome.id);
                    rj::getSafe(oObj, "title", outcome.title);
                    rj::getSafe(oObj, "totalUsers", outcome.totalUsers);

                    readInteger(oObj, "totalPoints", outcome.totalPoints);

                    if (outcomeCount == 2)
                        outcome.color = (i == 0) ? "BLUE" : "PINK";
                    else if (outcomeCount == 3)
                        outcome.color =
                            (i == 0) ? "BLUE" : (i == 1 ? "PINK" : "GREEN");
                    else
                        outcome.color = "BLUE";

                    if (oObj.HasMember("topPredictors") &&
                        oObj["topPredictors"].IsArray())
                    {
                        const auto &predictors =
                            oObj["topPredictors"].GetArray();
                        if (predictors.Size() > 0 && predictors[0].IsObject())
                        {
                            const auto &top = predictors[0];
                            readInteger(top, "points", outcome.topPoints);
                            if (top.HasMember("user") && top["user"].IsObject())
                            {
                                rj::getSafe(top["user"], "displayName",
                                            outcome.topPredictorName);
                                if (outcome.topPredictorName.isEmpty())
                                {
                                    rj::getSafe(top["user"], "login",
                                                outcome.topPredictorName);
                                }
                            }
                        }
                    }

                    prediction.outcomes.push_back(std::move(outcome));
                }
            }

            successCallback(prediction);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " +
                            QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::makePrediction(const QString &eventID, const QString &outcomeID,
                               int points, const QString &oauthToken,
                               std::function<void()> successCallback,
                               std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("eventID", eventID);
    input.insert("outcomeID", outcomeID);
    input.insert("points", points);

    auto uuid = generateUuid();
    uuid.remove('{').remove('}').remove('-');
    input.insert("transactionID", uuid);

    variables.insert("input", input);

    makePersistedGqlRequest("MakePrediction", "b44682ecc88358817009f20e69d75081b1e58825bb40aa53d5dbadcc17c881d8", variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            QString errorMessage;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() && doc[0].HasMember("errors")) {
                hasErrors = true;
                const auto &errors = doc[0]["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            } else if (doc.IsObject() && doc.HasMember("errors")) {
                hasErrors = true;
                const auto &errors = doc["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            }

            if (!hasErrors && doc.IsArray() && doc.Size() > 0 &&
                doc[0].IsObject() && doc[0].HasMember("data"))
            {
                const auto &data = doc[0]["data"];
                if (!data.IsObject() || !data.HasMember("makePrediction"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["makePrediction"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                    }
                }
            }
            else if (!hasErrors && doc.IsObject() && doc.HasMember("data"))
            {
                const auto &data = doc["data"];
                if (!data.IsObject() || !data.HasMember("makePrediction"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["makePrediction"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                    }
                }
            }
            else if (!hasErrors)
            {
                hasErrors = true;
            }

            if (hasErrors)
            {
                failureCallback(errorMessage.isEmpty()
                                    ? "Twitch API Error: Failed to place prediction"
                                    : "Twitch API Error: " + errorMessage);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::createPredictionEvent(
    const QString &channelId, const QString &title,
    const QStringList &outcomes, int predictionWindowSeconds,
    const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("title", title);
    input.insert("predictionWindowSeconds", predictionWindowSeconds);

    QJsonArray outcomesArray;
    const int outcomeCount = outcomes.size();
    for (int i = 0; i < outcomeCount; ++i)
    {
        QJsonObject outcome;
        outcome.insert("title", outcomes.at(i));
        outcome.insert("color", predictionCreateOutcomeColor(i, outcomeCount));
        outcomesArray.append(outcome);
    }
    input.insert("outcomes", outcomesArray);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "createPredictionEvent",
        "92268878ac4abe722bcdcba85a4e43acdd7a99d86b05851759e1d8f385cc32ea",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            QString errorMessage;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("errors"))
            {
                hasErrors = true;
                const auto &errors = doc[0]["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            }
            else if (doc.IsObject() && doc.HasMember("errors"))
            {
                hasErrors = true;
                const auto &errors = doc["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            }

            if (!hasErrors && doc.IsArray() && doc.Size() > 0 &&
                doc[0].IsObject() && doc[0].HasMember("data"))
            {
                const auto &data = doc[0]["data"];
                if (!data.IsObject() || !data.HasMember("createPredictionEvent"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["createPredictionEvent"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                        else if (payloadError.HasMember("message") &&
                                 payloadError["message"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["message"].GetString());
                        }
                    }
                    else if (!payload.HasMember("predictionEvent") ||
                             !payload["predictionEvent"].IsObject() ||
                             !payload["predictionEvent"].HasMember("id") ||
                             !payload["predictionEvent"]["id"].IsString() ||
                             QString::fromUtf8(payload["predictionEvent"]["id"]
                                                    .GetString())
                                 .isEmpty())
                    {
                        hasErrors = true;
                    }
                }
            }
            else if (!hasErrors && doc.IsObject() && doc.HasMember("data"))
            {
                const auto &data = doc["data"];
                if (!data.IsObject() || !data.HasMember("createPredictionEvent"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["createPredictionEvent"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                        else if (payloadError.HasMember("message") &&
                                 payloadError["message"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["message"].GetString());
                        }
                    }
                    else if (!payload.HasMember("predictionEvent") ||
                             !payload["predictionEvent"].IsObject() ||
                             !payload["predictionEvent"].HasMember("id") ||
                             !payload["predictionEvent"]["id"].IsString() ||
                             QString::fromUtf8(payload["predictionEvent"]["id"]
                                                    .GetString())
                                 .isEmpty())
                    {
                        hasErrors = true;
                    }
                }
            }
            else if (!hasErrors)
            {
                hasErrors = true;
            }

            if (hasErrors)
            {
                failureCallback(errorMessage.isEmpty()
                                    ? "Twitch API Error: Failed to create prediction"
                                    : "Twitch API Error: " + errorMessage);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::getPredictionTemplates(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(QVector<PredictionTemplate>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("count", 5);
    variables.insert("channelLogin", channelLogin);

    makePersistedGqlRequest(
        "ChannelPointsPredictionContext",
        "beb846598256b75bd7c1fe54a80431335996153e358ca9c7837ce7bb83d7d383",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            if (const auto error = extractFirstGqlErrorMessage(doc);
                !error.isEmpty())
            {
                failureCallback(error);
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") &&
                     doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (dataVal == nullptr || !dataVal->HasMember("community") ||
                !(*dataVal)["community"].IsObject())
            {
                failureCallback("Missing prediction history");
                return;
            }

            const auto &community = (*dataVal)["community"];
            if (!community.HasMember("channel") ||
                !community["channel"].IsObject())
            {
                failureCallback("Missing prediction channel");
                return;
            }

            const auto &channel = community["channel"];
            if (!channel.HasMember("resolvedPredictionEvents") ||
                !channel["resolvedPredictionEvents"].IsObject())
            {
                successCallback({});
                return;
            }

            const auto &connection = channel["resolvedPredictionEvents"];
            if (!connection.HasMember("edges") ||
                !connection["edges"].IsArray())
            {
                successCallback({});
                return;
            }

            QVector<PredictionTemplate> templates;
            templates.reserve(5);

            for (const auto &edge : connection["edges"].GetArray())
            {
                if (!edge.IsObject() || !edge.HasMember("node") ||
                    !edge["node"].IsObject())
                {
                    continue;
                }

                const auto &node = edge["node"];
                PredictionTemplate predictionTemplate;
                rj::getSafe(node, "title", predictionTemplate.title);
                rj::getSafe(node, "predictionWindowSeconds",
                            predictionTemplate.durationSeconds);

                if (predictionTemplate.title.trimmed().isEmpty() ||
                    !node.HasMember("outcomes") ||
                    !node["outcomes"].IsArray())
                {
                    continue;
                }

                for (const auto &outcome : node["outcomes"].GetArray())
                {
                    QString title;
                    if (outcome.IsObject() &&
                        rj::getSafe(outcome, "title", title) &&
                        !title.trimmed().isEmpty())
                    {
                        predictionTemplate.outcomes.push_back(title.trimmed());
                    }
                }

                if (predictionTemplate.outcomes.size() < 2)
                {
                    continue;
                }

                templates.push_back(std::move(predictionTemplate));
                if (templates.size() >= 5)
                {
                    break;
                }
            }

            successCallback(std::move(templates));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " +
                            QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::lockPrediction(
    const QString &eventId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", eventId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "LockPrediction",
        "1f2b1eb44af35f055308e78ffbe81c2f958408f9b32d076a759a84ab213285d4",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto error = predictionMutationError(
                result, QStringLiteral("Failed to lock prediction"));
            if (!error.isEmpty())
            {
                failureCallback("Twitch API Error: " + error);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::cancelPrediction(
    const QString &eventId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", eventId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "DeletePrediction",
        "35d375614e426624456ee7be4a2e0fbc0a410c0a91c21f6044cb3cd5c38c4e4d",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto error = predictionMutationError(
                result, QStringLiteral("Failed to delete prediction"));
            if (!error.isEmpty())
            {
                failureCallback("Twitch API Error: " + error);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::resolvePrediction(
    const QString &eventId, const QString &outcomeId,
    const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("eventID", eventId);
    input.insert("outcomeID", outcomeId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "ResolvePrediction",
        "10c803ec11bb8c2957d66bc6a47349dc3c5f51d694585b5ebc37ba656da413c1",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto error = predictionMutationError(
                result, QStringLiteral("Failed to resolve prediction"));
            if (!error.isEmpty())
            {
                failureCallback("Twitch API Error: " + error);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::createPollEvent(
    const QString &channelId, const QString &title, const QStringList &choices,
    int durationSeconds, std::optional<int> pointsPerVote,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("title", title);
    input.insert("durationSeconds", durationSeconds);
    input.insert("ownedBy", channelId);
    input.insert("multichoiceEnabled", true);
    input.insert("isCommunityPointsVotingEnabled", pointsPerVote.has_value());
    input.insert("communityPointsCost", pointsPerVote.value_or(0));

    QJsonArray choicesArray;
    for (const auto &choiceTitle : choices)
    {
        QJsonObject choice;
        choice.insert("title", choiceTitle);
        choicesArray.append(choice);
    }
    input.insert("choices", choicesArray);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "CreatePoll",
        "4b1461a13fe166a59044961db192747d606f71a89abc3bfdecf79fe862d205cf",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("createPoll").toObject();
            const auto payloadError = payload.value("error").toObject();
            if (!payloadError.isEmpty())
            {
                auto message = payloadError.value("message").toString();
                if (message.isEmpty())
                {
                    message = payloadError.value("code").toString();
                }
                failureCallback("Twitch API Error: " +
                                (message.isEmpty()
                                     ? QString("Failed to create poll")
                                     : message));
                return;
            }

            if (payload.value("poll").toObject().value("id").toString().isEmpty())
            {
                failureCallback("Twitch API Error: Failed to create poll");
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::terminatePoll(
    const QString &pollId, const QString &currentUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    sendTerminatePollRequest(pollId, currentUserId, oauthToken,
                             std::move(successCallback),
                             std::move(failureCallback));
}

void TwitchGql::archivePoll(
    const QString &pollId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("pollID", pollId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "ArchivePoll",
        "444ead3d68d94601cb66519e36c9f6c6fd9ba8b827a4299b8ed3604e57918d92",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("archivePoll").toObject();
            if (payload.value("poll").toObject().value("id").toString().isEmpty())
            {
                failureCallback("Twitch API Error: Failed to delete poll");
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::addChannelBlockedTerm(
    const QString &channelId, const QString &phrase, const QString &oauthToken,
    std::function<void(GqlAddBlockedTermResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{


    static constexpr auto MUTATION = R"(
        mutation AddChannelBlockedTerm($input: AddChannelBlockedTermInput!) {
            addChannelBlockedTerm(input: $input) {
                term {
                    id
                    phrase
                    expiresAt
                    isModEditable
                    hitCount
                }
                error
                wasRemovedFromPermittedList
            }
        }
    )";

    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("phrase", phrase);
    input.insert("isModEditable", true);

    QJsonObject variables;
    variables.insert("input", input);

    makeGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("addChannelBlockedTerm").toObject();
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Twitch rejected the blocked term"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            GqlAddBlockedTermResult addResult;
            addResult.term =
                blockedTermFromObject(payload.value("term").toObject());
            addResult.wasRemovedFromPermittedList =
                payload.value("wasRemovedFromPermittedList").toBool(false);

            if (addResult.term.id.isEmpty() || addResult.term.phrase.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to add blocked term");
                return;
            }

            successCallback(std::move(addResult));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelBlockedTerms(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlBlockedTerm>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);

    makePersistedGqlRequest(
        "BlockedTerms",
        "022dc6d166de51129700aa03482dca9e5fffc3a7045ba7f1deeaa3046a39577f",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            const auto blockedTerms = channel.value("blockedTerms").toObject();
            if (channel.isEmpty() || blockedTerms.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to fetch blocked terms");
                return;
            }

            QVector<GqlBlockedTerm> terms;
            const auto edges = blockedTerms.value("edges").toArray();
            terms.reserve(edges.size());
            for (const auto &edgeValue : edges)
            {
                const auto node =
                    edgeValue.toObject().value("node").toObject();
                auto term = blockedTermFromObject(node);
                if (!term.id.isEmpty() && !term.phrase.isEmpty())
                {
                    terms.push_back(std::move(term));
                }
            }

            const auto nodes = blockedTerms.value("nodes").toArray();
            terms.reserve(terms.size() + nodes.size());
            for (const auto &nodeValue : nodes)
            {
                auto term = blockedTermFromObject(nodeValue.toObject());
                if (!term.id.isEmpty() && !term.phrase.isEmpty())
                {
                    terms.push_back(std::move(term));
                }
            }

            successCallback(std::move(terms));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelSelfData(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(GqlChannelSelfData)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin.trimmed().toLower());

    makePersistedGqlRequest(
        "Chat_ChannelData",
        "863fda39ddc5ebac7453856eb00af2a587e27f48a2e521e9c01820c3c8c2c18a",
        variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            const auto self = channel.value("self").toObject();
            if (channel.isEmpty() || self.isEmpty())
            {
                failureCallback("Twitch API Error: Missing channel self data");
                return;
            }
            const auto returnedChannelId = channel.value("id").toString();
            if (!expectedChannelId.isEmpty() &&
                returnedChannelId != expectedChannelId)
            {
                failureCallback("Twitch API Error: Different channel");
                return;
            }

            successCallback(channelSelfDataFromObject(self));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::deleteChannelBlockedTerm(
    const QString &channelId, const QString &termId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("id", termId);
    input.insert("channelID", channelId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "DeleteChannelBlockedTerm",
        "bdfacf843eb536eef2720110cf73a4540506833b17a3f15313e461e57165c813",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("deleteChannelBlockedTermByID")
                    .toObject();
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to remove blocked term");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Twitch rejected the blocked term removal"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::grantVIP(const QString &channelId, const QString &targetLogin,
                         const QString &oauthToken,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "VIPUser",
        "e8c397f1ed8b1fdbaa201eedac92dd189ecfb2d828985ec159d4ae77f9920170",
        "grantVIP", "granteeLogin", channelId, targetLogin, oauthToken,
        "Failed to add VIP", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::revokeVIP(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "UnVIPUser",
        "2ce4fcdf6667d013aa1f820010e699d1d4abdda55e26539ecf4efba8aff2d661",
        "revokeVIP", "revokeeLogin", channelId, targetLogin, oauthToken,
        "Failed to remove VIP", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::modUser(const QString &channelId, const QString &targetLogin,
                        const QString &oauthToken,
                        std::function<void()> successCallback,
                        std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "ModUser",
        "46da4ec4229593fe4b1bce911c75625c299638e228262ff621f80d5067695a8a",
        "modUser", "targetLogin", channelId, targetLogin, oauthToken,
        "Failed to add moderator", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::unmodUser(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "UnmodUser",
        "1ed42ccb3bc3a6e79f51e954a2df233827f94491fbbb9bd05b22b1aaaf219b8b",
        "unmodUser", "targetLogin", channelId, targetLogin, oauthToken,
        "Failed to remove moderator", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::assignLeadModerator(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "AssignChannelRole",
        "2d373c90d0d0e6d4fe771bc6136febe6a148eb3d5700d2a0575883a043fbd581",
        "assignChannelRole", "targetUserID", channelId, targetUserId,
        oauthToken, "Failed to add lead moderator", "lead_mod", "isAssigned",
        std::move(successCallback), std::move(failureCallback));
}

void TwitchGql::unassignLeadModerator(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "UnassignChannelRole",
        "5edbf17877acdb91e65243b5148cfd15b98adc6d8f980492dcde9a7f2e8255e2",
        "unassignChannelRole", "targetUserID", channelId, targetUserId,
        oauthToken, "Failed to remove lead moderator", "lead_mod",
        "isUnassigned", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::addEditorUser(
    const QString &channelId, const QString &targetLogin,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "AddEditorUser",
        "3b52bf904ff9ce1b000ac2358080f538fbd1972c1869804f0d0f345d1a56676c",
        "addEditor", "targetUserLogin", channelId, targetLogin, oauthToken,
        "Failed to add editor", {}, {}, std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::removeEditorUser(
    const QString &channelId, const QString &targetLogin,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "RemoveEditorUser",
        "4699d38183050854dba547d07e340e72bf1f04578f1037a38a1189fa1827790f",
        "removeEditor", "targetUserLogin", channelId, targetLogin, oauthToken,
        "Failed to remove editor", {}, {}, std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::getRaidChannelIDs(
    const QString &sourceLogin, const QString &targetLogin,
    const QString &oauthToken,
    std::function<void(RaidChannelIDs)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("sourceLogin", normalizedRaidLogin(sourceLogin));
    variables.insert("targetLogin", normalizedRaidLogin(targetLogin));

    static const char *raidLookupQuery = R"(
        query MoltorinoRaidLookup($sourceLogin: String!, $targetLogin: String!) {
            source: user(login: $sourceLogin) {
                id
                login
                displayName
            }
            target: user(login: $targetLogin) {
                id
                login
                displayName
            }
        }
    )";

    makeInlineGqlRequest(raidLookupQuery, variables, oauthToken)
        .onSuccess([successCallback, failureCallback, sourceLogin,
                    targetLogin](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto sourceObject = data.value("source").toObject();
            const auto targetObject = data.value("target").toObject();

            RaidChannelIDs ids;
            ids.sourceId = raidUserIdFromObject(sourceObject);
            ids.targetId = raidUserIdFromObject(targetObject);

            ids.targetLogin =
                raidObjectString(targetObject,
                                 {
                                     QStringLiteral("login"),
                                     QStringLiteral("name"),
                                 });
            ids.targetDisplayName =
                raidObjectString(targetObject,
                                 {
                                     QStringLiteral("displayName"),
                                     QStringLiteral("display_name"),
                                     QStringLiteral("login"),
                                     QStringLiteral("name"),
                                 });
            if (ids.targetLogin.isEmpty())
            {
                ids.targetLogin = normalizedRaidLogin(targetLogin);
            }
            if (ids.targetDisplayName.isEmpty())
            {
                ids.targetDisplayName = ids.targetLogin;
            }

            if (ids.sourceId.isEmpty())
            {
                failureCallback(
                    QString("Could not resolve the broadcaster account for #%1.")
                        .arg(normalizedRaidLogin(sourceLogin)));
                return;
            }
            if (ids.targetId.isEmpty())
            {
                failureCallback(QString("Could not look up user: %1. Check the username or log in again.")
                                    .arg(normalizedRaidLogin(targetLogin)));
                return;
            }

            successCallback(std::move(ids));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::createRaid(
    const QString &sourceId, const QString &targetId, const QString &oauthToken,
    std::function<void(const QString &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("sourceID", sourceId);
    input.insert("targetID", targetId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "chatCreateRaid",
        "f4fc7ac482599d81dfb6aa37100923c8c9edeea9ca2be854102a6339197f840a",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(raidFailureMessage(gqlError));
                return;
            }

            const auto payload =
                payloadDataObject(root).value("createRaid").toObject();
            const auto payloadError = raidErrorMessage(payload.value("error"));
            if (!payloadError.isEmpty())
            {
                failureCallback(raidFailureMessage(payloadError));
                return;
            }

            const auto raidId =
                payload.value("raid").toObject().value("id").toString();
            if (raidId.isEmpty())
            {
                failureCallback("Failed to start raid");
                return;
            }

            successCallback(raidId);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendRaidNow(
    const QString &sourceId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("sourceID", sourceId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "GoRaid",
        "878ca88bed0c5a5f0687ad07562cffc0bf6a3136f15e5015c0f5f5f7f367f70a",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(raidFailureMessage(gqlError));
                return;
            }

            const auto dataError = gqlMutationDataError(
                root, QStringLiteral("Twitch did not confirm the raid"));
            if (!dataError.isEmpty())
            {
                failureCallback(raidFailureMessage(dataError));
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::cancelRaidGql(
    const QString &sourceId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("sourceID", sourceId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "CancelRaid",
        "42a2a699ac85256d72fff2471c75803f7ffbc767ba790725de5ad5d6e0163648",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(raidFailureMessage(gqlError));
                return;
            }

            const auto dataError = gqlMutationDataError(
                root, QStringLiteral("Twitch did not confirm the cancellation"));
            if (!dataError.isEmpty())
            {
                failureCallback(raidFailureMessage(dataError));
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::voteInPoll(const QString &pollId, const QString &choiceId,
                           const QString &userId, int extraVotes,
                           std::optional<int> pointsPerVote,
                           const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("pollID", pollId);
    input.insert("choiceID", choiceId);
    input.insert("userID", userId);
    input.insert("voteID",
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (extraVotes > 0)
    {
        if (!pointsPerVote.has_value() || *pointsPerVote <= 0)
        {
            failureCallback(
                "Twitch API Error: Paid voting is enabled, but the point cost is unknown. Refresh the poll and try again.");
            return;
        }

        if (extraVotes > std::numeric_limits<int>::max() / *pointsPerVote)
        {
            failureCallback("The requested vote cost is too large.");
            return;
        }
        QJsonObject tokens;
        tokens.insert("channelPoints", extraVotes * *pointsPerVote);
        input.insert("tokens", tokens);
    }
    variables.insert("input", input);

    static const char *voteInPollMutation = R"(
        mutation VoteInPoll($input: VoteInPollInput!) {
            voteInPoll(input: $input) {
                error {
                    code
                }
            }
        }
    )";

    makeInlineGqlRequest(voteInPollMutation, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("voteInPoll").toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return a vote result");
                return;
            }
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"), QStringLiteral("Failed to vote"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelPoints(const QString &channelLogin,
                                 const QString &oauthToken,
                                 std::function<void(qint64)> successCallback,
                                 std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin);

    static const char *channelPointsQuery = R"(
        query ChannelPointsContext($channelLogin: String!) {
            channel(name: $channelLogin) {
                self {
                    communityPoints {
                        balance
                    }
                }
            }
        }
    )";

    makeTvInlineGqlRequest(channelPointsQuery, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() && doc[0].HasMember("data") && doc[0]["data"].IsObject())
                dataVal = &doc[0]["data"];
            else if (doc.IsObject() && doc.HasMember("data") && doc["data"].IsObject())
                dataVal = &doc["data"];

            if (dataVal && dataVal->HasMember("community") && (*dataVal)["community"].IsObject())
            {
                const auto &community = (*dataVal)["community"];
                if (community.HasMember("channel") && community["channel"].IsObject())
                {
                    const auto &channel = community["channel"];
                    if (channel.HasMember("self") && channel["self"].IsObject())
                    {
                        const auto &self = channel["self"];
                        if (self.HasMember("communityPoints") && self["communityPoints"].IsObject())
                        {
                            const auto &cp = self["communityPoints"];
                            qint64 points = 0;
                            if (readInteger(cp, "balance", points))
                            {
                                successCallback(points);
                                return;
                            }
                        }
                    }
                }
            }
            if (dataVal && dataVal->HasMember("currentUser") && (*dataVal)["currentUser"].IsObject())
            {
                const auto &currentUser = (*dataVal)["currentUser"];
                if (currentUser.HasMember("communityPoints") && currentUser["communityPoints"].IsObject())
                {
                    const auto &cp = currentUser["communityPoints"];
                    qint64 points = 0;
                    if (readInteger(cp, "balance", points))
                    {
                        successCallback(points);
                        return;
                    }
                }
            }
            if (dataVal && dataVal->HasMember("channel") && (*dataVal)["channel"].IsObject())
            {
                const auto &channel = (*dataVal)["channel"];
                if (channel.HasMember("self") && channel["self"].IsObject())
                {
                    const auto &self = channel["self"];
                    if (self.HasMember("communityPoints") && self["communityPoints"].IsObject())
                    {
                        const auto &cp = self["communityPoints"];
                        qint64 points = 0;
                        if (readInteger(cp, "balance", points))
                        {
                            successCallback(points);
                            return;
                        }
                    }
                }
            }
            failureCallback("Could not parse channel points balance");
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
void TwitchGql::getRewardRequestOverview(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(GqlRewardRequestOverview)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoRewardRequestOverview($channelLogin: String!) {
            user(login: $channelLogin) {
                id
                channel {
                    id
                    communityPointsSettings {
                        isAvailable
                        isEnabled
                        summarizedRewards {
                            count
                            isCountAtMaximum
                            node {
                                id
                                title
                                prompt
                                cost
                                backgroundColor
                                isEnabled
                                isPaused
                                image {
                                    url
                                    url2x
                                }
                                defaultImage {
                                    url
                                    url2x
                                }
                            }
                        }
                    }
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelLogin"), channelLogin);
    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto user = payloadDataObject(root).value("user").toObject();
            const auto channel = user.value("channel").toObject();
            const auto settings =
                channel.value("communityPointsSettings").toObject();
            if (user.isEmpty() || channel.isEmpty() || settings.isEmpty())
            {
                failureCallback("Channel point requests are unavailable");
                return;
            }

            GqlRewardRequestOverview overview;
            overview.channelId =
                channel.value("id").toString(user.value("id").toString());
            if (!expectedChannelId.isEmpty() &&
                overview.channelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }
            overview.isAvailable = settings.value("isAvailable").toBool(false);
            overview.isEnabled = settings.value("isEnabled").toBool(false);

            const auto rewards = settings.value("summarizedRewards").toArray();
            overview.rewards.reserve(rewards.size());
            for (const auto &value : rewards)
            {
                const auto summarized = value.toObject();
                const auto object = summarized.value("node").toObject();
                GqlRewardRequestSummary reward;
                reward.id = object.value("id").toString();
                reward.title = object.value("title").toString();
                reward.prompt = object.value("prompt").toString();
                reward.backgroundColor =
                    object.value("backgroundColor").toString();
                reward.imageUrl = imageUrlFromRewardObject(object);
                reward.cost = object.value("cost").toInt(0);
                reward.pendingCount = summarized.value("count").toInt(0);
                reward.countAtMaximum =
                    summarized.value("isCountAtMaximum").toBool(false);
                reward.isEnabled = object.value("isEnabled").toBool(false);
                reward.isPaused = object.value("isPaused").toBool(false);
                if (reward.id.isEmpty())
                {
                    continue;
                }
                reward.pendingCount = std::max(0, reward.pendingCount);
                overview.totalPendingCount = int(std::min<qint64>(
                    std::numeric_limits<int>::max(),
                    qint64(overview.totalPendingCount) + reward.pendingCount));
                overview.countAtMaximum =
                    overview.countAtMaximum || reward.countAtMaximum;
                overview.rewards.push_back(std::move(reward));
            }
            successCallback(std::move(overview));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getRewardRequests(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &rewardId, const QString &cursor, const QString &oauthToken,
    std::function<void(GqlRewardRequestPage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static const char *query = R"(
        query MoltorinoRewardRequests(
            $channelLogin: String!
            $rewardID: ID
            $cursor: Cursor
        ) {
            user(login: $channelLogin) {
                id
                channel {
                    id
                    communityPointsRedemptionQueue(
                        options: {
                            rewardID: $rewardID
                            status: UNFULFILLED
                            order: OLDEST
                        }
                        first: 50
                        after: $cursor
                    ) {
                        edges {
                            cursor
                            node {
                                id
                                reward {
                                    id
                                    title
                                }
                                user {
                                    id
                                    login
                                    displayName
                                    chatColor
                                }
                                input
                                timestamp
                            }
                        }
                        pageInfo {
                            hasNextPage
                        }
                    }
                }
            }
        }
    )";

    QJsonObject variables;
    variables.insert(QStringLiteral("channelLogin"), channelLogin);
    variables.insert(QStringLiteral("rewardID"),
                     rewardId.isEmpty() ? QJsonValue(QJsonValue::Null)
                                        : QJsonValue(rewardId));
    variables.insert(
        QStringLiteral("cursor"),
        cursor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cursor));

    makeInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto user = payloadDataObject(root).value("user").toObject();
            const auto channel = user.value("channel").toObject();
            const auto connection =
                channel.value("communityPointsRedemptionQueue").toObject();
            if (user.isEmpty() || channel.isEmpty() || connection.isEmpty())
            {
                failureCallback("Twitch did not return reward requests");
                return;
            }
            const auto returnedChannelId =
                channel.value("id").toString(user.value("id").toString());
            if (!expectedChannelId.isEmpty() &&
                returnedChannelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }

            GqlRewardRequestPage page;
            const auto edges = connection.value("edges").toArray();
            page.requests.reserve(edges.size());
            for (const auto &edgeValue : edges)
            {
                const auto edge = edgeValue.toObject();
                const auto edgeCursor = edge.value("cursor").toString();
                if (!edgeCursor.isEmpty())
                {
                    page.nextCursor = edgeCursor;
                }
                const auto node = edge.value("node").toObject();
                GqlRewardRequest request;
                request.id = node.value("id").toString();
                request.cursor = edgeCursor;
                const auto reward = node.value("reward").toObject();
                request.rewardId = reward.value("id").toString();
                request.rewardTitle = reward.value("title").toString();
                request.user =
                    moderatorQueueUserFromObject(node.value("user").toObject());
                request.input = node.value("input").toString();
                request.timestamp = node.value("timestamp").toString();
                if (request.id.isEmpty())
                {
                    continue;
                }
                page.requests.push_back(std::move(request));
            }
            page.hasNextPage = connection.value("pageInfo")
                                   .toObject()
                                   .value("hasNextPage")
                                   .toBool(false) &&
                               !page.nextCursor.isEmpty();
            successCallback(std::move(page));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::updateRewardRequests(
    const QString &channelId, const QStringList &redemptionIds,
    GqlRewardRequestResolution resolution, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (redemptionIds.isEmpty())
    {
        failureCallback("Select at least one reward request");
        return;
    }

    static const char *mutation = R"(
        mutation MoltorinoUpdateRewardRequests(
            $input: UpdateCommunityPointsCustomRewardRedemptionStatusesByRedemptionsInput!
        ) {
            updateCommunityPointsCustomRewardRedemptionStatusesByRedemptions(
                input: $input
            ) {
                error {
                    code
                }
            }
        }
    )";

    QJsonArray ids;
    for (const auto &id : redemptionIds)
    {
        if (!id.trimmed().isEmpty())
        {
            ids.append(id.trimmed());
        }
    }
    if (ids.isEmpty())
    {
        failureCallback("Select at least one valid reward request");
        return;
    }
    if (channelId.trimmed().isEmpty())
    {
        failureCallback("Channel ID is missing");
        return;
    }
    QJsonObject input;
    input.insert(QStringLiteral("channelID"), channelId);
    input.insert(QStringLiteral("oldStatus"), QStringLiteral("UNFULFILLED"));
    input.insert(QStringLiteral("newStatus"),
                 resolution == GqlRewardRequestResolution::Complete
                     ? QStringLiteral("FULFILLED")
                     : QStringLiteral("CANCELED"));
    input.insert(QStringLiteral("redemptionIDs"), ids);
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    makeInlineGqlRequest(mutation, variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto payload = payloadDataObject(root)
                                     .value("updateCommunityPointsCustomRewardR"
                                            "edemptionStatusesByRedemptions")
                                     .toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to update reward requests",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not confirm the reward update");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::updateAllRewardRequests(
    const QString &channelId, const QString &rewardId,
    GqlRewardRequestResolution resolution, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (channelId.trimmed().isEmpty())
    {
        failureCallback("Channel ID is missing");
        return;
    }

    static const char *channelMutation = R"(
        mutation MoltorinoUpdateChannelRewardRequests(
            $input: UpdateCommunityPointsCustomRewardRedemptionStatusesByChannelInput!
        ) {
            updateCommunityPointsCustomRewardRedemptionStatusesByChannel(
                input: $input
            ) {
                error {
                    code
                }
            }
        }
    )";
    static const char *rewardMutation = R"(
        mutation MoltorinoUpdateRewardRequestsForReward(
            $input: UpdateCommunityPointsCustomRewardRedemptionStatusesByRewardInput!
        ) {
            updateCommunityPointsCustomRewardRedemptionStatusesByReward(
                input: $input
            ) {
                error {
                    code
                }
            }
        }
    )";

    QJsonObject input;
    input.insert(QStringLiteral("channelID"), channelId);
    input.insert(QStringLiteral("oldStatus"), QStringLiteral("UNFULFILLED"));
    input.insert(QStringLiteral("newStatus"),
                 resolution == GqlRewardRequestResolution::Complete
                     ? QStringLiteral("FULFILLED")
                     : QStringLiteral("CANCELED"));
    if (!rewardId.isEmpty())
    {
        input.insert(QStringLiteral("rewardID"), rewardId);
    }
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto payloadName =
        rewardId.isEmpty() ? QStringLiteral("updateCommunityPointsCustomRewardR"
                                            "edemptionStatusesByChannel")
                           : QStringLiteral("updateCommunityPointsCustomRewardR"
                                            "edemptionStatusesByReward");
    makeInlineGqlRequest(rewardId.isEmpty() ? channelMutation : rewardMutation,
                         variables, oauthToken)
        .onSuccess([successCallback, failureCallback,
                    payloadName](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto payload =
                payloadDataObject(root).value(payloadName).toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to update reward requests",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not confirm the reward update");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendGigantifiedChatEmote(
    const QString &channelId, const QString &emoteId,
    const QString &message, int bitsCost, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("bitsCost", bitsCost);


    input.insert("message", message);
    input.insert("emoteID", emoteId);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    static const char *query = R"(
        mutation SendGigantifiedChatEmote($input: SendGigantifiedChatEmoteInput!) {
            sendGigantifiedChatEmote(input: $input) {
                error {
                    code
                }
            }
        }
    )";

    makeTvInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("sendGigantifiedChatEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to gigantify emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to gigantify emote");
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelPointRewards(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRewards)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin);
    variables.insert("includeGoalTypes",
                     QJsonArray{QStringLiteral("CREATOR"),
                                QStringLiteral("BOOST")});

    makeTvPersistedGqlRequest(
        "ChannelPointsContext",
        "7fe050e3761eb2cf258d70ee1a21cbd76fa8cf3d7e7b12fc437e7029d446b5e3",
        variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto community = data.value("community").toObject();
            const auto channel = community.value("channel").toObject();
            const auto settings =
                channel.value("communityPointsSettings").toObject();
            const auto self = channel.value("self").toObject();
            const auto points =
                self.value("communityPoints").toObject();

            if (community.isEmpty() || channel.isEmpty() || settings.isEmpty())
            {
                failureCallback("Channel point rewards are unavailable");
                return;
            }

            GqlChannelPointRewards rewards;
            rewards.channelId = community.value("id").toString();
            if (!expectedChannelId.isEmpty() &&
                rewards.channelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }
            rewards.channelDisplayName =
                community.value("displayName").toString();
            rewards.balance = jsonIntegerValue(points.value("balance"));

            for (const auto &value : settings.value("customRewards").toArray())
            {
                const auto reward =
                    channelPointRewardFromObject(value.toObject(), false);
                if (reward.pricingType != "POINTS" || reward.cost <= 0)
                {
                    continue;
                }
                rewards.rewards.push_back(reward);
            }

            for (const auto &value :
                 settings.value("automaticRewards").toArray())
            {
                const auto reward =
                    channelPointRewardFromObject(value.toObject(), true);
                const bool isPointsReward = reward.pricingType == "POINTS";
                const bool isGigantifyBitsReward =
                    reward.rewardType == "SEND_GIGANTIFIED_EMOTE" &&
                    reward.pricingType == "BITS";
                if ((!isPointsReward && !isGigantifyBitsReward) ||
                    reward.cost <= 0)
                {
                    continue;
                }
                rewards.rewards.push_back(reward);
            }

            successCallback(std::move(rewards));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::redeemCustomReward(
    const QString &channelId, const GqlChannelPointReward &reward,
    const QString &textInput, const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", reward.cost);
    input.insert("pricingType", "POINTS");
    input.insert("rewardID", reward.id);
    input.insert("title", reward.title);
    input.insert("transactionID", makeTransactionId());
    input.insert("prompt", reward.prompt.trimmed().isEmpty()
                               ? QJsonValue(QJsonValue::Null)
                               : QJsonValue(reward.prompt));
    if (!textInput.trimmed().isEmpty())
    {
        input.insert("textInput", textInput);
    }

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "RedeemCustomReward",
        "d56249a7adb4978898ea3412e196688d4ac3cea1c0c2dfd65561d229ea5dcc42",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("redeemCommunityPointsCustomReward")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to redeem reward",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to redeem reward");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendHighlightedChatMessage(
    const QString &channelId, int cost, const QString &message,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", cost);
    input.insert("message", message);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "SendHighlightedChatMessage",
        "bb187d763156dc5c25c6457e1b32da6c5033cb7504854e6d33a8b876d10444b6",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("sendHighlightedChatMessage")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to send highlighted message",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to send highlighted message");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendSubOnlyBypassMessage(
    const QString &channelId, int cost, const QString &message,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", cost);
    input.insert("message", message);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    static const char *query = R"(
        mutation SendSubsOnlyMessage($input: SendChatMessageThroughSubscriberModeInput!) {
            sendChatMessageThroughSubscriberMode(input: $input) {
                balance
                error {
                    code
                }
            }
        }
    )";

    makeTvInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("sendChatMessageThroughSubscriberMode")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to send sub-only message",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to send sub-only message");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unlockRandomSubscriberEmote(
    const QString &channelId, int cost, const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", cost);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "UnlockRandomSubscriberEmote",
        "f548e89966b21d0094f3dc35233232eb6ec76d63e02594c8a494407712a85350",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("unlockRandomSubscriberEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to unlock emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to unlock emote");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unlockChosenSubscriberEmote(
    const QString &channelId, const QString &emoteId, int cost,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("emoteID", emoteId);
    input.insert("cost", cost);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    static const char *query = R"(
        mutation UnlockChosenSubscriberEmote($input: UnlockChosenSubscriberEmoteInput!) {
            unlockChosenSubscriberEmote(input: $input) {
                balance
                error {
                    code
                }
            }
        }
    )";

    makeTvInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("unlockChosenSubscriberEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to unlock emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to unlock emote");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unlockModifiedSubscriberEmote(
    const QString &channelId, const QString &modifiedEmoteId, int cost,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("emoteID", modifiedEmoteId);
    input.insert("cost", cost);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "UnlockModifiedEmote",
        "30e8cc29b1d6d96809f5e35f5e7a550ae8bf5d26966a9637d919477ffd0bfc52",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("unlockChosenModifiedSubscriberEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to unlock emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to unlock emote");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getAvailableChannelPointEmotes(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelOwnerID", channelId);

    makeTvPersistedGqlRequest(
        "EmotePicker_EmotePicker_UserSubscriptionProducts",
        "511bebfb513d0127d24a7fe49aa2b7717306a611e1f4269a93e0cc76e8a65a81",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            QVector<GqlChannelPointEmote> emotes;
            QSet<QString> seenIds;

            const auto user = payloadDataObject(root).value("user").toObject();
            const auto ownerLogin = user.value("login").toString();
            const auto ownerDisplayName = user.value("displayName").toString();
            for (const auto &productValue :
                 user.value("subscriptionProducts").toArray())
            {
                const auto product = productValue.toObject();
                for (const auto &emoteValue :
                     product.value("emotes").toArray())
                {
                    const auto emoteObj = emoteValue.toObject();
                    GqlChannelPointEmote emote;
                    emote.id = emoteObj.value("id").toString();
                    emote.token = emoteObj.value("token").toString();
                    emote.type = emoteObj.value("assetType")
                                     .toString(emoteObj.value("type")
                                                   .toString());
                    emote.ownerLogin = ownerLogin;
                    emote.ownerDisplayName = ownerDisplayName;
                    if (emote.id.isEmpty() || emote.token.isEmpty() ||
                        seenIds.contains(emote.id))
                    {
                        continue;
                    }
                    seenIds.insert(emote.id);
                    emotes.push_back(std::move(emote));
                }
            }

            successCallback(std::move(emotes));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getAvailableGigantifyEmotes(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    struct FetchState {
        QString channelId;
        QString oauthToken;
        QVector<GqlChannelPointEmote> emotes;
        QSet<QString> seenEmoteIds;
        QSet<QString> seenCursors;
        bool completed = false;
        int pageCount = 0;
        std::shared_ptr<std::function<void(QString)>> requestPage;
        std::function<void(QVector<GqlChannelPointEmote>)> successCallback;
        std::function<void(const QString &)> failureCallback;
    };

    static constexpr auto QUERY = R"(
        query AvailableEmotesForChannelPaginated(
            $channelID: ID!
            $withOwner: Boolean!
            $pageLimit: Int!
            $cursor: Cursor
        ) {
            channel(id: $channelID) {
                id
                self {
                    availableEmoteSetsPaginated(
                        pageLimit: $pageLimit
                        after: $cursor
                    ) {
                        edges {
                            cursor
                            node {
                                id
                                emotes {
                                    id
                                    setID
                                    token
                                    modifiers {
                                        code
                                        name
                                    }
                                    type
                                    assetType
                                }
                                owner @include(if: $withOwner) {
                                    id
                                    login
                                    displayName
                                    profileImageURL(width: 28)
                                }
                            }
                        }
                        pageInfo {
                            hasNextPage
                        }
                    }
                }
            }
        }
    )";

    static constexpr int PAGE_LIMIT = 350;
    static constexpr int MAX_PAGES = 100;

    auto state = std::make_shared<FetchState>();
    state->channelId = channelId;
    state->oauthToken = oauthToken;
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    auto finishSuccess = [](const std::shared_ptr<FetchState> &state) {
        if (state->completed || !state->successCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->successCallback);
        callback(std::move(state->emotes));
    };

    auto finishFailure = [](const std::shared_ptr<FetchState> &state,
                            const QString &error) {
        if (state->completed || !state->failureCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->failureCallback);
        callback(error);
    };

    auto requestPage = std::make_shared<std::function<void(QString)>>();
    state->requestPage = requestPage;
    std::weak_ptr<FetchState> weakState = state;
    std::weak_ptr<std::function<void(QString)>> weakRequestPage = requestPage;
    *requestPage = [weakState, weakRequestPage, finishSuccess,
                    finishFailure](QString cursor) {
        const auto state = weakState.lock();
        if (!state || state->completed)
        {
            return;
        }
        if (++state->pageCount > MAX_PAGES)
        {
            finishFailure(state,
                          "Twitch returned too many available emote pages");
            return;
        }

        QJsonObject variables;
        variables.insert("channelID", state->channelId);
        variables.insert("withOwner", true);
        variables.insert("pageLimit", PAGE_LIMIT);
        if (!cursor.isEmpty())
        {
            variables.insert("cursor", cursor);
        }

        makeTvInlineGqlRequest(QUERY, variables, state->oauthToken)
            .onSuccess([state, weakRequestPage, finishSuccess,
                        finishFailure](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    finishFailure(state, "Failed to parse GQL response");
                    return;
                }
                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    finishFailure(state, "Twitch API Error: " + gqlError);
                    return;
                }

                const auto channel =
                    payloadDataObject(root).value("channel").toObject();
                const auto self = channel.value("self").toObject();
                const auto connection =
                    self.value("availableEmoteSetsPaginated").toObject();
                if (channel.isEmpty() || self.isEmpty() ||
                    connection.isEmpty())
                {
                    finishFailure(state,
                                  "Available Twitch emotes are unavailable");
                    return;
                }

                QString nextCursor;
                for (const auto &edgeValue :
                     connection.value("edges").toArray())
                {
                    const auto edge = edgeValue.toObject();
                    const auto edgeCursor = edge.value("cursor").toString();
                    if (!edgeCursor.isEmpty())
                    {
                        nextCursor = edgeCursor;
                    }

                    const auto set = edge.value("node").toObject();
                    const auto owner = set.value("owner").toObject();
                    const auto ownerLogin = owner.value("login").toString();
                    const auto ownerDisplayName =
                        owner.value("displayName").toString();
                    for (const auto &emoteValue :
                         set.value("emotes").toArray())
                    {
                        const auto emoteObject = emoteValue.toObject();
                        GqlChannelPointEmote emote;
                        emote.id = emoteObject.value("id").toString();
                        emote.token = emoteObject.value("token").toString();
                        emote.type =
                            emoteObject.value("assetType")
                                .toString(emoteObject.value("type").toString());
                        emote.ownerLogin = ownerLogin;
                        emote.ownerDisplayName = ownerDisplayName;
                        if (emote.id.isEmpty() || emote.token.isEmpty() ||
                            state->seenEmoteIds.contains(emote.id))
                        {
                            continue;
                        }

                        if (state->emotes.size() >= 25000)
                        {
                            finishFailure(state, "Twitch returned too many available emotes");
                            return;
                        }
                        state->seenEmoteIds.insert(emote.id);
                        state->emotes.push_back(emote);

                        for (const auto &modifierValue :
                             emoteObject.value("modifiers").toArray())
                        {
                            const auto code = modifierValue.toObject()
                                                  .value("code")
                                                  .toString()
                                                  .trimmed();
                            if (code.isEmpty())
                            {
                                continue;
                            }

                            auto variant = emote;
                            variant.id =
                                emote.id + QStringLiteral("_") + code;
                            variant.token =
                                emote.token + QStringLiteral("_") + code;
                            if (state->seenEmoteIds.contains(variant.id))
                            {
                                continue;
                            }

                            if (state->emotes.size() >= 25000)
                            {
                                finishFailure(state, "Twitch returned too many available emotes");
                                return;
                            }
                            state->seenEmoteIds.insert(variant.id);
                            state->emotes.push_back(std::move(variant));
                        }
                    }
                }

                const auto hasNextPage = connection.value("pageInfo")
                                             .toObject()
                                             .value("hasNextPage")
                                             .toBool(false);
                if (!hasNextPage)
                {
                    finishSuccess(state);
                    return;
                }
                if (nextCursor.isEmpty())
                {
                    finishFailure(
                        state,
                        "Twitch did not return an available emote cursor");
                    return;
                }
                if (state->seenCursors.contains(nextCursor))
                {
                    finishFailure(
                        state,
                        "Twitch repeated an available emote pagination cursor");
                    return;
                }

                state->seenCursors.insert(nextCursor);
                if (const auto nextPage = weakRequestPage.lock())
                {
                    (*nextPage)(nextCursor);
                }
            })
            .onError([state, finishFailure](const NetworkResult &result) {
                finishFailure(state,
                              "Network Error: " + result.formatError());
            })
            .execute();
    };

    (*requestPage)({});
}

void TwitchGql::getModifiableChannelPointEmotes(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject contextVariables;
    contextVariables.insert("channelLogin", channelLogin);
    contextVariables.insert("includeGoalTypes",
                            QJsonArray{QStringLiteral("CREATOR"),
                                       QStringLiteral("BOOST")});

    QJsonArray payloadArray;
    payloadArray.append(persistedPayload(
        "ModifyEmoteOwnedEmotes", QJsonObject{},
        "e882551bf6a6abf14a1ec2deac4fe9a0af22f89f863818f7228da98d6b849cb4"));
    payloadArray.append(persistedPayload(
        "ChannelPointsContext", contextVariables,
        "7fe050e3761eb2cf258d70ee1a21cbd76fa8cf3d7e7b12fc437e7029d446b5e3"));

    makeTvPersistedGqlBatchRequest(payloadArray, oauthToken)
        .onSuccess([expectedChannelId, successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            QSet<QString> seenOwnedIds;
            const auto currentUser =
                payloadDataObjectForOperation(root, "ModifyEmoteOwnedEmotes")
                    .value("currentUser")
                    .toObject();
            for (const auto &setValue : currentUser.value("emoteSets").toArray())
            {
                const auto set = setValue.toObject();
                for (const auto &emoteValue : set.value("emotes").toArray())
                {
                    const auto emoteObj = emoteValue.toObject();
                    const auto id = emoteObj.value("id").toString();
                    if (!id.isEmpty())
                    {
                        seenOwnedIds.insert(id);
                    }
                    for (const auto &modifierValue :
                         emoteObj.value("modifiers").toArray())
                    {
                        const auto code = modifierValue.toObject()
                                              .value("code")
                                              .toString();
                        if (!id.isEmpty() && !code.isEmpty())
                        {
                            seenOwnedIds.insert(id + "_" + code);
                        }
                    }
                }
            }

            QVector<GqlChannelPointEmote> emotes;
            QSet<QString> seenVariantIds;
            const auto data =
                payloadDataObjectForOperation(root, "ChannelPointsContext");
            const auto community = data.value("community").toObject();
            const auto channel = community.value("channel").toObject();
            const auto settings =
                channel.value("communityPointsSettings").toObject();
            if (community.isEmpty() || channel.isEmpty() || settings.isEmpty())
            {
                failureCallback("Channel point emotes are unavailable");
                return;
            }
            const auto returnedChannelId = community.value("id").toString();
            if (!expectedChannelId.isEmpty() &&
                returnedChannelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }
            const auto ownerLogin = community.value("login").toString();
            const auto ownerDisplayName =
                community.value("displayName").toString();

            for (const auto &variantValue :
                 settings.value("emoteVariants").toArray())
            {
                const auto variant = variantValue.toObject();
                const auto baseObj = variant.value("emote").toObject();
                GqlChannelPointEmote emote;
                emote.id = baseObj.value("id").toString(
                    variant.value("id").toString());
                emote.token = baseObj.value("token").toString();
                emote.type = QStringLiteral("CHANNEL_POINTS_VARIANT");
                emote.ownerLogin = ownerLogin;
                emote.ownerDisplayName = ownerDisplayName;

                if (emote.id.isEmpty() || emote.token.isEmpty() ||
                    seenVariantIds.contains(emote.id))
                {
                    continue;
                }

                const auto isUnlockable =
                    variant.value("isUnlockable").toBool(false);
                if (!isUnlockable && !seenOwnedIds.contains(emote.id))
                {
                    continue;
                }

                for (const auto &modificationValue :
                     variant.value("modifications").toArray())
                {
                    const auto modification = modificationValue.toObject();
                    const auto modifierId = modification.value("modifier")
                                                .toObject()
                                                .value("id")
                                                .toString();
                    const auto modifiedEmote =
                        modification.value("emote").toObject();

                    GqlChannelPointEmoteModification parsedModification;
                    parsedModification.modifierId = modifierId;
                    parsedModification.emoteId =
                        modifiedEmote.value("id").toString(
                            modification.value("id").toString());
                    parsedModification.emoteToken =
                        modifiedEmote.value("token").toString();

                    if (parsedModification.modifierId.isEmpty() ||
                        parsedModification.emoteId.isEmpty() ||
                        seenOwnedIds.contains(parsedModification.emoteId))
                    {
                        continue;
                    }
                    if (parsedModification.emoteToken.isEmpty())
                    {
                        parsedModification.emoteToken = emote.token;
                    }

                    emote.modifications.push_back(std::move(parsedModification));
                }

                if (emote.modifications.isEmpty())
                {
                    continue;
                }

                seenVariantIds.insert(emote.id);
                emotes.push_back(std::move(emote));
            }

            successCallback(std::move(emotes));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelPointEmoteModifiers(
    const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmoteModifier>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    makeTvPersistedGqlRequest(
        "ChannelPointsGlobalContext",
        "d3fa3a96e78a3e62bdd3ef3c4effafeda52442906cec41a9440e609a388679e2",
        {}, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            QVector<GqlChannelPointEmoteModifier> modifiers;
            for (const auto &value :
                 payloadDataObject(root).value("emoteModifiers").toArray())
            {
                const auto obj = value.toObject();
                GqlChannelPointEmoteModifier modifier;
                modifier.id = obj.value("id").toString();
                modifier.title = obj.value("title").toString();
                if (!modifier.id.isEmpty())
                {
                    modifiers.push_back(std::move(modifier));
                }
            }

            successCallback(std::move(modifiers));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}
#endif

void TwitchGql::getChatWarningStatus(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::ChatWarning>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("targetUserID", targetUserId);

    makePersistedGqlRequest(
        "ChatModeratorStrikeStatus",
        "7f50f7190a840cd9fe9a91398f34ebb690eeba7cb28bce70e4cbf7ed1d06f268",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback, channelId](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto status =
                payloadDataObject(root)
                    .value("chatModeratorStrikeStatus")
                    .toObject();
            const auto warningValue = status.value("warningDetails");
            if (warningValue.isNull())
            {
                successCallback(std::nullopt);
                return;
            }
            const auto warningDetails = warningValue.toObject();
            if (warningDetails.value("id").toString().isEmpty())
            {
                failureCallback("Twitch did not return the warning status");
                return;
            }

            TwitchChannel::ChatWarning warning;
            warning.channelId = channelId;
            warning.id = warningDetails.value("id").toString();
            warning.reason = warningDetails.value("reason").toString();
            warning.createdAt =
                parseGqlDateTime(warningDetails.value("createdAt"));

            successCallback(std::move(warning));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::acknowledgeChatWarning(
    const QString &channelId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("channelID", channelId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "AcknowledgeChatWarning",
        "f97404a69caf9d152118bae17e962eca27c87c8a85224538173b3dfcd6c9df60",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto dataError = gqlMutationDataError(
                root, QStringLiteral("Twitch did not acknowledge the warning"));
            if (!dataError.isEmpty())
            {
                failureCallback("Twitch API Error: " + dataError);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getActivePoll(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::PollEvent>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("login", channelLogin);

    makePersistedGqlRequest(
        "ChannelPollContext_GetViewablePoll",
        "e83188a3836c636393df3191665e543a03733d7c51d3ade3d85e42aa46c2bf55",
        variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            auto context = data.value("channel").toObject();
            if (context.isEmpty())
            {
                context = data.value("user").toObject();
            }
            if (context.isEmpty() && !expectedChannelId.isEmpty())
            {
                failureCallback("Twitch did not return this channel");
                return;
            }
            const auto returnedChannelId = context.value("id").toString();
            if (!expectedChannelId.isEmpty() &&
                returnedChannelId != expectedChannelId)
            {
                failureCallback("Twitch returned a different channel");
                return;
            }

            const auto currentUserId = data.value("currentUser")
                                           .toObject()
                                           .value("id")
                                           .toString();
            successCallback(parsePollEventFromGql(
                context.value("viewablePoll").toObject(), currentUserId));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelEditorStatus(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(bool)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto QUERY = R"(
query AccessIsChannelEditorQuery($channelLogin: String!) {
  channel: user(login: $channelLogin) {
    id
    self {
      isEditor
    }
  }
}
)";

    QJsonObject variables;
    variables.insert("channelLogin", channelLogin.trimmed());

    makeInlineGqlRequest(QUERY, variables, oauthToken)
        .onSuccess([expectedChannelId, successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            const auto channelId = channel.value("id").toString();
            if (channelId.isEmpty())
            {
                failureCallback("Twitch did not return that channel");
                return;
            }
            if (channelId != expectedChannelId)
            {
                failureCallback(
                    "Twitch returned a different channel than the open "
                    "split");
                return;
            }

            successCallback(channel.value("self")
                                .toObject()
                                .value("isEditor")
                                .toBool(false));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getBroadcastSettings(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(GqlBroadcastSettings)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto QUERY = R"(
query MoltorinoChannelManagementBroadcastSettings($login: String!) {
  user(login: $login) {
    id
    broadcastSettings {
      id
      title
      language
      game {
        id
        name
        displayName
      }
    }
  }
}
)";

    QJsonObject variables;
    variables.insert("login", channelLogin.trimmed());

    makeInlineGqlRequest(QUERY, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto user =
                payloadDataObject(root).value("user").toObject();
            const auto settings =
                user.value("broadcastSettings").toObject();
            if (user.value("id").toString().isEmpty() || settings.isEmpty())
            {
                failureCallback(
                    "Twitch did not return the channel's stream information");
                return;
            }

            auto parsed =
                parseBroadcastSettings(user.value("id").toString(), settings);
            if (parsed.language.isEmpty())
            {
                failureCallback(
                    "Twitch did not return the channel's broadcast language");
                return;
            }

            successCallback(std::move(parsed));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getBroadcastManagementState(
    const QString &channelLogin, const QString &expectedChannelId,
    const QString &oauthToken,
    std::function<void(GqlBroadcastSettings)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedLogin = channelLogin.trimmed();
    const auto normalizedChannelId = expectedChannelId.trimmed();
    if (normalizedLogin.isEmpty() || normalizedChannelId.isEmpty())
    {
        failureCallback("The channel identity is incomplete");
        return;
    }

    static constexpr auto CONTEXT_QUERY = R"(
query MoltorinoBroadcastManagementContext(
  $login: String!
  $channelID: ID!
) {
  currentUser {
    id
  }
  channelRerunStatus(channelID: $channelID) {
    isRerun
  }
  user(login: $login) {
    id
    channel {
      id
      restriction {
        id
        type
        options
      }
      contentClassificationLabels(includesDisabled: true) {
        id
        localizedName
        description
        isEnabled
        isLocked
        lockedUntil
        isSelectable
      }
    }
    broadcastSettings {
      id
      title
      language
      contentClassificationLabelBroadcasterPolicyProperties {
        contentClassificationLabelsAllowed
      }
      game {
        id
        name
        displayName
      }
    }
  }
}
)";

    static constexpr auto TAGS_QUERY = R"(
query MoltorinoBroadcastManagementFreeformTags($login: String!) {
  user(login: $login) {
    id
    freeformTags {
      id
      name
    }
  }
}
)";

    auto state = std::make_shared<BroadcastManagementRequestState>();
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    QJsonObject contextVariables;
    contextVariables.insert("login", normalizedLogin);
    contextVariables.insert("channelID", normalizedChannelId);

    makeInlineGqlRequest(CONTEXT_QUERY, contextVariables, oauthToken)
        .onSuccess([state, normalizedChannelId](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failBroadcastManagementRequest(
                    state, "Failed to parse Twitch's channel response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failBroadcastManagementRequest(
                    state, "Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto user = data.value("user").toObject();
            const auto returnedUserId =
                user.value("id").toString().trimmed();
            if (returnedUserId.isEmpty())
            {
                failBroadcastManagementRequest(
                    state, "Twitch did not return that channel");
                return;
            }
            if (returnedUserId != normalizedChannelId)
            {
                failBroadcastManagementRequest(
                    state,
                    "Twitch returned a different channel than the open split");
                return;
            }

            const auto channel = user.value("channel").toObject();
            const auto returnedChannelId =
                channel.value("id").toString().trimmed();
            if (returnedChannelId != normalizedChannelId)
            {
                failBroadcastManagementRequest(
                    state,
                    "Twitch returned mismatched channel management data");
                return;
            }

            const auto broadcastSettings =
                user.value("broadcastSettings").toObject();
            if (broadcastSettings.isEmpty())
            {
                failBroadcastManagementRequest(
                    state,
                    "Twitch did not return the channel's stream information");
                return;
            }

            auto parsed =
                parseBroadcastSettings(returnedUserId, broadcastSettings);
            if (parsed.language.isEmpty())
            {
                failBroadcastManagementRequest(
                    state,
                    "Twitch did not return the channel's broadcast language");
                return;
            }

            const auto labels = parseContentClassificationLabels(
                channel.value("contentClassificationLabels"));
            if (!labels)
            {
                failBroadcastManagementRequest(
                    state,
                    "Twitch returned invalid content classification labels");
                return;
            }
            parsed.contentLabels = *labels;

            const auto restrictionValue = channel.value("restriction");
            if (restrictionValue.isNull())
            {
                parsed.audience = QStringLiteral("EVERYONE");
            }
            else if (restrictionValue.isObject())
            {
                const auto restriction = restrictionValue.toObject();
                parsed.audience =
                    restriction.value("type").toString().trimmed();
                if (parsed.audience.isEmpty())
                {
                    failBroadcastManagementRequest(
                        state, "Twitch returned an invalid audience setting");
                    return;
                }
                parsed.audienceOptions =
                    parseStringArray(restriction.value("options"));
            }
            else
            {
                failBroadcastManagementRequest(
                    state, "Twitch returned an invalid audience setting");
                return;
            }

            parsed.isRerun = data.value("channelRerunStatus")
                                 .toObject()
                                 .value("isRerun")
                                 .toBool(false);
            parsed.canEditAudience =
                data.value("currentUser")
                    .toObject()
                    .value("id")
                    .toString()
                    .trimmed() == normalizedChannelId;
            parsed.allowedContentLabelIds = parseStringArray(
                broadcastSettings
                    .value(
                        "contentClassificationLabelBroadcasterPolicyProperties")
                    .toObject()
                    .value("contentClassificationLabelsAllowed"));

            {
                const std::lock_guard guard(state->mutex);
                if (state->completed)
                {
                    return;
                }
                state->settings = std::move(parsed);
                state->contextReady = true;
            }
            finishBroadcastManagementRequestIfReady(state);
        })
        .onError([state](const NetworkResult &result) {
            failBroadcastManagementRequest(
                state, "Network Error: " + result.formatError());
        })
        .execute();

    QJsonObject tagsVariables;
    tagsVariables.insert("login", normalizedLogin);

    makeInlineGqlRequest(TAGS_QUERY, tagsVariables, oauthToken)
        .onSuccess([state, normalizedChannelId](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failBroadcastManagementRequest(
                    state, "Failed to parse Twitch's tags response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failBroadcastManagementRequest(
                    state, "Twitch API Error: " + gqlError);
                return;
            }

            const auto user =
                payloadDataObject(root).value("user").toObject();
            const auto returnedUserId =
                user.value("id").toString().trimmed();
            if (returnedUserId != normalizedChannelId)
            {
                failBroadcastManagementRequest(
                    state,
                    returnedUserId.isEmpty()
                        ? QStringLiteral(
                              "Twitch did not return the channel's tags")
                        : QStringLiteral(
                              "Twitch returned tags for a different channel"));
                return;
            }

            const auto tagsValue = user.value("freeformTags");
            if (!tagsValue.isArray())
            {
                failBroadcastManagementRequest(
                    state, "Twitch returned invalid channel tags");
                return;
            }

            QStringList tags;
            for (const auto &tagValue : tagsValue.toArray())
            {
                const auto name = tagValue.toObject()
                                      .value("name")
                                      .toString()
                                      .trimmed();
                if (!name.isEmpty())
                {
                    tags.push_back(name);
                }
            }

            {
                const std::lock_guard guard(state->mutex);
                if (state->completed)
                {
                    return;
                }

                state->tags = std::move(tags);
                state->tagsReady = true;
            }
            finishBroadcastManagementRequestIfReady(state);
        })
        .onError([state](const NetworkResult &result) {
            failBroadcastManagementRequest(
                state, "Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::updateBroadcastSettings(
    const GqlBroadcastSettings &settings, const QString &oauthToken,
    std::function<void(GqlBroadcastSettings)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (settings.userId.trimmed().isEmpty() ||
        settings.language.trimmed().isEmpty())
    {
        failureCallback("Stream information is incomplete");
        return;
    }

    static constexpr auto MUTATION = R"(
mutation EditBroadcastContext_BroadcastSettingsMutation(
  $input: UpdateBroadcastSettingsInput!
) {
  updateBroadcastSettings(input: $input) {
    broadcastSettings {
      id
      title
      language
      game {
        id
        name
        displayName
      }
    }
    error
  }
}
)";

    QJsonObject input;
    input.insert("broadcasterLanguage", settings.language);
    input.insert("game", settings.category.name);
    input.insert("categoryID", settings.category.id);
    input.insert("status", settings.title);
    input.insert("userID", settings.userId);

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([successCallback, failureCallback, settings](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto update = payloadDataObject(root)
                                    .value("updateBroadcastSettings")
                                    .toObject();
            if (update.isEmpty())
            {
                failureCallback(
                    "Twitch did not return an update result");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                update.value("error"), "Failed to update stream information");
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            const auto returnedSettings =
                update.value("broadcastSettings").toObject();
            if (returnedSettings.isEmpty())
            {
                failureCallback(
                    "Twitch accepted the request but returned no stream information");
                return;
            }

            auto parsed =
                parseBroadcastSettings(settings.userId, returnedSettings);
            if (parsed.language.isEmpty())
            {
                parsed.language = settings.language;
            }
            parsed.tags = settings.tags;
            parsed.isRerun = settings.isRerun;
            parsed.audience = settings.audience;
            parsed.canEditAudience = settings.canEditAudience;
            parsed.contentLabels = settings.contentLabels;
            parsed.audienceOptions = settings.audienceOptions;
            parsed.allowedContentLabelIds =
                settings.allowedContentLabelIds;
            successCallback(std::move(parsed));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::setFreeformTags(
    const QString &channelId, const QStringList &tags,
    const QString &oauthToken,
    std::function<void(QStringList)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedChannelId = channelId.trimmed();
    if (normalizedChannelId.isEmpty())
    {
        failureCallback("The channel ID is missing");
        return;
    }
    if (tags.size() > 10)
    {
        failureCallback("Twitch supports at most 10 channel tags");
        return;
    }

    QStringList normalizedTags;
    QSet<QString> seenTags;
    for (const auto &tag : tags)
    {
        const auto normalizedTag = tag.trimmed();
        if (normalizedTag.isEmpty())
        {
            failureCallback("Channel tags cannot be empty");
            return;
        }
        if (normalizedTag.size() > 25)
        {
            failureCallback(
                QStringLiteral("Channel tag '%1' is longer than 25 characters")
                    .arg(normalizedTag));
            return;
        }

        const auto comparisonKey = normalizedTag.toCaseFolded();
        if (seenTags.contains(comparisonKey))
        {
            failureCallback(
                QStringLiteral("Channel tag '%1' is duplicated")
                    .arg(normalizedTag));
            return;
        }
        seenTags.insert(comparisonKey);
        normalizedTags.push_back(normalizedTag);
    }

    static constexpr auto MUTATION = R"(
mutation MoltorinoSetFreeformTags($input: SetFreeformTagsInput!) {
  setFreeformTags(input: $input) {
    error {
      code
      message
      failedFreeformTagNames
    }
  }
}
)";

    QJsonArray tagNames;
    for (const auto &tag : normalizedTags)
    {
        tagNames.push_back(tag);
    }

    QJsonObject input;
    input.insert("contentID", normalizedChannelId);
    input.insert("contentType", QStringLiteral("CHANNEL"));
    input.insert("freeformTagNames", tagNames);

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([successCallback, failureCallback, normalizedTags](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch's tags response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("setFreeformTags").toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return a channel tags result");
                return;
            }

            auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to update channel tags"));
            if (!payloadError.isEmpty())
            {
                const auto failedTags = parseStringArray(
                    payload.value("error")
                        .toObject()
                        .value("failedFreeformTagNames"));
                if (!failedTags.isEmpty())
                {
                    payloadError +=
                        QStringLiteral(" (%1)").arg(failedTags.join(", "));
                }
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback(normalizedTags);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::setContentClassificationLabels(
    const QString &channelId,
    const QVector<GqlContentClassificationLabel> &labels,
    const QString &oauthToken,
    std::function<void(QVector<GqlContentClassificationLabel>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedChannelId = channelId.trimmed();
    if (normalizedChannelId.isEmpty())
    {
        failureCallback("The channel ID is missing");
        return;
    }
    if (labels.isEmpty())
    {
        failureCallback(
            "Content classification state is missing; refusing a partial update");
        return;
    }

    QJsonArray labelInputs;
    QSet<QString> seenLabelIds;
    for (const auto &label : labels)
    {
        const auto labelId = label.id.trimmed();
        if (labelId.isEmpty())
        {
            failureCallback("A content classification label has no ID");
            return;
        }
        if (seenLabelIds.contains(labelId))
        {
            failureCallback(
                QStringLiteral("Content classification label '%1' is duplicated")
                    .arg(labelId));
            return;
        }
        seenLabelIds.insert(labelId);

        QJsonObject labelInput;
        labelInput.insert("contentClassificationLabelID", labelId);
        labelInput.insert("isEnabled", label.isEnabled);
        labelInputs.push_back(labelInput);
    }

    static constexpr auto MUTATION = R"(
mutation MoltorinoSetContentClassificationLabels(
  $input: SetContentClassificationLabelsInput!
) {
  setContentClassificationLabels(input: $input) {
    contentClassificationLabels {
      id
      localizedName
      description
      isEnabled
      isLocked
      lockedUntil
      isSelectable
    }
    error {
      code
      message
    }
  }
}
)";

    QJsonObject input;
    input.insert("contentID", normalizedChannelId);
    input.insert("contentType", QStringLiteral("CONTENT_TYPE_CHANNEL"));
    input.insert("contentClassificationLabels", labelInputs);

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback(
                    "Failed to parse Twitch's content classification response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload = payloadDataObject(root)
                                     .value("setContentClassificationLabels")
                                     .toObject();
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch did not return a content classification result");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral(
                    "Failed to update content classification labels"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            auto returnedLabels = parseContentClassificationLabels(
                payload.value("contentClassificationLabels"));
            if (!returnedLabels)
            {
                failureCallback(
                    "Twitch accepted the request but returned invalid content classification labels");
                return;
            }
            successCallback(std::move(*returnedLabels));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::setChannelRerunStatus(
    const QString &channelId, bool shouldBeRerun,
    const QString &oauthToken, std::function<void(bool)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedChannelId = channelId.trimmed();
    if (normalizedChannelId.isEmpty())
    {
        failureCallback("The channel ID is missing");
        return;
    }

    static constexpr auto MUTATION = R"(
mutation MoltorinoSetChannelRerunStatus(
  $input: SetChannelRerunStatusInput!
) {
  setChannelRerunStatus(input: $input) {
    channel {
      id
    }
    channelRerunStatus {
      isRerun
    }
    error {
      code
    }
  }
}
)";

    QJsonObject input;
    input.insert("channelID", normalizedChannelId);
    input.insert("shouldBeRerun", shouldBeRerun);

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([normalizedChannelId, shouldBeRerun, successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse Twitch's rerun response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload = payloadDataObject(root)
                                     .value("setChannelRerunStatus")
                                     .toObject();
            if (payload.isEmpty())
            {
                failureCallback("Twitch did not return a rerun result");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to update rerun status"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            const auto returnedChannelId = payload.value("channel")
                                               .toObject()
                                               .value("id")
                                               .toString()
                                               .trimmed();
            if (returnedChannelId != normalizedChannelId)
            {
                failureCallback(
                    "Twitch returned a rerun result for a different channel");
                return;
            }

            const auto rerunStatus =
                payload.value("channelRerunStatus").toObject();
            if (!rerunStatus.value("isRerun").isBool())
            {
                failureCallback(
                    "Twitch accepted the request but returned no rerun status");
                return;
            }

            const auto appliedStatus =
                rerunStatus.value("isRerun").toBool(false);
            if (appliedStatus != shouldBeRerun)
            {
                failureCallback("Twitch did not apply the requested rerun status");
                return;
            }
            successCallback(appliedStatus);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::startAd(
    const QString &channelId, int lengthSeconds, GqlStartAdTrigger trigger,
    const QString &oauthToken,
    std::function<void(GqlStartAdResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr auto MUTATION = R"(
mutation StartAd($input: StartAdInput!) {
  startAd(input: $input) {
    adSession {
      id
      lengthSeconds
    }
    error {
      code
      retryAfterSeconds
    }
  }
}
)";

    QJsonObject input;
    input.insert("channelID", channelId.trimmed());
    input.insert("lengthSeconds", lengthSeconds);
    input.insert("trigger",
                 trigger == GqlStartAdTrigger::ChatCommand
                     ? QStringLiteral("CHAT_COMMAND")
                     : QStringLiteral("QUICK_ACTION"));
    input.insert("commercialID",
                 QUuid::createUuid().toString(QUuid::Id128).toLower());

    QJsonObject variables;
    variables.insert("input", input);

    makeInlineGqlRequest(MUTATION, variables, oauthToken)
        .onSuccess([successCallback, failureCallback, lengthSeconds](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto startAd =
                payloadDataObject(root).value("startAd").toObject();
            if (startAd.isEmpty())
            {
                failureCallback("Twitch did not return a commercial result");
                return;
            }

            GqlStartAdResult parsed;
            const auto session = startAd.value("adSession").toObject();
            parsed.adSessionId = session.value("id").toString();
            parsed.lengthSeconds =
                session.value("lengthSeconds").toInt(lengthSeconds);

            const auto error = startAd.value("error").toObject();
            parsed.errorCode = error.value("code").toString().trimmed();
            parsed.retryAfterSeconds =
                error.value("retryAfterSeconds").toInt(0);
            if (parsed.errorCode.isEmpty() &&
                (parsed.adSessionId.isEmpty() ||
                 parsed.lengthSeconds <= 0))
            {
                failureCallback(
                    "Twitch returned neither a commercial error nor a usable ad session");
                return;
            }
            successCallback(std::move(parsed));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::validateCustomAuthToken(
    const QString &oauthToken,
    std::function<void(CustomAuthValidationResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
    if (normalizedToken.isEmpty())
    {
        failureCallback("No token provided");
        return;
    }

    static const char *validationQuery = R"(
        query MoltorinoValidateCustomAuthToken {
            currentUser {
                id
                login
                displayName
            }
        }
    )";

    makeInlineGqlRequest(validationQuery, QJsonObject{}, normalizedToken)
        .onSuccess([successCallback, failureCallback, normalizedToken](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse Twitch response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(doc);
            if (!gqlError.isEmpty())
            {
                failureCallback(gqlError);
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") &&
                     doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (dataVal == nullptr || !dataVal->HasMember("currentUser") ||
                !(*dataVal)["currentUser"].IsObject())
            {
                failureCallback("Twitch did not return a user for this token");
                return;
            }

            const auto &currentUser = (*dataVal)["currentUser"];

            CustomAuthValidationResult validation;
            validation.normalizedToken = normalizedToken;

            if (!rj::getSafe(currentUser, "id", validation.userId) ||
                validation.userId.isEmpty())
            {
                failureCallback("Validated token did not include a user ID");
                return;
            }

            rj::getSafe(currentUser, "login", validation.login);
            rj::getSafe(currentUser, "displayName", validation.displayName);

            if (validation.displayName.isEmpty())
            {
                validation.displayName = validation.login;
            }
            if (validation.login.isEmpty())
            {
                validation.login = validation.displayName;
            }

            successCallback(std::move(validation));
        })
        .onError([failureCallback](const NetworkResult &result) {
            auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                failureCallback(QString("Network Error: %1 | %2")
                                    .arg(result.formatError(), body.left(200)));
                return;
            }

            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModeratedChannels(
    const QString &oauthToken,
    std::function<void(QVector<GqlModeratedChannel>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    struct FetchState {
        QString oauthToken;
        QVector<GqlModeratedChannel> channels;
        QSet<QString> seenChannels;
        bool completed = false;
        std::function<void(QVector<GqlModeratedChannel>)> successCallback;
        std::function<void(const QString &)> failureCallback;
    };

    static constexpr auto INLINE_QUERY = R"(
query ModeratedChannels {
  moderatedChannels(first: 50) {
    edges {
      node {
        id
        login
        displayName
      }
    }
    pageInfo {
      hasNextPage
    }
  }
}
)";
    static const QString PERSISTED_OPERATION =
        QStringLiteral("TopLevelModViewBar_ModeratedChannels");
    static const QString PERSISTED_HASH = QStringLiteral(
        "fd5a87dfe32f74dce2a6c2ddca74da7b65dcbf688a3420c4376c3c3319db454e");

    auto state = std::make_shared<FetchState>();
    state->oauthToken = oauthToken;
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    auto appendChannel = [](const std::shared_ptr<FetchState> &state,
                            const QJsonObject &node) {
        GqlModeratedChannel channel;
        channel.id = node.value("id").toString().trimmed();
        channel.login = node.value("login").toString().trimmed();
        channel.displayName = node.value("displayName").toString().trimmed();
        if (channel.id.isEmpty() && channel.login.isEmpty())
        {
            return;
        }

        const auto key = channel.id.isEmpty()
                             ? QStringLiteral("login:") +
                                   channel.login.toLower()
                             : QStringLiteral("id:") + channel.id;
        if (state->seenChannels.contains(key))
        {
            return;
        }

        state->seenChannels.insert(key);
        state->channels.push_back(std::move(channel));
    };

    auto appendConnection = [appendChannel](
                                const std::shared_ptr<FetchState> &state,
                                const QJsonObject &connection) {
        const auto edges = connection.value("edges").toArray();
        const auto nodes = connection.value("nodes").toArray();

        for (const auto &edgeValue : edges)
        {
            const auto edge = edgeValue.toObject();
            const auto node = edge.value("node").toObject();
            appendChannel(state, node.isEmpty() ? edge : node);
        }
        for (const auto &nodeValue : nodes)
        {
            appendChannel(state, nodeValue.toObject());
        }
    };

    auto finishSuccess = [](const std::shared_ptr<FetchState> &state) {
        if (state->completed || !state->successCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->successCallback);
        callback(std::move(state->channels));
    };

    auto finishFailure = [](const std::shared_ptr<FetchState> &state,
                            const QString &error) {
        if (state->completed || !state->failureCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->failureCallback);
        callback(error);
    };

    auto requestPersistedFallback =
        [state, appendConnection, finishSuccess,
         finishFailure](QString initialError) mutable {
            makePersistedGqlRequest(PERSISTED_OPERATION, PERSISTED_HASH,
                                    QJsonObject{}, state->oauthToken)
                .onSuccess([state, appendConnection, finishSuccess,
                            finishFailure, initialError](
                               const NetworkResult &result) mutable {
                    const auto value = result.parseJsonValue();
                    const auto gqlError = extractFirstGqlErrorMessage(value);
                    if (!gqlError.isEmpty())
                    {
                        finishFailure(
                            state,
                            initialError.isEmpty()
                                ? gqlError
                                : initialError + "; Mod View fallback: " +
                                      gqlError);
                        return;
                    }

                    const auto moderatedChannels =
                        findModeratedChannelsConnection(
                            payloadDataObject(value));
                    if (moderatedChannels.isEmpty())
                    {
                        finishFailure(
                            state,
                            initialError.isEmpty()
                                ? QString("Could not find moderated channels "
                                          "in Mod View response")
                                : initialError +
                                      "; Mod View fallback returned no "
                                      "moderated channels connection");
                        return;
                    }

                    appendConnection(state, moderatedChannels);
                    const bool stillHasNextPage =
                        moderatedChannels.value("pageInfo")
                            .toObject()
                            .value("hasNextPage")
                            .toBool(false);
                    if (stillHasNextPage)
                    {
                        finishFailure(
                            state,
                            initialError.isEmpty()
                                ? QString("Mod View returned an incomplete "
                                          "moderated channel list")
                                : initialError +
                                      "; Mod View fallback was still "
                                      "incomplete");
                        return;
                    }
                    finishSuccess(state);
                })
                .onError([state, finishFailure, initialError](
                             const NetworkResult &result) mutable {
                    auto error = "Network Error: " + result.formatError();
                    const auto body =
                        QString::fromUtf8(result.getData()).trimmed();
                    if (!body.isEmpty())
                    {
                        error += " | " + body.left(200);
                    }
                    if (!initialError.isEmpty())
                    {
                        error = initialError + "; Mod View fallback: " + error;
                    }
                    finishFailure(state, error);
                })
                .execute();
    };

    makeTvInlineGqlRequest(INLINE_QUERY, QJsonObject{}, state->oauthToken)
        .onSuccess([state, appendConnection, finishSuccess,
                    requestPersistedFallback](
                       const NetworkResult &result) mutable {
            const auto value = result.parseJsonValue();
            const auto gqlError = extractFirstGqlErrorMessage(value);
            if (!gqlError.isEmpty())
            {
                requestPersistedFallback(gqlError);
                return;
            }

            const auto moderatedChannels = findModeratedChannelsConnection(
                payloadDataObject(value));
            if (moderatedChannels.isEmpty())
            {
                requestPersistedFallback(
                    "Could not find moderated channels in inline response");
                return;
            }

            appendConnection(state, moderatedChannels);
            const bool hasNextPage = moderatedChannels.value("pageInfo")
                                         .toObject()
                                         .value("hasNextPage")
                                         .toBool(false);
            if (hasNextPage)
            {
                requestPersistedFallback(
                    "Twitch's moderated channel cursor is not usable");
                return;
            }

            finishSuccess(state);
        })
        .onError([requestPersistedFallback](const NetworkResult &result) mutable {
            auto error = "Network Error: " + result.formatError();
            const auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                error += " | " + body.left(200);
            }
            requestPersistedFallback(std::move(error));
        })
        .execute();
}

}  // namespace chatterino
