// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/pronouns/alejo/PronounsAlejoApi.hpp"
#include "providers/pronouns/UserPronouns.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace chatterino::pronouns {

class Pronouns
{
public:
    using Clock = std::chrono::steady_clock;
    using FetchCallback = std::function<void(std::optional<UserPronouns>)>;
    using Fetcher = std::function<void(const QString &, FetchCallback)>;
    using NowProvider = std::function<Clock::time_point()>;

    Pronouns();
    explicit Pronouns(
        Fetcher fetcher,
        NowProvider nowProvider = [] { return Clock::now(); });

    void getUserPronoun(
        const QString &username,
        const std::function<void(UserPronouns)> &callbackSuccess,
        const std::function<void()> &callbackFail);

    std::optional<UserPronouns> getCachedUserPronoun(const QString &username);

private:
    using SuccessCallback = std::function<void(UserPronouns)>;
    using FailureCallback = std::function<void()>;

    struct CacheEntry {
        UserPronouns pronouns;
        Clock::time_point expiresAt;
        std::list<QString>::iterator order;
    };

    struct PendingRequest {
        std::vector<SuccessCallback> successCallbacks;
        std::vector<FailureCallback> failureCallbacks;
    };

    static QString normalizeUsername(const QString &username);
    std::optional<UserPronouns> getCachedUserPronounLocked(
        const QString &username, Clock::time_point now);
    void cacheUserPronounLocked(const QString &username,
                                const UserPronouns &pronouns,
                                Clock::time_point now);

    static constexpr size_t MAX_CACHE_ENTRIES = 5000;
    static constexpr size_t MAX_FAILURE_BACKOFFS = 5000;
    static constexpr auto CACHE_TTL = std::chrono::hours(1);
    static constexpr auto FAILURE_BACKOFF = std::chrono::seconds(30);

    std::mutex mutex;

    std::unordered_map<QString, CacheEntry> saved;
    std::list<QString> cacheOrder_;
    std::unordered_map<QString, PendingRequest> pending_;
    std::unordered_map<QString, Clock::time_point> failureBackoffs_;

    std::unique_ptr<AlejoApi> alejoApi;
    Fetcher fetcher_;
    NowProvider nowProvider_;
};

}
