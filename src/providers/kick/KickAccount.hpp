#pragma once

#include "controllers/accounts/Account.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace chatterino {

namespace kick {
inline constexpr QLatin1StringView AUTH_PROXY{"https://c7-auth.nerixyz.de"};
inline constexpr QLatin1StringView AUTH_CLIENT_ID{"01KEBZBHCX3DKEJ0KRQBDNT05B"};
}

struct KickAccountData {
    QString username;
    uint64_t userID = 0;
    QString clientID;
    QString clientSecret;
    QString publicProxy;
    QString authToken;
    QString refreshToken;
    QDateTime expiresAt;

    QString tokenUrl() const;
    bool setTokens(const QJsonObject &response);
    void save() const;
    static std::optional<KickAccountData> loadRaw(const std::string &key);
};

class KickAccount : public Account,
                    public std::enable_shared_from_this<KickAccount>
{
public:
    KickAccount(const KickAccountData &args);
    ~KickAccount() override;

    constexpr static std::chrono::minutes CHECK_REFRESH_INTERVAL{5};

    Q_DISABLE_COPY_MOVE(KickAccount);

    void save() const;

    bool update(const KickAccountData &data);

    QString toString() const override;

    bool isAnonymous() const
    {
        return this->userID_ == 0;
    }

    QString username() const
    {
        return this->username_;
    }
    uint64_t userID() const
    {
        return this->userID_;
    }
    QString clientID() const
    {
        return this->clientID_;
    }
    QString clientSecret() const
    {
        return this->clientSecret_;
    }
    QString authToken() const
    {
        return this->authToken_;
    }
    QString publicProxy() const
    {
        return this->publicProxy_;
    }
    QString refreshToken() const
    {
        return this->refreshToken_;
    }

    QString seventvUserID() const
    {
        return this->seventvUserID_;
    }

    void refreshIfNeeded();
    void cancelRefresh();
    void loadSeventvUser();

    pajlada::Signals::NoArgSignal authUpdated;

private:
    QString username_;
    uint64_t userID_ = 0;
    QString clientID_;
    QString clientSecret_;
    QString publicProxy_;
    QString authToken_;
    QString refreshToken_;
    QDateTime expiresAt_;
    uint64_t refreshGeneration_ = 0;
    bool refreshing_ = false;

    QString seventvUserID_;
};

}
