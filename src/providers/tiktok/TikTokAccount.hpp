#pragma once

#include "controllers/accounts/Account.hpp"
#include "providers/tiktok/TikTokSession.hpp"

namespace chatterino {

struct TikTokAccountData {
    QString userID;
    QString handle;
    QString displayName;
    TikTokSession session;
};

class TikTokAccount : public Account
{
public:
    explicit TikTokAccount(TikTokAccountData data);
    QString toString() const override;
    const QString &userID() const;
    const QString &handle() const;
    const QString &displayName() const;
    QString avatarUrl() const;
    const TikTokSession &session() const;
    TikTokSession &session();
    bool isAnonymous() const;
    bool hasCredentials() const;
    bool isLoadingCredentials() const;
    void setLoadingCredentials(bool loading);
    void clearSession();

private:
    TikTokAccountData data_;
    bool loadingCredentials_ = false;
};

bool isTikTokUserID(QStringView value);

}
