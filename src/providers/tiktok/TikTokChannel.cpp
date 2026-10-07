#include "providers/tiktok/TikTokChannel.hpp"

#include "Application.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/tiktok/TikTokChatServer.hpp"
#include "providers/tiktok/TikTokMessageBuilder.hpp"
#include "providers/tiktok/TikTokText.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"

#include <QRandomGenerator>

#include <algorithm>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

TikTokChannel::TikTokChannel(QString handle, TikTokChatServer &server)
    : Channel(handle, Type::TikTok)
    , ChannelChatters(static_cast<Channel &>(*this))
    , server_(server)
    , channelAvatar_(std::make_shared<ChannelAvatarSource>())
{
    this->setMentionFlag(MessageElementFlag::NoUsernamePaint);
    this->probeTimer_.setSingleShot(true);
    QObject::connect(&this->probeTimer_, &QTimer::timeout, &this->lifetime_,
                     [this] {
                         if (auto self = this->weakFromThis().lock())
                         {
                             this->server_.resolve(self);
                         }
                     });
    this->heartbeatTimer_.setInterval(10000);
    QObject::connect(
        &this->heartbeatTimer_, &QTimer::timeout, &this->lifetime_, [this] {
            if (this->lastTraffic_.isValid() &&
                this->lastTraffic_.elapsed() > 40000)
            {
                this->retry(u"TikTok chat stopped responding. Reconnecting…"_s);
                return;
            }
            this->socket_.sendBinary(
                tiktok::heartbeat(this->room_.id.toULongLong()));
        });
}

TikTokChannel::~TikTokChannel()
{
    this->stop();
}

std::weak_ptr<TikTokChannel> TikTokChannel::weakFromThis()
{
    return std::static_pointer_cast<TikTokChannel>(this->shared_from_this());
}

const QString &TikTokChannel::getDisplayName() const
{
    return this->getName();
}

bool TikTokChannel::isLive() const
{
    return this->room_.live;
}

bool TikTokChannel::isWritable() const
{
    return this->room_.live;
}

bool TikTokChannel::canSendMessage() const
{
    return !this->stopped_ && this->room_.live && this->server_.canSend(*this);
}

bool TikTokChannel::canReconnect() const
{
    return true;
}

QString TikTokChannel::getCurrentStreamID() const
{
    return this->room_.id;
}

const TikTokRoom &TikTokChannel::room() const
{
    return this->room_;
}

std::shared_ptr<ChannelAvatarSource> TikTokChannel::channelAvatar() const
{
    return this->channelAvatar_;
}

QUrl TikTokChannel::browserUrl() const
{
    auto url = tikTokProfileUrl(this->getName());
    url.setPath(url.path() + u"/live"_s);
    return url;
}

std::optional<TikTokAuthor> TikTokChannel::author(QStringView id) const
{
    const auto it = this->authors_.constFind(id.toString());
    if (it != this->authors_.cend())
    {
        return *it;
    }
    for (const auto &author : this->authors_)
    {
        if (!id.isEmpty() && !author.handle.isEmpty() &&
            id.compare(author.handle, Qt::CaseInsensitive) == 0)
        {
            return author;
        }
    }
    return std::nullopt;
}

void TikTokChannel::start()
{
    this->stopped_ = false;
    this->probeTimer_.start(0);
}

void TikTokChannel::stop()
{
    this->stopped_ = true;
    ++this->resolutionGeneration_;
    this->resolvePending_ = false;
    this->probeTimer_.stop();
    this->closeSocket();
}

void TikTokChannel::reconnect()
{
    this->stop();
    this->failures_ = 0;
    this->endedRoomID_.clear();
    this->start();
}

void TikTokChannel::closeSocket()
{
    ++this->generation_;
    this->socket_.close();
    this->socket_ = {};
    this->heartbeatTimer_.stop();
    this->connected_ = false;
}

void TikTokChannel::openSocket()
{
    this->closeSocket();
    auto self = this->weakFromThis().lock();
    this->socket_ = this->server_.connectRoom(self, this->generation_);
    if (this->socket_.isValid())
    {
        this->lastTraffic_.start();
        this->heartbeatTimer_.start();
    }
}

void TikTokChannel::applyRoom(TikTokRoom room)
{
    if (this->connected_ && room.live && room.id != this->room_.id &&
        this->lastTraffic_.isValid() && this->lastTraffic_.elapsed() < 40000)
    {
        this->probeTimer_.start(60000);
        return;
    }
    if (!this->endedRoomID_.isEmpty() && room.id == this->endedRoomID_)
    {
        room.live = false;
        room.id.clear();
    }
    const bool changedLive = this->room_.live != room.live;
    const bool changedRoom = this->room_.id != room.id;
    if (changedRoom)
    {
        this->closeSocket();
        this->seen_.clear();
        this->seenOrder_.clear();
        this->authors_.clear();
        this->authorOrder_.clear();
        this->deletedMessages_.clear();
        this->deletedMessageOrder_.clear();
    }

    if (!changedRoom && room.viewers < 0)
    {
        room.viewers = this->room_.viewers;
    }
    if (!changedRoom && room.live)
    {
        room.paused = this->room_.paused;
    }
    this->room_ = std::move(room);
    if (!this->room_.avatarUrl.isEmpty())
    {
        this->channelAvatar_->setAvatarUrl(this->room_.avatarUrl);
    }
    if (changedLive)
    {
        this->liveStatusChanged.invoke();
        this->userStateChanged.invoke();
    }
    this->streamDataChanged.invoke();
    if (this->room_.live)
    {
        if (!this->socket_.isValid())
        {
            this->openSocket();
        }
    }
    else
    {
        this->closeSocket();
        this->failures_ = 0;
        this->notice(
            u"This TikTok channel is offline. Waiting for a live broadcast."_s);
    }
    this->probeTimer_.start(60000 + QRandomGenerator::global()->bounded(10000));
}

void TikTokChannel::resolutionFailed(const QString &error)
{
    if (this->connected_)
    {
        this->probeTimer_.start(120000);
        return;
    }
    this->retry(error);
}

void TikTokChannel::retry(const QString &error)
{
    this->closeSocket();
    this->notice(error);
    this->failures_ = std::min(this->failures_ + 1, 6);
    const auto delay = std::min(300000, 5000 * (1 << this->failures_));
    this->probeTimer_.start(delay + QRandomGenerator::global()->bounded(2000));
}

void TikTokChannel::notice(const QString &text)
{
    if (text != this->lastNotice_)
    {
        this->lastNotice_ = text;
        this->addSystemMessage(text);
    }
}

void TikTokChannel::socketOpened(quint64 generation)
{
    if (generation != this->generation_ || this->stopped_)
    {
        return;
    }
    const auto id = this->room_.id.toULongLong();
    this->socket_.sendBinary(tiktok::heartbeat(id));
    this->socket_.sendBinary(tiktok::enterRoom(id));
}

void TikTokChannel::socketClosed(quint64 generation)
{
    if (generation == this->generation_ && !this->stopped_)
    {
        this->retry(u"TikTok chat disconnected. Reconnecting…"_s);
    }
}

void TikTokChannel::protocolFailed(quint64 generation)
{
    if (generation == this->generation_ && !this->stopped_)
    {
        this->retry(
            u"TikTok sent an unsupported chat response. Retrying…"_s);
    }
}

TikTokAuthor TikTokChannel::rememberAuthor(TikTokAuthor author)
{
    if (author.id.isEmpty())
    {
        return author;
    }
    if (!this->authors_.contains(author.id))
    {
        this->authorOrder_.enqueue(author.id);
    }
    if (const auto previous = this->authors_.constFind(author.id);
        previous != this->authors_.cend())
    {
        if (author.handle.isEmpty())
        {
            author.handle = previous->handle;
        }
        if (author.displayName.isEmpty())
        {
            author.displayName = previous->displayName;
        }
        if (author.avatarUrl.isEmpty())
        {
            author.avatarUrl = previous->avatarUrl;
        }
        if (author.bio.isEmpty())
        {
            author.bio = previous->bio;
        }
    }
    this->authors_.insert(author.id, author);
    while (this->authorOrder_.size() > 512)
    {
        this->authors_.remove(this->authorOrder_.dequeue());
    }
    return author;
}

void TikTokChannel::rememberEvent(const QString &key)
{
    if (key.isEmpty() || this->seen_.contains(key))
    {
        return;
    }
    this->seen_.insert(key);
    this->seenOrder_.enqueue(key);
    while (this->seenOrder_.size() > 4096)
    {
        this->seen_.remove(this->seenOrder_.dequeue());
    }
}

void TikTokChannel::receive(quint64 generation, tiktok::Batch batch)
{
    if (generation != this->generation_ || this->stopped_)
    {
        return;
    }
    this->lastTraffic_.restart();
    if (!batch.acknowledgement.isEmpty())
    {
        this->socket_.sendBinary(batch.acknowledgement);
    }
    if (batch.roomTraffic && !this->connected_)
    {
        this->connected_ = true;
        this->failures_ = 0;
        this->notice(u"Connected to TikTok chat."_s);
    }
    std::vector<MessagePtr> history;
    bool streamDataChanged = false;
    auto flushHistory = [this, &history] {
        if (history.empty())
        {
            return;
        }
        std::stable_sort(
            history.begin(), history.end(), [](const auto &a, const auto &b) {
                return a->serverReceivedTime < b->serverReceivedTime;
            });
        this->fillInMissingMessages(history);
        history.clear();
    };
    for (auto &event : batch.events)
    {
        if (!event.roomID.isEmpty() && event.roomID != this->room_.id)
        {
            continue;
        }
        if (event.kind == TikTokEvent::Kind::Viewers)
        {
            const auto viewers =
                qint64(std::min<quint64>(event.value, 1000000000));
            if (viewers != this->room_.viewers)
            {
                this->room_.viewers = viewers;
                streamDataChanged = true;
            }
            continue;
        }
        if (event.kind == TikTokEvent::Kind::Control)
        {
            flushHistory();
            if (event.value == 3 || event.value == 4)
            {
                this->endedRoomID_ = this->room_.id;
                this->room_.live = false;
                this->room_.paused = false;
                this->closeSocket();
                this->notice(event.value == 4
                                 ? u"The TikTok broadcast was suspended."_s
                                 : u"The TikTok broadcast ended."_s);
                this->liveStatusChanged.invoke();
                this->streamDataChanged.invoke();
                this->userStateChanged.invoke();
                this->probeTimer_.start(60000);
                return;
            }
            if ((event.value == 1 || event.value == 2) &&
                this->room_.paused != (event.value == 1))
            {
                this->room_.paused = event.value == 1;
                streamDataChanged = true;
                this->notice(this->room_.paused
                                 ? u"The TikTok broadcast is paused."_s
                                 : u"The TikTok broadcast resumed."_s);
            }
            continue;
        }
        if (event.kind == TikTokEvent::Kind::AccessDenied)
        {
            flushHistory();
            this->stop();
            this->notice(
                u"TikTok is not allowing anonymous access to this live chat."_s);
            this->userStateChanged.invoke();
            if (streamDataChanged)
            {
                this->streamDataChanged.invoke();
            }
            return;
        }
        if (event.kind == TikTokEvent::Kind::Delete)
        {
            flushHistory();
            for (const auto &id : event.deletedMessages)
            {
                if (!this->deletedMessages_.contains(id))
                {
                    this->deletedMessages_.insert(id);
                    this->deletedMessageOrder_.enqueue(id);
                    while (this->deletedMessageOrder_.size() > 4096)
                    {
                        this->deletedMessages_.remove(
                            this->deletedMessageOrder_.dequeue());
                    }
                }
            }
            const QSet<QString> deletedUsers(event.deletedUsers.begin(),
                                             event.deletedUsers.end());
            const auto messages = this->getMessageSnapshot();
            for (size_t index = 0; index < messages.size(); ++index)
            {
                const auto &message = messages[index];
                if (!message->flags.has(MessageFlag::Disabled) &&
                    (this->deletedMessages_.contains(message->id) ||
                     deletedUsers.contains(message->userID)))
                {
                    message->flags.set(MessageFlag::Disabled,
                                       MessageFlag::InvalidReplyTarget);

                    this->replaceMessage(index, message, message);
                    getApp()->getTwitch()->getMentionsChannel()->replaceMessage(
                        message, message);
                }
            }
            continue;
        }

        QString giftKey;
        if (event.kind == TikTokEvent::Kind::Gift && event.gift &&
            !event.gift->groupID.isEmpty() && !event.gift->id.isEmpty())
        {
            giftKey = u"gift:%1:%2:%3:%4"_s.arg(event.author.id, event.gift->id,
                                                event.gift->recipient.id,
                                                event.gift->groupID);
        }

        event.author = this->rememberAuthor(std::move(event.author));
        if (event.gift)
        {
            event.gift->recipient =
                this->rememberAuthor(std::move(event.gift->recipient));
        }
        if ((!event.id.isEmpty() && this->seen_.contains(event.id)) ||
            (!giftKey.isEmpty() && this->seen_.contains(giftKey)))
        {
            continue;
        }
        auto [message, alert] = makeTikTokMessage(this, event);
        if (!message)
        {
            continue;
        }
        this->rememberEvent(event.id);
        this->rememberEvent(giftKey);

        if (this->deletedMessages_.contains(event.id))
        {
            message->flags.set(MessageFlag::Disabled,
                               MessageFlag::InvalidReplyTarget,
                               MessageFlag::DoNotTriggerNotification);
            message->flags.unset(MessageFlag::ShowInMentions);
            alert = {};
        }
        if (event.historical)
        {
            history.push_back(std::move(message));
            continue;
        }
        flushHistory();
        this->addMessage(message, MessageContext::Original);
        if (message->flags.has(MessageFlag::ShowInMentions))
        {
            getApp()->getTwitch()->getMentionsChannel()->addMessage(
                message, MessageContext::Repost);
        }
        MessageBuilder::triggerHighlights(this, message, alert);
    }
    flushHistory();
    if (streamDataChanged)
    {
        this->streamDataChanged.invoke();
    }
}

void TikTokChannel::sendMessage(const QString &message)
{
    if (message.trimmed().isEmpty())
    {
        return;
    }
    if (this->stopped_)
    {
        this->addSystemMessage(
            u"Reconnect this TikTok chat before sending messages."_s);
        return;
    }
    if (message.size() > 32768 ||
        tiktok::livetext::analyzeEditorText(message).count >
            TIKTOK_MESSAGE_LIMIT)
    {
        this->addSystemMessage(
            u"TikTok messages are limited to %1 characters."_s.arg(
                TIKTOK_MESSAGE_LIMIT));
        return;
    }
    if (!this->room_.live)
    {
        this->addSystemMessage(u"This TikTok channel has no active chat."_s);
        return;
    }
    this->server_.send(this->weakFromThis().lock(), message);
}

}
