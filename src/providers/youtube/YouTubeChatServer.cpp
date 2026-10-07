#include "providers/youtube/YouTubeChatServer.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeAccountManager.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeCredentials.hpp"
#include "singletons/Settings.hpp"

#include <QPointer>

#include <algorithm>
#include <chrono>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

YouTubeChatServer::YouTubeChatServer()
{
    this->resolveTimer_.setSingleShot(true);
    this->resolveTimer_.setInterval(750);
    QObject::connect(&this->resolveTimer_, &QTimer::timeout, this, [this] {
        this->processResolveQueue();
    });
    this->probeDispatchTimer_.setSingleShot(true);
    this->probeDispatchTimer_.setInterval(250);
    QObject::connect(&this->probeDispatchTimer_, &QTimer::timeout, this,
                     [this] {
                         this->processProbeQueue();
                     });
    this->cleanupTimer_.setInterval(std::chrono::minutes(1));
    QObject::connect(&this->cleanupTimer_, &QTimer::timeout, this, [this] {
        for (auto it = this->sessions_.begin(); it != this->sessions_.end();)
        {
            this->prune(it->second);
            if (it->second.channels.empty())
            {
                it = this->sessions_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = this->channels_.begin(); it != this->channels_.end();)
        {
            if (!it->second.expired())
            {
                ++it;
                continue;
            }
            const auto source = it->first;
            it = this->channels_.erase(it);
            if (!this->resolvesInFlight_.contains(source))
            {
                this->resolveGenerations_.erase(source);
                this->resolveFailures_.erase(source);
            }
            if (!this->probesInFlight_.contains(source))
            {
                this->probeStates_.erase(source);
            }
        }
    });

    this->offlineProbeTimer_.setInterval(std::chrono::minutes(2));
    QObject::connect(&this->offlineProbeTimer_, &QTimer::timeout, this, [this] {
        std::vector<std::shared_ptr<YouTubeChannel>> channels;
        channels.reserve(this->channels_.size());
        for (auto it = this->channels_.begin(); it != this->channels_.end();)
        {
            if (auto channel = it->second.lock())
            {
                channels.emplace_back(std::move(channel));
                ++it;
            }
            else
            {
                it = this->channels_.erase(it);
            }
        }
        for (const auto &channel : channels)
        {
            this->probe(channel);
        }
    });
}

YouTubeChatServer::~YouTubeChatServer()
{
    this->resolveTimer_.stop();
    this->probeDispatchTimer_.stop();
    this->cleanupTimer_.stop();
    this->offlineProbeTimer_.stop();
    this->sessions_.clear();
}

void YouTubeChatServer::initialize()
{
    this->initialized_ = true;
    this->cleanupTimer_.start();
    this->offlineProbeTimer_.start();
    if (!this->resolveQueue_.empty() && !this->resolveTimer_.isActive())
    {
        this->resolveTimer_.start();
    }
    this->scheduleProbeDispatch();
    auto *accounts = getApp()->getAccounts();
    const auto refreshAccountState = [this] {
        for (auto it = this->channels_.begin(); it != this->channels_.end();)
        {
            if (auto channel = it->second.lock())
            {
                channel->refreshAccountState();
                ++it;
            }
            else
            {
                it = this->channels_.erase(it);
            }
        }
    };
    this->signals_.managedConnect(accounts->youtube.currentChanged,
                                  refreshAccountState);
    this->signals_.managedConnect(accounts->youtube.credentialsChanged,
                                  refreshAccountState);
}

void YouTubeChatServer::restart(const std::shared_ptr<YouTubeChannel> &channel)
{
    if (!channel)
    {
        return;
    }

    std::vector<std::weak_ptr<YouTubeChannel>> attached;
    const auto liveChatID = channel->liveChatID();
    if (!liveChatID.isEmpty())
    {
        auto it = this->sessions_.find(liveChatID);
        if (it != this->sessions_.end())
        {
            this->prune(it->second);
            attached = it->second.channels;
            this->sessions_.erase(it);
        }
    }

    this->resolve(channel, true);
    for (const auto &weak : attached)
    {
        if (auto other = weak.lock(); other && other != channel)
        {
            this->resolve(other);
        }
    }
}

void YouTubeChatServer::refreshAccountRoles()
{
    for (auto it = this->channels_.begin(); it != this->channels_.end();)
    {
        if (auto channel = it->second.lock())
        {
            channel->refreshAccountState(true);
            ++it;
        }
        else
        {
            it = this->channels_.erase(it);
        }
    }
}

ChannelPtr YouTubeChatServer::getOrCreate(const QString &source)
{
    const auto normalized = YouTubeApi::normalizeSource(source);
    if (auto existing = this->findBySource(normalized))
    {
        return existing;
    }

    auto channel = std::make_shared<YouTubeChannel>(normalized, *this);
    this->channels_[normalized] = channel;

    this->resolve(channel);
    return channel;
}

void YouTubeChatServer::setActiveChannels(
    const std::vector<std::shared_ptr<YouTubeChannel>> &channels,
    QObject *owner)
{
    QObject::disconnect(this->activeOwnerDestroyed_);
    this->activeOwner_ = owner;
    if (owner != nullptr)
    {
        this->activeOwnerDestroyed_ =
            QObject::connect(owner, &QObject::destroyed, this, [this] {
                this->setActiveChannels({}, nullptr);
            });
    }

    std::unordered_map<QString, std::weak_ptr<YouTubeChannel>> next;
    next.reserve(channels.size());
    for (const auto &channel : channels)
    {
        if (channel)
        {
            next.insert_or_assign(channel->getName(), channel);
        }
    }
    this->activeChannels_ = std::move(next);

    for (const auto &active : this->activeChannels_)
    {
        auto channel = active.second.lock();
        if (!channel)
        {
            continue;
        }
        const auto liveChatID = channel->liveChatID();
        auto current = liveChatID.isEmpty() ? this->sessions_.end()
                                            : this->sessions_.find(liveChatID);
        if (current != this->sessions_.end())
        {
            this->attachChannel(current->second, channel);
        }
        else if (!channel->isLive())
        {
            this->probe(channel, true);
        }
        else
        {
            this->resolve(channel, true);
        }
    }
}

void YouTubeChatServer::probe(const std::shared_ptr<YouTubeChannel> &channel,
                              bool immediate)
{
    if (!YouTubeApi::isConfigured() || !channel || channel->isLive() ||
        !this->isRegistered(channel))
    {
        return;
    }

    const auto source = channel->getName();
    if (this->resolvesInFlight_.contains(source) ||
        std::ranges::any_of(this->resolveQueue_, [&channel](const auto &weak) {
            return weak.lock() == channel;
        }))
    {
        return;
    }
    if (this->probesInFlight_.contains(source))
    {
        return;
    }

    auto &state = this->probeStates_[source];
    const auto now = std::chrono::steady_clock::now();
    constexpr auto FOCUS_DEBOUNCE = std::chrono::seconds(10);
    if (immediate && state.lastStarted.time_since_epoch().count() != 0 &&
        now - state.lastStarted < FOCUS_DEBOUNCE)
    {
        return;
    }

    std::erase_if(this->probeQueue_, [&channel](const auto &weak) {
        return weak.expired() || weak.lock() == channel;
    });
    if (immediate)
    {
        this->probeQueue_.push_front(channel);

        if (this->initialized_ && this->probesInFlight_.empty())
        {
            this->probeDispatchTimer_.stop();
            this->processProbeQueue();
        }
        else
        {
            this->scheduleProbeDispatch();
        }
    }
    else
    {
        this->probeQueue_.push_back(channel);
        this->scheduleProbeDispatch();
    }
}

void YouTubeChatServer::clearActiveChannels(QObject *owner)
{
    if (owner == nullptr || this->activeOwner_ != owner)
    {
        return;
    }
    this->setActiveChannels({}, nullptr);
}

std::shared_ptr<YouTubeChannel> YouTubeChatServer::findBySource(
    const QString &source) const
{
    const auto normalized = YouTubeApi::normalizeSource(source);
    const auto it = this->channels_.find(normalized);
    return it == this->channels_.end() ? nullptr : it->second.lock();
}

bool YouTubeChatServer::hasLiveNotificationSibling(
    const YouTubeChannel &channel) const
{
    if (channel.channelID().isEmpty())
    {
        return false;
    }
    for (const auto &[source, weak] : this->channels_)
    {
        const auto other = weak.lock();
        if (!other || other.get() == &channel || !other->isLive() ||
            other->channelID() != channel.channelID())
        {
            continue;
        }
        bool sameBroadcast = true;
        if (!channel.videoID().isEmpty() && !other->videoID().isEmpty())
        {
            sameBroadcast = other->videoID() == channel.videoID();
        }
        else if (!channel.liveChatID().isEmpty() &&
                 !other->liveChatID().isEmpty())
        {
            sameBroadcast = other->liveChatID() == channel.liveChatID();
        }
        if (sameBroadcast &&
            (getSettings()->notificationOnAnyChannel ||
             getApp()->getNotifications()->isChannelNotified(
                 other->getName(), Platform::YouTube, other->channelID())))
        {
            return true;
        }
    }
    return false;
}

void YouTubeChatServer::resolve(const std::shared_ptr<YouTubeChannel> &channel,
                                bool immediate)
{
    if (!channel)
    {
        return;
    }
    if (!this->isRegistered(channel))
    {
        return;
    }
    const auto source = channel->getName();
    std::erase_if(this->probeQueue_, [&channel](const auto &weak) {
        return weak.expired() || weak.lock() == channel;
    });
    if (!immediate && this->resolvesInFlight_.contains(source))
    {
        return;
    }
    if (immediate && this->resolvesInFlight_.contains(source))
    {
        ++this->resolveGenerations_[source];
        this->resolvesInFlight_.erase(source);
    }
    std::erase_if(this->resolveQueue_, [&channel](const auto &weak) {
        return weak.expired() || weak.lock() == channel;
    });
    if (immediate)
    {
        this->resolveQueue_.push_front(channel);
        if (this->initialized_)
        {
            this->resolveTimer_.stop();
            this->processResolveQueue();
        }
    }
    else
    {
        this->resolveQueue_.push_back(channel);
        if (this->initialized_ && !this->resolveTimer_.isActive())
        {
            this->resolveTimer_.start();
        }
    }
}

void YouTubeChatServer::processResolveQueue()
{
    if (!this->initialized_)
    {
        return;
    }
    while (!this->resolveQueue_.empty())
    {
        auto weak = this->resolveQueue_.front();
        this->resolveQueue_.pop_front();
        auto channel = weak.lock();
        if (!channel || !this->isRegistered(channel))
        {
            continue;
        }
        const auto source = channel->getName();
        const auto generation = ++this->resolveGenerations_[source];
        this->resolvesInFlight_.insert(source);
        channel->beginResolving();
        YouTubeApi::resolveSource(
            source, [self = QPointer(this), weak, source,
                     generation](ExpectedStr<YouTubeResolvedChannel> result) {
                if (!self)
                {
                    return;
                }
                const auto generationIt =
                    self->resolveGenerations_.find(source);
                if (generationIt == self->resolveGenerations_.end() ||
                    generationIt->second != generation)
                {
                    return;
                }
                self->resolvesInFlight_.erase(source);
                auto current = weak.lock();
                if (!current)
                {
                    self->resolveGenerations_.erase(source);
                    self->resolveFailures_.erase(source);
                    return;
                }
                if (!self->isRegistered(current))
                {
                    return;
                }
                if (!result)
                {
                    current->applyResolutionError(result.error());
                    if (!YouTubeApi::isConfigured())
                    {
                        return;
                    }
                    auto &failures = self->resolveFailures_[source];
                    failures = std::min<std::uint8_t>(failures + 1, 4);
                    if (failures <= 3)
                    {
                        const auto delay = std::chrono::seconds(
                            1 << static_cast<unsigned>(failures));
                        self->scheduleResolve(current, delay);
                    }
                    return;
                }
                self->resolveFailures_.erase(source);
                self->attachResolved(current, *result);
            });
        break;
    }

    if (!this->resolveQueue_.empty())
    {
        this->resolveTimer_.start();
    }
}

void YouTubeChatServer::processProbeQueue()
{
    if (!this->initialized_ ||
        this->probesInFlight_.size() >= MAX_CONCURRENT_PROBES)
    {
        return;
    }
    while (!this->probeQueue_.empty())
    {
        auto weak = this->probeQueue_.front();
        this->probeQueue_.pop_front();
        auto channel = weak.lock();
        if (!channel || channel->isLive() || !this->isRegistered(channel))
        {
            continue;
        }

        const auto source = channel->getName();
        if (this->probesInFlight_.contains(source) ||
            this->resolvesInFlight_.contains(source) ||
            std::ranges::any_of(this->resolveQueue_,
                                [&channel](const auto &weak) {
                                    return weak.lock() == channel;
                                }))
        {
            continue;
        }

        auto &state = this->probeStates_[source];
        state.lastStarted = std::chrono::steady_clock::now();
        this->probesInFlight_.insert(source);
        YouTubeApi::probeLiveSource(
            source, [self = QPointer(this), weak,
                     source](ExpectedStr<YouTubeLiveProbe> result) {
                if (!self)
                {
                    return;
                }
                self->probesInFlight_.erase(source);
                self->scheduleProbeDispatch();
                auto current = weak.lock();
                if (!current || !self->isRegistered(current))
                {
                    self->probeStates_.erase(source);
                    return;
                }

                auto &state = self->probeStates_[source];
                if (result)
                {
                    state.consecutiveFailures = 0;
                    if (result->isLive && !current->isLive() &&
                        !self->resolvesInFlight_.contains(source) &&
                        std::ranges::none_of(self->resolveQueue_,
                                             [&current](const auto &queued) {
                                                 return queued.lock() ==
                                                        current;
                                             }))
                    {
                        self->resolve(current, true);
                    }
                    return;
                }

                state.consecutiveFailures =
                    std::min<std::uint8_t>(state.consecutiveFailures + 1, 3);
                const auto active = self->activeChannels_.find(source);
                if (active == self->activeChannels_.end() ||
                    active->second.expired() || state.consecutiveFailures < 3)
                {
                    return;
                }

                const auto now = std::chrono::steady_clock::now();
                constexpr auto SAFETY_FALLBACK_COOLDOWN = std::chrono::hours(6);
                if (state.lastSafetyFallback.time_since_epoch().count() != 0 &&
                    now - state.lastSafetyFallback < SAFETY_FALLBACK_COOLDOWN)
                {
                    return;
                }
                state.lastSafetyFallback = now;
                state.consecutiveFailures = 0;
                self->resolve(current, true);
            });
        break;
    }

    this->scheduleProbeDispatch();
}

void YouTubeChatServer::scheduleProbeDispatch()
{
    if (this->initialized_ && !this->probeQueue_.empty() &&
        this->probesInFlight_.size() < MAX_CONCURRENT_PROBES &&
        !this->probeDispatchTimer_.isActive())
    {
        this->probeDispatchTimer_.start();
    }
}

void YouTubeChatServer::attachResolved(
    const std::shared_ptr<YouTubeChannel> &channel,
    const YouTubeResolvedChannel &resolved)
{
    this->probeStates_[channel->getName()].consecutiveFailures = 0;
    const auto previousLiveChatID = channel->liveChatID();
    channel->applyResolved(resolved);
    if (!this->isRegistered(channel))
    {
        return;
    }
    if (!previousLiveChatID.isEmpty() &&
        previousLiveChatID != resolved.liveChatID)
    {
        auto previous = this->sessions_.find(previousLiveChatID);
        if (previous != this->sessions_.end())
        {
            std::erase_if(previous->second.channels,
                          [&channel](const auto &weak) {
                              return weak.expired() || weak.lock() == channel;
                          });
            if (previous->second.channels.empty())
            {
                this->sessions_.erase(previous);
            }
        }
    }
    if (resolved.isUpcoming &&
        resolved.scheduledStartTime > QDateTime::currentDateTimeUtc())
    {
        const auto secondsUntilStart =
            QDateTime::currentDateTimeUtc().secsTo(resolved.scheduledStartTime);

        this->scheduleResolve(channel, std::chrono::seconds(std::max<qint64>(
                                           30, secondsUntilStart + 15)));
    }

    if (resolved.liveChatID.isEmpty())
    {
        return;
    }

    constexpr std::size_t MAX_REPLAY_MESSAGES = 2'000;
    const auto replayLimit = std::min(
        sanitizeScrollbackLimit(getSettings()->scrollbackSplitLimit.getValue()),
        MAX_REPLAY_MESSAGES);
    auto entryIt =
        this->sessions_.try_emplace(resolved.liveChatID, replayLimit).first;
    auto &entry = entryIt->second;
    entry.videoID = resolved.videoID;
    entry.readTicket = resolved.readTicket;
    if (!entry.relayFallback && !entry.innertubeSession && !entry.relaySession)
    {
        entry.readTransport = resolved.readTransport;
    }
    this->attachChannel(entry, channel);
    if (entry.replay.size() != 0)
    {
        const auto identity = entry.deduper;
        channel->receiveMessages(entry.replay.snapshot(), true);
        entryIt = this->sessions_.find(resolved.liveChatID);
        if (entryIt == this->sessions_.end() ||
            entryIt->second.deduper != identity)
        {
            return;
        }
    }
    auto &currentEntry = entryIt->second;
    if (currentEntry.innertubeSession || currentEntry.relaySession)
    {
        return;
    }

    if (youtube::credentials::directReadEnabled() &&
        currentEntry.readTransport == YouTubeReadTransport::Innertube)
    {
        this->startInnertubeSession(resolved.videoID, resolved.liveChatID,
                                    currentEntry);
        return;
    }

    this->startRelaySession(resolved.liveChatID, currentEntry.readTicket,
                            currentEntry);
}

void YouTubeChatServer::attachChannel(
    SessionEntry &entry, const std::shared_ptr<YouTubeChannel> &channel)
{
    this->prune(entry);
    const auto found =
        std::ranges::any_of(entry.channels, [&channel](const auto &weak) {
            return weak.lock() == channel;
        });
    if (!found)
    {
        entry.channels.emplace_back(channel);
    }
}

void YouTubeChatServer::startInnertubeSession(const QString &videoID,
                                              const QString &liveChatID,
                                              SessionEntry &entry)
{
    this->prune(entry);
    if (entry.channels.empty() || videoID.isEmpty())
    {
        return;
    }
    entry.innertubeSession = std::make_shared<YouTubeInnertubeSession>(
        videoID, liveChatID,
        [self = QPointer(this), liveChatID](
            std::vector<YouTubeMessage> messages, bool historical) {
            if (self)
            {
                self->fanOut(liveChatID, std::move(messages), historical);
            }
        },
        [self = QPointer(this), liveChatID](YouTubeLiveChatSession::State state,
                                            const QString &detail) {
            if (self)
            {
                self->sessionState(liveChatID, state, detail, false);
            }
        },
        entry.deduper);
    entry.innertubeSession->start();
}

void YouTubeChatServer::startRelaySession(const QString &liveChatID,
                                          const QString &readTicket,
                                          SessionEntry &entry)
{
    this->prune(entry);
    if (entry.channels.empty())
    {
        return;
    }
    entry.relaySession = std::make_shared<YouTubeLiveChatSession>(
        liveChatID, readTicket,
        [self = QPointer(this), liveChatID](
            std::vector<YouTubeMessage> messages, bool historical) {
            if (self)
            {
                self->fanOut(liveChatID, std::move(messages), historical);
            }
        },
        [self = QPointer(this), liveChatID](YouTubeLiveChatSession::State state,
                                            const QString &detail) {
            if (self)
            {
                self->sessionState(liveChatID, state, detail, true);
            }
        },
        entry.deduper);
    entry.relaySession->start();
}

void YouTubeChatServer::scheduleInnertubeRecovery(
    const QString &liveChatID, SessionEntry &entry,
    std::chrono::milliseconds delay)
{
    this->prune(entry);
    if (!entry.relayFallback || !entry.relaySession || entry.innertubeSession ||
        entry.innertubeRecoveryScheduled || entry.videoID.isEmpty() ||
        entry.channels.empty())
    {
        return;
    }

    entry.innertubeRecoveryScheduled = true;
    const std::weak_ptr<YouTubeLiveChatSession> relaySession =
        entry.relaySession;
    QTimer::singleShot(delay, this, [this, liveChatID, relaySession] {
        const auto expectedSession = relaySession.lock();
        auto current = this->sessions_.find(liveChatID);
        if (!expectedSession || current == this->sessions_.end() ||
            current->second.relaySession != expectedSession)
        {
            return;
        }

        auto &entry = current->second;
        entry.innertubeRecoveryScheduled = false;
        this->prune(entry);
        if (!entry.relayFallback || !entry.relaySession ||
            entry.innertubeSession || entry.videoID.isEmpty() ||
            entry.channels.empty())
        {
            return;
        }
        this->startInnertubeSession(entry.videoID, liveChatID, entry);
    });
}

void YouTubeChatServer::fanOut(const QString &liveChatID,
                               std::vector<YouTubeMessage> messages,
                               bool historical)
{
    auto it = this->sessions_.find(liveChatID);
    if (it == this->sessions_.end())
    {
        return;
    }
    this->prune(it->second);
    auto &entry = it->second;
    const auto channels = entry.channels;

    for (const auto &message : messages)
    {
        entry.replay.append(message);
    }
    for (auto &weak : channels)
    {
        if (auto channel = weak.lock())
        {
            channel->receiveMessages(messages, historical);
        }
    }
    if (channels.empty())
    {
        const auto innertubeSession = it->second.innertubeSession;
        const auto relaySession = it->second.relaySession;
        QTimer::singleShot(
            0, this, [this, liveChatID, innertubeSession, relaySession] {
                auto current = this->sessions_.find(liveChatID);
                if (current != this->sessions_.end() &&
                    current->second.innertubeSession == innertubeSession &&
                    current->second.relaySession == relaySession)
                {
                    this->prune(current->second);
                    if (current->second.channels.empty())
                    {
                        this->sessions_.erase(current);
                    }
                }
            });
    }
}

void YouTubeChatServer::sessionState(const QString &liveChatID,
                                     YouTubeLiveChatSession::State state,
                                     const QString &detail, bool fromRelay)
{
    auto it = this->sessions_.find(liveChatID);
    if (it == this->sessions_.end())
    {
        return;
    }
    this->prune(it->second);
    const bool recoveryProbe = !fromRelay && it->second.relayFallback &&
                               static_cast<bool>(it->second.relaySession);
    const auto identity = it->second.deduper;
    const auto channels = it->second.channels;
    const auto innertubeSession = it->second.innertubeSession;
    const auto relaySession = it->second.relaySession;

    if (recoveryProbe && state != YouTubeLiveChatSession::State::Connected)
    {
        if (state == YouTubeLiveChatSession::State::RefreshRequired ||
            state == YouTubeLiveChatSession::State::Ended ||
            state == YouTubeLiveChatSession::State::Failed)
        {
            QTimer::singleShot(0, this, [this, liveChatID, innertubeSession] {
                auto current = this->sessions_.find(liveChatID);
                if (current == this->sessions_.end() ||
                    current->second.innertubeSession != innertubeSession)
                {
                    return;
                }
                current->second.innertubeSession.reset();
                this->scheduleInnertubeRecovery(liveChatID, current->second,
                                                std::chrono::minutes(5));
            });
        }
        return;
    }

    if (recoveryProbe && state == YouTubeLiveChatSession::State::Connected)
    {
        it->second.relayFallback = false;
        it->second.innertubeRecoveryScheduled = false;
        it->second.readTransport = YouTubeReadTransport::Innertube;
        it->second.relaySession.reset();
    }
    for (auto &weak : channels)
    {
        auto channel = weak.lock();
        if (!channel)
        {
            continue;
        }
        switch (state)
        {
            case YouTubeLiveChatSession::State::Connected:
                channel->setConnectionState(true);
                break;
            case YouTubeLiveChatSession::State::Reconnecting:
                channel->setConnectionState(false, detail);
                break;
            case YouTubeLiveChatSession::State::RefreshRequired:
                channel->setConnectionState(false);
                break;
            case YouTubeLiveChatSession::State::Ended:
                channel->markChatEnded();
                break;
            case YouTubeLiveChatSession::State::Failed:
                channel->setConnectionState(false, detail);
                channel->applyResolutionError(
                    detail.isEmpty()
                        ? u"YouTube chat streaming stopped."_s
                        : u"YouTube chat streaming stopped: "_s + detail);
                break;
        }
    }

    it = this->sessions_.find(liveChatID);
    if (it == this->sessions_.end() || it->second.deduper != identity)
    {
        return;
    }

    if (fromRelay && state == YouTubeLiveChatSession::State::Connected &&
        it->second.relayFallback)
    {
        this->scheduleInnertubeRecovery(liveChatID, it->second,
                                        std::chrono::minutes(2));
    }

    if (!fromRelay &&
        (state == YouTubeLiveChatSession::State::RefreshRequired ||
         state == YouTubeLiveChatSession::State::Failed))
    {
        QTimer::singleShot(0, this, [this, liveChatID, innertubeSession] {
            auto current = this->sessions_.find(liveChatID);
            if (current == this->sessions_.end() ||
                current->second.innertubeSession != innertubeSession)
            {
                return;
            }
            current->second.innertubeSession.reset();
            current->second.readTransport = YouTubeReadTransport::Relay;
            current->second.relayFallback = true;
            this->startRelaySession(liveChatID, current->second.readTicket,
                                    current->second);
        });
        return;
    }

    if (state == YouTubeLiveChatSession::State::RefreshRequired ||
        state == YouTubeLiveChatSession::State::Ended ||
        state == YouTubeLiveChatSession::State::Failed)
    {
        QTimer::singleShot(
            0, this,
            [this, liveChatID, channels, innertubeSession, relaySession,
             state] {
                auto current = this->sessions_.find(liveChatID);
                if (current == this->sessions_.end() ||
                    current->second.innertubeSession != innertubeSession ||
                    current->second.relaySession != relaySession)
                {
                    return;
                }
                this->sessions_.erase(current);
                for (const auto &weak : channels)
                {
                    if (auto channel = weak.lock())
                    {
                        if (state ==
                            YouTubeLiveChatSession::State::RefreshRequired)
                        {
                            this->resolve(channel, true);
                        }
                        else
                        {
                            this->scheduleResolve(channel,
                                                  std::chrono::minutes(1));
                        }
                    }
                }
            });
    }
}

void YouTubeChatServer::scheduleResolve(
    const std::shared_ptr<YouTubeChannel> &channel,
    std::chrono::milliseconds delay)
{
    if (!channel)
    {
        return;
    }
    const auto source = channel->getName();
    const auto generation = this->resolveGenerations_[source];
    QTimer::singleShot(
        delay, this,
        [this, weak = channel->weakFromThis(), source, generation] {
            const auto current = this->resolveGenerations_.find(source);
            if (current == this->resolveGenerations_.end() ||
                current->second != generation)
            {
                return;
            }
            if (auto channel = weak.lock())
            {
                if (!this->isRegistered(channel))
                {
                    return;
                }
                this->resolve(channel);
            }
        });
}

void YouTubeChatServer::prune(SessionEntry &entry)
{
    std::erase_if(entry.channels, [](const auto &weak) {
        return weak.expired();
    });
}

bool YouTubeChatServer::isRegistered(
    const std::shared_ptr<YouTubeChannel> &channel) const
{
    if (!channel)
    {
        return false;
    }
    const auto it = this->channels_.find(channel->getName());
    return it != this->channels_.end() && it->second.lock() == channel;
}

}
