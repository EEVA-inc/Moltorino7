// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/pronouns/Pronouns.hpp"

#include "common/QLogging.hpp"
#include "providers/pronouns/alejo/PronounsAlejoApi.hpp"
#include "providers/pronouns/UserPronouns.hpp"

#include <mutex>
#include <utility>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoPronouns;

}

namespace chatterino::pronouns {

Pronouns::Pronouns()
    : alejoApi(std::make_unique<AlejoApi>())
    , fetcher_([this](const QString &username, FetchCallback onDone) {
        this->alejoApi->fetch(username, onDone);
    })
    , nowProvider_([] { return Clock::now(); })
{
}

Pronouns::Pronouns(Fetcher fetcher, NowProvider nowProvider)
    : fetcher_(std::move(fetcher))
    , nowProvider_(std::move(nowProvider))
{
    if (!this->nowProvider_)
    {
        this->nowProvider_ = [] { return Clock::now(); };
    }
}

void Pronouns::getUserPronoun(
    const QString &username,
    const std::function<void(UserPronouns)> &callbackSuccess,
    const std::function<void()> &callbackFail)
{
    const auto normalizedUsername = normalizeUsername(username);
    if (normalizedUsername.isEmpty())
    {
        callbackFail();
        return;
    }
    if (!this->fetcher_)
    {
        callbackFail();
        return;
    }

    std::optional<UserPronouns> cachedPronoun;
    bool failureBackoffActive = false;
    {
        std::unique_lock lock(this->mutex);
        const auto now = this->nowProvider_();
        cachedPronoun =
            this->getCachedUserPronounLocked(normalizedUsername, now);
        if (!cachedPronoun.has_value())
        {
            auto failure = this->failureBackoffs_.find(normalizedUsername);
            if (failure != this->failureBackoffs_.end())
            {
                if (failure->second > now)
                {
                    failureBackoffActive = true;
                }
                else
                {
                    this->failureBackoffs_.erase(failure);
                }
            }

            if (!failureBackoffActive)
            {
                auto pending = this->pending_.find(normalizedUsername);
                if (pending != this->pending_.end())
                {
                    pending->second.successCallbacks.push_back(
                        callbackSuccess);
                    pending->second.failureCallbacks.push_back(callbackFail);
                    return;
                }

                PendingRequest request;
                request.successCallbacks.push_back(callbackSuccess);
                request.failureCallbacks.push_back(callbackFail);
                this->pending_.emplace(normalizedUsername,
                                       std::move(request));
            }
        }
    }

    if (cachedPronoun.has_value())
    {
        callbackSuccess(*cachedPronoun);
        return;
    }
    if (failureBackoffActive)
    {
        callbackFail();
        return;
    }

    this->fetcher_(
        normalizedUsername,
        [this, normalizedUsername](std::optional<UserPronouns> userPronoun) {
            std::vector<SuccessCallback> successCallbacks;
            std::vector<FailureCallback> failureCallbacks;
            const auto now = this->nowProvider_();
            {
                std::unique_lock lock(this->mutex);
                auto pending = this->pending_.find(normalizedUsername);
                if (pending == this->pending_.end())
                {
                    return;
                }

                successCallbacks =
                    std::move(pending->second.successCallbacks);
                failureCallbacks =
                    std::move(pending->second.failureCallbacks);
                this->pending_.erase(pending);

                if (userPronoun.has_value())
                {
                    qCDebug(LOG)
                        << "Caching pronoun" << userPronoun->format()
                        << "for user" << normalizedUsername;
                    this->cacheUserPronounLocked(normalizedUsername,
                                                 *userPronoun, now);
                    this->failureBackoffs_.erase(normalizedUsername);
                }
                else
                {
                    if (this->failureBackoffs_.size() >=
                        MAX_FAILURE_BACKOFFS)
                    {
                        for (auto it = this->failureBackoffs_.begin();
                             it != this->failureBackoffs_.end();)
                        {
                            if (it->second <= now)
                            {
                                it = this->failureBackoffs_.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                        }
                    }
                    if (this->failureBackoffs_.size() >=
                        MAX_FAILURE_BACKOFFS)
                    {
                        this->failureBackoffs_.erase(
                            this->failureBackoffs_.begin());
                    }
                    this->failureBackoffs_[normalizedUsername] =
                        now + FAILURE_BACKOFF;
                }
            }

            if (userPronoun.has_value())
            {
                for (const auto &callback : successCallbacks)
                {
                    if (callback)
                    {
                        callback(*userPronoun);
                    }
                }
                return;
            }

            for (const auto &callback : failureCallbacks)
            {
                if (callback)
                {
                    callback();
                }
            }
        });
}

std::optional<UserPronouns> Pronouns::getCachedUserPronoun(
    const QString &username)
{
    const auto normalizedUsername = normalizeUsername(username);
    if (normalizedUsername.isEmpty())
    {
        return {};
    }

    std::unique_lock lock(this->mutex);
    return this->getCachedUserPronounLocked(normalizedUsername,
                                            this->nowProvider_());
}

QString Pronouns::normalizeUsername(const QString &username)
{
    return username.trimmed().toLower();
}

std::optional<UserPronouns> Pronouns::getCachedUserPronounLocked(
    const QString &username, Clock::time_point now)
{
    auto it = this->saved.find(username);
    if (it == this->saved.end())
    {
        return {};
    }

    if (it->second.expiresAt <= now)
    {
        this->cacheOrder_.erase(it->second.order);
        this->saved.erase(it);
        return {};
    }

    this->cacheOrder_.splice(this->cacheOrder_.begin(), this->cacheOrder_,
                             it->second.order);
    it->second.order = this->cacheOrder_.begin();
    return it->second.pronouns;
}

void Pronouns::cacheUserPronounLocked(const QString &username,
                                      const UserPronouns &pronouns,
                                      Clock::time_point now)
{
    auto existing = this->saved.find(username);
    if (existing != this->saved.end())
    {
        this->cacheOrder_.erase(existing->second.order);
        this->saved.erase(existing);
    }

    this->cacheOrder_.push_front(username);
    this->saved.emplace(
        username,
        CacheEntry{pronouns, now + CACHE_TTL, this->cacheOrder_.begin()});

    while (this->saved.size() > MAX_CACHE_ENTRIES)
    {
        const auto &oldest = this->cacheOrder_.back();
        this->saved.erase(oldest);
        this->cacheOrder_.pop_back();
    }
}

}
