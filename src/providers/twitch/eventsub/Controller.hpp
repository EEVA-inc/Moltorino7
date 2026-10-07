// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/eventsub/SubscriptionHandle.hpp"
#include "providers/twitch/eventsub/SubscriptionRequest.hpp"
#include "twitch-eventsub-ws/logger.hpp"
#include "twitch-eventsub-ws/session.hpp"
#include "util/ExponentialBackoff.hpp"
#include "util/ThreadGuard.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/functional/hash.hpp>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

namespace chatterino::eventsub {

class IController
{
public:
    virtual ~IController() = default;

    virtual void removeRef(const SubscriptionRequest &request) = 0;

    virtual void setQuitting() = 0;

    [[nodiscard]] virtual SubscriptionHandle subscribe(
        const SubscriptionRequest &request) = 0;

    virtual void reconnectConnection(
        std::unique_ptr<lib::Listener> connection,
        const std::optional<std::string> &reconnectURL,
        const std::unordered_set<SubscriptionRequest> &subs) = 0;

    virtual void debug() = 0;
    virtual void subscriptionRevoked(const QString &subscriptionID,
                                     const QString &status) = 0;
};

class Controller : public IController
{
public:
    Controller();
    ~Controller() override;

    void removeRef(const SubscriptionRequest &request) override;

    void setQuitting() override;

    [[nodiscard]] SubscriptionHandle subscribe(
        const SubscriptionRequest &request) override;

    void reconnectConnection(
        std::unique_ptr<lib::Listener> connection,
        const std::optional<std::string> &reconnectURL,
        const std::unordered_set<SubscriptionRequest> &subs) override;

    void debug() override;
    void subscriptionRevoked(const QString &subscriptionID,
                             const QString &status) override;

private:
    void subscribe(const SubscriptionRequest &request, uint64_t generation,
                   bool isRetry = false);
    void unsubscribe(const SubscriptionRequest &request, uint64_t generation);

    void createConnection();
    void createConnection(std::string host, std::string port, std::string path,
                          std::unique_ptr<lib::Listener> listener);
    void registerConnection(std::weak_ptr<lib::Session> &&connection);

    void retrySubscription(const SubscriptionRequest &request,
                           uint64_t generation);

    void markRequestSubscribed(const SubscriptionRequest &request,
                               uint64_t generation,
                               const QString &subscriptionID);

    void markRequestFailed(const SubscriptionRequest &request,
                           uint64_t generation);

    void markRequestUnsubscribed(const SubscriptionRequest &request,
                                 uint64_t generation);

    void clearConnections();
    void checkModeratorAccess(const SubscriptionRequest &request,
                              uint64_t generation);

    std::shared_ptr<lib::Logger> logProxy;

    const std::string userAgent;

    std::string eventSubHost;
    std::string eventSubPort;
    std::string eventSubPath;

    std::unique_ptr<std::thread> thread;
    std::unique_ptr<ThreadGuard> threadGuard;
    boost::asio::io_context ioContext;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work;

    std::vector<std::weak_ptr<lib::Session>> connections;

    [[nodiscard]] std::optional<std::shared_ptr<lib::Session>>
        getViableConnection(const QString &ownerTwitchUserID,
                            uint32_t &openButNotReadyConnections);

    struct Subscription {
        enum class State : uint8_t {

            Unsubscribed,

            Failed,

            Subscribing,

            Retrying,

            Subscribed,

            Unsubscribing,
        } state = State::Unsubscribed;

        int32_t refCount = 0;
        uint64_t generation = 0;
        bool inFlight = false;
        std::weak_ptr<lib::Session> connection;

        QString subscriptionID;
        QString revocationStatus;
        bool moderatorRecoveryAttempted = false;

        std::unique_ptr<boost::asio::system_timer> retryTimer;

        ExponentialBackoff<6> backoff{std::chrono::milliseconds{500}};
    };

    std::mutex subscriptionsMutex;
    std::unordered_map<SubscriptionRequest, Subscription> subscriptions;

    std::atomic<bool> quitting = false;
    uint64_t nextGeneration = 0;
    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);
};

class DummyController : public IController
{
public:
    ~DummyController() override = default;

    void removeRef(const SubscriptionRequest &request) override
    {
        (void)request;
    }

    void setQuitting() override
    {

    }

    [[nodiscard]] SubscriptionHandle subscribe(
        const SubscriptionRequest &request) override
    {
        (void)request;
        return {};
    }

    void reconnectConnection(
        std::unique_ptr<lib::Listener> connection,
        const std::optional<std::string> &reconnectURL,
        const std::unordered_set<SubscriptionRequest> &subs) override;

    void subscriptionRevoked(const QString &, const QString &) override
    {
    }

    void debug() override
    {
    }
};

}
