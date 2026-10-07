#include "controllers/ignores/HiddenUser.hpp"

#include <utility>

namespace chatterino {

namespace {

QString normalizedHiddenName(QString value)
{
    value = value.trimmed();
    while (value.startsWith(u'@'))
    {
        value.remove(0, 1);
    }
    return value.toCaseFolded();
}

QString normalizedHiddenID(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QStringLiteral("id:"), Qt::CaseInsensitive))
    {
        value.remove(0, 3);
    }
    if (value.startsWith(QStringLiteral("kick:"), Qt::CaseInsensitive))
    {
        value.remove(0, 5);
    }
    return value;
}

}

QString hiddenUserPlatformName(HiddenUserPlatform platform)
{
    switch (platform)
    {
        case HiddenUserPlatform::Twitch:
            return QStringLiteral("Twitch");
        case HiddenUserPlatform::Kick:
            return QStringLiteral("Kick");
        case HiddenUserPlatform::YouTube:
            return QStringLiteral("YouTube");
        case HiddenUserPlatform::TikTok:
            return QStringLiteral("TikTok");
    }
    return QStringLiteral("Twitch");
}

QString hiddenUserPlatformKey(HiddenUserPlatform platform)
{
    return hiddenUserPlatformName(platform).toLower();
}

HiddenUserPlatform hiddenUserPlatformFromKey(const QString &key)
{
    if (key.compare(QStringLiteral("tiktok"), Qt::CaseInsensitive) == 0)
    {
        return HiddenUserPlatform::TikTok;
    }
    if (key.compare(QStringLiteral("kick"), Qt::CaseInsensitive) == 0)
    {
        return HiddenUserPlatform::Kick;
    }
    if (key.compare(QStringLiteral("youtube"), Qt::CaseInsensitive) == 0)
    {
        return HiddenUserPlatform::YouTube;
    }
    return HiddenUserPlatform::Twitch;
}

HiddenUser::HiddenUser(HiddenUserPlatform platform, QString userID,
                       QString login, QString displayName)
    : platform_(platform)
    , userID_(std::move(userID))
    , login_(std::move(login))
    , displayName_(std::move(displayName))
{
}

bool HiddenUser::operator==(const HiddenUser &other) const
{
    return std::tie(this->platform_, this->userID_, this->login_,
                    this->displayName_) ==
           std::tie(other.platform_, other.userID_, other.login_,
                    other.displayName_);
}

HiddenUserPlatform HiddenUser::platform() const
{
    return this->platform_;
}

const QString &HiddenUser::userID() const
{
    return this->userID_;
}

const QString &HiddenUser::login() const
{
    return this->login_;
}

const QString &HiddenUser::displayName() const
{
    return this->displayName_;
}

QString HiddenUser::bestName() const
{
    if (!this->displayName_.trimmed().isEmpty())
    {
        return this->displayName_.trimmed();
    }
    if (!this->login_.trimmed().isEmpty())
    {
        return this->login_.trimmed();
    }
    return this->userID_.trimmed();
}

QString HiddenUser::displayLabel() const
{
    return QStringLiteral("%1 (%2)")
        .arg(this->bestName(), hiddenUserPlatformName(this->platform_));
}

bool HiddenUser::matchesIdentity(HiddenUserPlatform platform,
                                 const QString &userID, const QString &login,
                                 const QString &displayName) const
{
    if (this->platform_ != platform)
    {
        return false;
    }

    const auto ownID = normalizedHiddenID(this->userID_);
    const auto otherID = normalizedHiddenID(userID);
    if (!ownID.isEmpty() && !otherID.isEmpty())
    {
        return ownID == otherID;
    }

    const auto ownLogin = normalizedHiddenName(this->login_);
    const auto ownDisplayName = normalizedHiddenName(this->displayName_);
    const auto matchesName = [&](const QString &candidate) {
        const auto name = normalizedHiddenName(candidate);
        return !name.isEmpty() &&
               (name == ownLogin || name == ownDisplayName);
    };
    return matchesName(login) || matchesName(displayName);
}

}
