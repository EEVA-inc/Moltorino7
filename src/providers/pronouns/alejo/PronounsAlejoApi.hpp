// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/pronouns/UserPronouns.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino::pronouns {

class AlejoApi : public QObject
{
public:
    AlejoApi();

    void fetch(const QString &username,
               const std::function<void(std::optional<UserPronouns>)> &onDone);

private:
    struct PronounEntry {
        QString subject;
        QString object;
        bool singular = false;
    };

    struct PendingFetch {
        QString username;
        std::function<void(std::optional<UserPronouns>)> onDone;
    };

    void loadAvailablePronouns();
    void scheduleAvailablePronounsRetry();
    void drainPendingFetches();
    void startUserFetch(PendingFetch fetch);
    void finishUserFetch();
    void failPendingFetches();

    std::shared_mutex mutex;

    std::unordered_map<QString, PronounEntry> pronouns;
    std::deque<PendingFetch> pendingFetches_;
    size_t activeUserFetches_ = 0;
    std::atomic_bool pronounsLoadInFlight_{false};
    std::atomic_bool pronounsRetryScheduled_{false};
    std::atomic_bool pronounsCooldownActive_{false};
    std::atomic_int pronounsLoadRetryCount_{0};

    UserPronouns parsePronoun(const QJsonObject &object);
};

}
