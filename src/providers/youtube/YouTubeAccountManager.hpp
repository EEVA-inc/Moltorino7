#pragma once

#include "common/SignalVector.hpp"
#include "util/Expected.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace chatterino {

class YouTubeAccount;
struct YouTubeAccountData;

class YouTubeAccountManager
{
public:
    using AccessTokenCallback = std::function<void(ExpectedStr<QString>)>;

    struct PendingAccessToken {
        std::weak_ptr<YouTubeAccount> account;
        std::uint64_t selectionGeneration = 0;
        bool requireCurrentAccount = true;
        AccessTokenCallback callback;
    };

    YouTubeAccountManager();
    ~YouTubeAccountManager();

    void load();

    std::shared_ptr<YouTubeAccount> addAccount(const YouTubeAccountData &data);
    bool removeAccount(const QString &channelID);

    std::shared_ptr<YouTubeAccount> current() const;
    std::shared_ptr<YouTubeAccount> findByChannelID(
        const QString &channelID) const;
    std::vector<std::shared_ptr<YouTubeAccount>> accountList() const;
    std::vector<std::pair<QString, QString>> credentialCleanupErrors() const;
    void dismissCredentialCleanupErrors();

    void selectAccount(const QString &channelID);
    bool isLoggedIn() const;
    void getAccessToken(AccessTokenCallback callback);
    bool invalidateAccessToken(const QString &channelID,
                               const QString &rejectedAccessToken);
    bool requireReconnect(const QString &channelID,
                          const QString &rejectedAccessToken);

    pajlada::Signals::NoArgSignal currentChanged;
    pajlada::Signals::NoArgSignal credentialsChanged;
    pajlada::Signals::NoArgSignal userListUpdated;
    pajlada::Signals::NoArgSignal credentialCleanupChanged;
    SignalVector<std::shared_ptr<YouTubeAccount>> accounts;

private:
    struct TokenRefreshState {
        bool inFlight = false;
        std::uint64_t generation = 0;
        std::weak_ptr<YouTubeAccount> account;
        std::vector<PendingAccessToken> callbacks;
    };

    void applyCurrent(const QString &channelID);
    void getAccessTokenForAccount(
        const std::shared_ptr<YouTubeAccount> &account,
        bool requireCurrentAccount, AccessTokenCallback callback);
    bool validatePendingAccessToken(const PendingAccessToken &pending,
                                    const QString &channelID,
                                    QString &error) const;
    void eraseStoredAccount(const QString &channelID,
                            const QString &refreshToken);
    void beginCredentialCleanup(const QString &channelID,
                                const QString &refreshToken);
    void removeStoredCredential(const QString &channelID,
                                std::uint64_t cleanupGeneration,
                                int attempt = 0);
    void revokeRemovedToken(const QString &channelID, const QString &token,
                            std::uint64_t cleanupGeneration,
                            int attempt = 0);
    void recordCredentialCleanupFailure(const QString &channelID,
                                        const QString &error);
    void retryPendingCredentialCleanup();
    void clearPendingCredentialCleanup(const QString &channelID);
    bool isCredentialCleanupCurrent(
        const QString &channelID,
        std::uint64_t cleanupGeneration) const;
    void auditStoredMetadata();
    void refreshCurrentMetadataIfNeeded();
    void refreshMetadata(const std::shared_ptr<YouTubeAccount> &account);
    void fetchMetadata(const std::shared_ptr<YouTubeAccount> &account,
                       const QString &accessToken,
                       bool retriedUnauthorized = false);
    void applyMetadataRefresh(const std::shared_ptr<YouTubeAccount> &account,
                              const QString &handle,
                              const QString &displayName,
                              const QString &avatarUrl);
    void failMetadataRefresh(const std::shared_ptr<YouTubeAccount> &account,
                             const QString &reason);
    void purgeExpiredMetadata(const std::shared_ptr<YouTubeAccount> &account,
                              const QString &reason);
    void loadStoredCredentials(const std::shared_ptr<YouTubeAccount> &account);
    void observeAccount(const std::shared_ptr<YouTubeAccount> &account);
    void finishCredentialLoad(const QString &channelID,
                              const QString &error = {});
    void finishTokenRefresh(const QString &channelID,
                            ExpectedStr<QString> result);
    void finishTokenRefresh(const QString &channelID, std::uint64_t generation,
                            ExpectedStr<QString> result);
    bool isTokenRefreshCurrent(
        const QString &channelID, std::uint64_t generation,
        const std::shared_ptr<YouTubeAccount> &account) const;

    pajlada::Settings::Setting<QString> currentChannelID_{
        "/youtubeAccounts/current", ""};
    std::shared_ptr<YouTubeAccount> current_;
    std::shared_ptr<YouTubeAccount> anonymous_;
    pajlada::Signals::SignalHolder signals_;
    std::set<QString> credentialLoads_;
    std::map<QString, std::vector<PendingAccessToken>> credentialLoadWaiters_;
    std::map<QString, TokenRefreshState> tokenRefreshes_;
    std::map<QString, std::uint64_t> credentialCleanupGenerations_;
    std::set<QString> metadataRefreshes_;
    std::set<QString> accountsRequiringReconnect_;
    QTimer metadataPolicyTimer_;
    std::uint64_t nextTokenRefreshGeneration_ = 0;
    std::uint64_t nextCredentialCleanupGeneration_ = 0;
    std::uint64_t selectionGeneration_ = 0;
    bool loaded_ = false;
    std::shared_ptr<char> lifetimeToken_ = std::make_shared<char>();
};

}
