#include "providers/tiktok/TikTokAccount.hpp"

#include "providers/tiktok/TikTokTypes.hpp"

namespace chatterino {

TikTokAccount::TikTokAccount(TikTokAccountData data)
    : Account(ProviderId::TikTok)
    , data_(std::move(data))
{
}

QString TikTokAccount::toString() const
{
    return this->data_.displayName;
}

const QString &TikTokAccount::userID() const
{
    return this->data_.userID;
}

const QString &TikTokAccount::handle() const
{
    return this->data_.handle;
}

const QString &TikTokAccount::displayName() const
{
    return this->data_.displayName;
}

QString TikTokAccount::avatarUrl() const
{
    const auto user = this->data_.session.context.value("user").toObject();
    if (user.value("uid").toString() != this->userID())
    {
        return {};
    }
    for (const auto *key : {"avatarLarger", "avatarMedium", "avatarThumb"})
    {
        const auto url = user.value(key).toString();
        if (isTikTokImageUrl(QUrl(url)))
        {
            return url;
        }
    }
    return {};
}

const TikTokSession &TikTokAccount::session() const
{
    return this->data_.session;
}

TikTokSession &TikTokAccount::session()
{
    return this->data_.session;
}

bool TikTokAccount::isAnonymous() const
{
    return this->data_.userID.isEmpty();
}

bool TikTokAccount::hasCredentials() const
{
    return !this->isAnonymous() && this->data_.session.hasCredentials();
}

bool TikTokAccount::isLoadingCredentials() const
{
    return this->loadingCredentials_;
}

void TikTokAccount::setLoadingCredentials(bool loading)
{
    this->loadingCredentials_ = loading;
}

void TikTokAccount::clearSession()
{
    this->loadingCredentials_ = false;
    this->data_.session = {};
}

bool isTikTokUserID(QStringView value)
{
    if (value.isEmpty() || value.size() > 20)
    {
        return false;
    }
    for (const auto c : value)
    {
        if (c < u'0' || c > u'9')
        {
            return false;
        }
    }
    bool ok = false;
    const auto id = value.toULongLong(&ok);
    return ok && id != 0;
}
}
