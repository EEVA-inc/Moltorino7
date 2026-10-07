#include "providers/twitch/TwitchIrcServer.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Common.hpp"
#include "common/Env.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/irc/IrcConnection2.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchHelpers.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "util/RatelimitBucket.hpp"
#include "util/Twitch.hpp"

#include <IrcCommand>
#include <IrcMessage>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QCoreApplication>
#include <QMetaEnum>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>

using namespace std::chrono_literals;

namespace {

constexpr int JOIN_RATELIMIT_BUDGET = 18;
constexpr int JOIN_RATELIMIT_COOLDOWN = 12500;
constexpr int JOIN_CONFIRMATION_TIMEOUT = 15000;
constexpr int JOIN_MAX_ATTEMPTS = 3;

using namespace chatterino;

bool isWarningAcknowledgeNotice(const QString &text)
{
    return text.startsWith(
               "You received a Warning from a moderator in this channel.",
               Qt::CaseInsensitive) ||
           text.contains("Acknowledge the Warning at",
                         Qt::CaseInsensitive);
}

thread_local bool preferAnonymousTwitchChannels = false;
thread_local ChannelPtr receivingChannel;

class ScopedAnonymousTwitchLookup
{
public:
    explicit ScopedAnonymousTwitchLookup(bool enabled, ChannelPtr channel = {})
        : previous_(preferAnonymousTwitchChannels)
        , previousChannel_(std::move(receivingChannel))
    {
        preferAnonymousTwitchChannels = enabled;
        receivingChannel = std::move(channel);
    }

    ~ScopedAnonymousTwitchLookup()
    {
        preferAnonymousTwitchChannels = this->previous_;
        receivingChannel = std::move(this->previousChannel_);
    }

private:
    bool previous_;
    ChannelPtr previousChannel_;
};

QStringList makeIrcTags(QStringList tags = {})
{
    if (getSettings()->spoofIrcMessagesAsWeb)
    {
        tags.prepend(QStringLiteral("client-nonce=") + makeTwitchClientNonce());
    }

    return tags;
}

QString makePrivmsg(const QString &channelName, const QString &message,
                    QStringList tags = {})
{
    QString prefix;
    if (!tags.isEmpty())
    {
        prefix =
            QStringLiteral("@") + tags.join(QLatin1Char(';')) +
            QStringLiteral(" ");
    }

    return prefix + QStringLiteral("PRIVMSG #") + channelName +
           QStringLiteral(" :") + message;
}

void sendHelixMessage(const std::shared_ptr<TwitchChannel> &channel,
                      const QString &message, const QString &replyParentId = {})
{
    auto broadcasterID = channel->roomId();
    if (broadcasterID.isEmpty())
    {
        channel->addSystemMessage(
            "Sending messages in this channel isn't possible.");
        return;
    }

    getHelix()->sendChatMessage(
        {
            .broadcasterID = broadcasterID,
            .senderID =
                getApp()->getAccounts()->twitch.getCurrent()->getUserId(),
            .message = message,
            .replyParentMessageID = replyParentId,
        },
        [weak = std::weak_ptr(channel)](const auto &res) {
            auto chan = weak.lock();
            if (!chan)
            {
                return;
            }

            if (res.isSent)
            {
                return;
            }

            if (res.dropReason)
            {
                if (isWarningAcknowledgeNotice(res.dropReason->message))
                {
                    chan->handleChatWarningNotice();
                    return;
                }

                chan->addSystemMessage(res.dropReason->message);
            }
            else
            {
                chan->addSystemMessage("Your message was not sent.");
            }
        },
        [weak = std::weak_ptr(channel)](auto error, auto message) {
            auto chan = weak.lock();
            if (!chan)
            {
                return;
            }

            if (message.isEmpty())
            {
                message = "(empty message)";
            }

            using Error = decltype(error);

            auto errorMessage = [&]() -> QString {
                switch (error)
                {
                    case Error::MissingText:
                        return "You can't send an empty message.";
                    case Error::BadRequest:
                        return "Failed to send message: " + message;
                    case Error::Forbidden:
                        return "You are not allowed to send messages in this "
                               "channel.";
                    case Error::MessageTooLarge:
                        return "Your message was too long.";
                    case Error::UserMissingScope:
                        return "Missing required scope. Re-login with your "
                               "account and try again.";
                    case Error::Forwarded:
                        return message;
                    case Error::Unknown:
                    default:
                        return "Unknown error: " + message;
                }
            }();
            if (isWarningAcknowledgeNotice(errorMessage))
            {
                chan->handleChatWarningNotice();
                return;
            }

            chan->addSystemMessage(errorMessage);
        });
}

}

namespace chatterino {

using namespace literals;

TwitchIrcServer::TwitchIrcServer()
    : whispersChannel(new Channel("/whispers", Channel::Type::TwitchWhispers))
    , mentionsChannel(new Channel("/mentions", Channel::Type::TwitchMentions))
    , liveChannel(new Channel("/live", Channel::Type::TwitchLive))
    , automodChannel(new Channel("/automod", Channel::Type::TwitchAutomod))
    , watchingChannel(Channel::getEmpty(), Channel::Type::TwitchWatching)
{

    this->writeConnection_.reset(new IrcConnection);
    this->writeConnection_->moveToThread(
        QCoreApplication::instance()->thread());

    auto actuallyJoin = [&](QString message) {
        const auto channel = std::dynamic_pointer_cast<TwitchChannel>(
            this->channels.value(message).lock());
        if (!channel || channel->isReadingAnonymously())
        {
            return;
        }
        this->readConnection_->sendRaw("JOIN #" + message);
        this->sentJoin(message, false);
    };
    this->joinBucket_.reset(new RatelimitBucket(
        JOIN_RATELIMIT_BUDGET, JOIN_RATELIMIT_COOLDOWN, actuallyJoin, this));

    auto actuallyJoinAnonymous = [&](QString message) {
        if (this->readChannels(true, message).empty() ||
            !this->anonymousReadConnection_)
        {
            return;
        }
        this->anonymousReadConnection_->sendRaw("JOIN #" + message);
        this->sentJoin(message, true);
    };
    this->anonymousJoinBucket_.reset(
        new RatelimitBucket(JOIN_RATELIMIT_BUDGET, JOIN_RATELIMIT_COOLDOWN,
                            actuallyJoinAnonymous, this));

    this->joinRetryTimer_.setSingleShot(true);
    QObject::connect(&this->joinRetryTimer_, &QTimer::timeout, this, [this] {
        this->retryUnconfirmedJoins(false);
    });
    this->anonymousJoinRetryTimer_.setSingleShot(true);
    QObject::connect(&this->anonymousJoinRetryTimer_, &QTimer::timeout, this,
                     [this] {
                         this->retryUnconfirmedJoins(true);
                     });

    QObject::connect(this->writeConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     [this](auto msg) {
                         this->writeConnectionMessageReceived(msg);
                     });
    QObject::connect(this->writeConnection_.get(),
                     &Communi::IrcConnection::connected, this, [this] {
                         this->onWriteConnected(this->writeConnection_.get());
                     });
    this->signalHolder.managedConnect(
        this->writeConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Write connection reconnect requested. Timeout:" << timeout;
            this->writeConnection_->smartReconnect();
        });

    this->readConnection_.reset(new IrcConnection);
    this->readConnection_->moveToThread(QCoreApplication::instance()->thread());

    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     [this](auto msg) {
                         this->readConnectionMessageReceived(msg);
                     });
    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::privateMessageReceived, this,
                     [this](auto msg) {
                         this->privateMessageReceived(msg);
                     });
    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::connected, this, [this] {
                         this->onReadConnected(this->readConnection_.get());
                     });
    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::disconnected, this, [this] {
                         this->onDisconnected();
                     });
    this->signalHolder.managedConnect(
        this->readConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Read connection reconnect requested. Timeout:" << timeout;
            if (timeout)
            {

                this->addGlobalSystemMessage(
                    "Server connection timed out, reconnecting");
            }
            this->readConnection_->smartReconnect();
        });
    this->signalHolder.managedConnect(this->readConnection_->heartbeat, [this] {
        this->markChannelsConnected();
    });

    this->anonymousReadConnection_.reset(new IrcConnection);
    this->anonymousReadConnection_->moveToThread(
        QCoreApplication::instance()->thread());

    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     [this](auto msg) {
                         this->readConnectionMessageReceived(msg, true);
                     });
    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::privateMessageReceived, this,
                     [this](auto msg) {
                         this->privateMessageReceived(msg, true);
                     });
    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::connected, this, [this] {
                         this->onAnonymousReadConnected(
                             this->anonymousReadConnection_.get());
                     });
    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::disconnected, this, [this] {
                         this->onAnonymousDisconnected();
                         this->anonymousReadConnectionStarted_ = false;
                     });
    this->signalHolder.managedConnect(
        this->anonymousReadConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Anonymous read connection reconnect requested. Timeout:"
                << timeout;
            this->anonymousReadConnection_->smartReconnect();
        });
    this->signalHolder.managedConnect(
        this->anonymousReadConnection_->heartbeat, [this] {
            this->markAnonymousChannelsConnected();
        });
}

void TwitchIrcServer::initialize()
{
    this->signalHolder.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, [this]() {
            this->clearAnonymousFallbacks();
            postToThread([this] {
                this->connect();
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pointReward.redeemed, [this](auto &data) {
            QString channelId = data.value("channel_id").toString();
            if (channelId.isEmpty())
            {
                qCDebug(chatterinoApp)
                    << "Couldn't find channel id of point reward";
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            auto reward = ChannelPointReward(data);

            postToThread([chan, reward] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->addChannelPointReward(reward);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pinnedChat.updated, [this](auto &data) {
            if (!getSettings()->enablePinnedMessages)
            {
                return;
            }
            QString topic = data.value("topic").toString();

            if (!topic.startsWith("pinned-chat-updates-v1.")) {
                return;
            }
            QString channelId = topic.mid(23);
            if (channelId.isEmpty()) {
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            postToThread([chan, data] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->handlePinnedChatUpdate(data);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->prediction.updated,
        [this](const auto &data) {
            QString topic = data.value("topic").toString();

            if (!topic.startsWith("predictions-channel-v1."))
            {
                return;
            }
            QString channelId = topic.mid(23);
            if (channelId.isEmpty())
            {
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            postToThread([chan, data] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->handlePredictionUpdate(data);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->chatWarning.updated,
        [this](const auto &payload) {
            const auto data = payload.value("data").toObject();
            const auto channelId = data.value("channel_id").toString();
            if (channelId.isEmpty())
            {
                qCDebug(chatterinoApp)
                    << "Couldn't find channel id of chat warning";
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            postToThread([chan, payload] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->handleChatWarningPubSub(payload);
                }
            });
        });
}

void TwitchIrcServer::aboutToQuit()
{
    this->signalHolder.clear();

    this->clearJoinAttempts(false);
    this->clearJoinAttempts(true);

    this->channels.clear();
    this->anonymousChannels.clear();
}

void TwitchIrcServer::initializeConnection(IrcConnection *connection,
                                           ConnectionType type)
{
    std::shared_ptr<TwitchAccount> account =
        getApp()->getAccounts()->twitch.getCurrent();

    const bool anonymous = type == ConnectionType::AnonymousRead;

    qCDebug(chatterinoTwitch)
        << "logging in as"
        << (anonymous ? QStringLiteral("anonymous") : account->getUserName());

    QStringList caps{"twitch.tv/tags", "twitch.tv/commands"};
    if (type != ConnectionType::Write)
    {
        caps.push_back("twitch.tv/membership");
    }

    connection->network()->setSkipCapabilityValidation(true);
    connection->network()->setRequestedCapabilities(caps);

    QString username = account->getUserName();
    QString oauthToken = account->getOAuthToken();

    if (anonymous)
    {
        username = QStringLiteral("justinfan%1").arg(
            QRandomGenerator::global()->bounded(100000, 1000000));
    }

    if (!anonymous && !oauthToken.startsWith("oauth:"))
    {
        oauthToken.prepend("oauth:");
    }

    connection->setUserName(username);
    connection->setNickName(username);
    connection->setRealName(username);

    if (!anonymous && !account->isAnon())
    {
        connection->setPassword(oauthToken);
    }

    connection->setHost(Env::get().twitchServerHost);
    connection->setPort(Env::get().twitchServerPort);
    connection->setSecure(Env::get().twitchServerSecure);

    this->open(type);
}

std::shared_ptr<Channel> TwitchIrcServer::createChannel(
    const QString &channelName, bool anonymous)
{
    auto channel = std::make_shared<TwitchChannel>(channelName, anonymous);
    channel->initialize();

    std::ignore = channel->sendMessageSignal.connect(
        [this, channel = std::weak_ptr(channel)](auto &msg, bool &sent) {
            auto c = channel.lock();
            if (!c)
            {
                return;
            }
            this->onMessageSendRequested(c, msg, sent);
        });
    std::ignore = channel->sendReplySignal.connect(
        [this, channel = std::weak_ptr(channel)](auto &msg, auto &replyId,
                                                 bool &sent) {
            auto c = channel.lock();
            if (!c)
            {
                return;
            }
            this->onReplySendRequested(c, msg, replyId, sent);
        });

    return channel;
}

QVarLengthArray<ChannelPtr, 2> TwitchIrcServer::readChannels(
    bool anonymous, const QString &name)
{
    std::lock_guard lock(this->channelMutex);
    QVarLengthArray<ChannelPtr, 2> result;
    auto append = [&](const std::weak_ptr<Channel> &weak) {
        auto channel = weak.lock();
        auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
        if (tc && tc->isReadingAnonymously() == anonymous)
        {
            result.push_back(std::move(channel));
        }
    };
    for (const auto *map : {&this->channels, &this->anonymousChannels})
    {
        if (name.isEmpty())
        {
            for (const auto &weak : *map)
            {
                append(weak);
            }
        }
        else
        {
            append(map->value(cleanChannelName(name)));
        }
    }
    return result;
}

void TwitchIrcServer::handleReadBlock(Communi::IrcMessage *message)
{
    if (message->tags().contains("historical"))
    {
        return;
    }
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon())
    {
        return;
    }
    const auto &tags = message->tags();
    const auto notice = tags.value("msg-id").toString();
    const bool bannedNotice =
        message->command() == "NOTICE" &&
        (notice == "msg_banned" || notice == "msg_channel_blocked");
    const auto targetId = tags.value("target-user-id").toString();
    const bool selfBan =
        message->command() == "CLEARCHAT" && !tags.contains("ban-duration") &&
        (!targetId.isEmpty()
             ? targetId == account->getUserId()
             : message->parameter(1).compare(account->getUserName(),
                                             Qt::CaseInsensitive) == 0);
    QString name;
    if ((!bannedNotice && !selfBan) || !message->parameter(0).startsWith('#') ||
        !trimChannelName(message->parameter(0), name))
    {
        return;
    }
    const auto targets = this->readChannels(false, name);
    if (targets.empty())
    {
        return;
    }
    const bool alreadyReading = !this->readChannels(true, name).empty();
    auto *channel = static_cast<TwitchChannel *>(targets.front().get());
    channel->setAnonymousFallback(true);
    this->cancelJoinAttempt(name, false);
    this->readConnection_->sendRaw("PART #" + name);
    channel->addSystemMessage(
        "You are banned from this channel. Connecting anonymously to keep "
        "reading. Reconnect after an unban to chat again.");
    this->ensureAnonymousReadConnection();
    if (!alreadyReading && this->anonymousReadConnection_->isConnected())
    {
        this->anonymousJoinBucket_->send(name);
    }
}

void TwitchIrcServer::partUnusedAnonymousChannel(const QString &name)
{
    if (!this->readChannels(true, name).empty())
    {
        return;
    }
    this->cancelJoinAttempt(name, true);
    if (this->anonymousReadConnection_)
    {
        if (!name.startsWith('/'))
        {
            this->anonymousReadConnection_->sendRaw("PART #" + name);
        }
        if (this->readChannels(true).empty())
        {
            this->anonymousReadConnectionStarted_ = false;
            this->anonymousReadConnection_->close();
        }
    }
}

void TwitchIrcServer::clearAnonymousFallbacks()
{
    for (const auto &channel : this->readChannels(true))
    {
        auto *tc = static_cast<TwitchChannel *>(channel.get());
        if (tc->anonymousFallback_)
        {
            tc->setAnonymousFallback(false);
            this->partUnusedAnonymousChannel(tc->getName());
        }
    }
}

void TwitchIrcServer::privateMessageReceived(
    Communi::IrcPrivateMessage *message, bool anonymous)
{
    const auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    if (!pending.isEmpty() && message->target().startsWith('#'))
    {
        QString channelName;
        if (trimChannelName(message->target(), channelName))
        {
            this->confirmJoin(channelName, anonymous, true);
        }
    }

    QString channelName;
    if (!trimChannelName(message->target(), channelName))
    {
        return;
    }
    for (const auto &chan : this->readChannels(anonymous, channelName))
    {
        ScopedAnonymousTwitchLookup lookup(anonymous, chan);
        auto *twitchChannel = static_cast<TwitchChannel *>(chan.get());
        IrcMessageHandler::parsePrivMessageInto(message, *twitchChannel,
                                                twitchChannel);
    }
}

void TwitchIrcServer::readConnectionMessageReceived(
    Communi::IrcMessage *message, bool anonymous)
{
    if (message->type() == Communi::IrcMessage::Type::Private)
    {

        return;
    }

    QString channelName;
    if (message->parameter(0).startsWith('#') &&
        trimChannelName(message->parameter(0), channelName))
    {
        for (const auto &channel : this->readChannels(anonymous, channelName))
        {
            ScopedAnonymousTwitchLookup lookup(anonymous, channel);
            this->handleReadMessage(message, anonymous);
        }
    }
    else
    {
        ScopedAnonymousTwitchLookup lookup(anonymous);
        this->handleReadMessage(message, anonymous);
    }
    if (!anonymous)
    {
        this->handleReadBlock(message);
    }
}

void TwitchIrcServer::handleReadMessage(Communi::IrcMessage *message,
                                        bool anonymous)
{
    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();

    if (command == "JOIN")
    {
        auto *connection = anonymous ? this->anonymousReadConnection_.get()
                                     : this->readConnection_.get();
        QString channelName;
        if (connection &&
            message->nick().compare(connection->nickName(),
                                    Qt::CaseInsensitive) == 0 &&
            message->parameter(0).startsWith('#') &&
            trimChannelName(message->parameter(0), channelName))
        {
            this->confirmJoin(channelName, anonymous, false);
            if (anonymous)
            {
                auto channel = this->getAnonymousChannelOrEmpty(channelName);
                if (auto *twitchChannel =
                        dynamic_cast<TwitchChannel *>(channel.get()))
                {
                    twitchChannel->joined.invoke();
                }
            }
        }
        handler.handleJoinMessage(message);
    }
    else if (command == "PART")
    {
        handler.handlePartMessage(message);
    }
    else if (command == "USERSTATE")
    {

        handler.handleUserStateMessage(message);
        QString channelName;
        if (message->parameter(0).startsWith('#') &&
            trimChannelName(message->parameter(0), channelName))
        {
            this->confirmJoin(channelName, anonymous, true);
        }
    }
    else if (command == "ROOMSTATE")
    {

        handler.handleRoomStateMessage(message);
        QString channelName;
        if (message->parameter(0).startsWith('#') &&
            trimChannelName(message->parameter(0), channelName))
        {
            this->confirmJoin(channelName, anonymous, true);
        }
    }
    else if (command == "CLEARCHAT")
    {
        handler.handleClearChatMessage(message);
    }
    else if (command == "CLEARMSG")
    {
        handler.handleClearMessageMessage(message);
    }
    else if (command == "USERNOTICE")
    {
        handler.handleUserNoticeMessage(message, *this);
    }
    else if (command == "NOTICE")
    {
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
    else if (command == "WHISPER")
    {
        handler.handleWhisperMessage(message);
    }
    else if (command == "RECONNECT")
    {
        if (anonymous)
        {
            this->markAnonymousChannelsConnected();
            this->reconnectAnonymousChannels();
            return;
        }

        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->markChannelsConnected();
        this->connect();
    }
}

void TwitchIrcServer::writeConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();

    if (command == "USERSTATE")
    {
        if (this->readChannels(false, cleanChannelName(message->parameter(0)))
                .empty())
        {
            return;
        }
        handler.handleUserStateMessage(message);
    }
    else if (command == "NOTICE")
    {

        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
        this->handleReadBlock(message);
    }
    else if (command == "RECONNECT")
    {
        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->connect();
    }
}

void TwitchIrcServer::sentJoin(const QString &dirtyChannelName, bool anonymous)
{
    const auto channelName = cleanChannelName(dirtyChannelName);
    auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    auto &attempt = pending[channelName];
    attempt.attempts++;
    attempt.sentAt = std::chrono::steady_clock::now();
    attempt.queued = false;
    this->scheduleJoinRetry(anonymous);
}

void TwitchIrcServer::confirmJoin(const QString &dirtyChannelName,
                                  bool anonymous, bool restoreChannelState)
{
    auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    if (pending.isEmpty())
    {
        return;
    }

    const auto channelName = cleanChannelName(dirtyChannelName);
    auto pendingIt = pending.find(channelName);
    if (pendingIt == pending.end())
    {
        return;
    }

    ChannelPtr channelToRestore;
    if (restoreChannelState)
    {
        channelToRestore = anonymous
                               ? this->getAnonymousChannelOrEmpty(channelName)
                               : this->getChannelOrEmpty(channelName);
        if (auto *twitchChannel =
                dynamic_cast<TwitchChannel *>(channelToRestore.get());
            twitchChannel && twitchChannel->hasExpectedReconnectPart())
        {
            return;
        }
    }

    pending.erase(pendingIt);

    auto *bucket =
        anonymous ? this->anonymousJoinBucket_.get() : this->joinBucket_.get();
    bucket->removePending(channelName);

    if (restoreChannelState)
    {
        if (auto *twitchChannel =
                dynamic_cast<TwitchChannel *>(channelToRestore.get()))
        {
            twitchChannel->joined.invoke();
        }
        for (const auto &channel : this->readChannels(anonymous, channelName))
        {
            if (channel != channelToRestore)
            {
                static_cast<TwitchChannel *>(channel.get())->joined.invoke();
            }
        }
    }

    this->scheduleJoinRetry(anonymous);
}

void TwitchIrcServer::retryUnconfirmedJoins(bool anonymous)
{
    auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    auto *bucket =
        anonymous ? this->anonymousJoinBucket_.get() : this->joinBucket_.get();
    const auto now = std::chrono::steady_clock::now();
    QStringList retries;
    QStringList failed;

    for (auto it = pending.begin(); it != pending.end();)
    {
        if (it->queued || now - it->sentAt < std::chrono::milliseconds{
                                                 JOIN_CONFIRMATION_TIMEOUT})
        {
            ++it;
            continue;
        }
        if (it->attempts >= JOIN_MAX_ATTEMPTS)
        {
            failed.push_back(it.key());
            it = pending.erase(it);
            continue;
        }

        it->queued = true;
        retries.push_back(it.key());
        ++it;
    }

    for (const auto &channelName : retries)
    {
        qCDebug(chatterinoIrc)
            << "Retrying unconfirmed JOIN for" << channelName;
        bucket->send(channelName);
    }
    for (const auto &channelName : failed)
    {
        auto channel = anonymous ? this->getAnonymousChannelOrEmpty(channelName)
                                 : this->getChannelOrEmpty(channelName);
        if (channel && !channel->isEmpty())
        {
            channel->addSystemMessage(
                "Could not join this channel. Choose Reconnect in the channel "
                "menu to try again.");
        }
        qCWarning(chatterinoIrc) << "JOIN failed for" << channelName;
    }

    this->scheduleJoinRetry(anonymous);
}

void TwitchIrcServer::scheduleJoinRetry(bool anonymous)
{
    const auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    auto &timer =
        anonymous ? this->anonymousJoinRetryTimer_ : this->joinRetryTimer_;
    std::optional<std::chrono::steady_clock::time_point> next;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
    {
        if (it->queued)
        {
            continue;
        }
        const auto deadline =
            it->sentAt + std::chrono::milliseconds{JOIN_CONFIRMATION_TIMEOUT};
        if (!next || deadline < *next)
        {
            next = deadline;
        }
    }
    if (!next)
    {
        timer.stop();
        return;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            *next - std::chrono::steady_clock::now())
            .count();
    timer.start(static_cast<int>(std::clamp<std::int64_t>(
        remaining, 1, std::numeric_limits<int>::max())));
}

void TwitchIrcServer::clearJoinAttempts(bool anonymous)
{
    auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    auto &timer =
        anonymous ? this->anonymousJoinRetryTimer_ : this->joinRetryTimer_;
    auto *bucket =
        anonymous ? this->anonymousJoinBucket_.get() : this->joinBucket_.get();
    pending.clear();
    timer.stop();
    bucket->clearPending();
}

void TwitchIrcServer::cancelJoinAttempt(const QString &dirtyChannelName,
                                        bool anonymous)
{
    const auto channelName = cleanChannelName(dirtyChannelName);
    auto &pending =
        anonymous ? this->anonymousJoinAttempts_ : this->joinAttempts_;
    auto *bucket =
        anonymous ? this->anonymousJoinBucket_.get() : this->joinBucket_.get();
    pending.remove(channelName);
    bucket->removePending(channelName);
    this->scheduleJoinRetry(anonymous);
}

void TwitchIrcServer::onReadConnected(IrcConnection *connection)
{
    (void)connection;

    this->clearJoinAttempts(false);

    auto activeChannels = this->readChannels(false);

    auto visible = getApp()->getWindows()->getVisibleChannelNames();

    std::ranges::stable_partition(activeChannels, [&](const auto &chan) {
        return visible.contains(chan->getName());
    });

    for (const auto &channel : activeChannels)
    {

        if (channel->getName().startsWith("/"))
        {
            continue;
        }
        this->joinBucket_->send(channel->getName());
    }

    auto connectedMsg = makeSystemMessage("connected");
    connectedMsg->flags.set(MessageFlag::ConnectedMessage);
    auto reconnected = makeSystemMessage("reconnected");
    reconnected->flags.set(MessageFlag::ConnectedMessage);

    for (const auto &chan : activeChannels)
    {
        MessagePtr last = chan->getLastMessage();

        bool replaceMessage =
            last && last->flags.has(MessageFlag::DisconnectedMessage);

        if (replaceMessage)
        {
            chan->replaceMessage(last, reconnected);
        }
        else
        {
            chan->addMessage(connectedMsg, MessageContext::Original);
        }
    }

    this->falloffCounter_ = 1;
}

void TwitchIrcServer::onAnonymousReadConnected(IrcConnection *connection)
{
    (void)connection;

    this->clearJoinAttempts(true);

    auto activeChannels = this->readChannels(true);

    auto visible = getApp()->getWindows()->getVisibleChannelNames();

    std::ranges::stable_partition(activeChannels, [&](const auto &chan) {
        return visible.contains(chan->getName());
    });

    QSet<QString> joined;
    for (const auto &channel : activeChannels)
    {
        if (channel->getName().startsWith("/") ||
            joined.contains(channel->getName()))
        {
            continue;
        }
        joined.insert(channel->getName());
        this->anonymousJoinBucket_->send(channel->getName());
    }

    auto connectedMsg = makeSystemMessage("connected anonymously");
    connectedMsg->flags.set(MessageFlag::ConnectedMessage);
    auto reconnected = makeSystemMessage("reconnected anonymously");
    reconnected->flags.set(MessageFlag::ConnectedMessage);

    for (const auto &chan : activeChannels)
    {
        MessagePtr last = chan->getLastMessage();

        bool replaceMessage =
            last && last->flags.has(MessageFlag::DisconnectedMessage);

        if (replaceMessage)
        {
            chan->replaceMessage(last, reconnected);
        }
        else
        {
            chan->addMessage(connectedMsg, MessageContext::Original);
        }
    }
}

void TwitchIrcServer::onWriteConnected(IrcConnection *connection)
{
    (void)connection;
}

void TwitchIrcServer::onDisconnected()
{
    this->clearJoinAttempts(false);

    MessageBuilder b(systemMessage, "disconnected");
    b->flags.set(MessageFlag::DisconnectedMessage);
    auto disconnectedMsg = b.release();

    for (const auto &chan : this->readChannels(false))
    {
        chan->addMessage(disconnectedMsg, MessageContext::Original);

        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->clearExpectedReconnectParts();
            channel->markDisconnected();
        }
    }
}

void TwitchIrcServer::onAnonymousDisconnected()
{
    this->clearJoinAttempts(true);

    MessageBuilder b(systemMessage, "anonymous disconnected");
    b->flags.set(MessageFlag::DisconnectedMessage);
    auto disconnectedMsg = b.release();

    for (const auto &chan : this->readChannels(true))
    {
        chan->addMessage(disconnectedMsg, MessageContext::Original);

        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->clearExpectedReconnectParts();
            channel->markDisconnected();
        }
    }
}

std::shared_ptr<Channel> TwitchIrcServer::getCustomChannel(
    const QString &channelName)
{
    if (channelName == "/whispers")
    {
        return this->whispersChannel;
    }

    if (channelName == "/mentions")
    {
        return this->mentionsChannel;
    }

    if (channelName == "/live")
    {
        return this->liveChannel;
    }

    if (channelName == "/automod")
    {
        return this->automodChannel;
    }

    static auto getTimer = [this](ChannelPtr channel, int msBetweenMessages,
                                  bool addInitialMessages) {
        if (addInitialMessages)
        {
            for (auto i = 0; i < 1000; i++)
            {
                channel->addSystemMessage(QString::number(i + 1));
            }
        }

        auto *timer = new QTimer;
        QObject::connect(timer, &QTimer::timeout, this, [channel] {
            channel->addSystemMessage(QTime::currentTime().toString());
        });
        timer->start(msBetweenMessages);
        return timer;
    };

    if (channelName == "$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, true);

        return channel;
    }
    if (channelName == "$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, false);

        return channel;
    }
    if (channelName == "$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, true);

        return channel;
    }
    if (channelName == "$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, false);

        return channel;
    }
    if (channelName == "$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, true);

        return channel;
    }
    if (channelName == "$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, false);

        return channel;
    }
    if (channelName == "$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, true);

        return channel;
    }
    if (channelName == "$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, false);

        return channel;
    }
    if (channelName == "$$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, true);

        return channel;
    }
    if (channelName == "$$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, false);

        return channel;
    }

    return nullptr;
}

void TwitchIrcServer::forEachChannelAndSpecialChannels(
    std::function<void(ChannelPtr)> func)
{
    this->forEachChannel(func);

    func(this->whispersChannel);
    func(this->mentionsChannel);
    func(this->liveChannel);
    func(this->automodChannel);
}

std::shared_ptr<Channel> TwitchIrcServer::getChannelOrEmptyByID(
    const QString &channelId)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (const auto &weakChannel : this->channels)
    {
        auto channel = weakChannel.lock();
        if (!channel)
        {
            continue;
        }

        auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(channel);
        if (!twitchChannel)
        {
            continue;
        }

        if (twitchChannel->roomId() == channelId &&
            twitchChannel->getName().count(':') < 2)
        {
            return twitchChannel;
        }
    }

    return Channel::getEmpty();
}

bool TwitchIrcServer::prepareToSend(
    const std::shared_ptr<TwitchChannel> &channel)
{
    if (!channel->canSendMessage())
    {
        channel->showAnonymousReadOnlyMessage();
        return false;
    }

    std::lock_guard<std::mutex> guard(this->lastMessageMutex_);

    auto &lastMessage = channel->hasHighRateLimit() ? this->lastMessageMod_
                                                    : this->lastMessagePleb_;
    size_t maxMessageCount = channel->hasHighRateLimit() ? 99 : 19;
    auto minMessageOffset = (channel->hasHighRateLimit() ? 100ms : 1100ms);

    auto now = std::chrono::steady_clock::now();

    if (!lastMessage.empty() && lastMessage.back() + minMessageOffset > now)
    {
        if (this->lastErrorTimeSpeed_ + 30s < now)
        {
            channel->addSystemMessage("You are sending messages too quickly.");

            this->lastErrorTimeSpeed_ = now;
        }
        return false;
    }

    while (!lastMessage.empty() && lastMessage.front() + 32s < now)
    {
        lastMessage.pop();
    }

    if (lastMessage.size() >= maxMessageCount)
    {
        if (this->lastErrorTimeAmount_ + 30s < now)
        {
            channel->addSystemMessage("You are sending too many messages.");

            this->lastErrorTimeAmount_ = now;
        }
        return false;
    }

    lastMessage.push(now);
    return true;
}

void TwitchIrcServer::onMessageSendRequested(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat())
    {
        sendHelixMessage(channel, message);
    }
    else
    {
        this->sendMessage(channel->getName(), message);
    }

    sent = true;
}

void TwitchIrcServer::onReplySendRequested(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    const QString &replyId, bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat())
    {
        sendHelixMessage(channel, message, replyId);
    }
    else
    {
        this->sendRawMessage(makePrivmsg(
            channel->getName(), message,
            makeIrcTags(QStringList{QStringLiteral("reply-parent-msg-id=") +
                                    replyId})));
    }
    sent = true;
}

const IndirectChannel &TwitchIrcServer::getWatchingChannel() const
{
    return this->watchingChannel;
}

void TwitchIrcServer::setWatchingChannel(ChannelPtr newWatchingChannel)
{
    assertInGuiThread();

    this->watchingChannel.reset(newWatchingChannel);
}

ChannelPtr TwitchIrcServer::getWhispersChannel() const
{
    return this->whispersChannel;
}

ChannelPtr TwitchIrcServer::getMentionsChannel() const
{
    return this->mentionsChannel;
}

ChannelPtr TwitchIrcServer::getLiveChannel() const
{
    return this->liveChannel;
}

ChannelPtr TwitchIrcServer::getAutomodChannel() const
{
    return this->automodChannel;
}

QString TwitchIrcServer::getLastUserThatWhisperedMe() const
{
    return this->lastUserThatWhisperedMe.get();
}

void TwitchIrcServer::setLastUserThatWhisperedMe(const QString &user)
{
    assertInGuiThread();

    this->lastUserThatWhisperedMe.set(user);
}

void TwitchIrcServer::initEventAPIs(BttvLiveUpdates *bttvLiveUpdates,
                                    SeventvEventAPI *seventvEventAPI)
{
    assertInGuiThread();

    if (bttvLiveUpdates != nullptr)
    {
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteAdded, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->addBttvEmote(data);
                        }
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteUpdated, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->updateBttvEmote(data);
                        }
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteRemoved, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->removeBttvEmote(data);
                        }
                    },
                    this);
            });
    }
    else
    {
        qCDebug(chatterinoBttv)
            << "Skipping initialization of Live Updates as it's disabled";
    }

    if (seventvEventAPI != nullptr)
    {
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteAdded, [&](const auto &data) {
                if (getApp()->getSeventvPersonalEmotes()->hasEmoteSet(
                        data.emoteSetID))
                {
                    getApp()->getSeventvPersonalEmotes()->updateEmoteSet(
                        data.emoteSetID, data);
                }
                else
                {
                    postToThread(
                        [this, data] {
                            this->forEachSeventvEmoteSet(
                                data.emoteSetID, [data](TwitchChannel &chan) {
                                    chan.addSeventvEmote(data);
                                });
                        },
                        this);
                }
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteUpdated, [&](const auto &data) {
                if (getApp()->getSeventvPersonalEmotes()->hasEmoteSet(
                        data.emoteSetID))
                {
                    getApp()->getSeventvPersonalEmotes()->updateEmoteSet(
                        data.emoteSetID, data);
                }
                else
                {
                    postToThread(
                        [this, data] {
                            this->forEachSeventvEmoteSet(
                                data.emoteSetID, [data](TwitchChannel &chan) {
                                    chan.updateSeventvEmote(data);
                                });
                        },
                        this);
                }
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteRemoved, [&](const auto &data) {
                if (getApp()->getSeventvPersonalEmotes()->hasEmoteSet(
                        data.emoteSetID))
                {
                    getApp()->getSeventvPersonalEmotes()->updateEmoteSet(
                        data.emoteSetID, data);
                }
                else
                {
                    postToThread(
                        [this, data] {
                            this->forEachSeventvEmoteSet(
                                data.emoteSetID, [data](TwitchChannel &chan) {
                                    chan.removeSeventvEmote(data);
                                });
                        },
                        this);
                }
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.userUpdated, [&](const auto &data) {
                this->forEachSeventvUser(data.userID,
                                         [data](TwitchChannel &chan) {
                                             chan.updateSeventvUser(data);
                                         });
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.personalEmoteSetAdded,
            [&](const seventv::eventapi::PersonalEmoteSetAdded &data) {
                QVarLengthArray<QString, 1> names;
                for (const auto &user : data.connections)
                {
                    if (const auto *u =
                            std::get_if<seventv::eventapi::TwitchUser>(&user))
                    {
                        names.emplace_back(u->userName);
                    }
                }
                if (names.empty())
                {
                    return;
                }

                postToThread(
                    [this, emoteSet = data.emoteSet,
                     names{std::move(names)}]() {
                        this->forEachChannelAndSpecialChannels([&](const auto
                                                                       &chan) {
                            if (auto *twitchChannel =
                                    dynamic_cast<TwitchChannel *>(chan.get()))
                            {
                                for (const auto &name : names)
                                {
                                    twitchChannel->upsertPersonalSeventvEmotes(
                                        name, emoteSet);
                                }
                            }
                        });
                    },
                    this);
            });
    }
    else
    {
        qCDebug(chatterinoSeventvEventAPI)
            << "Skipping initialization as the EventAPI is disabled";
    }
}

void TwitchIrcServer::reloadAllBTTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshBTTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadAllFFZChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshFFZChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadAllSevenTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshSevenTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::forEachSeventvEmoteSet(
    const QString &emoteSetId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([emoteSetId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvEmoteSetID() == emoteSetId)
        {
            func(*channel);
        }
    });
}
void TwitchIrcServer::forEachSeventvUser(
    const QString &userId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([userId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvUserID() == userId)
        {
            func(*channel);
        }
    });
}

void TwitchIrcServer::dropSeventvChannel(const QString &userID,
                                         const QString &emoteSetID)
{
    if (!getApp()->getSeventvEventAPI())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    bool skipUser = userID.isEmpty();
    bool skipSet = emoteSetID.isEmpty();

    bool foundUser = skipUser;
    bool foundSet = skipSet;
    for (std::weak_ptr<Channel> &weak : this->channels)
    {
        ChannelPtr chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!foundSet && channel->seventvEmoteSetID() == emoteSetID)
        {
            foundSet = true;
        }
        if (!foundUser && channel->seventvUserID() == userID)
        {
            foundUser = true;
        }

        if (foundSet && foundUser)
        {
            break;
        }
    }

    if (!foundUser)
    {
        getApp()->getSeventvEventAPI()->unsubscribeUser(userID);
    }
    if (!foundSet)
    {
        getApp()->getSeventvEventAPI()->unsubscribeEmoteSet(emoteSetID);
    }
}

void TwitchIrcServer::markChannelsConnected()
{
    for (const auto &chan : this->readChannels(false))
    {
        static_cast<TwitchChannel *>(chan.get())->markConnected();
    }
}

void TwitchIrcServer::markAnonymousChannelsConnected()
{
    for (const auto &chan : this->readChannels(true))
    {
        static_cast<TwitchChannel *>(chan.get())->markConnected();
    }
}

void TwitchIrcServer::ensureAnonymousReadConnection()
{
    if (this->readChannels(true).empty())
    {
        return;
    }
    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> locker(this->connectionMutex_);

        if (this->anonymousReadConnection_ &&
            !this->anonymousReadConnection_->isConnected() &&
            !this->anonymousReadConnectionStarted_)
        {
            this->anonymousReadConnectionStarted_ = true;
            shouldStart = true;
        }
    }

    if (!shouldStart)
    {
        return;
    }

    this->initializeConnection(this->anonymousReadConnection_.get(),
                               ConnectionType::AnonymousRead);
}

void TwitchIrcServer::addFakeMessage(const QString &data)
{
    assertInGuiThread();

    auto *fakeMessage = Communi::IrcMessage::fromData(
        data.toUtf8(), this->readConnection_.get());

    if (fakeMessage->command() == "PRIVMSG")
    {
        this->privateMessageReceived(
            static_cast<Communi::IrcPrivateMessage *>(fakeMessage));
    }
    else
    {
        this->readConnectionMessageReceived(fakeMessage);
    }
}

void TwitchIrcServer::addGlobalSystemMessage(const QString &messageText)
{
    MessageBuilder b(systemMessage, messageText);
    auto message = b.release();
    this->forEachChannel([&message](const auto &channel) {
        channel->addMessage(message, MessageContext::Original);
    });
}

void TwitchIrcServer::forEachChannel(std::function<void(ChannelPtr)> func)
{
    QVarLengthArray<ChannelPtr, 16> channels;
    {
        std::lock_guard lock(this->channelMutex);
        for (const auto &weak : this->channels)
        {
            if (auto channel = weak.lock())
            {
                channels.push_back(std::move(channel));
            }
        }
    }
    for (const auto &channel : channels)
    {
        func(channel);
    }
}

void TwitchIrcServer::connect()
{
    assertInGuiThread();

    if (auto *provider = getApp()->getMoltorinoSupporterBadges())
    {
        provider->refreshPassive();
    }

    this->disconnect();

    this->initializeConnection(this->writeConnection_.get(),
                               ConnectionType::Write);
    this->initializeConnection(this->readConnection_.get(),
                               ConnectionType::Read);
}

void TwitchIrcServer::disconnect()
{
    this->clearJoinAttempts(false);

    std::lock_guard<std::mutex> locker(this->connectionMutex_);

    this->readConnection_->close();
    this->writeConnection_->close();
}

void TwitchIrcServer::sendMessage(const QString &channelName,
                                  const QString &message)
{
    this->sendRawMessage(makePrivmsg(channelName, message, makeIrcTags()));
}

void TwitchIrcServer::sendRawMessage(const QString &rawMessage)
{
    std::lock_guard<std::mutex> locker(this->connectionMutex_);

    this->writeConnection_->sendRaw(rawMessage);
}

bool TwitchIrcServer::sendInvisibleMessage(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    const QString &oauthToken)
{
    if (!channel)
    {
        return false;
    }
    if (channel->isReadingAnonymously())
    {
        channel->showAnonymousReadOnlyMessage();
        return false;
    }
    if (oauthToken.trimmed().isEmpty())
    {
        channel->addSystemMessage(
            MoltorinoAuth::authRequiredMessage("sending invisible messages"));
        return false;
    }

    const auto channelId = channel->roomId();
    if (channelId.isEmpty())
    {
        channel->addSystemMessage(
            "Sending messages in this channel isn't possible yet.");
        return false;
    }

    const auto parsedMessage = channel->prepareMessage(message);
    if (parsedMessage.isEmpty())
    {
        return false;
    }
    if (parsedMessage.size() > TWITCH_MESSAGE_LIMIT)
    {
        channel->addSystemMessage("Your message was too long.");
        return false;
    }
    if (!this->prepareToSend(channel))
    {
        return false;
    }

    const auto nonce = makeInvisibleTwitchClientNonce(parsedMessage);
    const auto weak = std::weak_ptr<TwitchChannel>(channel);
    TwitchGql::sendChatMessageWithNonce(
        channelId, parsedMessage, nonce, oauthToken,
        [weak, parsedMessage] {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            shared->updateBttvActivity();
            shared->updateSevenTVActivity();
            shared->lastSentMessage_ = parsedMessage;
        },
        [weak](const QString &error) {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            shared->addSystemMessage("Failed to send invisible message: " +
                                     MoltorinoAuth::normalizeAuthError(
                                         "sending invisible messages", error));
        });
    return true;
}

ChannelPtr TwitchIrcServer::getOrAddChannel(const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    if (auto custom = this->getCustomChannel(channelName))
    {
        return custom;
    }

    {
        std::lock_guard<std::mutex> lock(this->channelMutex);

        auto it = this->channels.find(channelName);
        if (it != this->channels.end())
        {
            if (auto chan = it.value().lock())
            {
                return chan;
            }
        }
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    ChannelPtr chan = this->createChannel(channelName);
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
    if (!chan || !twitchChannel)
    {
        return Channel::getEmpty();
    }

    this->channels.insert(channelName, chan);
    this->signalHolder.managedConnect(
        twitchChannel->destroyed, [this, channelName] {

            qCDebug(chatterinoIrc) << "[TwitchIrcServer::addChannel]"
                                   << channelName << "was destroyed";
            this->cancelJoinAttempt(channelName, false);
            this->channels.remove(channelName);
            this->partUnusedAnonymousChannel(channelName);

            if (this->readConnection_)
            {

                if (!channelName.startsWith("/"))
                {
                    this->readConnection_->sendRaw("PART #" + channelName);
                }
            }
        });

    {
        std::lock_guard<std::mutex> lock2(this->connectionMutex_);

        if (this->readConnection_ && this->readConnection_->isConnected())
        {

            if (!channelName.startsWith("/"))
            {
                this->joinBucket_->send(channelName);
            }
        }
    }

    return chan;
}

ChannelPtr TwitchIrcServer::getOrAddAnonymousChannel(
    const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    if (auto custom = this->getCustomChannel(channelName))
    {
        return custom;
    }

    {
        std::lock_guard<std::mutex> lock(this->channelMutex);

        auto it = this->anonymousChannels.find(channelName);
        if (it != this->anonymousChannels.end())
        {
            if (auto chan = it.value().lock())
            {
                return chan;
            }
        }
    }

    ChannelPtr chan;
    {
        std::lock_guard<std::mutex> lock(this->channelMutex);

        chan = this->createChannel(channelName, true);
        auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!chan || !twitchChannel)
        {
            return Channel::getEmpty();
        }

        this->anonymousChannels.insert(channelName, chan);
        this->signalHolder.managedConnect(
            twitchChannel->destroyed, [this, channelName] {
                qCDebug(chatterinoIrc)
                    << "[TwitchIrcServer::addAnonymousChannel]" << channelName
                    << "was destroyed";
                this->anonymousChannels.remove(channelName);
                this->partUnusedAnonymousChannel(channelName);
            });
    }

    this->ensureAnonymousReadConnection();

    {
        std::lock_guard<std::mutex> lock2(this->connectionMutex_);

        if (this->anonymousReadConnection_ &&
            this->anonymousReadConnection_->isConnected() &&
            !channelName.startsWith("/"))
        {
            this->anonymousJoinBucket_->send(channelName);
        }
    }

    return chan;
}

ChannelPtr TwitchIrcServer::getChannelOrEmpty(const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    if (receivingChannel && receivingChannel->getName() == channelName)
    {
        return receivingChannel;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    ChannelPtr chan = this->getCustomChannel(channelName);
    if (chan)
    {
        return chan;
    }

    if (preferAnonymousTwitchChannels)
    {
        auto anonymous = this->anonymousChannels.find(channelName);
        if (anonymous != this->anonymousChannels.end())
        {
            chan = anonymous.value().lock();

            if (chan)
            {
                return chan;
            }
        }
    }

    auto it = this->channels.find(channelName);
    if (it != this->channels.end())
    {
        chan = it.value().lock();

        if (chan)
        {
            return chan;
        }
    }

    if (!preferAnonymousTwitchChannels)
    {
        auto anonymous = this->anonymousChannels.find(channelName);
        if (anonymous != this->anonymousChannels.end())
        {
            chan = anonymous.value().lock();

            if (chan)
            {
                return chan;
            }
        }
    }

    return Channel::getEmpty();
}

ChannelPtr TwitchIrcServer::getAnonymousChannelOrEmpty(
    const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    if (receivingChannel && receivingChannel->getName() == channelName)
    {
        return receivingChannel;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    auto it = this->anonymousChannels.find(channelName);
    if (it != this->anonymousChannels.end())
    {
        if (auto chan = it.value().lock())
        {
            return chan;
        }
    }

    if (auto channel = this->channels.value(channelName).lock(); channel)
    {
        if (auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
            tc && tc->anonymousFallback_)
        {
            return channel;
        }
    }

    return Channel::getEmpty();
}

void TwitchIrcServer::reconnectChannel(
    const std::shared_ptr<TwitchChannel> &channel)
{
    assertInGuiThread();

    if (!channel)
    {
        return;
    }

    const auto channelName = cleanChannelName(channel->getName());
    if (channelName.isEmpty() || channelName.startsWith('/'))
    {
        return;
    }

    const bool anonymous = channel->isAnonymous();
    {
        std::lock_guard<std::mutex> lock(this->channelMutex);
        const auto &channelMap =
            anonymous ? this->anonymousChannels : this->channels;
        const auto it = channelMap.find(channelName);
        const auto current = it == channelMap.end() ? nullptr : it->lock();
        if (!current || current.get() != channel.get())
        {
            return;
        }
        if (channel->hasExpectedReconnectPart())
        {
            return;
        }
    }

    if (channel->anonymousFallback_)
    {
        channel->setAnonymousFallback(false);
        this->partUnusedAnonymousChannel(channelName);
    }

    this->cancelJoinAttempt(channelName, anonymous);

    bool connectionReady = false;
    std::uint64_t expectedReconnectGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(this->connectionMutex_);
        auto *connection = anonymous ? this->anonymousReadConnection_.get()
                                     : this->readConnection_.get();
        connectionReady = connection && connection->isConnected();
        if (connectionReady)
        {
            expectedReconnectGeneration = channel->expectReconnectPart();
            connection->sendRaw("PART #" + channelName);
        }
    }

    if (!connectionReady)
    {
        if (anonymous)
        {
            this->reconnectAnonymousChannels();
        }
        else
        {
            this->connect();
        }
        return;
    }

    QTimer::singleShot(
        250, this,
        [this, weak = std::weak_ptr(channel), channelName, anonymous] {
            auto current = weak.lock();
            if (!current)
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(this->channelMutex);
                const auto &channelMap =
                    anonymous ? this->anonymousChannels : this->channels;
                const auto it = channelMap.find(channelName);
                const auto mapped =
                    it == channelMap.end() ? nullptr : it->lock();
                if (!mapped || mapped.get() != current.get())
                {
                    return;
                }
            }

            bool connectionReady = false;
            {
                std::lock_guard<std::mutex> lock(this->connectionMutex_);
                auto *connection = anonymous
                                       ? this->anonymousReadConnection_.get()
                                       : this->readConnection_.get();
                connectionReady = connection && connection->isConnected();
                if (connectionReady)
                {
                    auto *bucket = anonymous ? this->anonymousJoinBucket_.get()
                                             : this->joinBucket_.get();
                    if (bucket)
                    {
                        bucket->send(channelName);
                        return;
                    }
                    connectionReady = false;
                }
            }

            if (anonymous)
            {
                this->reconnectAnonymousChannels();
            }
            else if (!connectionReady)
            {
                this->connect();
            }
        });

    QTimer::singleShot(
        10000, this,
        [weak = std::weak_ptr(channel), expectedReconnectGeneration] {
            if (auto current = weak.lock())
            {
                current->clearExpectedReconnectPart(
                    expectedReconnectGeneration);
            }
        });
}

void TwitchIrcServer::reconnectAnonymousChannels()
{
    if (this->readChannels(true).empty())
    {
        return;
    }

    this->clearJoinAttempts(true);

    {
        std::lock_guard<std::mutex> lock(this->connectionMutex_);
        this->anonymousReadConnectionStarted_ = false;
        if (this->anonymousReadConnection_)
        {
            this->anonymousReadConnection_->close();
        }
    }

    QTimer::singleShot(0, this, [this] {
        this->ensureAnonymousReadConnection();
    });
}

void TwitchIrcServer::open(ConnectionType type)
{
    std::lock_guard<std::mutex> lock(this->connectionMutex_);

    if (type == ConnectionType::Write)
    {
        this->writeConnection_->open();
    }
    if (type == ConnectionType::Read)
    {
        this->readConnection_->open();
    }
    if (type == ConnectionType::AnonymousRead)
    {
        this->anonymousReadConnection_->open();
    }
}

}
