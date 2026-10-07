// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "common/SignalVector.hpp"
#include "util/Expected.hpp"
#include "util/QStringHash.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/signals/signal.hpp>
#include <QString>
#include <QTimer>

#include <memory>
#include <mutex>
#include <vector>

namespace chatterino {

class TwitchAccount;
class AccountController;

extern const std::vector<QStringView> AUTH_SCOPES;

class TwitchAccountManager
{
    TwitchAccountManager();

public:
    struct UserData {
        QString username;
        QString userID;
        QString clientID;
        QString oauthToken;
    };

    std::shared_ptr<TwitchAccount> getCurrent();

    std::vector<QString> getUsernames() const;

    std::shared_ptr<TwitchAccount> findUserByUsername(
        const QString &username) const;
    bool userExists(const QString &username) const;

    void reloadUsers();
    void load();
    void validateCurrentAccount(const QString &eventSubUserID = {});

    bool isLoggedIn() const;

    pajlada::Settings::Setting<QString> currentUsername{"/accounts/current",
                                                        ""};

    pajlada::Signals::Signal<std::shared_ptr<TwitchAccount>,
                             std::shared_ptr<TwitchAccount>>
        currentUserAboutToChange;

    pajlada::Signals::NoArgSignal currentUserChanged;
    pajlada::Signals::NoArgSignal userListUpdated;
    pajlada::Signals::NoArgSignal currentUserNameChanged;

    SignalVector<std::shared_ptr<TwitchAccount>> accounts;

    pajlada::Signals::Signal<void *, ExpectedStr<void>> emotesReloaded;

private:
    enum class AddUserResponse {
        UserAlreadyExists,
        UserValuesUpdated,
        UserAdded,
    };
    AddUserResponse addUser(const UserData &data);
    bool removeUser(TwitchAccount *account);

    void showAccountWarning(const QString &text,
                            const QString &missingScopes = {});
    QTimer validationTimer_;
    int validationGeneration_ = 0;
    bool validationInFlight_ = false;
    bool eventSubAuthFailed_ = false;
    QString lastValidationWarning_;

    std::shared_ptr<TwitchAccount> currentUser_;

    std::shared_ptr<TwitchAccount> anonymousUser_;
    mutable std::mutex mutex_;

    friend class AccountController;
};

}
