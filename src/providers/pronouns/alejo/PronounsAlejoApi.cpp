// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/pronouns/alejo/PronounsAlejoApi.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/pronouns/UserPronouns.hpp"
#include "util/PostToThread.hpp"

#include <QStringBuilder>
#include <QTimer>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoPronouns;

constexpr QStringView API_URL = u"https://api.pronouns.alejo.io/v1";
constexpr QStringView API_USERS_ENDPOINT = u"/users";
constexpr QStringView API_PRONOUNS_ENDPOINT = u"/pronouns";
constexpr int API_TIMEOUT_MS = 5 * 1000;
constexpr int MAX_PRONOUN_LIST_RETRIES = 5;
constexpr int PRONOUN_LIST_COOLDOWN_MS = 5 * 60 * 1000;
constexpr size_t MAX_CONCURRENT_USER_FETCHES = 4;
constexpr size_t MAX_QUEUED_USER_FETCHES = 1000;

}

namespace chatterino::pronouns {

AlejoApi::AlejoApi()
{
    this->loadAvailablePronouns();
}

void AlejoApi::fetch(
    const QString &username,
    const std::function<void(std::optional<UserPronouns>)> &onDone)
{
    bool needsPronounList = false;
    bool queueFull = false;
    bool cooldownActive = false;
    {
        std::unique_lock lock(this->mutex);
        cooldownActive = this->pronounsCooldownActive_.load();
        if (!cooldownActive)
        {
            queueFull = this->pendingFetches_.size() >= MAX_QUEUED_USER_FETCHES;
            if (!queueFull)
            {
                needsPronounList = this->pronouns.empty();
                this->pendingFetches_.push_back({username, onDone});
            }
        }
    }

    if (cooldownActive)
    {
        onDone({});
        return;
    }
    if (queueFull)
    {
        qCWarning(LOG) << "Dropping pronoun request because the queue is full";
        onDone({});
        return;
    }

    if (needsPronounList)
    {
        this->loadAvailablePronouns();
    }
    this->drainPendingFetches();
}

void AlejoApi::drainPendingFetches()
{
    std::vector<PendingFetch> fetches;
    {
        std::unique_lock lock(this->mutex);
        if (this->pronouns.empty())
        {
            return;
        }

        while (this->activeUserFetches_ < MAX_CONCURRENT_USER_FETCHES &&
               !this->pendingFetches_.empty())
        {
            fetches.push_back(std::move(this->pendingFetches_.front()));
            this->pendingFetches_.pop_front();
            ++this->activeUserFetches_;
        }
    }

    for (auto &fetch : fetches)
    {
        this->startUserFetch(std::move(fetch));
    }
}

void AlejoApi::startUserFetch(PendingFetch fetch)
{
    qCDebug(LOG) << "Fetching pronouns from alejo.io for" << fetch.username;

    const auto username = fetch.username;
    QString endpoint = API_URL % API_USERS_ENDPOINT % "/" % username;
    auto onDone = std::make_shared<
        std::function<void(std::optional<UserPronouns>)>>(
        std::move(fetch.onDone));

    NetworkRequest(endpoint)
        .caller(this)
        .timeout(API_TIMEOUT_MS)
        .maximumResponseSize(64 * 1024)
        .onSuccess([this, username, onDone](const auto &result) {
            auto object = result.parseJson();
            auto parsed = this->parsePronoun(object);
            this->finishUserFetch();
            (*onDone)({parsed});
        })
        .onError([this, onDone, username](auto result) {
            auto status = result.status();
            if (status.has_value() && status == 404)
            {

                this->finishUserFetch();
                (*onDone)({UserPronouns()});
                return;
            }
            qCWarning(LOG) << "alejo.io returned " << status.value_or(-1)
                           << " when fetching pronouns for " << username;
            this->finishUserFetch();
            (*onDone)({});
        })
        .execute();
}

void AlejoApi::finishUserFetch()
{
    {
        std::unique_lock lock(this->mutex);
        if (this->activeUserFetches_ > 0)
        {
            --this->activeUserFetches_;
        }
    }
    this->drainPendingFetches();
}

void AlejoApi::failPendingFetches()
{
    std::deque<PendingFetch> pending;
    {
        std::unique_lock lock(this->mutex);
        pending.swap(this->pendingFetches_);
    }

    for (auto &fetch : pending)
    {
        fetch.onDone({});
    }
}

void AlejoApi::loadAvailablePronouns()
{
    if (this->pronounsRetryScheduled_)
    {
        return;
    }
    if (this->pronounsLoadInFlight_.exchange(true))
    {
        return;
    }

    qCDebug(LOG) << "Fetching available pronouns for alejo.io";

    QString endpoint = API_URL % API_PRONOUNS_ENDPOINT;

    NetworkRequest(endpoint)
        .caller(this)
        .timeout(API_TIMEOUT_MS)
        .maximumResponseSize(64 * 1024)
        .onSuccess([this](const auto &result) {
            auto root = result.parseJson();
            if (root.isEmpty())
            {
                this->failPendingFetches();
                this->scheduleAvailablePronounsRetry();
                this->pronounsLoadInFlight_ = false;
                return;
            }

            std::unordered_map<QString, PronounEntry> newPronouns;

            for (auto it = root.begin(); it != root.end(); ++it)
            {
                const auto &pronounId = it.key();
                const auto &pronounObj = it.value().toObject();

                const auto &subject = pronounObj["subject"].toString();
                const auto &object = pronounObj["object"].toString();
                const auto &singular = pronounObj["singular"].toBool();

                if (subject.isEmpty() || object.isEmpty())
                {
                    qCWarning(LOG) << "Pronoun" << pronounId
                                   << "was malformed:" << pronounObj;
                    continue;
                }

                newPronouns[pronounId] = {subject, object, singular};
            }

            if (newPronouns.empty())
            {
                qCWarning(LOG) << "alejo.io returned no usable pronouns";
                this->failPendingFetches();
                this->scheduleAvailablePronounsRetry();
                this->pronounsLoadInFlight_ = false;
                return;
            }

            {
                std::unique_lock lock(this->mutex);
                this->pronouns = std::move(newPronouns);
            }
            this->pronounsLoadRetryCount_ = 0;
            this->pronounsRetryScheduled_ = false;
            this->pronounsCooldownActive_ = false;
            this->pronounsLoadInFlight_ = false;
            this->drainPendingFetches();
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(LOG) << "Failed to load pronouns from alejo.io"
                           << result.formatError();
            this->failPendingFetches();
            this->scheduleAvailablePronounsRetry();
            this->pronounsLoadInFlight_ = false;
        })
        .execute();
}

void AlejoApi::scheduleAvailablePronounsRetry()
{
    if (this->pronounsRetryScheduled_.exchange(true))
    {
        return;
    }

    const auto retryCount = this->pronounsLoadRetryCount_.fetch_add(1);
    if (retryCount >= MAX_PRONOUN_LIST_RETRIES)
    {
        this->pronounsCooldownActive_ = true;

        this->failPendingFetches();
        postToThread([this] {
            QTimer::singleShot(PRONOUN_LIST_COOLDOWN_MS, this, [this] {
                if (isAppAboutToQuit())
                {
                    return;
                }
                this->pronounsCooldownActive_ = false;
                this->pronounsRetryScheduled_ = false;
                this->pronounsLoadRetryCount_ = 0;

                {
                    std::shared_lock lock(this->mutex);
                    if (!this->pronouns.empty())
                    {
                        return;
                    }
                }
                this->loadAvailablePronouns();
            });
        }, this);
        return;
    }

    const auto delayMs = (retryCount + 1) * 5000;
    postToThread([this, delayMs] {
        QTimer::singleShot(delayMs, this, [this] {
            if (isAppAboutToQuit())
            {
                return;
            }
            this->pronounsRetryScheduled_ = false;

            {
                std::shared_lock lock(this->mutex);
                if (!this->pronouns.empty())
                {
                    return;
                }
            }
            this->loadAvailablePronouns();
        });
    }, this);
}

UserPronouns AlejoApi::parsePronoun(const QJsonObject &object)
{
    const auto &primaryValue = object["pronoun_id"];
    const auto &alternateValue = object["alt_pronoun_id"];

    if (!primaryValue.isString())
    {
        return {};
    }

    std::shared_lock lock(this->mutex);
    if (this->pronouns.empty())
    {
        return {};
    }

    const auto primary = this->pronouns.find(primaryValue.toString());
    if (primary == this->pronouns.end())
    {
        return {};
    }

    if (alternateValue.isString())
    {
        const auto alternate =
            this->pronouns.find(alternateValue.toString());
        if (alternate != this->pronouns.end())
        {
            return {QString(primary->second.subject % "/" %
                            alternate->second.subject)};
        }
    }

    if (primary->second.singular)
    {
        return {primary->second.subject};
    }
    return {QString(primary->second.subject % "/" % primary->second.object)};
}

}
