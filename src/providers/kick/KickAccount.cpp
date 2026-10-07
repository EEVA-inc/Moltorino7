#include "providers/kick/KickAccount.hpp"

#include "Application.hpp"
#include "common/ChatterinoSetting.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "singletons/Settings.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/settings/settinglistener.hpp>
#include <pajlada/settings/settingmanager.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QUrlQuery>

#include <algorithm>
#include <limits>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

QString KickAccountData::tokenUrl() const
{
    if (!this->publicProxy.isEmpty())
    {
        if (this->publicProxy != kick::AUTH_PROXY ||
            this->clientID != kick::AUTH_CLIENT_ID)
        {
            return {};
        }
        return this->publicProxy + u"/oauth/token"_s;
    }
    return !this->clientID.isEmpty() && !this->clientSecret.isEmpty()
               ? u"https://id.kick.com/oauth/token"_s
               : QString{};
}

bool KickAccountData::setTokens(const QJsonObject &response)
{
    const auto access = response["access_token"].toString().trimmed();
    const auto refresh = response["refresh_token"].toString().trimmed();
    const auto expiry = response["expires_in"];
    const auto seconds =
        expiry.isString() ? expiry.toString().toLongLong() : expiry.toInteger();
    if (access.isEmpty() || refresh.isEmpty() || seconds <= 0)
    {
        return false;
    }
    this->authToken = access;
    this->refreshToken = refresh;
    this->expiresAt = QDateTime::currentDateTimeUtc().addSecs(
        std::min<qint64>(seconds, std::numeric_limits<qint32>::max()));
    return true;
}

std::optional<KickAccountData> KickAccountData::loadRaw(const std::string &key)
{
    auto username = QStringSetting::get("/kickAccounts/" + key + "/username");
    auto userID = UInt64Setting::get("/kickAccounts/" + key + "/userID");
    auto clientID = QStringSetting::get("/kickAccounts/" + key + "/clientID");
    auto clientSecret =
        QStringSetting::get("/kickAccounts/" + key + "/clientSecret");
    auto publicProxy =
        QStringSetting::get("/kickAccounts/" + key + "/publicProxy");
    auto authToken = QStringSetting::get("/kickAccounts/" + key + "/authToken");
    auto refreshToken =
        QStringSetting::get("/kickAccounts/" + key + "/refreshToken");
    auto expiresAtStr =
        QStringSetting::get("/kickAccounts/" + key + "/expiresAt");

    if (username.trimmed().isEmpty() || userID == 0 ||
        authToken.trimmed().isEmpty() || refreshToken.trimmed().isEmpty())
    {
        return std::nullopt;
    }

    QDateTime expiresAt = QDateTime::fromString(expiresAtStr, Qt::ISODate);
    if (!expiresAt.isValid())
    {
        return std::nullopt;
    }

    KickAccountData data{
        .username = username.trimmed(),
        .userID = userID,
        .clientID = clientID.trimmed(),
        .clientSecret = publicProxy.trimmed().isEmpty() ? clientSecret.trimmed()
                                                        : QString{},
        .publicProxy = publicProxy.trimmed(),
        .authToken = authToken.trimmed(),
        .refreshToken = refreshToken.trimmed(),
        .expiresAt = expiresAt,
    };
    return data.tokenUrl().isEmpty() ? std::nullopt
                                     : std::make_optional(std::move(data));
}

void KickAccountData::save() const
{
    auto basePath = "/kickAccounts/uid" + std::to_string(this->userID);
    QStringSetting::set(basePath + "/username", this->username.toLower());
    UInt64Setting::set(basePath + "/userID", this->userID);
    QStringSetting::set(basePath + "/clientID", this->clientID);
    QStringSetting::set(basePath + "/clientSecret", this->publicProxy.isEmpty()
                                                        ? this->clientSecret
                                                        : QString{});
    QStringSetting::set(basePath + "/publicProxy", this->publicProxy);
    QStringSetting::set(basePath + "/authToken", this->authToken);
    QStringSetting::set(basePath + "/refreshToken", this->refreshToken);
    QStringSetting::set(basePath + "/expiresAt",
                        this->expiresAt.toString(Qt::ISODate));
    std::ignore = getSettings()->requestSave();
}

KickAccount::KickAccount(const KickAccountData &args)
    : Account(ProviderId::Kick)
    , username_(args.username.toLower())
    , userID_(args.userID)
    , clientID_(args.clientID)
    , clientSecret_(args.publicProxy.isEmpty() ? args.clientSecret : QString{})
    , publicProxy_(args.publicProxy)
    , authToken_(args.authToken)
    , refreshToken_(args.refreshToken)
    , expiresAt_(args.expiresAt)
{
}

KickAccount::~KickAccount() = default;

void KickAccount::save() const
{
    KickAccountData{
        .username = this->username_,
        .userID = this->userID_,
        .clientID = this->clientID_,
        .clientSecret = this->clientSecret_,
        .publicProxy = this->publicProxy_,
        .authToken = this->authToken_,
        .refreshToken = this->refreshToken_,
        .expiresAt = this->expiresAt_,
    }
        .save();
}

bool KickAccount::update(const KickAccountData &data)
{
    bool changed = false;

    if (this->username_ != data.username.toLower())
    {
        changed = true;
        this->username_ = data.username.toLower();
    }
    if (this->userID_ != data.userID)
    {
        changed = true;
        this->userID_ = data.userID;
    }
    if (this->clientID_ != data.clientID)
    {
        changed = true;
        this->clientID_ = data.clientID;
    }
    const auto secret =
        data.publicProxy.isEmpty() ? data.clientSecret : QString{};
    if (this->clientSecret_ != secret)
    {
        changed = true;
        this->clientSecret_ = secret;
    }
    if (this->publicProxy_ != data.publicProxy)
    {
        changed = true;
        this->publicProxy_ = data.publicProxy;
    }
    if (this->authToken_ != data.authToken)
    {
        changed = true;
        this->authToken_ = data.authToken;
    }
    if (this->refreshToken_ != data.refreshToken)
    {
        changed = true;
        this->refreshToken_ = data.refreshToken;
    }
    if (this->expiresAt_ != data.expiresAt)
    {
        changed = true;
        this->expiresAt_ = data.expiresAt;
    }

    if (changed)
    {
        this->cancelRefresh();
        this->save();
        this->authUpdated.invoke();
    }
    return changed;
}

QString KickAccount::toString() const
{
    return this->username_;
}

void KickAccount::refreshIfNeeded()
{
    if (this->isAnonymous() || this->refreshing_)
    {
        return;
    }

    auto now = QDateTime::currentDateTimeUtc() + CHECK_REFRESH_INTERVAL +
               std::chrono::seconds{30};
    if (now < this->expiresAt_)
    {
        return;
    }

    KickAccountData credentials{
        .clientID = this->clientID_,
        .clientSecret = this->clientSecret_,
        .publicProxy = this->publicProxy_,
    };
    const auto url = credentials.tokenUrl();
    if (url.isEmpty())
    {
        return;
    }
    QUrlQuery payload{
        {"refresh_token"_L1, this->refreshToken_},
        {"client_id"_L1, this->clientID_},
        {"grant_type"_L1, "refresh_token"_L1},
    };
    if (this->publicProxy_.isEmpty())
    {
        payload.addQueryItem(u"client_secret"_s, this->clientSecret_);
    }

    auto weak = this->weak_from_this();
    const auto generation = ++this->refreshGeneration_;
    this->refreshing_ = true;
    NetworkRequest(url, NetworkRequestType::Post)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .hideRequestBody()
        .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
        .timeout(20'000)
        .maximumResponseSize(64 * 1024)
        .onSuccess([weak, generation](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self || self->refreshGeneration_ != generation)
            {
                return;
            }

            self->refreshing_ = false;
            KickAccountData tokens;
            if (!tokens.setTokens(res.parseJson()))
            {
                qCWarning(chatterinoKick)
                    << "Invalid Kick token refresh response";
                return;
            }
            self->authToken_ = tokens.authToken;
            self->refreshToken_ = tokens.refreshToken;
            self->expiresAt_ = tokens.expiresAt;
            self->save();
            self->authUpdated.invoke();
        })
        .onError([weak, generation](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self || self->refreshGeneration_ != generation)
            {
                return;
            }
            self->refreshing_ = false;
            qCWarning(chatterinoKick) << "Failed to refresh" << self->username()
                                      << "error:" << res.formatError();
        })
        .execute();
}

void KickAccount::cancelRefresh()
{
    ++this->refreshGeneration_;
    this->refreshing_ = false;
}

void KickAccount::loadSeventvUser()
{
    if (this->isAnonymous())
    {
        return;
    }
    if (!this->seventvUserID_.isEmpty())
    {
        return;
    }

    const auto loadPersonalEmotes = [](uint64_t userID,
                                       const QString &emoteSetID) {
        SeventvEmotes::getEmoteSet(
            emoteSetID,
            [userID, emoteSetID](auto &&emoteMap,
                                 const auto & /*emoteSetName*/) {
                getApp()->getSeventvPersonalEmotes()->addEmoteSetForKickUser(
                    emoteSetID, std::forward<decltype(emoteMap)>(emoteMap),
                    userID);
            },
            [userID, emoteSetID](const auto &error) {
                qCDebug(chatterinoSeventv)
                    << "Failed to fetch personal emote-set. emote-set-id:"
                    << emoteSetID << "kick-user-id" << userID
                    << "error:" << error;
            });
    };

    auto *seventv = getApp()->getSeventvAPI();
    if (!seventv)
    {
        qCWarning(chatterinoSeventv)
            << "Not loading 7TV User ID because the 7TV API is not initialized";
        return;
    }

    seventv->getUserByKickID(
        this->userID(),
        [weak = this->weak_from_this(), loadPersonalEmotes](const auto &json,
                                                            const auto &) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            const auto user = json["user"].toObject();
            const auto id = user["id"].toString();
            if (id.isEmpty())
            {
                return;
            }
            self->seventvUserID_ = id;

            for (const auto &emoteSetJson : user["emote_sets"].toArray())
            {
                const auto emoteSet = emoteSetJson.toObject();
                if (SeventvEmoteSetFlags(
                        SeventvEmoteSetFlag(emoteSet["flags"].toInt()))
                        .has(SeventvEmoteSetFlag::Personal))
                {
                    loadPersonalEmotes(self->userID(),
                                       emoteSet["id"].toString());
                    break;
                }
            }
        },
        [](const auto &result) {
            qCDebug(chatterinoSeventv)
                << "Failed to load your 7TV user-id:" << result.formatError();
        });
}

}  // namespace chatterino
