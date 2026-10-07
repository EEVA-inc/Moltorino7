#pragma once

#include "common/SignalVector.hpp"
#include "providers/tiktok/TikTokAccount.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signal.hpp>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

namespace chatterino {

class TikTokAccountManager : public QObject
{
public:
    using SaveCallback = std::function<void(ExpectedStr<void>)>;
    struct CredentialStore {
        std::function<void(const QString &,
                           std::function<void(ExpectedStr<QString>)>)>
            read;
        std::function<void(const QString &, const QString &, SaveCallback)>
            write;
        std::function<void(const QString &, SaveCallback)> remove;
        std::function<bool()> saveSettings;
    };
    explicit TikTokAccountManager(CredentialStore store = {});
    ~TikTokAccountManager() override;
    void load();
    void addAccount(TikTokAccountData data, SaveCallback callback);
    void sessionUpdated(const std::shared_ptr<TikTokAccount> &account);
    void selectAccount(const QString &userID);
    std::shared_ptr<TikTokAccount> current() const;
    std::shared_ptr<TikTokAccount> findByUserID(const QString &userID) const;
    bool isLoggedIn() const;
    void requireLogin(const std::shared_ptr<TikTokAccount> &account);
    const QString &credentialError() const;

    SignalVector<std::shared_ptr<TikTokAccount>> accounts;
    pajlada::Signals::NoArgSignal currentChanged;
    pajlada::Signals::NoArgSignal credentialsChanged;
    pajlada::Signals::NoArgSignal userListUpdated;
    pajlada::Signals::NoArgSignal credentialErrorChanged;

private:
    struct Stored {
        QString userID;
        QString revision;
        int parts = 0;
    };
    struct Save;
    void saveAccount(TikTokAccountData data, SaveCallback callback,
                     std::shared_ptr<TikTokAccount> existing = {});
    void writeNext(const std::shared_ptr<Save> &save);
    void readNext(const std::shared_ptr<TikTokAccount> &account,
                  const Stored &stored, int index, QString data);
    void queueCleanup(const Stored &stored);
    void cleanupNext(const Stored &stored, int index);
    void removed(const std::shared_ptr<TikTokAccount> &account);
    void reportCredentialError(const QString &text);

    pajlada::Settings::Setting<QString> selected_{"/tiktokAccounts/current",
                                                  ""};
    QHash<QString, Stored> stored_;
    QHash<QString, quint64> generations_;
    QHash<QString, quint64> pendingLogins_;
    QHash<QString, QTimer *> refreshTimers_;
    QSet<QString> refreshingAccounts_;
    QSet<QString> dirtySessions_;
    CredentialStore store_;
    std::shared_ptr<TikTokAccount> current_;
    std::shared_ptr<TikTokAccount> anonymous_;
    QString credentialError_;
    bool loaded_ = false;
};
}
