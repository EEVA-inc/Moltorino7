#pragma once

#include "controllers/accounts/Account.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QString>

namespace chatterino {

struct YouTubeAccountData {
    QString channelID;
    QString handle;
    QString displayName;
    QString avatarUrl;
    QDateTime metadataRefreshedAt;
    QString accessToken;
    QString refreshToken;
    QDateTime expiresAt;
};

class YouTubeAccount : public Account
{
public:
    explicit YouTubeAccount(YouTubeAccountData data);
    ~YouTubeAccount() override;

    Q_DISABLE_COPY_MOVE(YouTubeAccount)

    QString toString() const override;

    bool isAnonymous() const;
    bool hasCredentials() const;

    const QString &channelID() const;
    const QString &handle() const;
    const QString &displayName() const;
    const QString &avatarUrl() const;
    const QDateTime &metadataRefreshedAt() const;
    const QString &accessToken() const;
    const QString &refreshToken() const;
    const QDateTime &expiresAt() const;

    bool update(const YouTubeAccountData &data);
    void replaceCredentials(const YouTubeAccountData &data);
    void updateTokens(const QString &accessToken, const QString &refreshToken,
                      const QDateTime &expiresAt);
    void clearTokens();

    void saveMetadata() const;

    pajlada::Signals::NoArgSignal credentialsUpdated;

private:
    QString channelID_;
    QString handle_;
    QString displayName_;
    QString avatarUrl_;
    QDateTime metadataRefreshedAt_;
    QString accessToken_;
    QString refreshToken_;
    QDateTime expiresAt_;
};

}
