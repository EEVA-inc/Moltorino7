#include "providers/youtube/YouTubeAccount.hpp"

#include "common/ChatterinoSetting.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep

#include <utility>

namespace chatterino {

YouTubeAccount::YouTubeAccount(YouTubeAccountData data)
    : Account(ProviderId::YouTube)
    , channelID_(std::move(data.channelID))
    , handle_(std::move(data.handle))
    , displayName_(std::move(data.displayName))
    , avatarUrl_(std::move(data.avatarUrl))
    , metadataRefreshedAt_(std::move(data.metadataRefreshedAt))
    , accessToken_(std::move(data.accessToken))
    , refreshToken_(std::move(data.refreshToken))
    , expiresAt_(std::move(data.expiresAt))
{
}

YouTubeAccount::~YouTubeAccount() = default;

QString YouTubeAccount::toString() const
{
    return this->displayName_;
}

bool YouTubeAccount::isAnonymous() const
{
    return this->channelID_.isEmpty();
}

bool YouTubeAccount::hasCredentials() const
{
    if (!this->refreshToken_.isEmpty())
    {
        return true;
    }
    return !this->accessToken_.isEmpty() && this->expiresAt_.isValid() &&
           this->expiresAt_ > QDateTime::currentDateTimeUtc();
}

const QString &YouTubeAccount::channelID() const
{
    return this->channelID_;
}

const QString &YouTubeAccount::handle() const
{
    return this->handle_;
}

const QString &YouTubeAccount::displayName() const
{
    return this->displayName_;
}

const QString &YouTubeAccount::avatarUrl() const
{
    return this->avatarUrl_;
}

const QDateTime &YouTubeAccount::metadataRefreshedAt() const
{
    return this->metadataRefreshedAt_;
}

const QString &YouTubeAccount::accessToken() const
{
    return this->accessToken_;
}

const QString &YouTubeAccount::refreshToken() const
{
    return this->refreshToken_;
}

const QDateTime &YouTubeAccount::expiresAt() const
{
    return this->expiresAt_;
}

bool YouTubeAccount::update(const YouTubeAccountData &data)
{
    bool metadataChanged = false;
    bool credentialsChanged = false;

    if (!data.handle.isEmpty() && this->handle_ != data.handle)
    {
        this->handle_ = data.handle;
        metadataChanged = true;
    }
    if (!data.displayName.isEmpty() && this->displayName_ != data.displayName)
    {
        this->displayName_ = data.displayName;
        metadataChanged = true;
    }
    if (!data.avatarUrl.isEmpty() && this->avatarUrl_ != data.avatarUrl)
    {
        this->avatarUrl_ = data.avatarUrl;
        metadataChanged = true;
    }
    if (data.metadataRefreshedAt.isValid() &&
        this->metadataRefreshedAt_ != data.metadataRefreshedAt)
    {
        this->metadataRefreshedAt_ = data.metadataRefreshedAt.toUTC();
        metadataChanged = true;
    }
    if (!data.accessToken.isEmpty() && this->accessToken_ != data.accessToken)
    {
        this->accessToken_ = data.accessToken;
        credentialsChanged = true;
    }
    if (!data.refreshToken.isEmpty() &&
        this->refreshToken_ != data.refreshToken)
    {
        this->refreshToken_ = data.refreshToken;
        credentialsChanged = true;
    }
    if (data.expiresAt.isValid() && this->expiresAt_ != data.expiresAt)
    {
        this->expiresAt_ = data.expiresAt;
        credentialsChanged = true;
    }

    if (credentialsChanged)
    {
        this->credentialsUpdated.invoke();
    }
    return metadataChanged;
}

void YouTubeAccount::replaceCredentials(const YouTubeAccountData &data)
{
    if (this->accessToken_ == data.accessToken &&
        this->refreshToken_ == data.refreshToken &&
        this->expiresAt_ == data.expiresAt)
    {
        return;
    }

    this->accessToken_ = data.accessToken;
    this->refreshToken_ = data.refreshToken;
    this->expiresAt_ = data.expiresAt;
    this->credentialsUpdated.invoke();
}

void YouTubeAccount::updateTokens(const QString &accessToken,
                                  const QString &refreshToken,
                                  const QDateTime &expiresAt)
{
    if (this->accessToken_ == accessToken &&
        (refreshToken.isEmpty() || this->refreshToken_ == refreshToken) &&
        this->expiresAt_ == expiresAt)
    {
        return;
    }

    this->accessToken_ = accessToken;
    if (!refreshToken.isEmpty())
    {
        this->refreshToken_ = refreshToken;
    }
    this->expiresAt_ = expiresAt;
    this->credentialsUpdated.invoke();
}

void YouTubeAccount::clearTokens()
{
    if (this->accessToken_.isEmpty() && this->refreshToken_.isEmpty() &&
        !this->expiresAt_.isValid())
    {
        return;
    }

    this->accessToken_.clear();
    this->refreshToken_.clear();
    this->expiresAt_ = {};
    this->credentialsUpdated.invoke();
}

void YouTubeAccount::saveMetadata() const
{
    if (this->isAnonymous())
    {
        return;
    }

    const auto basePath = "/youtubeAccounts/" + this->channelID_.toStdString();
    QStringSetting::set(basePath + "/channelID", this->channelID_);
    QStringSetting::set(basePath + "/handle", this->handle_);
    QStringSetting::set(basePath + "/displayName", this->displayName_);
    QStringSetting::set(basePath + "/avatarUrl", this->avatarUrl_);
    QStringSetting::set(
        basePath + "/metadataRefreshedAt",
        this->metadataRefreshedAt_.isValid()
            ? this->metadataRefreshedAt_.toUTC().toString(Qt::ISODateWithMs)
            : QString{});
}

}
