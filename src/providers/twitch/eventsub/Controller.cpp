// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/Controller.hpp"

#include "Application.hpp"
#include "util/PostToThread.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/Args.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/eventsub/Connection.hpp"
#include "util/QMagicEnum.hpp"
#include "util/RenameThread.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/certify/https_verification.hpp>
#include <twitch-eventsub-ws/session.hpp>

#include <QDateTime>
#include <QUrlQuery>
#include <algorithm>
#include <memory>
#include <utility>

namespace {

using namespace chatterino;

std::tuple<std::string, std::string, std::string> getEventSubHost()
{
#ifndef NDEBUG
    if (getApp()->getArgs().useLocalEventsub)
    {
        return {"localhost", "3012", "/ws"};
    }
#endif

    return {"eventsub.wss.twitch.tv", "443", "/ws"};
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoTwitchEventSub;

}  // namespace

namespace chatterino::eventsub {

using namespace std::literals::chrono_literals;

class QLogProxy : public lib::Logger
{
public:
    QLogProxy(const QLoggingCategory &loggingCategory_)
        : loggingCategory(loggingCategory_)
    {
    }

    void debug(std::string_view msg) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        qCDebug(this->loggingCategory).noquote() << msg;
#endif
    }

    void warn(std::string_view msg) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        qCWarning(this->loggingCategory).noquote() << msg;
#endif
    }

private:
    const QLoggingCategory &loggingCategory;
};

Controller::Controller()
    : logProxy(std::shared_ptr<lib::Logger>(new QLogProxy(LOG())))
    , userAgent(QStringLiteral("chatterino/%1 (%2)")
                    .arg(Version::instance().version(),
                         Version::instance().commitHash())
                    .toUtf8()
                    .toStdString())
    , ioContext(1)
    , work(boost::asio::make_work_guard(this->ioContext))
{
    std::tie(this->eventSubHost, this->eventSubPort, this->eventSubPath) =
        getEventSubHost();
    this->thread = std::make_unique<std::thread>([this] {
        this->ioContext.run();
    });
    renameThread(*this->thread, "C2EventSub");

    this->threadGuard = std::make_unique<ThreadGuard>(this->thread->get_id());
}

Controller::~Controller()
{
    this->setQuitting();
}

void Controller::removeRef(const SubscriptionRequest &request)
{
    if (this->quitting)
    {
        // We're quitting - we don't care to unsub
        return;
    }

    std::lock_guard lock(this->subscriptionsMutex);

    auto it = this->subscriptions.find(request);
    if (it == this->subscriptions.end() || it->second.refCount <= 0)
    {
        return;
    }
    if (--it->second.refCount == 0)
    {
        boost::asio::post(this->ioContext,
                          [this, request, generation = it->second.generation] {
                              this->unsubscribe(request, generation);
                          });
    }
}

void Controller::setQuitting()
{
    *this->alive = false;
    this->quitting = true;

    this->ioContext.stop();
    if (this->thread->joinable())
    {
        this->thread->join();
    }
}

SubscriptionHandle Controller::subscribe(const SubscriptionRequest &request)
{
    assert(!this->quitting &&
           "Subscribe cannot be called while we are quitting");

    assert(!request.ownerTwitchUserID.isEmpty() &&
           "Subscription requests must include a Twitch User ID");

    bool needToSubscribe = false;
    uint64_t generation = 0;

    {
        // TODO: Investigate if this scope can be done in boost::asio::post instead
        // Basically, if the SubscriptionHandle can be built & returned entirely without waiting for the subscriptionsMutex lock
        std::lock_guard lock(this->subscriptionsMutex);

        auto &subscription = this->subscriptions[request];

        assert(subscription.refCount >= 0);

        switch (subscription.state)
        {
            case Subscription::State::Unsubscribed:
                needToSubscribe = true;
                assert(subscription.refCount == 0 &&
                       "An unsubscribed subscription should have 0 references");
                break;

            case Subscription::State::Failed:
                qCDebug(LOG)
                    << "New subscription attempt to previously-failed request"
                    << request;
                needToSubscribe = true;
                subscription.moderatorRecoveryAttempted = false;
                break;

            case Subscription::State::Subscribing:
            case Subscription::State::Retrying:
            case Subscription::State::Subscribed:
            case Subscription::State::Unsubscribing:
                break;
        }

        if (needToSubscribe)
        {
            qCDebug(LOG) << "Set state to subscribing" << request;
            subscription.state = Subscription::State::Subscribing;
            subscription.generation = ++this->nextGeneration;

            // Ensure retries can work as expected since this is a fresh subscription
            subscription.backoff.reset();

            assert(subscription.retryTimer == nullptr &&
                   "A new subscription should not have a retry timer created");
        }

        generation = subscription.generation;
        subscription.refCount++;
        qCDebug(LOG) << "Added ref for" << request << subscription.refCount
                     << needToSubscribe
                     << "state:" << qmagicenum::enumName(subscription.state);
    }

    auto handle = std::make_unique<RawSubscriptionHandle>(request);

    if (needToSubscribe)
    {
        boost::asio::post(this->ioContext, [this, request, generation] {
            this->subscribe(request, generation);
        });
    }

    return handle;
}

void Controller::reconnectConnection(
    std::unique_ptr<lib::Listener> connection,
    const std::optional<std::string> &reconnectURL,
    const std::unordered_set<SubscriptionRequest> &subs)
{
    this->clearConnections();
    if (subs.empty())
    {
        return;
    }

    if (reconnectURL)
    {
        qCDebug(chatterinoTwitchEventSub) << "Using reconnect URL to reconnect";
        // this is epic
        QUrl url(QString::fromStdString(*reconnectURL));
        if (url.path().isEmpty())
        {
            url.setPath("/");
        }
        this->createConnection(url.host(QUrl::FullyEncoded).toStdString(),
                               std::to_string(url.port(443)),
                               url.toEncoded(QUrl::RemoveScheme |
                                             QUrl::RemoveAuthority |
                                             QUrl::RemoveFragment)
                                   .toStdString(),
                               std::move(connection));
        return;
    }

    std::lock_guard lock(this->subscriptionsMutex);
    for (const auto &request : subs)
    {
        auto it = this->subscriptions.find(request);
        if (it == this->subscriptions.end())
        {
            continue;
        }
        auto &subscription = it->second;
        subscription.retryTimer.reset();
        if (subscription.refCount == 0)
        {
            this->subscriptions.erase(it);
            continue;
        }
        subscription.connection.reset();
        subscription.subscriptionID.clear();
        subscription.inFlight = false;
        subscription.backoff.reset();
        subscription.state = Subscription::State::Subscribing;
        subscription.generation = ++this->nextGeneration;
        boost::asio::post(
            this->ioContext,
            [this, request, generation = subscription.generation] {
                this->subscribe(request, generation);
            });
    }
}

void Controller::unsubscribe(const SubscriptionRequest &request,
                             uint64_t generation)
{
    std::lock_guard lock(this->subscriptionsMutex);
    auto it = this->subscriptions.find(request);
    if (this->quitting || it == this->subscriptions.end() ||
        it->second.generation != generation || it->second.refCount != 0)
    {
        return;
    }
    auto &subscription = it->second;
    // No longer interested in this topic, ensure we don't have a retry in flight
    subscription.retryTimer.reset();


    if (subscription.inFlight ||
        subscription.state == Subscription::State::Unsubscribing)
    {
        return;
    }
    if (subscription.subscriptionID.isEmpty())
    {
        if (auto session = subscription.connection.lock())
        {
            if (auto *listener =
                    dynamic_cast<Connection *>(session->getListener()))
            {
                listener->markRequestUnsubscribed(request);
            }
        }
        this->subscriptions.erase(it);
        return;
    }
    subscription.state = Subscription::State::Unsubscribing;
    auto complete = [this, alive = this->alive, request, generation] {
        if (!*alive)
        {
            return;
        }
        boost::asio::post(this->ioContext, [this, request, generation] {
            this->markRequestUnsubscribed(request, generation);
        });
    };
    getHelix()->deleteEventSubSubscription(
        subscription.subscriptionID, complete,
        [this, alive = this->alive, request, generation](const auto &error) {
            qCWarning(LOG) << "EventSub unsubscribe failed:" << error;
            if (!*alive)
            {
                return;
            }
            boost::asio::post(this->ioContext, [this, request, generation] {
                std::shared_ptr<lib::Session> session;
                {
                    std::lock_guard lock(this->subscriptionsMutex);
                    auto it = this->subscriptions.find(request);
                    if (this->quitting || it == this->subscriptions.end() ||
                        it->second.generation != generation)
                    {
                        return;
                    }
                    session = it->second.connection.lock();
                }
                if (!session)
                {
                    this->markRequestUnsubscribed(request, generation);
                    return;
                }

                if (std::erase_if(this->connections, [&](const auto &weak) {
                        return weak.lock() == session;
                    }) != 0)
                {
                    session->close();
                }
            });
        });
}

void Controller::subscriptionRevoked(const QString &subscriptionID,
                                      const QString &status)
{
    if (this->quitting || subscriptionID.isEmpty())
    {
        return;
    }
    std::lock_guard lock(this->subscriptionsMutex);
    const auto broadcasterID = [](const SubscriptionRequest &request) {
        for (const auto &[key, value] : request.conditions)
        {
            if (key == "broadcaster_user_id")
                return value;
        }
        return QString{};
    };
    for (auto &[request, subscription] : this->subscriptions)
    {
        if (subscription.subscriptionID != subscriptionID)
        {
            continue;
        }
        const auto channelID = broadcasterID(request);
        const bool alreadyWarned =
            std::ranges::any_of(this->subscriptions, [&](const auto &entry) {
                return entry.first.ownerTwitchUserID ==
                           request.ownerTwitchUserID &&
                       broadcasterID(entry.first) == channelID &&
                       entry.second.revocationStatus == status &&
                       (status != "moderator_removed" ||
                        (entry.second.state == Subscription::State::Failed &&
                         !entry.second.moderatorRecoveryAttempted));
            });
        subscription.revocationStatus = status;
        if (auto connection = subscription.connection.lock())
        {
            if (auto *listener =
                    dynamic_cast<Connection *>(connection->getListener()))
            {
                listener->markRequestUnsubscribed(request);
            }
        }
        subscription.subscriptionID.clear();
        subscription.connection.reset();
        subscription.retryTimer.reset();
        subscription.inFlight = false;
        subscription.generation = ++this->nextGeneration;
        subscription.state = Subscription::State::Failed;
        if (subscription.refCount == 0)
        {
            boost::asio::post(
                this->ioContext,
                [this, request, generation = subscription.generation] {
                    this->unsubscribe(request, generation);
                });
            return;
        }
        if (alreadyWarned && status != "authorization_revoked")
            return;
        postToThread([this, alive = this->alive, request,
                      generation = subscription.generation,
                      userID = request.ownerTwitchUserID, channelID, status] {
            if (!*alive)
                return;
            if (auto *app = tryGetApp(); app && !isAppAboutToQuit())
            {
                if (status == "moderator_removed")
                {
                    this->checkModeratorAccess(request, generation);
                }
                else if (status == "authorization_revoked")
                {
                    app->getAccounts()->twitch.validateCurrentAccount(userID);
                }
                else
                {
                    const auto account =
                        app->getAccounts()->twitch.getCurrent();
                    if (account->isAnon() || account->getUserId() != userID)
                        return;
                    auto channel =
                        app->getTwitch()->getChannelOrEmptyByID(channelID);
                    const auto message =
                        QStringLiteral(
                            "Twitch stopped sending some live updates "
                            "(%1). Some features may stop working.")
                            .arg(status);
                    if (!channel->isEmpty())
                        channel->addSystemMessage(message);
                    else if (channelID.isEmpty())
                        app->getTwitch()->addGlobalSystemMessage(message);
                }
            }
        });
        return;
    }
}

void Controller::checkModeratorAccess(const SubscriptionRequest &request,
                                      uint64_t generation)
{
    auto *app = tryGetApp();
    if (!app || this->quitting || isAppAboutToQuit())
        return;
    auto account = app->getAccounts()->twitch.getCurrent();
    if (account->isAnon() || account->getUserId() != request.ownerTwitchUserID)
        return;
    QString channelID;
    for (const auto &[key, value] : request.conditions)
        if (key == "broadcaster_user_id")
            channelID = value;
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        app->getTwitch()->getChannelOrEmptyByID(channelID));
    if (!channel || channelID.isEmpty())
        return;
    const auto token = account->getOAuthToken();
    const auto checkedAt = QDateTime::currentDateTimeUtc();
    const auto roleRevision = channel->moderatorStatusRevision();
    auto isCurrent = [this, alive = this->alive, account, token, request,
                      generation, weak = channel->weakFromThis()] {
        if (!*alive || !tryGetApp() || isAppAboutToQuit() ||
            getApp()->getAccounts()->twitch.getCurrent() != account ||
            account->getOAuthToken() != token)
            return std::shared_ptr<TwitchChannel>{};
        auto channel = weak.lock();
        if (!channel)
            return channel;
        std::lock_guard lock(this->subscriptionsMutex);
        const auto it = this->subscriptions.find(request);
        return it != this->subscriptions.end() &&
                       it->second.generation == generation &&
                       it->second.refCount != 0 &&
                       it->second.state == Subscription::State::Failed
                   ? channel
                   : std::shared_ptr<TwitchChannel>{};
    };
    auto apply = [this, account, channelID, checkedAt, roleRevision,
                  isCurrent](bool moderator) {
        auto channel = isCurrent();
        if (!channel || (channel->moderatorStatusRevision() != roleRevision &&
                         channel->isMod() != moderator))
            return;
        if (moderator)
        {
            std::lock_guard lock(this->subscriptionsMutex);
            for (auto &[candidate, subscription] : this->subscriptions)
            {
                if (candidate.ownerTwitchUserID != account->getUserId() ||
                    subscription.state != Subscription::State::Failed ||
                    subscription.refCount == 0 ||
                    subscription.revocationStatus != "moderator_removed" ||
                    subscription.moderatorRecoveryAttempted ||
                    !std::ranges::any_of(
                        candidate.conditions, [&](const auto &condition) {
                            return condition.first == "broadcaster_user_id" &&
                                   condition.second == channelID;
                        }))
                    continue;
                subscription.moderatorRecoveryAttempted = true;
                subscription.state = Subscription::State::Subscribing;
                subscription.backoff.reset();
                subscription.generation = ++this->nextGeneration;
                boost::asio::post(
                    this->ioContext,
                    [this, candidate, generation = subscription.generation] {
                        this->subscribe(candidate, generation);
                    });
            }
        }
        channel->setKnownModeratorStatus(account->getUserName(), moderator,
                                         checkedAt);
    };
    QUrl url(QStringLiteral("https://api.twitch.tv/helix/chat/chatters"));
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", channelID);
    query.addQueryItem("moderator_id", account->getUserId());
    query.addQueryItem("first", "1");
    url.setQuery(query);
    NetworkRequest(url, NetworkRequestType::Get)
        .timeout(5000)
        .maximumResponseSize(64 * 1024)
        .followRedirects(false)
        .header("Client-ID", account->getOAuthClient())
        .header("Authorization", "Bearer " + token)
        .onSuccess([apply](const auto &result) {
            if (result.status() == 200 && result.parseJson()["data"].isArray())
                apply(true);
        })
        .onError([apply, isCurrent](const auto &result) {
            if (result.status() == 403)
                apply(false);
            else if (result.status() == 401 && isCurrent())
                getApp()->getAccounts()->twitch.validateCurrentAccount();
        })
        .execute();
}

void Controller::debug()
{
    std::lock_guard g(this->subscriptionsMutex);
    for (const auto &[request, subscription] : this->subscriptions)
    {
        QString sessionID;
        auto connection = subscription.connection.lock();
        if (connection)
        {
            auto *connection2 =
                dynamic_cast<Connection *>(connection->getListener());
            if (connection2)
            {
                sessionID = connection2->getSessionID();
            }
            else
            {
                sessionID = "BAD";
            }
        }
        else
        {
            sessionID = "DEAD";
        }

        qCInfo(LOG).noquote().nospace()
            << request << " (" << qmagicenum::enumName(subscription.state)
            << ") -> " << sessionID;
    }

    boost::asio::post(this->ioContext, [this] {
        for (const auto &weakConnection : this->connections)
        {
            auto connection = weakConnection.lock();
            if (connection)
            {
                auto *connection2 =
                    dynamic_cast<Connection *>(connection->getListener());
                if (connection2)
                {
                    // qCInfo(LOG)
                    //     << "Connected to" << connection2->getSessionID();
                    connection2->debug();
                }
            }
            else
            {
                qCInfo(LOG) << "Dead connection";
            }
        }
    });
}

void Controller::subscribe(const SubscriptionRequest &request,
                           uint64_t generation, bool isRetry)
{
    if (this->quitting)
    {
        return;
    }
    // 1. Flush dead connections (maybe this should not be done here)
    // TODO: implement

    {
        std::lock_guard lock(this->subscriptionsMutex);
        auto it = this->subscriptions.find(request);
        if (it == this->subscriptions.end() ||
            it->second.generation != generation || it->second.refCount == 0 ||
            it->second.inFlight)
        {
            return;
        }
        auto &subscription = it->second;
        if (isRetry)
        {
            qCDebug(LOG) << "Retry subscribe request for" << request;

            assert(subscription.retryTimer != nullptr);

            subscription.retryTimer.reset();
        }
        else
        {
            qCDebug(LOG) << "New subscribe request for" << request;
        }

        assert(subscription.retryTimer == nullptr);
    }

    uint32_t openButNotReadyConnections = 0;

    // 2. Check if any currently open connection can handle this subscription
    auto viableConnection = this->getViableConnection(
        request.ownerTwitchUserID, openButNotReadyConnections);

    if (viableConnection.has_value())
    {
        const auto &connection = *viableConnection;
        auto *listener = dynamic_cast<Connection *>(connection->getListener());

        assert(listener != nullptr && "Something goofy has gone wrong, Session "
                                      "listener must be our Connection type");

        {
            std::lock_guard lock(this->subscriptionsMutex);
            auto it = this->subscriptions.find(request);
            if (it == this->subscriptions.end() ||
                it->second.generation != generation || it->second.refCount == 0)
            {
                return;
            }
            it->second.connection = connection;
            it->second.inFlight = true;
            listener->markRequestSubscribed(request);
        }
        qCDebug(LOG) << "Make helix request for" << request;
        getHelix()->createEventSubSubscription(
            request, listener->getSessionID(),
            [this, alive = this->alive, request, generation](const auto &res) {
                if (!*alive)
                {
                    return;
                }
                boost::asio::post(this->ioContext, [this, request, generation,
                                                    id = res.subscriptionID] {
                    this->markRequestSubscribed(request, generation, id);
                });
            },
            [this, alive = this->alive, request, generation](
                const auto &error, const auto &errorString) {
                if (!*alive)
                {
                    return;
                }
                boost::asio::post(this->ioContext, [this, request, generation,
                                                    error, errorString] {
                    bool wanted = false;
                    {
                        std::lock_guard lock(this->subscriptionsMutex);
                        auto it = this->subscriptions.find(request);
                        if (it == this->subscriptions.end() ||
                            it->second.generation != generation)
                        {
                            return;
                        }
                        it->second.inFlight = false;
                        wanted = it->second.refCount != 0;
                    }
                    using Error = HelixCreateEventSubSubscriptionError;

                    if (wanted && (error == Error::Unauthorized ||
                                   error == Error::Forbidden))
                    {
                        postToThread([userID = request.ownerTwitchUserID] {
                            if (auto *app = tryGetApp();
                                app && !isAppAboutToQuit())
                            {
                                app->getAccounts()
                                    ->twitch.validateCurrentAccount(userID);
                            }
                        });
                    }

                    bool retry = false;
                    switch (error)
                    {
                        case Error::BadRequest:
                            qCDebug(LOG)
                                << "Bad request" << errorString << request;
                            break;

                        case Error::Unauthorized:
                            qCDebug(LOG)
                                << "Unauthorized" << errorString << request;
                            break;

                        case Error::Forbidden:
                            qCDebug(LOG)
                                << "Forbidden" << errorString << request;
                            break;

                        case Error::Conflict:
                            // This session ID is already subscribed to this request, some logic of ours is wrong
                            qCWarning(LOG)
                                << "Conflict" << errorString << request;
                            break;

                        case Error::Ratelimited:
                            qCDebug(LOG)
                                << "Ratelimited" << errorString << request;
                            retry = true;
                            break;

                        case Error::NoSession:
                            qCDebug(LOG) << "Session expired, retrying"
                                         << errorString << request;
                            retry = true;
                            break;

                        case Error::Forwarded:
                        default:
                            qCWarning(LOG) << "Unhandled error, retrying "
                                              "subscription"
                                           << errorString << request;
                            retry = true;
                            break;
                    }

                    if (retry)
                    {
                        this->retrySubscription(request, generation);
                    }
                    else
                    {
                        this->markRequestFailed(request, generation);
                    }
                });
            });

        return;
    }

    if (openButNotReadyConnections == 0)
    {
        // No connection was available to handle this subscription request, create a new connection
        this->createConnection();
    }
    else if (openButNotReadyConnections > 1)
    {
        // There should only ever be 0 or 1
        qCWarning(LOG) << "We have" << openButNotReadyConnections
                       << "open but no ready connections";
    }

    this->retrySubscription(request, generation);
}

std::optional<std::shared_ptr<lib::Session>> Controller::getViableConnection(
    const QString &ownerTwitchUserID, uint32_t &openButNotReadyConnections)
{
    for (const auto &weakConnection : this->connections)
    {
        auto connection = weakConnection.lock();
        if (!connection)
        {
            // TODO: remove it here?
            continue;
        }

        auto *listener = dynamic_cast<Connection *>(connection->getListener());

        if (!listener)
        {
            continue;  // dead connection
        }

        if (listener->getSessionID().isEmpty())
        {
            // This connection is open but it's not ready (i.e. no welcome has been posted yet)
            ++openButNotReadyConnections;
            continue;
        }

        if (!listener->canHandleSubscriptionFrom(ownerTwitchUserID))
        {
            continue;  // Connection is active with another Twitch User's subscriptions
        }

        // TODO: Check if this listener has room for another subscription

        return connection;
    }

    return {};
}

void Controller::createConnection()
{
    this->createConnection(this->eventSubHost, this->eventSubPort,
                           this->eventSubPath, std::make_unique<Connection>());
}

void Controller::createConnection(std::string host, std::string port,
                                  std::string path,
                                  std::unique_ptr<lib::Listener> listener)
{
    qCDebug(LOG) << "Create EventSub connection";

    try
    {
        boost::asio::ssl::context sslContext{
            boost::asio::ssl::context::tlsv12_client};

#ifndef NDEBUG
        if (!getApp()->getArgs().useLocalEventsub)
#endif
        {
            sslContext.set_verify_mode(
                boost::asio::ssl::verify_peer |
                boost::asio::ssl::verify_fail_if_no_peer_cert);
            sslContext.set_default_verify_paths();

            boost::certify::enable_native_https_server_verification(sslContext);
        }

        auto connection = std::make_shared<lib::Session>(
            this->ioContext, sslContext, std::move(listener), this->logProxy);

        this->registerConnection(connection);

        connection->run(std::move(host), std::move(port), std::move(path),
                        this->userAgent);
    }
    catch (std::exception &e)
    {
        qCWarning(LOG) << "Error in EventSub run thread" << e.what();
    }
}

void Controller::registerConnection(std::weak_ptr<lib::Session> &&connection)
{
    this->threadGuard->guard();

    if (auto session = connection.lock())
    {
        if (auto *listener = dynamic_cast<Connection *>(session->getListener()))
        {
            std::lock_guard lock(this->subscriptionsMutex);
            for (auto &[request, subscription] : this->subscriptions)
            {
                if (listener->isSubscribedTo(request))
                {
                    subscription.connection = session;
                }
            }
        }
    }
    this->connections.emplace_back(std::move(connection));
}

void Controller::retrySubscription(const SubscriptionRequest &request,
                                   uint64_t generation)
{
    if (this->quitting || isAppAboutToQuit())
    {
        qCDebug(LOG) << "retrySubscription, but app is quitting" << request;
        return;
    }

    std::lock_guard lock(this->subscriptionsMutex);

    auto it = this->subscriptions.find(request);
    if (it == this->subscriptions.end() || it->second.generation != generation)
    {
        return;
    }
    auto &subscription = it->second;

    if (subscription.refCount == 0)
    {
        qCDebug(LOG) << "No one is interested in this subscription anymore, "
                        "stop trying"
                     << request << "from state"
                     << qmagicenum::enumName(subscription.state);
        qCDebug(LOG) << "Set state to unsubscribed" << request;
        subscription.state = Subscription::State::Unsubscribed;

        if (auto session = subscription.connection.lock())
        {
            if (auto *listener =
                    dynamic_cast<Connection *>(session->getListener()))
            {
                listener->markRequestUnsubscribed(request);
            }
        }
        this->subscriptions.erase(it);

        return;
    }

    if (subscription.state != Subscription::State::Retrying)
    {
        qCDebug(LOG) << "Set state to retrying" << request;
        subscription.state = Subscription::State::Retrying;
    }

    // we don't need a strong RNG here
    // NOLINTNEXTLINE(cert-*)
    std::chrono::milliseconds jitter{std::rand() % 256};

    auto retryTimer =
        std::make_unique<boost::asio::system_timer>(this->ioContext);
    retryTimer->expires_after(subscription.backoff.next() + jitter);
    retryTimer->async_wait([this, request, generation](const auto &ec) {
        if (isAppAboutToQuit())
        {
            qCDebug(LOG)
                << "Retry was going to fire, but app is quitting so we won't."
                << request;
            return;
        }

        if (!ec)
        {
            qCDebug(LOG) << "Firing retry" << request;
            // The timer passed naturally
            this->subscribe(request, generation, true);
        }
        else
        {
            qCDebug(LOG) << "Retry timer for" << request << "was cancelled";
            // If we mark the request as unsubscribed here, and we had to actually unsubscribe,
            // if an actual unsubscribe happens then it'll go from Unsubscribed -> ACTUALLY unsubscribed
            //
            // We might still need to update the state here, but we might want some new state for that
            // e.g. RetryCancelled or something
            // this->markRequestUnsubscribed(request);
        }
    });

    assert(subscription.retryTimer == nullptr &&
           "Timer should not already be set");

    subscription.retryTimer = std::move(retryTimer);
}

void Controller::markRequestSubscribed(const SubscriptionRequest &request,
                                       uint64_t generation,
                                       const QString &subscriptionID)
{
    if (this->quitting)
    {
        return;
    }
    bool unwanted = false;
    {
        std::lock_guard lock(this->subscriptionsMutex);
        auto it = this->subscriptions.find(request);
        if (it == this->subscriptions.end() ||
            it->second.generation != generation)
        {

            getHelix()->deleteEventSubSubscription(
                subscriptionID, [] {}, [](const auto &) {});
            return;
        }
        auto &subscription = it->second;
        subscription.inFlight = false;
        subscription.subscriptionID = subscriptionID;
        subscription.revocationStatus.clear();
        qCDebug(LOG) << "Set state to subscribed" << request;
        subscription.state = Subscription::State::Subscribed;
        subscription.backoff.reset();
        unwanted = subscription.refCount == 0;
    }
    if (unwanted)
    {
        this->unsubscribe(request, generation);
    }
}

void Controller::markRequestFailed(const SubscriptionRequest &request,
                                   uint64_t generation)
{
    std::lock_guard lock(this->subscriptionsMutex);
    auto it = this->subscriptions.find(request);
    if (this->quitting || it == this->subscriptions.end() ||
        it->second.generation != generation)
    {
        return;
    }
    auto &subscription = it->second;
    if (auto session = subscription.connection.lock())
    {
        if (auto *listener = dynamic_cast<Connection *>(session->getListener()))
        {
            listener->markRequestUnsubscribed(request);
        }
    }
    subscription.connection.reset();
    subscription.inFlight = false;
    qCWarning(LOG) << "Request" << request << "marked as failed";
    qCDebug(LOG) << "Set state to failed" << request;
    subscription.state = Subscription::State::Failed;
    if (subscription.refCount == 0)
    {
        this->subscriptions.erase(it);
    }
}

void Controller::markRequestUnsubscribed(const SubscriptionRequest &request,
                                         uint64_t generation)
{
    std::lock_guard lock(this->subscriptionsMutex);
    auto it = this->subscriptions.find(request);
    if (this->quitting || it == this->subscriptions.end() ||
        it->second.generation != generation)
    {
        return;
    }
    auto &subscription = it->second;

    qCDebug(LOG) << "Request" << request << "marked as unsubscribed from state"
                 << qmagicenum::enumName(subscription.state);

    if (auto session = subscription.connection.lock())
    {
        if (auto *listener = dynamic_cast<Connection *>(session->getListener()))
        {
            listener->markRequestUnsubscribed(request);
        }
    }
    subscription.connection.reset();
    subscription.subscriptionID.clear();
    subscription.backoff.reset();
    if (subscription.refCount == 0)
    {
        this->subscriptions.erase(it);
        return;
    }
    // someone subscribed in the meantime
    subscription.state = Subscription::State::Subscribing;
    subscription.generation = ++this->nextGeneration;
    boost::asio::post(this->ioContext,
                      [this, request, generation = subscription.generation] {
                          this->subscribe(request, generation);
                      });
}

void Controller::clearConnections()
{
    std::erase_if(this->connections, [](const auto &it) {
        auto conn = it.lock();
        return !conn || !conn->getListener();
    });
}

void DummyController::reconnectConnection(
    std::unique_ptr<lib::Listener> /* connection */,
    const std::optional<std::string> & /* reconnectURL */,
    const std::unordered_set<SubscriptionRequest> & /* subs */)
{
}

}  // namespace chatterino::eventsub
