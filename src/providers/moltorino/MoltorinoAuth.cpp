#include "providers/moltorino/MoltorinoAuth.hpp"

#include "providers/moltorino/MoltorinoAuthPagination.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "singletons/Settings.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace chatterino::MoltorinoAuth {
namespace {

constexpr int HELIX_MODERATED_CHANNEL_TIMEOUT_MS = 20 * 1000;
constexpr int HELIX_MODERATED_CHANNEL_PAGE_SIZE = 100;
constexpr int HELIX_MODERATED_CHANNEL_FALLBACK_PAGE_SIZE = 1;
constexpr int MAX_HELIX_MODERATED_CHANNEL_REQUESTS = 5000;
constexpr auto TWITCH_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";

QString normalizeToken(QString token)
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

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString lower(QString text)
{
    return text.trimmed().toLower();
}

struct RefreshCoordinator {
    bool running = false;
    MoltorinoAuthRefreshMode mode = MoltorinoAuthRefreshMode::Automatic;
    std::vector<std::function<void(MoltorinoAuthRefreshResult)>> callbacks;
    std::vector<std::function<void(MoltorinoAuthRefreshResult)>>
        queuedManualCallbacks;
};

RefreshCoordinator &refreshCoordinator()
{
    static RefreshCoordinator coordinator;
    return coordinator;
}

MoltorinoAuthChannel channelFromJson(const QJsonObject &obj)
{
    return {
        .id = obj.value("id").toString().trimmed(),
        .login = obj.value("login").toString().trimmed().toLower(),
        .displayName = obj.value("displayName").toString().trimmed(),
    };
}

QJsonObject channelToJson(const MoltorinoAuthChannel &channel)
{
    QJsonObject obj;
    obj.insert("id", channel.id);
    obj.insert("login", channel.login);
    obj.insert("displayName", channel.displayName);
    return obj;
}

MoltorinoAuthAccount accountFromJson(const QJsonObject &obj)
{
    MoltorinoAuthAccount account;
    account.userId = obj.value("userId").toString().trimmed();
    account.login = obj.value("login").toString().trimmed().toLower();
    account.displayName = obj.value("displayName").toString().trimmed();
    account.token = obj.value("token").toString().trimmed();
    account.clientId = obj.value("clientId").toString().trimmed();
    account.enabled =
        !obj.contains("enabled") || obj.value("enabled").toBool(true);
    account.valid = obj.value("valid").toBool(false);
    account.lastError = obj.value("lastError").toString();
    account.lastValidatedAt = obj.value("lastValidatedAt").toString();
    account.moderatedChannelsManualRefreshOnly =
        obj.value("moderatedChannelsManualRefreshOnly").toBool(false);

    const auto channels = obj.value("moderatedChannels").toArray();
    account.moderatedChannels.reserve(channels.size());
    for (const auto &channelValue : channels)
    {
        if (channelValue.isObject())
        {
            account.moderatedChannels.push_back(
                channelFromJson(channelValue.toObject()));
        }
    }

    const auto editorChannels = obj.value("verifiedEditorChannels").toArray();
    account.verifiedEditorChannels.reserve(editorChannels.size());
    for (const auto &channelValue : editorChannels)
    {
        if (channelValue.isObject())
        {
            account.verifiedEditorChannels.push_back(
                channelFromJson(channelValue.toObject()));
        }
    }
    return account;
}

QJsonObject accountToJson(const MoltorinoAuthAccount &account)
{
    QJsonObject obj;
    obj.insert("userId", account.userId);
    obj.insert("login", account.login);
    obj.insert("displayName", account.displayName);
    obj.insert("token", account.token);
    obj.insert("clientId", account.clientId);
    obj.insert("enabled", account.enabled);
    obj.insert("valid", account.valid);
    obj.insert("lastError", account.lastError);
    obj.insert("lastValidatedAt", account.lastValidatedAt);
    obj.insert("moderatedChannelsManualRefreshOnly",
               account.moderatedChannelsManualRefreshOnly);

    QJsonArray channels;
    for (const auto &channel : account.moderatedChannels)
    {
        channels.append(channelToJson(channel));
    }
    obj.insert("moderatedChannels", channels);

    QJsonArray editorChannels;
    for (const auto &channel : account.verifiedEditorChannels)
    {
        editorChannels.append(channelToJson(channel));
    }
    obj.insert("verifiedEditorChannels", editorChannels);
    return obj;
}

void sortAccounts(std::vector<MoltorinoAuthAccount> &accounts)
{
    std::sort(accounts.begin(), accounts.end(), [](const auto &a, const auto &b) {
        return lower(a.login).localeAwareCompare(lower(b.login)) < 0;
    });
}

void saveAccounts(const std::vector<MoltorinoAuthAccount> &accounts)
{
    QJsonArray array;
    for (const auto &account : accounts)
    {
        array.append(accountToJson(account));
    }

    auto json =
        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    getSettings()->moltorinoAuthAccounts = json;
    getSettings()->requestSave();
}

bool sameAccount(const MoltorinoAuthAccount &account, const QString &userId,
                 const QString &token)
{
    if (!userId.isEmpty() && account.userId == userId)
    {
        return true;
    }
    return !token.isEmpty() && normalizeToken(account.token) == token;
}

void upsertAccount(MoltorinoAuthAccount account)
{
    account.token = normalizeToken(account.token);
    account.login = account.login.trimmed().toLower();

    auto current = accounts();
    const auto token = account.token;
    const auto existing =
        std::find_if(current.begin(), current.end(), [&](const auto &saved) {
            return sameAccount(saved, account.userId, token);
        });
    if (existing != current.end())
    {
        account.enabled = existing->enabled;
        account.verifiedEditorChannels = existing->verifiedEditorChannels;
        if (account.clientId.isEmpty())
        {
            account.clientId = existing->clientId;
        }
        if ((account.displayName.isEmpty() ||
             account.displayName.compare(account.login,
                                         Qt::CaseInsensitive) == 0) &&
            !existing->displayName.isEmpty())
        {
            account.displayName = existing->displayName;
        }
    }
    current.erase(std::remove_if(current.begin(), current.end(),
                                 [&](const auto &existing) {
                                     return sameAccount(existing,
                                                        account.userId, token);
                                 }),
                  current.end());
    current.push_back(std::move(account));
    sortAccounts(current);
    saveAccounts(current);
}

bool accountMatchesChannel(const MoltorinoAuthAccount &account,
                           const QString &channelId,
                           const QString &channelLogin)
{
    const auto normalizedLogin = lower(channelLogin);
    if (!channelId.isEmpty() && account.userId == channelId)
    {
        return true;
    }
    if (!normalizedLogin.isEmpty() && lower(account.login) == normalizedLogin)
    {
        return true;
    }

    for (const auto &channel : account.moderatedChannels)
    {
        if (!channelId.isEmpty() && channel.id == channelId)
        {
            return true;
        }
        if (!normalizedLogin.isEmpty() && lower(channel.login) == normalizedLogin)
        {
            return true;
        }
    }
    return false;
}

bool accountMatchesBroadcaster(const MoltorinoAuthAccount &account,
                               const QString &channelId,
                               const QString &channelLogin)
{
    const auto normalizedLogin = lower(channelLogin);
    if (!channelId.isEmpty() && account.userId == channelId)
    {
        return true;
    }
    return !normalizedLogin.isEmpty() && lower(account.login) == normalizedLogin;
}

bool accountMatchesCurrentUser(const MoltorinoAuthAccount &account)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (!current || current->isAnon())
    {
        return false;
    }

    const auto currentUserId = current->getUserId();
    if (!currentUserId.isEmpty() && account.userId == currentUserId)
    {
        return true;
    }

    const auto currentLogin = lower(current->getUserName());
    return !currentLogin.isEmpty() && lower(account.login) == currentLogin;
}

std::vector<MoltorinoAuthAccount> validAccounts()
{
    auto loaded = accounts();
    loaded.erase(std::remove_if(loaded.begin(), loaded.end(),
                                [](const auto &account) {
                                    return !account.enabled || !account.valid ||
                                           account.token.trimmed().isEmpty();
                                }),
                 loaded.end());
    return loaded;
}

MoltorinoAuthToken makeToken(const MoltorinoAuthAccount &account)
{
    return {
        .token = account.token,
        .userId = account.userId,
        .login = account.login,
        .clientId = account.clientId,
        .legacy = false,
    };
}

MoltorinoAuthToken makeLegacyToken()
{
    auto token = MoltorinoAuthToken{
        .token = legacyToken(),
        .legacy = true,
    };

    const auto normalizedToken = normalizeToken(token.token);
    if (normalizedToken.isEmpty())
    {
        return token;
    }

    for (const auto &account : accounts())
    {
        if (normalizeToken(account.token) == normalizedToken)
        {
            token.userId = account.userId;
            token.login = account.login;
            token.clientId = account.clientId;
            break;
        }
    }

    return token;
}

QString &lastResolvedPersonalToken()
{
    static QString token;
    return token;
}

MoltorinoAuthToken rememberPersonalToken(MoltorinoAuthToken token)
{
    const auto normalizedToken = normalizeToken(token.token);
    if (!normalizedToken.isEmpty())
    {
        lastResolvedPersonalToken() = normalizedToken;
    }
    return token;
}

std::optional<MoltorinoAuthAccount> validAccountForToken(
    const std::vector<MoltorinoAuthAccount> &valid,
    const QString &token)
{
    const auto normalizedToken = normalizeToken(token);
    if (normalizedToken.isEmpty())
    {
        return std::nullopt;
    }

    auto found = std::find_if(valid.begin(), valid.end(), [&](const auto &account) {
        return normalizeToken(account.token) == normalizedToken;
    });
    if (found == valid.end())
    {
        return std::nullopt;
    }
    return *found;
}

std::optional<MoltorinoAuthAccount> mostRecentlyValidatedAccount(
    const std::vector<MoltorinoAuthAccount> &valid)
{
    if (valid.empty())
    {
        return std::nullopt;
    }

    const MoltorinoAuthAccount *best = &valid.front();
    auto bestTime = QDateTime::fromString(best->lastValidatedAt, Qt::ISODate);
    for (const auto &account : valid)
    {
        const auto accountTime =
            QDateTime::fromString(account.lastValidatedAt, Qt::ISODate);
        if (!bestTime.isValid() ||
            (accountTime.isValid() && accountTime > bestTime))
        {
            best = &account;
            bestTime = accountTime;
        }
    }

    return *best;
}

std::optional<MoltorinoAuthAccount> broadcasterAccountForChannel(
    const std::vector<MoltorinoAuthAccount> &valid, const QString &channelId,
    const QString &channelLogin)
{
    for (const auto &account : valid)
    {
        if (accountMatchesCurrentUser(account) &&
            accountMatchesBroadcaster(account, channelId, channelLogin))
        {
            return account;
        }
    }

    for (const auto &account : valid)
    {
        if (accountMatchesBroadcaster(account, channelId, channelLogin))
        {
            return account;
        }
    }

    return std::nullopt;
}

bool looksLikeAuthError(const QString &error)
{
    const auto lowered = error.toLower();
    return lowered.contains("unauthenticated") ||
           lowered.contains("unauthorized") ||
           lowered.contains("authentication required") ||
           lowered.contains("authentication credentials") ||
           lowered.contains("invalid oauth") ||
           lowered.contains("invalid access token") ||
           lowered.contains("invalid token") ||
           lowered.contains("token is invalid") ||
           lowered.contains("token is not valid") ||
           lowered.contains("token has expired") ||
           lowered.contains("token expired") ||
           lowered.contains("expired oauth token") ||
           lowered.contains("expired access token") ||
           lowered.contains("missing oauth token") ||
           lowered.contains("oauth token is missing") ||
           lowered.contains("missing access token") ||
           lowered.contains("no token provided") ||
           lowered.contains("rejected the token") || lowered.contains("401");
}

std::shared_ptr<TwitchAccount> localAccountForAuthAccount(
    const MoltorinoAuthAccount &account)
{
    const auto normalizedLogin = lower(account.login);
    auto localAccounts = getApp()->getAccounts()->twitch.accounts.readOnly();
    for (const auto &localAccount : *localAccounts)
    {
        if (!localAccount || localAccount->isAnon() ||
            localAccount->getOAuthClient().trimmed().isEmpty() ||
            localAccount->getOAuthToken().trimmed().isEmpty())
        {
            continue;
        }

        const auto localUserId = localAccount->getUserId().trimmed();
        if (!localUserId.isEmpty() && localUserId == account.userId)
        {
            return localAccount;
        }

        const auto localLogin = lower(localAccount->getUserName());
        if (!normalizedLogin.isEmpty() && localLogin == normalizedLogin)
        {
            return localAccount;
        }
    }

    return nullptr;
}

QString moderatedChannelKey(const QString &id, const QString &login)
{
    if (!id.trimmed().isEmpty())
    {
        return QStringLiteral("id:") + id.trimmed();
    }
    if (!login.trimmed().isEmpty())
    {
        return QStringLiteral("login:") + lower(login);
    }
    return {};
}

void rememberManualModeratedChannelRefresh(const MoltorinoAuthAccount &account)
{
    auto current = accounts();
    const auto saved =
        std::find_if(current.begin(), current.end(), [&](const auto &existing) {
            return sameAccount(existing, account.userId,
                               normalizeToken(account.token));
        });
    if (saved == current.end() ||
        saved->moderatedChannelsManualRefreshOnly)
    {
        return;
    }

    saved->moderatedChannelsManualRefreshOnly = true;
    saveAccounts(current);
}

void fetchModeratedChannelsWithHelix(
    const MoltorinoAuthAccount &account, QString clientId, QString oauthToken,
    std::function<void(QVector<MoltorinoAuthChannel>, bool)> successCallback,
    std::function<void(const QString &, bool)> failureCallback)
{
    clientId = clientId.trimmed();
    oauthToken = normalizeToken(oauthToken);
    if (account.userId.isEmpty() || clientId.isEmpty() || oauthToken.isEmpty())
    {
        failureCallback("Missing account details for Helix mod access", false);
        return;
    }

    enum class FetchMode {
        Normal,
        FastSeed,
        FastPage,
        FastVerification,
        OneAtATimeFallback,
    };

    struct FetchState {
        MoltorinoAuthAccount account;
        QString clientId;
        QString oauthToken;
        QVector<MoltorinoAuthChannel> channels;
        QSet<QString> seenChannels;
        QSet<QString> seenCursors;
        QSet<QString> seenFastBoundaries;
        int requestCount = 0;
        FetchMode mode = FetchMode::Normal;
        QString firstChannelId;
        QString fastTargetBoundaryId;
        QString fastBoundaryId;
        int oneAtATimeRequestCount = 0;
        bool usedOneAtATimeFallback = false;
        bool completed = false;
        std::shared_ptr<std::function<void(QString)>> requestPage;
        std::function<void(QVector<MoltorinoAuthChannel>, bool)>
            successCallback;
        std::function<void(const QString &, bool)> failureCallback;
    };

    auto state = std::make_shared<FetchState>();
    state->account = account;
    state->clientId = std::move(clientId);
    state->oauthToken = std::move(oauthToken);
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    auto finishSuccess = [](const std::shared_ptr<FetchState> &state) {
        if (state->completed || !state->successCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->successCallback);
        callback(std::move(state->channels),
                 state->usedOneAtATimeFallback);
    };

    auto finishFailure = [](const std::shared_ptr<FetchState> &state,
                            const QString &error) {
        if (state->completed || !state->failureCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->failureCallback);
        callback(error, state->usedOneAtATimeFallback);
    };

    auto requestPage = std::make_shared<std::function<void(QString)>>();
    state->requestPage = requestPage;
    std::weak_ptr<FetchState> weakState = state;
    std::weak_ptr<std::function<void(QString)>> weakRequestPage = requestPage;
    *requestPage = [weakState, weakRequestPage, finishSuccess,
                    finishFailure](QString cursor) mutable {
        auto state = weakState.lock();
        if (!state)
        {
            return;
        }
        if (state->completed)
        {
            return;
        }

        if (++state->requestCount > MAX_HELIX_MODERATED_CHANNEL_REQUESTS)
        {
            finishFailure(state,
                          "Helix moderated channel list has too many requests");
            return;
        }

        const auto requestMode = state->mode;
        const auto pageSize =
            requestMode == FetchMode::Normal ||
                    requestMode == FetchMode::FastPage
                ? HELIX_MODERATED_CHANNEL_PAGE_SIZE
                : HELIX_MODERATED_CHANNEL_FALLBACK_PAGE_SIZE;

        if (requestMode == FetchMode::OneAtATimeFallback &&
            ++state->oneAtATimeRequestCount > 1 &&
            !state->usedOneAtATimeFallback)
        {
            state->usedOneAtATimeFallback = true;
            rememberManualModeratedChannelRefresh(state->account);
        }

        auto startOneAtATimeFallback =
            [state, weakRequestPage, finishFailure](const QString &reason) {
                if (state->completed)
                {
                    return;
                }
                if (state->mode == FetchMode::OneAtATimeFallback)
                {
                    finishFailure(state, reason);
                    return;
                }

                state->mode = FetchMode::OneAtATimeFallback;
                state->oneAtATimeRequestCount = 0;
                state->channels.clear();
                state->seenChannels.clear();
                state->seenCursors.clear();
                state->seenFastBoundaries.clear();
                state->firstChannelId.clear();
                state->fastTargetBoundaryId.clear();
                state->fastBoundaryId.clear();

                if (auto requestPage = weakRequestPage.lock())
                {
                    (*requestPage)({});
                    return;
                }
                finishFailure(state,
                              "Could not start Twitch's one at a time "
                              "moderated channel fallback");
            };

        QUrl url("https://api.twitch.tv/helix/moderation/channels");
        QUrlQuery query;
        query.addQueryItem("user_id", state->account.userId);
        query.addQueryItem("first", QString::number(pageSize));
        if (!cursor.isEmpty())
        {
            query.addQueryItem("after", cursor);
        }
        url.setQuery(query);

        NetworkRequest(url, NetworkRequestType::Get)
            .timeout(HELIX_MODERATED_CHANNEL_TIMEOUT_MS)
            .maximumResponseSize(1024 * 1024)
            .hideRequestBody()
            .followRedirects(true)
            .header("Accept", "application/json")
            .header("Client-ID", state->clientId)
            .header("Authorization", "Bearer " + state->oauthToken)
            .onSuccess([state, weakRequestPage, finishSuccess, finishFailure,
                        startOneAtATimeFallback, requestMode, pageSize,
                        cursor](const NetworkResult &result) mutable {
                auto rejectResponse =
                    [state, finishFailure, startOneAtATimeFallback,
                     requestMode](const QString &error) {
                        if (requestMode == FetchMode::FastSeed ||
                            requestMode == FetchMode::FastPage ||
                            requestMode == FetchMode::FastVerification)
                        {
                            startOneAtATimeFallback(error);
                            return;
                        }
                        finishFailure(state, error);
                    };

                const auto root = result.parseJson();
                if (!root.contains("data") || !root.value("data").isArray())
                {
                    const auto message = root.value("message").toString();
                    rejectResponse(
                        message.isEmpty()
                            ? QString("Failed to parse Helix moderated "
                                      "channels response")
                            : message);
                    return;
                }

                const auto data = root.value("data").toArray();
                if (!root.value("message").toString().isEmpty() &&
                    data.isEmpty())
                {
                    rejectResponse(root.value("message").toString());
                    return;
                }

                if (data.size() > pageSize)
                {
                    rejectResponse(
                        "Twitch returned more moderated channels than requested");
                    return;
                }

                QVector<MoltorinoAuthChannel> pageChannels;
                pageChannels.reserve(data.size());
                QSet<QString> pageKeys;
                bool pageCanAdvance = true;
                for (const auto &value : data)
                {
                    if (!value.isObject())
                    {
                        pageCanAdvance = false;
                        continue;
                    }
                    const auto obj = value.toObject();
                    MoltorinoAuthChannel channel{
                        .id = obj.value("broadcaster_id")
                                  .toString()
                                  .trimmed(),
                        .login = obj.value("broadcaster_login")
                                     .toString()
                                     .trimmed()
                                     .toLower(),
                        .displayName = obj.value("broadcaster_name")
                                           .toString()
                                           .trimmed(),
                    };
                    if (channel.id.isEmpty() && channel.login.isEmpty())
                    {
                        pageCanAdvance = false;
                        continue;
                    }

                    const auto key =
                        moderatedChannelKey(channel.id, channel.login);
                    if (channel.id.isEmpty() || key.isEmpty() ||
                        pageKeys.contains(key))
                    {
                        pageCanAdvance = false;
                        continue;
                    }

                    pageKeys.insert(key);
                    pageChannels.push_back(std::move(channel));
                }

                if (pageChannels.size() != data.size())
                {
                    pageCanAdvance = false;
                }

                const auto nextCursor = root.value("pagination")
                                            .toObject()
                                            .value("cursor")
                                            .toString()
                                            .trimmed();

                auto queuePage =
                    [state, weakRequestPage, finishFailure,
                     startOneAtATimeFallback, rejectResponse](
                        const QString &next, const QString &boundaryId,
                        FetchMode nextMode) {
                        if (next.isEmpty())
                        {
                            rejectResponse(
                                "Twitch's moderated channel cursor was empty");
                            return false;
                        }

                        if (state->seenCursors.contains(next))
                        {
                            rejectResponse(
                                "Twitch repeated a moderated channel cursor");
                            return false;
                        }

                        if (!boundaryId.isEmpty())
                        {
                            if (state->seenFastBoundaries.contains(boundaryId))
                            {
                                startOneAtATimeFallback(
                                    "Twitch repeated a moderated channel boundary");
                                return false;
                            }
                            state->seenFastBoundaries.insert(boundaryId);
                            state->fastBoundaryId = boundaryId;
                        }

                        state->seenCursors.insert(next);
                        state->mode = nextMode;
                        if (auto requestPage = weakRequestPage.lock())
                        {
                            (*requestPage)(next);
                            return true;
                        }

                        finishFailure(state,
                                      "Could not continue Twitch moderated "
                                      "channel pagination");
                        return false;
                    };

                if (requestMode == FetchMode::FastSeed)
                {
                    if (!pageCanAdvance || pageChannels.size() != 1 ||
                        pageChannels.front().id != state->firstChannelId ||
                        nextCursor.isEmpty())
                    {
                        startOneAtATimeFallback(
                            "Twitch's moderated channel seed did not match "
                            "the first page");
                        return;
                    }

                    const auto advanced =
                        detail::advanceModeratedChannelsCursor(
                            nextCursor, state->firstChannelId,
                            state->fastTargetBoundaryId);
                    if (!advanced)
                    {
                        startOneAtATimeFallback(
                            "Twitch's moderated channel cursor format changed");
                        return;
                    }

                    queuePage(*advanced, state->fastTargetBoundaryId,
                              FetchMode::FastPage);
                    return;
                }

                bool pageOverlapsExisting = false;
                for (const auto &channel : pageChannels)
                {
                    const auto key =
                        moderatedChannelKey(channel.id, channel.login);
                    if (state->seenChannels.contains(key))
                    {
                        pageOverlapsExisting = true;
                        continue;
                    }
                    state->seenChannels.insert(key);
                    state->channels.push_back(channel);
                }

                if (requestMode == FetchMode::Normal)
                {
                    if (!pageCanAdvance)
                    {
                        finishFailure(
                            state,
                            "Twitch returned an invalid moderated channel page");
                        return;
                    }
                    if (pageOverlapsExisting)
                    {
                        finishFailure(
                            state,
                            "Twitch repeated a moderated channel while "
                            "paginating");
                        return;
                    }

                    if (state->firstChannelId.isEmpty() && cursor.isEmpty() &&
                        !pageChannels.isEmpty())
                    {
                        state->firstChannelId = pageChannels.front().id;
                    }

                    if (!nextCursor.isEmpty())
                    {
                        queuePage(nextCursor, {}, FetchMode::Normal);
                        return;
                    }

                    if (data.size() < pageSize)
                    {
                        finishSuccess(state);
                        return;
                    }

                    if (state->firstChannelId.isEmpty() ||
                        pageChannels.isEmpty())
                    {
                        startOneAtATimeFallback(
                            "Twitch's full moderated channel page could not "
                            "be safely advanced");
                        return;
                    }

                    state->fastTargetBoundaryId = pageChannels.back().id;
                    state->mode = FetchMode::FastSeed;
                    if (auto requestPage = weakRequestPage.lock())
                    {
                        (*requestPage)({});
                        return;
                    }
                    finishFailure(state,
                                  "Could not request a Twitch moderated "
                                  "channel cursor seed");
                    return;
                }

                if (requestMode == FetchMode::OneAtATimeFallback)
                {
                    if (!pageCanAdvance || pageOverlapsExisting)
                    {
                        finishFailure(
                            state,
                            "Twitch's one at a time moderated channel "
                            "pagination did not make progress");
                        return;
                    }
                    if (nextCursor.isEmpty())
                    {
                        finishSuccess(state);
                        return;
                    }
                    if (pageChannels.isEmpty())
                    {
                        finishFailure(
                            state,
                            "Twitch returned an empty moderated channel page "
                            "with another cursor");
                        return;
                    }
                    queuePage(nextCursor, {},
                              FetchMode::OneAtATimeFallback);
                    return;
                }

                if (!pageCanAdvance || pageOverlapsExisting)
                {
                    startOneAtATimeFallback(
                        "Twitch's fast moderated channel pagination did not "
                        "make progress");
                    return;
                }

                if (requestMode == FetchMode::FastVerification)
                {
                    if (pageChannels.isEmpty())
                    {
                        if (nextCursor.isEmpty())
                        {
                            finishSuccess(state);
                        }
                        else
                        {
                            startOneAtATimeFallback(
                                "Twitch returned an empty verification page "
                                "with another cursor");
                        }
                        return;
                    }

                    if (nextCursor.isEmpty())
                    {

                        finishSuccess(state);
                        return;
                    }

                    queuePage(nextCursor, pageChannels.back().id,
                              FetchMode::FastPage);
                    return;
                }

                if (pageChannels.isEmpty())
                {
                    if (nextCursor.isEmpty())
                    {
                        finishSuccess(state);
                    }
                    else
                    {
                        startOneAtATimeFallback(
                            "Twitch returned an empty fast page with another "
                            "cursor");
                    }
                    return;
                }

                if (!nextCursor.isEmpty())
                {
                    queuePage(nextCursor, pageChannels.back().id,
                              FetchMode::FastPage);
                    return;
                }

                const auto advanced =
                    detail::advanceModeratedChannelsCursor(
                        cursor, state->fastBoundaryId,
                        pageChannels.back().id);
                if (!advanced)
                {
                    startOneAtATimeFallback(
                        "Twitch's moderated channel cursor format changed");
                    return;
                }

                queuePage(*advanced, pageChannels.back().id,
                          data.size() == pageSize
                              ? FetchMode::FastPage
                              : FetchMode::FastVerification);
            })
            .onError([state, finishFailure, startOneAtATimeFallback,
                      requestMode](const NetworkResult &result) mutable {
                const auto body = QString::fromUtf8(result.getData()).trimmed();
                auto error = result.formatError();
                if (!body.isEmpty())
                {
                    error = QString("%1 | %2").arg(error, body.left(200));
                }

                if (requestMode == FetchMode::FastSeed ||
                    requestMode == FetchMode::FastPage ||
                    requestMode == FetchMode::FastVerification)
                {
                    startOneAtATimeFallback(
                        QString("Fast moderated channel pagination failed: %1")
                            .arg(error));
                }
                else
                {
                    finishFailure(state, error);
                }
            })
            .execute();
    };

    (*requestPage)({});
}

void fetchModeratedChannels(
    MoltorinoAuthAccount account, bool refreshManualOnlyAccount,
    std::function<void(MoltorinoAuthAccount)> callback)
{
    struct CallbackState {
        std::function<void(MoltorinoAuthAccount)> callback;
        bool completed = false;
    };

    auto state = std::make_shared<CallbackState>();
    state->callback = std::move(callback);

    if (account.moderatedChannelsManualRefreshOnly &&
        !refreshManualOnlyAccount)
    {
        account.valid = true;
        account.lastValidatedAt = nowIso();
        auto finishCallback = std::move(state->callback);
        finishCallback(std::move(account));
        return;
    }

    auto finish = [state](MoltorinoAuthAccount refreshedAccount) mutable {
        if (state->completed || !state->callback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->callback);
        callback(std::move(refreshedAccount));
    };

    auto finishWithChannels =
        [finish](MoltorinoAuthAccount baseAccount,
                 QVector<MoltorinoAuthChannel> channels,
                 bool manualRefreshOnly) mutable {
            baseAccount.moderatedChannels = std::move(channels);
            baseAccount.moderatedChannelsManualRefreshOnly =
                manualRefreshOnly;
            baseAccount.valid = true;
            baseAccount.lastError.clear();
            baseAccount.lastValidatedAt = nowIso();
            finish(std::move(baseAccount));
        };

    auto finishWithCachedChannels =
        [finish](MoltorinoAuthAccount baseAccount,
                 const QString &error,
                 bool usedOneAtATimeFallback) mutable {
            auto account = baseAccount;
            for (const auto &existing : accounts())
            {
                if (sameAccount(existing, account.userId,
                                normalizeToken(account.token)))
                {
                    account.moderatedChannels = existing.moderatedChannels;
                    account.verifiedEditorChannels =
                        existing.verifiedEditorChannels;
                    account.moderatedChannelsManualRefreshOnly =
                        existing.moderatedChannelsManualRefreshOnly ||
                        usedOneAtATimeFallback;
                    break;
                }
            }
            account.valid = true;
            account.lastError =
                QString("Could not refresh mod access: %1").arg(error);
            account.lastValidatedAt = nowIso();
            finish(std::move(account));
        };

    auto fetchWithGql =
        [finishWithChannels, finishWithCachedChannels](
            MoltorinoAuthAccount baseAccount,
            bool usedOneAtATimeFallback) mutable {
            TwitchGql::getModeratedChannels(
                baseAccount.token,
                [baseAccount, finishWithChannels, usedOneAtATimeFallback](
                    QVector<GqlModeratedChannel> channels) mutable {
                    QVector<MoltorinoAuthChannel> converted;
                    converted.reserve(channels.size());
                    for (const auto &channel : channels)
                    {
                        converted.push_back({
                            .id = channel.id,
                            .login = channel.login.trimmed().toLower(),
                            .displayName = channel.displayName,
                        });
                    }
                    finishWithChannels(
                        baseAccount, std::move(converted),
                        baseAccount.moderatedChannelsManualRefreshOnly ||
                            usedOneAtATimeFallback);
                },
                [baseAccount, finishWithCachedChannels,
                 usedOneAtATimeFallback](const QString &error) mutable {
                    finishWithCachedChannels(baseAccount, error,
                                             usedOneAtATimeFallback);
                });
        };

    auto baseAccount = std::move(account);
    auto localAccount = localAccountForAuthAccount(baseAccount);
    if (!localAccount)
    {
        fetchModeratedChannelsWithHelix(
            baseAccount,
            baseAccount.clientId.isEmpty()
                ? QString::fromUtf8(TWITCH_TV_CLIENT_ID)
                : baseAccount.clientId,
            baseAccount.token,
            [baseAccount, finishWithChannels](
                QVector<MoltorinoAuthChannel> channels,
                bool usedOneAtATimeFallback) mutable {
                finishWithChannels(baseAccount, std::move(channels),
                                   usedOneAtATimeFallback);
            },
            [baseAccount, fetchWithGql](const QString &,
                                        bool usedOneAtATimeFallback) mutable {
                fetchWithGql(std::move(baseAccount),
                             usedOneAtATimeFallback);
            });
        return;
    }

    fetchModeratedChannelsWithHelix(
        baseAccount, localAccount->getOAuthClient(),
        localAccount->getOAuthToken(),
        [baseAccount, finishWithChannels](
            QVector<MoltorinoAuthChannel> channels,
            bool usedOneAtATimeFallback) mutable {
            finishWithChannels(baseAccount, std::move(channels),
                               usedOneAtATimeFallback);
        },
        [baseAccount, fetchWithGql, finishWithChannels](
            const QString &, bool localUsedOneAtATimeFallback) mutable {
            fetchModeratedChannelsWithHelix(
                baseAccount,
                baseAccount.clientId.isEmpty()
                    ? QString::fromUtf8(TWITCH_TV_CLIENT_ID)
                    : baseAccount.clientId,
                baseAccount.token,
                [baseAccount, finishWithChannels,
                 localUsedOneAtATimeFallback](
                    QVector<MoltorinoAuthChannel> channels,
                    bool tvUsedOneAtATimeFallback) mutable {
                    finishWithChannels(baseAccount, std::move(channels),
                                       localUsedOneAtATimeFallback ||
                                           tvUsedOneAtATimeFallback);
                },
                [baseAccount, fetchWithGql,
                 localUsedOneAtATimeFallback](
                    const QString &,
                    bool tvUsedOneAtATimeFallback) mutable {
                    fetchWithGql(
                        std::move(baseAccount),
                        localUsedOneAtATimeFallback ||
                            tvUsedOneAtATimeFallback);
                });
        });
}

void validateWithOAuth(
    const QString &token,
    std::function<void(MoltorinoAuthAccount)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedToken = normalizeToken(token);
    if (normalizedToken.isEmpty())
    {
        failureCallback("No token provided");
        return;
    }

    NetworkRequest(QUrl("https://id.twitch.tv/oauth2/validate"),
                   NetworkRequestType::Get)
        .timeout(20000)
        .maximumResponseSize(64 * 1024)
        .hideRequestBody()
        .followRedirects(true)
        .header("Accept", "application/json")
        .header("Authorization", "OAuth " + normalizedToken)
        .onSuccess([normalizedToken, successCallback = std::move(successCallback),
                    failureCallback](
                       const NetworkResult &result) {
            const auto json = result.parseJson();
            MoltorinoAuthAccount account;
            account.token = normalizedToken;
            account.userId = json.value("user_id").toString().trimmed();
            account.login = json.value("login").toString().trimmed().toLower();
            account.clientId =
                json.value("client_id").toString().trimmed();
            account.displayName = account.login;
            account.valid = true;
            account.lastValidatedAt = nowIso();

            if (account.userId.isEmpty() || account.login.isEmpty())
            {
                failureCallback(
                    "Twitch validated the token without returning account details");
                return;
            }
            successCallback(std::move(account));
        })
        .onError([failureCallback = std::move(failureCallback)](
                     const NetworkResult &result) {
            const auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                failureCallback(QString("%1 | %2")
                                    .arg(result.formatError(), body.left(200)));
                return;
            }
            failureCallback(result.formatError());
        })
        .execute();
}

}  // namespace

std::vector<MoltorinoAuthAccount> accounts()
{
    const auto raw = getSettings()->moltorinoAuthAccounts.getValue().trimmed();
    if (raw.isEmpty())
    {
        return {};
    }

    const auto doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isArray())
    {
        return {};
    }

    std::vector<MoltorinoAuthAccount> result;
    const auto array = doc.array();
    result.reserve(size_t(array.size()));
    for (const auto &value : array)
    {
        if (value.isObject())
        {
            auto account = accountFromJson(value.toObject());
            if (!account.token.trimmed().isEmpty())
            {
                result.push_back(std::move(account));
            }
        }
    }
    sortAccounts(result);
    return result;
}

MoltorinoAuthSummary summary()
{
    MoltorinoAuthSummary result;
    result.hasLegacyToken = !legacyToken().isEmpty();

    QSet<QString> channels;
    auto addChannelKey = [&channels](const QString &id, const QString &login) {
        const auto key = moderatedChannelKey(id, login);
        if (!key.isEmpty())
        {
            channels.insert(key);
        }
    };

    for (const auto &account : accounts())
    {
        ++result.accountCount;
        if (!account.enabled)
        {
            ++result.disabledAccountCount;
            continue;
        }

        ++result.enabledAccountCount;
        if (account.valid)
        {
            ++result.validAccountCount;
            addChannelKey(account.userId, account.login);
            for (const auto &channel : account.moderatedChannels)
            {
                addChannelKey(channel.id, channel.login);
            }
        }
        else
        {
            ++result.invalidAccountCount;
        }
    }

    result.moderatedChannelCount = channels.size();
    result.hasOnlyLegacyToken = result.accountCount == 0 && result.hasLegacyToken;
    return result;
}

QString legacyToken()
{
    return normalizeToken(getSettings()->customPinAuthToken.getValue());
}

bool hasConfiguredAuth()
{
    const auto saved = accounts();
    if (std::any_of(saved.begin(), saved.end(), [](const auto &account) {
            return account.enabled;
        }))
    {
        return true;
    }

    return saved.empty() && !legacyToken().isEmpty();
}

void addOrUpdateToken(
    const QString &token,
    std::function<void(MoltorinoAuthAccount)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    validateWithOAuth(
        token,
        [successCallback = std::move(successCallback)](
            MoltorinoAuthAccount account) mutable {
            fetchModeratedChannels(
                std::move(account), true,
                [successCallback = std::move(successCallback)](
                    MoltorinoAuthAccount account) mutable {
                    upsertAccount(account);
                    successCallback(std::move(account));
                });
        },
        std::move(failureCallback));
}

void rememberEditorChannel(const QString &token,
                           const MoltorinoAuthChannel &channel)
{
    const auto normalizedToken = normalizeToken(token);
    auto normalizedChannel = channel;
    normalizedChannel.id = normalizedChannel.id.trimmed();
    normalizedChannel.login = lower(normalizedChannel.login);
    normalizedChannel.displayName = normalizedChannel.displayName.trimmed();
    if (normalizedToken.isEmpty() ||
        (normalizedChannel.id.isEmpty() && normalizedChannel.login.isEmpty()))
    {
        return;
    }

    auto current = accounts();
    auto account = std::find_if(
        current.begin(), current.end(), [&normalizedToken](const auto &saved) {
            return normalizeToken(saved.token) == normalizedToken;
        });
    if (account == current.end() || !account->enabled || !account->valid)
    {
        return;
    }

    auto existing = std::find_if(
        account->verifiedEditorChannels.begin(),
        account->verifiedEditorChannels.end(),
        [&normalizedChannel](const MoltorinoAuthChannel &saved) {
            if (!normalizedChannel.id.isEmpty() && !saved.id.isEmpty())
            {
                return normalizedChannel.id == saved.id;
            }
            return !normalizedChannel.login.isEmpty() &&
                   lower(saved.login) == normalizedChannel.login;
        });
    if (existing == account->verifiedEditorChannels.end())
    {
        account->verifiedEditorChannels.push_back(std::move(normalizedChannel));
        saveAccounts(current);
        return;
    }

    if (existing->id != normalizedChannel.id ||
        existing->login != normalizedChannel.login ||
        existing->displayName != normalizedChannel.displayName)
    {
        *existing = std::move(normalizedChannel);
        saveAccounts(current);
    }
}

void forgetEditorChannel(const QString &token, const QString &channelId,
                         const QString &channelLogin)
{
    const auto normalizedToken = normalizeToken(token);
    const auto normalizedChannelId = channelId.trimmed();
    const auto normalizedChannelLogin = lower(channelLogin);
    if (normalizedToken.isEmpty() ||
        (normalizedChannelId.isEmpty() && normalizedChannelLogin.isEmpty()))
    {
        return;
    }

    auto current = accounts();
    auto account = std::find_if(
        current.begin(), current.end(), [&normalizedToken](const auto &saved) {
            return normalizeToken(saved.token) == normalizedToken;
        });
    if (account == current.end())
    {
        return;
    }

    const auto previousSize = account->verifiedEditorChannels.size();
    account->verifiedEditorChannels.erase(
        std::remove_if(
            account->verifiedEditorChannels.begin(),
            account->verifiedEditorChannels.end(),
            [&normalizedChannelId,
             &normalizedChannelLogin](const MoltorinoAuthChannel &saved) {
                if (!normalizedChannelId.isEmpty() && !saved.id.isEmpty())
                {
                    return saved.id.trimmed() == normalizedChannelId;
                }
                return !normalizedChannelLogin.isEmpty() &&
                       lower(saved.login) == normalizedChannelLogin;
            }),
        account->verifiedEditorChannels.end());
    if (account->verifiedEditorChannels.size() != previousSize)
    {
        saveAccounts(current);
    }
}

bool setAccountEnabled(const QString &userId, const QString &token,
                       bool enabled)
{
    const auto normalizedToken = normalizeToken(token);
    auto current = accounts();
    auto account =
        std::find_if(current.begin(), current.end(), [&](const auto &saved) {
            return sameAccount(saved, userId, normalizedToken);
        });
    if (account == current.end())
    {
        return false;
    }

    if (account->enabled != enabled)
    {
        account->enabled = enabled;
        saveAccounts(current);
    }
    return true;
}

void removeAccount(const QString &userId, const QString &token)
{
    const auto normalizedToken = normalizeToken(token);
    auto current = accounts();
    current.erase(std::remove_if(current.begin(), current.end(),
                                 [&](const auto &account) {
                                     return sameAccount(account, userId,
                                                        normalizedToken);
                                 }),
                  current.end());
    saveAccounts(current);

    if (!normalizedToken.isEmpty() && legacyToken() == normalizedToken)
    {
        getSettings()->customPinAuthToken = "";
        getSettings()->requestSave();
    }
}

void refreshAccounts(
    MoltorinoAuthRefreshMode mode,
    std::function<void(MoltorinoAuthRefreshResult)> callback)
{
    if (isAppAboutToQuit())
    {
        if (callback)
        {
            callback({});
        }
        return;
    }
    auto &coordinator = refreshCoordinator();
    if (coordinator.running)
    {
        if (mode == MoltorinoAuthRefreshMode::Manual &&
            coordinator.mode == MoltorinoAuthRefreshMode::Automatic)
        {
            coordinator.queuedManualCallbacks.push_back(std::move(callback));
        }
        else
        {
            coordinator.callbacks.push_back(std::move(callback));
        }
        return;
    }
    coordinator.callbacks.push_back(std::move(callback));
    coordinator.running = true;
    coordinator.mode = mode;

    auto finishRefresh = [](MoltorinoAuthRefreshResult result) {
        auto &coordinator = refreshCoordinator();
        auto callbacks = std::move(coordinator.callbacks);
        auto queuedManualCallbacks =
            std::move(coordinator.queuedManualCallbacks);
        coordinator.callbacks.clear();
        coordinator.queuedManualCallbacks.clear();
        coordinator.running = false;

        for (auto &callback : queuedManualCallbacks)
        {
            refreshAccounts(MoltorinoAuthRefreshMode::Manual,
                            std::move(callback));
        }

        for (auto &callback : callbacks)
        {
            if (callback)
            {
                callback(result);
            }
        }
    };

    auto existing = accounts();

    std::vector<QString> tokens;
    tokens.reserve(existing.size() + 1);
    for (const auto &account : existing)
    {
        if (!account.enabled)
        {
            continue;
        }
        const auto token = normalizeToken(account.token);
        if (!token.isEmpty() && std::find(tokens.begin(), tokens.end(), token) == tokens.end())
        {
            tokens.push_back(token);
        }
    }

    const auto legacy = legacyToken();
    if (existing.empty() && !legacy.isEmpty() &&
        std::find(tokens.begin(), tokens.end(), legacy) == tokens.end())
    {
        tokens.push_back(legacy);
    }

    if (tokens.empty())
    {
        finishRefresh({});
        return;
    }

    struct RefreshState {
        int pending = 0;
        std::vector<MoltorinoAuthAccount> accounts;
        MoltorinoAuthRefreshResult result;
        std::function<void(MoltorinoAuthRefreshResult)> callback;
    };

    auto state = std::make_shared<RefreshState>();
    state->pending = static_cast<int>(tokens.size());
    state->result.total = state->pending;
    state->callback = std::move(finishRefresh);

    auto existingAccountForToken = [existing](const QString &token) {
        const auto normalizedToken = normalizeToken(token);
        auto found =
            std::find_if(existing.begin(), existing.end(), [&](const auto &account) {
                return normalizeToken(account.token) == normalizedToken;
            });
        if (found != existing.end())
        {
            return *found;
        }

        MoltorinoAuthAccount account;
        account.token = normalizedToken;
        account.login = "legacy token";
        return account;
    };

    auto finishOne = [state](MoltorinoAuthAccount account) mutable {
        if (account.valid)
        {
            ++state->result.valid;
        }
        else
        {
            ++state->result.invalid;
        }
        if (!account.lastError.isEmpty())
        {
            state->result.errors.push_back(
                QString("%1: %2")
                    .arg(account.login.isEmpty() ? QString("Legacy token")
                                                 : account.login,
                         account.lastError));
        }

        state->accounts.push_back(std::move(account));
        --state->pending;
        if (state->pending > 0)
        {
            return;
        }

        auto merged = accounts();
        const auto currentLegacy = legacyToken();

        for (auto &refreshed : state->accounts)
        {
            const auto token = normalizeToken(refreshed.token);
            if (token.isEmpty())
            {
                continue;
            }

            auto current = std::find_if(
                merged.begin(), merged.end(), [&](const auto &saved) {
                    return normalizeToken(saved.token) == token;
                });
            if (current != merged.end())
            {
                if (!current->enabled)
                {
                    continue;
                }
                refreshed.enabled = true;
                refreshed.verifiedEditorChannels =
                    current->verifiedEditorChannels;
                if ((refreshed.displayName.isEmpty() ||
                     refreshed.displayName.compare(refreshed.login,
                                                   Qt::CaseInsensitive) == 0) &&
                    !current->displayName.isEmpty())
                {
                    refreshed.displayName = current->displayName;
                }
                *current = std::move(refreshed);
                continue;
            }

            if (merged.empty() && !currentLegacy.isEmpty() &&
                token == currentLegacy)
            {
                refreshed.enabled = true;
                merged.push_back(std::move(refreshed));
            }
        }

        sortAccounts(merged);
        saveAccounts(merged);
        state->result.moderatedChannels = summary().moderatedChannelCount;
        state->callback(state->result);
    };

    for (const auto &token : tokens)
    {
        validateWithOAuth(
            token,
            [finishOne, existingAccountForToken,
             mode](MoltorinoAuthAccount account) mutable {
                const auto existing =
                    existingAccountForToken(account.token);
                account.moderatedChannels = existing.moderatedChannels;
                account.verifiedEditorChannels =
                    existing.verifiedEditorChannels;
                account.lastError = existing.lastError;
                account.moderatedChannelsManualRefreshOnly =
                    existing.moderatedChannelsManualRefreshOnly;
                fetchModeratedChannels(
                    std::move(account),
                    mode == MoltorinoAuthRefreshMode::Manual, finishOne);
            },
            [token, finishOne, existingAccountForToken](const QString &error) mutable {
                auto account = existingAccountForToken(token);
                account.valid = account.valid && !looksLikeAuthError(error);
                account.lastError = error;
                account.lastValidatedAt = nowIso();
                finishOne(std::move(account));
            });
    }
}

void scheduleStartupRefresh()
{
    static bool scheduled = false;
    if (scheduled)
    {
        return;
    }
    scheduled = true;

    if (!hasConfiguredAuth())
    {
        return;
    }

    auto *app = QCoreApplication::instance();
    if (app == nullptr)
    {
        return;
    }

    QTimer::singleShot(10000, app, [] {
        if (!hasConfiguredAuth())
        {
            return;
        }

        refreshAccounts(MoltorinoAuthRefreshMode::Automatic,
                        [](MoltorinoAuthRefreshResult) {});
    });
}

MoltorinoAuthToken resolveModerationToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage)
{
    const auto valid = validAccounts();
    if (!valid.empty())
    {
        for (const auto &account : valid)
        {
            if (accountMatchesCurrentUser(account) &&
                accountMatchesChannel(account, channelId, channelLogin))
            {
                return makeToken(account);
            }
        }

        for (const auto &account : valid)
        {
            if (accountMatchesChannel(account, channelId, channelLogin))
            {
                return makeToken(account);
            }
        }

        if (errorMessage)
        {
            *errorMessage =
                QString("No saved account has cached moderator access "
                        "for #%1. Refresh accounts in Settings -> Moltorino -> "
                        "Authentication or add the account that moderates this channel.")
                    .arg(channelLogin);
        }
        return {};
    }

    if (accounts().empty() && !legacyToken().isEmpty())
    {
        return makeLegacyToken();
    }

    if (errorMessage)
    {
        *errorMessage = authRequiredMessage("this action");
    }
    return {};
}

MoltorinoAuthToken resolveSavedBroadcasterToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage)
{
    const auto valid = validAccounts();
    if (const auto account =
            broadcasterAccountForChannel(valid, channelId, channelLogin))
    {
        return makeToken(*account);
    }

    if (errorMessage)
    {
        *errorMessage =
            QString("No saved broadcaster account matches #%1. Add the "
                    "broadcaster account in Settings -> Moltorino -> "
                    "Authentication.")
                .arg(channelLogin);
    }
    return {};
}

MoltorinoAuthToken resolveBroadcasterToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage)
{
    const auto valid = validAccounts();
    if (!valid.empty())
    {
        if (const auto account =
                broadcasterAccountForChannel(valid, channelId, channelLogin))
        {
            return makeToken(*account);
        }

        if (errorMessage)
        {
            *errorMessage =
                QString("No saved account matches #%1. Add the broadcaster "
                        "account in Settings -> Moltorino -> Authentication.")
                    .arg(channelLogin);
        }
        return {};
    }

    if (accounts().empty() && !legacyToken().isEmpty())
    {
        return makeLegacyToken();
    }

    if (errorMessage)
    {
        *errorMessage = authRequiredMessage("raid controls");
    }
    return {};
}

MoltorinoAuthToken resolveSelectedUserToken(QString *errorMessage)
{
    const auto valid = validAccounts();
    for (const auto &account : valid)
    {
        if (accountMatchesCurrentUser(account))
        {
            return rememberPersonalToken(makeToken(account));
        }
    }

    if (errorMessage)
    {
        *errorMessage =
            "Saved login for the current Twitch account was not found. "
            "Add this account in Settings -> Moltorino -> Authentication.";
    }
    return {};
}

MoltorinoAuthToken resolveCurrentUserToken(QString *errorMessage)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    const auto currentUserId =
        current && !current->isAnon() ? current->getUserId() : QString();
    const auto currentLogin =
        current && !current->isAnon() ? lower(current->getUserName())
                                      : QString();

    const auto valid = validAccounts();
    if (!valid.empty())
    {
        for (const auto &account : valid)
        {
            if ((!currentUserId.isEmpty() && account.userId == currentUserId) ||
                (!currentLogin.isEmpty() && lower(account.login) == currentLogin))
            {
                return rememberPersonalToken(makeToken(account));
            }
        }

        if (auto lastAccount =
                validAccountForToken(valid, lastResolvedPersonalToken()))
        {
            return rememberPersonalToken(makeToken(*lastAccount));
        }

        if (valid.size() == 1)
        {
            return rememberPersonalToken(makeToken(valid.front()));
        }

        if (auto latest = mostRecentlyValidatedAccount(valid))
        {
            return rememberPersonalToken(makeToken(*latest));
        }

        if (errorMessage)
        {
            *errorMessage =
                "Saved login for the current Twitch account was not found. "
                "Add this account in Settings -> Moltorino -> Authentication.";
        }
        return {};
    }

    if (accounts().empty() && !legacyToken().isEmpty())
    {
        return rememberPersonalToken(makeLegacyToken());
    }

    if (errorMessage)
    {
        *errorMessage = authRequiredMessage("this action");
    }
    return {};
}

MoltorinoAuthToken resolveReadToken(QString *errorMessage)
{
    QString ignored;
    auto currentUserToken = resolveCurrentUserToken(&ignored);
    if (currentUserToken.hasToken())
    {
        return currentUserToken;
    }

    auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (current && !current->isAnon() && !current->getOAuthToken().isEmpty())
    {
        return {
            .token = current->getOAuthToken(),
            .userId = current->getUserId(),
            .login = current->getUserName(),
            .clientId = current->getOAuthClient(),
            .legacy = false,
        };
    }

    if (errorMessage)
    {
        *errorMessage = ignored.isEmpty() ? authRequiredMessage("this action")
                                         : ignored;
    }
    return {};
}

QString authRequiredMessage(const QString &action)
{
    return QString(
               "Additional login required for %1. Add an account in Settings -> "
               "Moltorino -> Authentication, then try again.")
        .arg(action);
}

QString authExpiredMessage(const QString &action)
{
    return QString(
               "Your saved login is missing or may have expired. "
               "Refresh accounts or add the account again in Settings -> "
               "Moltorino -> Authentication, then try %1 again.")
        .arg(action);
}

QString normalizeAuthError(const QString &action, const QString &error)
{
    if (looksLikeAuthError(error))
    {
        return authExpiredMessage(action);
    }
    return error;
}

}  // namespace chatterino::MoltorinoAuth
