#include "providers/youtube/YouTubeChannel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/recording/ChatRecordingMessage.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeAccountManager.hpp"
#include "providers/youtube/YouTubeChatServer.hpp"
#include "providers/youtube/YouTubeEmotes.hpp"
#include "providers/youtube/YouTubeMessageBuilder.hpp"
#include "singletons/Settings.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <unordered_set>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

namespace {

constexpr std::size_t MAX_PENDING_EMOJI_UPGRADES = 256;
constexpr std::size_t MAX_PENDING_MODERATION_ECHOES = 256;
constexpr auto MODERATION_ECHO_WINDOW = std::chrono::seconds(8);
constexpr auto BAN_DELETE_SETTLE_TIME = std::chrono::milliseconds(1500);

QString moderationEventKey(const YouTubeMessage &message)
{
    if (message.kind != YouTubeMessageKind::UserBanned || !message.ban ||
        message.ban->targetChannelId.isEmpty())
    {
        return {};
    }

    return message.ban->targetChannelId + u':' +
           QString::number(static_cast<int>(message.ban->kind)) + u':' +
           QString::number(message.ban->duration.count());
}

QString moderationPurgeKey(QStringView channelID)
{
    return channelID.isEmpty() ? QString{}
                               : u"purge:"_s + channelID.toString();
}

bool containsPotentialCustomEmoji(QStringView text)
{
    qsizetype opening = text.indexOf(u':');
    while (opening >= 0)
    {
        const auto closing = text.indexOf(u':', opening + 1);
        if (closing < 0)
        {
            return false;
        }
        const auto length = closing - opening + 1;
        if (length > 2 && length <= 80)
        {
            const auto inner = text.sliced(opening + 1, length - 2);
            if (std::ranges::none_of(inner, [](QChar c) {
                    return c.isSpace();
                }))
            {
                return true;
            }
        }
        opening = text.indexOf(u':', closing + 1);
    }
    return false;
}

QString initialDisplayName(QStringView stableSource)
{
    constexpr QStringView HANDLE_PREFIX = u"handle:";
    constexpr QStringView VIDEO_PREFIX = u"video:";
    constexpr QStringView CHANNEL_PREFIX = u"channel:";

    if (stableSource.startsWith(HANDLE_PREFIX, Qt::CaseInsensitive))
    {
        auto handle = stableSource.sliced(HANDLE_PREFIX.size()).toString();
        if (handle.startsWith(u'@'))
        {
            handle.remove(0, 1);
        }
        return handle;
    }
    if (stableSource.startsWith(VIDEO_PREFIX, Qt::CaseInsensitive))
    {
        return stableSource.sliced(VIDEO_PREFIX.size()).toString();
    }
    if (stableSource.startsWith(CHANNEL_PREFIX, Qt::CaseInsensitive))
    {
        return stableSource.sliced(CHANNEL_PREFIX.size()).toString();
    }
    return stableSource.toString();
}

}

YouTubeChannel::YouTubeChannel(QString stableSource, YouTubeChatServer &server)
    : Channel(stableSource, Type::YouTube)
    , ChannelChatters(static_cast<Channel &>(*this))
    , server_(server)
    , displayName_(initialDisplayName(stableSource))
{
    this->youtubeEmotes_ = YouTubeEmotes::globalEmotes();
}

YouTubeChannel::~YouTubeChannel() = default;

std::shared_ptr<YouTubeChannel> YouTubeChannel::sharedFromThis()
{
    return std::static_pointer_cast<YouTubeChannel>(this->shared_from_this());
}

std::weak_ptr<YouTubeChannel> YouTubeChannel::weakFromThis()
{
    return this->sharedFromThis();
}

const QString &YouTubeChannel::getDisplayName() const
{
    return this->displayName_;
}

const QString &YouTubeChannel::getLocalizedName() const
{
    return this->displayName_;
}

bool YouTubeChannel::canSendMessage() const
{
    return !this->liveChatID_.isEmpty() &&
           getApp()->getAccounts()->youtube.isLoggedIn();
}

bool YouTubeChannel::isWritable() const
{
    return !this->liveChatID_.isEmpty();
}

void YouTubeChannel::sendMessage(const QString &message)
{
    const auto text = message.trimmed();
    if (text.isEmpty())
    {
        return;
    }
    if (text.size() > YOUTUBE_MESSAGE_LIMIT)
    {
        this->addSystemMessage(
            u"YouTube messages are limited to %1 characters."_s.arg(
                YOUTUBE_MESSAGE_LIMIT));
        return;
    }
    if (this->liveChatID_.isEmpty())
    {
        this->addSystemMessage(u"This YouTube channel has no active chat."_s);
        return;
    }
    if (!getApp()->getAccounts()->youtube.isLoggedIn())
    {
        this->addLoginMessage();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (this->lastSend_.time_since_epoch().count() != 0 &&
        now - this->lastSend_ < std::chrono::milliseconds(500))
    {
        this->addSystemMessage(
            u"You are sending YouTube messages too quickly."_s);
        return;
    }
    this->lastSend_ = now;
    if (!this->customEmojiSendNoticeShown_ &&
        this->containsKnownCustomEmote(text))
    {
        this->customEmojiSendNoticeShown_ = true;
        this->addSystemMessage(
            u"Emoji shortcuts may appear as text on youtube.com."_s);
    }
    const auto liveChatID = this->liveChatID_;
    const auto account = getApp()->getAccounts()->youtube.current();
    auto handle = visibleYouTubeName(account->handle());
    const auto displayName = visibleYouTubeName(account->displayName());
    if (!handle.isEmpty() &&
        handle.compare(displayName, Qt::CaseInsensitive) == 0)
    {
        handle = displayName;
    }

    if (const auto observed = YouTubeMessageBuilder::cachedAuthorForChannel(
            this->getName(), account->channelID());
        observed && !handle.isEmpty() &&
        handle.compare(visibleYouTubeName(observed->handle),
                       Qt::CaseInsensitive) == 0)
    {
        handle = visibleYouTubeName(observed->handle);
    }
    const YouTubeAuthor sender{
        .channelId = account->channelID(),
        .handle = handle,
        .displayName = handle.isEmpty() ? account->displayName() : handle,
        .avatarUrl = account->avatarUrl(),
        .isOwner = this->isBroadcaster() || this->observedOwner_,
        .isModerator = this->observedMod_,
    };
    const auto localMessageID =
        u"youtube-local:"_s +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto sentAt = QDateTime::currentDateTimeUtc();
    this->pendingSends_.track(YouTubePendingSend{
        .localMessageID = localMessageID,
        .visibleMessageID = localMessageID,
        .liveChatID = liveChatID,
        .text = text,
        .sentAt = sentAt,
        .author = sender,
        .trackedAt = std::chrono::steady_clock::now(),
    });

    YouTubeMessage localEcho{
        .id = localMessageID,
        .liveChatId = liveChatID,
        .kind = YouTubeMessageKind::Text,
        .text = text,
        .publishedAt = sentAt,
        .author = sender,
        .localEcho = true,
    };
    this->receiveMessages({std::move(localEcho)}, false);

    this->withAccessToken(
        [weak = this->weakFromThis(), text, liveChatID, sender, localMessageID](
            const QString &accessToken, UnauthorizedRetry retryUnauthorized) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (self->liveChatID_ != liveChatID)
            {
                self->failPendingMessage(
                    localMessageID,
                    u"The YouTube broadcast changed before the message could be sent. Please send it again."_s);
                return;
            }

            auto completed = [weak, liveChatID, sender, localMessageID,
                              retryUnauthorized = std::move(retryUnauthorized)](
                                 Expected<YouTubeMessage, YouTubeApiError>
                                     result) mutable {
                auto current = weak.lock();
                if (!current)
                {
                    return;
                }
                if (!result)
                {
                    if (!retryUnauthorized(result.error()))
                    {
                        current->failPendingMessage(localMessageID,
                                                    result.error().message);
                    }
                    return;
                }
                if (current->liveChatID_ != liveChatID)
                {
                    current->failPendingMessage(
                        localMessageID,
                        u"The broadcast changed before YouTube confirmed the message."_s);
                    return;
                }
                if (result->id.isEmpty())
                {
                    return;
                }

                result->author = sender;
                current->receiveAcknowledgedMessage(std::move(*result),
                                                    localMessageID);
            };
            YouTubeApi::sendMessage(liveChatID, text, accessToken,
                                    std::move(completed));
        },
        [weak = this->weakFromThis(), localMessageID](const QString &error) {
            if (auto self = weak.lock())
            {
                self->failPendingMessage(localMessageID, error);
            }
        });
}

void YouTubeChannel::receiveAcknowledgedMessage(
    YouTubeMessage message, QStringView localMessageID)
{
    if (message.id.isEmpty() || message.liveChatId != this->liveChatID_)
    {
        return;
    }

    const auto previousVisibleID =
        this->pendingSends_.acknowledge(localMessageID, message.id);
    if (!previousVisibleID)
    {
        return;
    }

    this->pendingEmojiUpgrades_.erase(*previousVisibleID);
    message.replacesExisting = true;
    message.targetMessageID = *previousVisibleID;
    this->receiveMessages({std::move(message)}, false);
}

void YouTubeChannel::failPendingMessage(QStringView localMessageID,
                                        const QString &error)
{
    const auto pending = this->pendingSends_.takeByLocalID(localMessageID);
    if (!pending)
    {
        return;
    }

    this->pendingEmojiUpgrades_.erase(pending->visibleMessageID);
    const auto existing = this->findMessageByID(pending->visibleMessageID);
    if (!existing)
    {
        this->addSystemMessage(u"Failed to send YouTube message: "_s + error);
        return;
    }

    auto failure = std::const_pointer_cast<Message>(makeSystemMessage(
        u"Failed to send YouTube message: "_s + error,
        existing->serverReceivedTime.time()));
    failure->id = existing->id;
    failure->channelName = this->getName();
    failure->platform = MessagePlatform::YouTube;
    failure->serverReceivedTime = existing->serverReceivedTime;
    failure->flags.set(MessageFlag::DoNotLog);
    this->replaceMessage(existing, failure);
    getApp()->getTwitch()->getMentionsChannel()->replaceMessage(existing,
                                                                failure);
}

void YouTubeChannel::failAllPendingMessages(const QString &error)
{
    const auto localMessageIDs =
        this->pendingSends_.unacknowledgedLocalMessageIDs();
    for (const auto &localMessageID : localMessageIDs)
    {
        this->failPendingMessage(localMessageID, error);
    }

    this->pendingSends_.clear();
}

bool YouTubeChannel::isMod() const
{
    return this->observedMod_;
}

bool YouTubeChannel::isBroadcaster() const
{
    const auto account = getApp()->getAccounts()->youtube.current();
    return !account->isAnonymous() && account->hasCredentials() &&
           !this->channelID_.isEmpty() &&
           account->channelID() == this->channelID_;
}

bool YouTubeChannel::hasModRights() const
{
    return this->isBroadcaster() || this->observedOwner_ || this->observedMod_;
}

bool YouTubeChannel::canUseModerationTools() const
{
    if (this->hasModRights())
    {
        return true;
    }
    const auto account = getApp()->getAccounts()->youtube.current();
    return !this->selfRoleKnown_ && !this->liveChatID_.isEmpty() &&
           !account->isAnonymous() && account->hasCredentials();
}

bool YouTubeChannel::isLive() const
{
    return this->live_;
}

bool YouTubeChannel::canReconnect() const
{
    return true;
}

void YouTubeChannel::reconnect()
{
    this->server_.restart(this->sharedFromThis());
}

QString YouTubeChannel::getCurrentStreamID() const
{
    return this->videoID_;
}

QUrl YouTubeChannel::browserUrl() const
{
    if (!this->videoID_.isEmpty())
    {
        QUrl url(QStringLiteral("https://www.youtube.com/watch"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("v"), this->videoID_);
        url.setQuery(query);
        return url;
    }
    if (!this->channelID_.isEmpty())
    {
        return QUrl(youtubeChannelUrl(this->channelID_));
    }

    const auto source = this->getName();
    if (source.startsWith(u"video:", Qt::CaseInsensitive))
    {
        QUrl url(QStringLiteral("https://www.youtube.com/watch"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("v"), source.sliced(6));
        url.setQuery(query);
        return url;
    }
    if (source.startsWith(u"channel:", Qt::CaseInsensitive))
    {
        return QUrl(youtubeChannelUrl(source.sliced(8)));
    }
    if (source.startsWith(u"handle:", Qt::CaseInsensitive))
    {
        auto handle = source.sliced(7);
        if (!handle.startsWith(u'@'))
        {
            handle.prepend(u'@');
        }
        return QUrl(QStringLiteral("https://www.youtube.com/") +
                    QString::fromUtf8(QUrl::toPercentEncoding(handle)));
    }
    return QUrl::fromUserInput(source);
}

const QString &YouTubeChannel::channelID() const
{
    return this->channelID_;
}

const QString &YouTubeChannel::videoID() const
{
    return this->videoID_;
}

const QString &YouTubeChannel::liveChatID() const
{
    return this->liveChatID_;
}

const QString &YouTubeChannel::streamTitle() const
{
    return this->streamTitle_;
}

std::shared_ptr<const EmoteMap> YouTubeChannel::youtubeEmotes() const
{
    return this->youtubeEmotes_ ? this->youtubeEmotes_ : EMPTY_EMOTE_MAP;
}

EmotePtr YouTubeChannel::youtubeEmote(QStringView shortcut) const
{
    const auto emotes = this->youtubeEmotes();
    if (!emotes || emotes->empty())
    {
        return nullptr;
    }
    const auto it = emotes->find(EmoteName{shortcut.toString()});
    return it == emotes->end() ? nullptr : it->second;
}

void YouTubeChannel::beginResolving()
{
    if (this->initialResolutionNoticeShown_)
    {
        return;
    }
    this->initialResolutionNoticeShown_ = true;
    this->addSystemMessage(u"connecting to YouTube"_s);
}

void YouTubeChannel::applyResolved(const YouTubeResolvedChannel &resolved)
{
    const auto oldDisplayName = this->displayName_;
    const auto oldChannelID = this->channelID_;
    const auto oldLive = this->live_;
    const bool isInitialUpdate = !this->liveStatusKnown_;
    this->liveStatusKnown_ = true;
    const auto oldLiveChatID = this->liveChatID_;
    const auto oldVideoID = this->videoID_;
    const auto oldReadTransport = this->readTransport_;

    if (!resolved.displayName.isEmpty())
    {
        this->displayName_ = resolved.displayName;
    }
    this->channelID_ = resolved.channelID;
    if (!oldChannelID.isEmpty() && oldChannelID != this->channelID_)
    {
        this->knownAuthorRoles_.clear();
    }
    this->videoID_ = resolved.videoID;
    if (oldLiveChatID != resolved.liveChatID)
    {
        ++this->moderationGeneration_;
        this->failAllPendingMessages(
            u"The YouTube broadcast changed before the message was confirmed."_s);
        this->chatEndedMessageSeen_ = false;
        this->banIDsByChannel_.clear();
        this->banCreatedAtByChannel_.clear();
        this->pendingModerationChannelIDs_.clear();
        this->restorableBanMessageIDs_.clear();
        this->pendingModerationEchoes_.clear();
    }
    this->liveChatID_ = resolved.liveChatID;
    this->streamTitle_ = resolved.streamTitle;
    this->readTransport_ = resolved.readTransport;
    this->live_ = resolved.isLive;
    this->resolutionErrorShown_ = false;
    const bool customEmotesAllowed =
        this->readTransport_ == YouTubeReadTransport::Innertube;
    if (oldVideoID != this->videoID_ ||
        oldReadTransport != this->readTransport_)
    {
        ++this->youtubeEmoteGeneration_;
        this->youtubeEmotes_ = YouTubeEmotes::globalEmotes();
        this->youtubeEmotesLoading_ = false;
        this->pendingEmojiUpgrades_.clear();
        this->youtubeEmotesChanged.invoke();
        if (customEmotesAllowed)
        {
            this->loadCustomEmotes();
        }
    }
    else if (customEmotesAllowed && !this->videoID_.isEmpty() &&
             !this->youtubeEmotesLoading_)
    {
        this->loadCustomEmotes();
    }

    if (oldDisplayName != this->displayName_)
    {
        this->displayNameChanged.invoke();
    }
    if (oldLive != this->live_)
    {
        this->liveStatusChanged.invoke();
        auto *notifications = getApp()->getNotifications();
        const auto notificationID = u":youtube:"_s + this->getName();
        if (this->live_)
        {
            notifications->notifyChannelLive({
                .channelId = notificationID,
                .channelName = this->getName(),
                .displayName = this->getDisplayName(),
                .title = this->streamTitle_,
                .isInitialUpdate = isInitialUpdate,
                .platform = Platform::YouTube,
                .url = this->browserUrl(),
                .resolvedChannelId = this->channelID_,
                .isDuplicateBroadcast = this->server_.hasLiveNotificationSibling(*this),
            });
        }
        else
        {
            notifications->notifyTwitchChannelOffline(notificationID);
        }
    }
    this->streamDataChanged.invoke();
    this->refreshAccountState();

    if (resolved.liveChatID.isEmpty())
    {
        this->connected_ = false;
        if (!this->offlineNoticeShown_)
        {
            this->offlineNoticeShown_ = true;
            this->addSystemMessage(
                u"YouTube channel is offline or chat is unavailable"_s);
        }
    }
    else
    {
        this->offlineNoticeShown_ = false;
    }
}

void YouTubeChannel::applyResolutionError(const QString &error)
{
    if (this->resolutionErrorShown_)
    {
        return;
    }
    this->resolutionErrorShown_ = true;
    this->addSystemMessage(u"could not connect to YouTube: "_s + error);
}

void YouTubeChannel::receiveMessages(
    const std::vector<YouTubeMessage> &messages, bool historical)
{
    this->learnMessageEmotes(messages);

    const auto moderationNow = std::chrono::steady_clock::now();
    std::erase_if(this->pendingModerationEchoes_,
                  [moderationNow](const auto &entry) {
                      return moderationNow - entry.receivedAt >
                             MODERATION_ECHO_WINDOW;
                  });

    const auto takePendingModerationEcho =
        [this](const QString &key) -> MessagePtr {
        if (key.isEmpty())
        {
            return {};
        }

        const auto visibleMessages = this->getMessageSnapshot();
        for (auto it = this->pendingModerationEchoes_.begin();
             it != this->pendingModerationEchoes_.end();)
        {
            if (it->key != key)
            {
                ++it;
                continue;
            }

            const auto message = it->message;
            it = this->pendingModerationEchoes_.erase(it);
            if (std::ranges::find(visibleMessages, message) !=
                visibleMessages.end())
            {
                return message;
            }
        }
        return {};
    };

    const auto rememberModerationEcho =
        [this, moderationNow](const QString &key, const QString &targetChannelID,
                              const MessagePtr &message) {
        if (key.isEmpty() || targetChannelID.isEmpty() || !message)
        {
            return;
        }
        if (this->pendingModerationEchoes_.size() >=
            MAX_PENDING_MODERATION_ECHOES)
        {
            this->pendingModerationEchoes_.pop_front();
        }
        this->pendingModerationEchoes_.push_back(
            PendingModerationEcho{key, targetChannelID, message,
                                  moderationNow});
    };

    const auto hasPendingModerationEchoForTarget =
        [this](const QString &targetChannelID) {
        return !targetChannelID.isEmpty() &&
               std::ranges::any_of(
                   this->pendingModerationEchoes_,
                   [&targetChannelID](const auto &entry) {
                       return entry.targetChannelID == targetChannelID;
                   });
    };

    std::vector<MessagePtr> history;
    std::unordered_map<QString, std::size_t> stagedIndexByID;
    std::unordered_set<QString> purgedAuthors;
    std::unordered_set<QString> bannedAuthors;
    if (historical)
    {
        history.reserve(messages.size());
        stagedIndexByID.reserve(messages.size());
    }

    const auto replaceStaged =
        [&history, &stagedIndexByID](std::size_t index,
                                    MessagePtr replacement) {
            const auto previousID = history[index]->id;
            const auto replacementID = replacement->id;
            history[index] = std::move(replacement);
            if (previousID == replacementID)
            {
                return;
            }

            const auto previous = stagedIndexByID.find(previousID);
            if (previous != stagedIndexByID.end() &&
                previous->second == index)
            {
                stagedIndexByID.erase(previous);
            }
            if (!replacementID.isEmpty())
            {
                auto [current, inserted] =
                    stagedIndexByID.try_emplace(replacementID, index);
                if (!inserted && index < current->second)
                {
                    current->second = index;
                }
            }
        };

    const auto replaceVisible =
        [this, historical](const MessagePtr &existing,
                           const MessagePtr &message,
                           const YouTubeMessage &source) {
            if (!historical && !source.localEcho)
            {
                recording::deliverLive(
                    message,
                    [&] {
                        return recording::normalizeYouTube(*this, *message,
                                                           source);
                    },
                    [&] {
                        this->replaceMessage(existing, message);
                    });
            }
            else
            {
                this->replaceMessage(existing, message);
            }
            getApp()->getTwitch()->getMentionsChannel()->replaceMessage(
                existing, message);
        };

    for (const auto &source : messages)
    {
        if (source.kind == YouTubeMessageKind::AuthorMessagesDeleted &&
            !source.targetAuthorChannelID.isEmpty())
        {
            purgedAuthors.insert(source.targetAuthorChannelID);
        }
        else if (source.kind == YouTubeMessageKind::UserBanned && source.ban &&
                 !source.ban->targetChannelId.isEmpty())
        {
            purgedAuthors.insert(source.ban->targetChannelId);
            bannedAuthors.insert(source.ban->targetChannelId);
        }
    }

    if (!purgedAuthors.empty() && !this->restorableBanMessageIDs_.empty())
    {
        std::unordered_set<QString> visibleIDs;
        for (const auto &message : this->getMessageSnapshot())
        {
            visibleIDs.insert(message->id);
        }
        for (auto it = this->restorableBanMessageIDs_.begin();
             it != this->restorableBanMessageIDs_.end();)
        {
            std::erase_if(it->second, [&visibleIDs](const auto &id) {
                return !visibleIDs.contains(id);
            });
            if (it->second.empty())
            {
                it = this->restorableBanMessageIDs_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto source : messages)
    {
        QString moderationKey;
        MessagePtr moderationReplacement;
        if (!source.author.channelId.isEmpty() &&
            source.author.roleMetadataKnown)
        {
            if (source.author.isOwner || source.author.isModerator)
            {
                this->knownAuthorRoles_.insert_or_assign(
                    source.author.channelId,
                    KnownAuthorRole{source.author.isOwner,
                                    source.author.isModerator});
            }
            else
            {
                this->knownAuthorRoles_.erase(source.author.channelId);
            }
        }
        if (source.isIgnored())
        {
            continue;
        }
        if (source.kind == YouTubeMessageKind::AuthorMessagesDeleted)
        {
            if (source.targetAuthorChannelID.isEmpty())
            {
                continue;
            }
            const auto liveMessages = this->getMessageSnapshot();
            for (const auto &candidate : liveMessages)
            {
                if (candidate->userID == source.targetAuthorChannelID)
                {
                    if (source.targetAuthorDisplayName.isEmpty())
                    {
                        source.targetAuthorDisplayName =
                            !candidate->displayName.isEmpty()
                                ? candidate->displayName
                                : candidate->loginName;
                    }
                    std::ignore =
                        this->pendingSends_.takeByMessageID(candidate->id);
                    this->pendingEmojiUpgrades_.erase(candidate->id);
                    if (!candidate->flags.has(MessageFlag::Disabled) &&
                        !candidate->id.isEmpty())
                    {
                        this->restorableBanMessageIDs_[
                                source.targetAuthorChannelID]
                            .insert(candidate->id);
                    }

                    candidate->flags.set(MessageFlag::Disabled);
                }
            }
            for (const auto &candidate : history)
            {
                if (candidate->userID == source.targetAuthorChannelID)
                {
                    if (source.targetAuthorDisplayName.isEmpty())
                    {
                        source.targetAuthorDisplayName =
                            !candidate->displayName.isEmpty()
                                ? candidate->displayName
                                : candidate->loginName;
                    }
                    if (!candidate->flags.has(MessageFlag::Disabled) &&
                        !candidate->id.isEmpty())
                    {
                        this->restorableBanMessageIDs_[
                                source.targetAuthorChannelID]
                            .insert(candidate->id);
                    }
                    candidate->flags.set(MessageFlag::Disabled);
                }
            }
            moderationKey =
                moderationPurgeKey(source.targetAuthorChannelID);
            if (historical ||
                bannedAuthors.contains(source.targetAuthorChannelID) ||
                hasPendingModerationEchoForTarget(
                    source.targetAuthorChannelID))
            {
                continue;
            }
        }
        else if (source.kind == YouTubeMessageKind::UserBanned && source.ban)
        {
            moderationKey = moderationEventKey(source);
            if (!historical && !source.localEcho)
            {
                moderationReplacement =
                    takePendingModerationEcho(moderationKey);
                if (!moderationReplacement)
                {
                    moderationReplacement = takePendingModerationEcho(
                        moderationPurgeKey(source.ban->targetChannelId));
                }
            }
        }
        if (source.isUserChatMessage() &&
            purgedAuthors.contains(source.author.channelId))
        {
            continue;
        }

        if (source.kind == YouTubeMessageKind::Text &&
            !source.replacesExisting && !source.localEcho)
        {
            if (const auto pending =
                    this->pendingSends_.takeMatching(source))
            {
                this->pendingEmojiUpgrades_.erase(
                    pending->visibleMessageID);
                source.replacesExisting = true;
                source.targetMessageID = pending->visibleMessageID;

                const auto sourceDisplayName =
                    visibleYouTubeName(source.author.displayName);
                const bool sourceOnlyHasHandle =
                    !source.author.handle.trimmed().isEmpty() &&
                    sourceDisplayName.compare(
                        visibleYouTubeName(source.author.handle),
                        Qt::CaseInsensitive) == 0;
                if ((sourceDisplayName.isEmpty() || sourceOnlyHasHandle) &&
                    !pending->author.displayName.trimmed().isEmpty())
                {
                    source.author.displayName =
                        pending->author.displayName;
                }
                if (source.author.avatarUrl.isEmpty())
                {
                    source.author.avatarUrl = pending->author.avatarUrl;
                }
                if (!source.author.roleMetadataKnown)
                {
                    source.author.isOwner = pending->author.isOwner;
                    source.author.isModerator = pending->author.isModerator;
                    source.author.roleMetadataKnown =
                        pending->author.roleMetadataKnown;
                }
                source.author.isVerified =
                    source.author.isVerified || pending->author.isVerified;
            }
        }

        const auto lookupID = source.replacesExisting &&
                                      !source.targetMessageID.isEmpty()
                                  ? source.targetMessageID
                                  : source.id;
        const auto existing = lookupID.isEmpty()
                                  ? MessagePtr{}
                                  : this->findMessageByID(lookupID);
        if (source.kind == YouTubeMessageKind::Tombstone)
        {
            std::ignore = this->pendingSends_.takeByMessageID(lookupID);
            this->pendingEmojiUpgrades_.erase(lookupID);
        }
        auto staged = stagedIndexByID.end();
        if (historical && !lookupID.isEmpty())
        {
            staged = stagedIndexByID.find(lookupID);
        }
        if (source.kind == YouTubeMessageKind::Tombstone && existing)
        {
            if (const auto hidden =
                    this->restorableBanMessageIDs_.find(existing->userID);
                hidden != this->restorableBanMessageIDs_.end())
            {
                hidden->second.erase(existing->id);
                if (hidden->second.empty())
                {
                    this->restorableBanMessageIDs_.erase(hidden);
                }
            }

            source.author.channelId = existing->userID;
            source.author.displayName = existing->displayName;
            source.author.handle = existing->loginName;
        }
        if (source.kind != YouTubeMessageKind::Tombstone &&
            !source.replacesExisting &&
            (existing || staged != stagedIndexByID.end()))
        {
            continue;
        }
        source.historical = historical;
        if (this->readTransport_ == YouTubeReadTransport::Innertube &&
            (this->youtubeEmotesLoading_ ||
             (!this->videoID_.isEmpty() && this->youtubeEmotes_->empty())) &&
            source.isUserChatMessage() && !source.id.isEmpty() &&
            containsPotentialCustomEmoji(source.text) &&
            (this->pendingEmojiUpgrades_.contains(source.id) ||
             this->pendingEmojiUpgrades_.size() < MAX_PENDING_EMOJI_UPGRADES))
        {
            this->pendingEmojiUpgrades_.insert_or_assign(source.id, source);
        }
        auto [message, alert] =
            YouTubeMessageBuilder::makeMessage(this, source);
        if (!message)
        {
            continue;
        }

        if (source.kind == YouTubeMessageKind::Tombstone && existing)
        {
            replaceVisible(existing, message, source);
            continue;
        }
        if (source.kind == YouTubeMessageKind::Tombstone &&
            staged != stagedIndexByID.end())
        {
            const auto &stagedMessage = history[staged->second];
            if (const auto hidden = this->restorableBanMessageIDs_.find(
                    stagedMessage->userID);
                hidden != this->restorableBanMessageIDs_.end())
            {
                hidden->second.erase(stagedMessage->id);
                if (hidden->second.empty())
                {
                    this->restorableBanMessageIDs_.erase(hidden);
                }
            }
            replaceStaged(staged->second, std::move(message));
            continue;
        }
        if (source.replacesExisting && existing)
        {
            const auto replacementFlags = message->flags;
            const auto replacementHighlightColor = message->highlightColor;
            message->flags = existing->flags;

            message->flags.set(
                MessageFlag::System,
                replacementFlags.has(MessageFlag::System));
            message->flags.set(
                MessageFlag::Subscription,
                replacementFlags.has(MessageFlag::Subscription));
            message->flags.set(MessageFlag::DoNotLog);
            if (replacementFlags.has(
                    MessageFlag::DoNotTriggerNotification))
            {
                message->flags.set(
                    MessageFlag::DoNotTriggerNotification);
            }
            if (!message->flags.has(MessageFlag::Highlighted) &&
                replacementFlags.has(MessageFlag::Highlighted))
            {
                message->flags.set(MessageFlag::Highlighted);
                message->highlightColor = replacementHighlightColor;
            }
            else
            {
                message->highlightColor = existing->highlightColor;
            }
            message->count = existing->count;
            message->replyThread = existing->replyThread;
            message->replyParent = existing->replyParent;
            replaceVisible(existing, message, source);
            continue;
        }
        if (source.replacesExisting && staged != stagedIndexByID.end())
        {
            replaceStaged(staged->second, std::move(message));
            continue;
        }

        if (source.kind == YouTubeMessageKind::ChatEnded)
        {
            this->chatEndedMessageSeen_ = true;
        }
        if (source.kind == YouTubeMessageKind::UserBanned && source.ban)
        {
            this->disableMessagesForBan(source.ban->targetChannelId);
            for (const auto &candidate : history)
            {
                if (candidate->userID == source.ban->targetChannelId)
                {
                    if (!candidate->flags.has(MessageFlag::Disabled) &&
                        !candidate->id.isEmpty())
                    {
                        this->restorableBanMessageIDs_[
                                source.ban->targetChannelId]
                            .insert(candidate->id);
                    }
                    candidate->flags.set(MessageFlag::Disabled);
                }
            }
        }

        if (!historical && moderationReplacement)
        {
            recording::deliverLive(
                message,
                [&] {
                    return recording::normalizeYouTube(*this, *message, source);
                },
                [&] {
                    this->replaceMessage(moderationReplacement, message);
                });
            continue;
        }

        this->applySimilarityFilters(message);
        if (!historical &&
            (!message->flags.has(MessageFlag::Similar) ||
             (!getSettings()->hideSimilar &&
              getSettings()->shownSimilarTriggerHighlights)))
        {
            MessageBuilder::triggerHighlights(this, message, alert);
        }
        if (!historical && message->flags.has(MessageFlag::Highlighted) &&
            message->flags.has(MessageFlag::ShowInMentions))
        {
            getApp()->getTwitch()->getMentionsChannel()->addMessage(
                message, MessageContext::Original);
        }
        if (historical)
        {
            const auto index = history.size();
            if (!message->id.isEmpty())
            {
                stagedIndexByID.try_emplace(message->id, index);
            }
            history.emplace_back(std::move(message));
            continue;
        }
        if (!source.localEcho)
        {
            recording::deliverLive(
                message,
                [&] {
                    return recording::normalizeYouTube(*this, *message, source);
                },
                [&] {
                    this->addMessage(message, MessageContext::Original);
                });
        }
        else
        {
            this->addMessage(message, MessageContext::Original);
        }
        if ((source.localEcho ||
             source.kind == YouTubeMessageKind::AuthorMessagesDeleted) &&
            !moderationKey.isEmpty())
        {
            const auto targetChannelID =
                source.kind == YouTubeMessageKind::AuthorMessagesDeleted
                    ? source.targetAuthorChannelID
                    : source.ban ? source.ban->targetChannelId : QString{};
            rememberModerationEcho(moderationKey, targetChannelID, message);
        }
    }

    if (!history.empty())
    {
        this->fillInMissingMessages(history);
    }
}

void YouTubeChannel::setObservedSelfRole(const YouTubeAuthor &author)
{
    const auto account = getApp()->getAccounts()->youtube.current();
    if (account->isAnonymous() || !account->hasCredentials() ||
        author.channelId != account->channelID() ||
        !author.roleMetadataKnown)
    {
        return;
    }
    const bool changed = this->observedMod_ != author.isModerator ||
                         this->observedOwner_ != author.isOwner ||
                         !this->selfRoleKnown_;
    this->observedMod_ = author.isModerator;
    this->observedOwner_ = author.isOwner;
    this->selfRoleKnown_ = true;
    if (changed)
    {
        this->userStateChanged.invoke();
        this->moderationStateChanged.invoke();
    }
}

void YouTubeChannel::setConnectionState(bool connected, const QString &detail)
{
    if (this->connected_ == connected)
    {
        return;
    }
    const bool wasConnected = this->connected_;
    this->connected_ = connected;
    recording::publicEvent(
        *this, {{"kind", connected ? "connected" : "disconnected"},
                {"text", connected ? "Chat connected" : "Chat disconnected"}});
    if (connected)
    {
        this->resolutionErrorShown_ = false;
        this->offlineNoticeShown_ = false;
        this->addSystemMessage(this->everConnected_ ? u"reconnected to YouTube"_s
                                                    : u"connected to YouTube"_s);
        this->everConnected_ = true;
    }
    else if (wasConnected)
    {
        (void)detail;
        this->addSystemMessage(u"disconnected from YouTube, reconnecting"_s);
    }
}

QString YouTubeChannel::displayNameForUser(const QString &channelID) const
{
    if (channelID.isEmpty())
    {
        return {};
    }
    const auto messages = this->getMessageSnapshot();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        const auto &message = *it;
        if (message->userID != channelID)
        {
            continue;
        }
        if (!message->displayName.isEmpty())
        {
            return message->displayName;
        }
        if (!message->loginName.isEmpty())
        {
            return message->loginName;
        }
    }
    return {};
}

void YouTubeChannel::markChatEnded()
{
    const bool wasLive = this->live_;
    this->failAllPendingMessages(
        u"YouTube chat ended before the message was confirmed."_s);
    this->live_ = false;
    this->connected_ = false;
    if (wasLive)
    {
        recording::publicEvent(*this, {{"kind", "broadcastEnded"},
                                       {"text", "YouTube chat ended"}});
    }
    this->liveChatID_.clear();
    if (wasLive)
    {
        this->liveStatusChanged.invoke();
        getApp()->getNotifications()->notifyTwitchChannelOffline(
            u":youtube:"_s + this->getName());
    }
    if (!this->chatEndedMessageSeen_)
    {
        this->addSystemMessage(u"YouTube chat has ended."_s);
    }
}

void YouTubeChannel::refreshAccountState(bool recheckRole)
{
    const auto account = getApp()->getAccounts()->youtube.current();
    const auto activeAccountID =
        !account->isAnonymous() && account->hasCredentials()
            ? account->channelID()
            : QString{};
    const bool identityChanged =
        this->roleAccountChannelID_ != activeAccountID ||
        this->roleTargetChannelID_ != this->channelID_;
    if (identityChanged)
    {
        ++this->moderationGeneration_;
        this->roleAccountChannelID_ = activeAccountID;
        this->roleTargetChannelID_ = this->channelID_;
        this->banIDsByChannel_.clear();
        this->banCreatedAtByChannel_.clear();
        this->pendingModerationChannelIDs_.clear();
        this->restorableBanMessageIDs_.clear();
    }
    const auto oldOwner = this->observedOwner_;
    const auto oldMod = this->observedMod_;
    const auto oldKnown = this->selfRoleKnown_;
    if (identityChanged || recheckRole)
    {
        this->observedOwner_ = this->isBroadcaster();
        this->observedMod_ = false;
        this->selfRoleKnown_ = this->observedOwner_;

        if (!recheckRole && !activeAccountID.isEmpty() && !this->selfRoleKnown_)
        {
            const auto known = this->knownAuthorRoles_.find(activeAccountID);
            if (known != this->knownAuthorRoles_.end())
            {
                this->observedOwner_ = known->second.isOwner;
                this->observedMod_ = known->second.isModerator;
                this->selfRoleKnown_ = true;
            }
        }
        this->moderationStateChanged.invoke();
    }
    if (identityChanged || recheckRole ||
        oldOwner != this->observedOwner_ ||
        oldKnown != this->selfRoleKnown_ ||
        oldMod != this->observedMod_)
    {
        this->userStateChanged.invoke();
    }
}

void YouTubeChannel::disableMessagesForBan(const QString &channelID)
{
    if (channelID.isEmpty())
    {
        return;
    }

    for (const auto &message : this->getMessageSnapshot())
    {
        if (message->userID != channelID)
        {
            continue;
        }
        if (!message->flags.has(MessageFlag::Disabled) &&
            !message->id.isEmpty())
        {
            this->restorableBanMessageIDs_[channelID].insert(message->id);
        }
        message->flags.set(MessageFlag::Disabled);
    }
}

void YouTubeChannel::restoreMessagesForUser(const QString &channelID)
{
    const auto found = this->restorableBanMessageIDs_.find(channelID);
    if (found == this->restorableBanMessageIDs_.end())
    {
        return;
    }

    auto messageIDs = std::move(found->second);
    this->restorableBanMessageIDs_.erase(found);
    auto mentions = getApp()->getTwitch()->getMentionsChannel();
    for (const auto &message : this->getMessageSnapshot())
    {
        if (message->userID != channelID || message->id.isEmpty() ||
            !messageIDs.contains(message->id) ||
            !message->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        auto restored = message->clone();
        restored->flags.unset(MessageFlag::Disabled);
        this->replaceMessage(message, restored);
        if (mentions)
        {
            mentions->replaceMessage(message, restored);
        }
    }
}

void YouTubeChannel::loadCustomEmotes()
{
    if (this->readTransport_ != YouTubeReadTransport::Innertube ||
        this->videoID_.isEmpty())
    {
        return;
    }

    this->youtubeEmotesLoading_ = true;
    const auto videoID = this->videoID_;
    const auto generation = this->youtubeEmoteGeneration_;
    YouTubeEmotes::loadForVideo(
        videoID, [weak = this->weakFromThis(), videoID,
                  generation](std::shared_ptr<const EmoteMap> emotes) {
            auto self = weak.lock();
            if (!self || self->videoID_ != videoID ||
                self->youtubeEmoteGeneration_ != generation)
            {
                return;
            }

            const auto base = emotes && !emotes->empty()
                                  ? std::move(emotes)
                                  : YouTubeEmotes::globalEmotes();
            const auto current = self->youtubeEmotes();
            if (current == YouTubeEmotes::globalEmotes())
            {
                self->youtubeEmotes_ = base;
            }
            else
            {
                auto merged = std::make_shared<EmoteMap>(*base);
                for (const auto &[name, emote] : *current)
                {
                    merged->insert_or_assign(name, emote);
                }
                self->youtubeEmotes_ = std::move(merged);
            }
            self->youtubeEmotesLoading_ = false;
            self->youtubeEmotesChanged.invoke();
            self->upgradePendingCustomEmotes();
        });
}

void YouTubeChannel::learnMessageEmotes(
    const std::vector<YouTubeMessage> &messages)
{
    constexpr std::size_t MAX_CHANNEL_EMOTES = 2'048;
    std::shared_ptr<EmoteMap> updated;

    for (const auto &message : messages)
    {
        for (const auto &run : message.runs)
        {
            if (run.kind != YouTubeMessageRun::Kind::Emoji ||
                !run.customEmoji || run.emojiImageUrl.isEmpty())
            {
                continue;
            }
            for (const auto &rawShortcut : run.emojiShortcuts)
            {
                auto shortcut = rawShortcut.trimmed();
                if (shortcut.isEmpty() || shortcut.size() > 80)
                {
                    continue;
                }
                if (!shortcut.startsWith(u':'))
                {
                    shortcut.prepend(u':');
                }
                if (!shortcut.endsWith(u':'))
                {
                    shortcut.append(u':');
                }

                const EmoteName name{shortcut};
                const auto &current = updated ? *updated : *this->youtubeEmotes();
                if (current.contains(name) ||
                    current.size() >= MAX_CHANNEL_EMOTES)
                {
                    continue;
                }
                if (!updated)
                {
                    updated = std::make_shared<EmoteMap>(current);
                }
                updated->emplace(
                    name, YouTubeEmotes::makeCustomEmoji(
                              shortcut, run.emojiImageUrl, run.emojiID));
            }
        }
    }

    if (updated)
    {
        this->youtubeEmotes_ = std::move(updated);
        this->youtubeEmotesChanged.invoke();
    }
}

bool YouTubeChannel::containsKnownCustomEmote(QStringView text) const
{
    qsizetype opening = text.indexOf(u':');
    while (opening >= 0)
    {
        const auto closing = text.indexOf(u':', opening + 1);
        if (closing < 0)
        {
            return false;
        }
        const auto length = closing - opening + 1;
        if (length > 2 && length <= 80 &&
            this->youtubeEmote(text.sliced(opening, length)))
        {
            return true;
        }
        opening = text.indexOf(u':', closing + 1);
    }
    return false;
}

void YouTubeChannel::upgradePendingCustomEmotes()
{
    if (!this->youtubeEmotes_ || this->youtubeEmotes_->empty())
    {
        return;
    }

    auto pending = std::move(this->pendingEmojiUpgrades_);
    this->pendingEmojiUpgrades_.clear();
    for (const auto &[messageID, source] : pending)
    {
        if (!this->containsKnownCustomEmote(source.text))
        {
            continue;
        }

        const auto existing = this->findMessageByID(messageID);
        if (!existing || existing->flags.has(MessageFlag::System) ||
            existing->flags.has(MessageFlag::Disabled) ||
            existing->translatedFrom)
        {
            continue;
        }

        auto rebuilt = YouTubeMessageBuilder::makeMessage(this, source);
        auto replacement = std::move(rebuilt.first);
        if (!replacement)
        {
            continue;
        }

        replacement->flags = existing->flags;
        replacement->highlightColor = existing->highlightColor;
        replacement->count = existing->count;
        replacement->replyThread = existing->replyThread;
        replacement->replyParent = existing->replyParent;
        this->replaceMessage(existing, replacement);
        getApp()->getTwitch()->getMentionsChannel()->replaceMessage(
            existing, replacement);
    }
}

void YouTubeChannel::deleteMessage(const QString &messageID)
{
    const auto liveChatID = this->liveChatID_;
    const auto generation = this->moderationGeneration_;
    const auto actingAccountID =
        getApp()->getAccounts()->youtube.current()->channelID();
    const auto targetMessage = this->findMessageByID(messageID);
    const auto targetAuthorID =
        targetMessage ? targetMessage->userID : QString{};
    const auto targetText =
        targetMessage ? targetMessage->messageText : QString{};
    const auto targetPublishedAt =
        targetMessage ? targetMessage->serverReceivedTime : QDateTime{};
    const bool confirmsModeratorAccess =
        targetMessage && !targetMessage->userID.isEmpty() &&
        targetMessage->userID != actingAccountID;
    this->withAccessToken([weak = this->weakFromThis(), messageID, liveChatID,
                           actingAccountID, generation, confirmsModeratorAccess,
                           targetAuthorID, targetText, targetPublishedAt](
                              const QString &accessToken,
                              UnauthorizedRetry retryUnauthorized) {
        auto self = weak.lock();
        if (!self || !self->isCurrentModerationContext(generation, liveChatID,
                                                       actingAccountID))
        {
            return;
        }
        YouTubeApi::AuthenticatedCallback<void> completed =
            [weak, messageID, liveChatID, actingAccountID, generation,
             confirmsModeratorAccess,
             retryUnauthorized](Expected<void, YouTubeApiError> result) {
                auto self = weak.lock();
                if (!self || !self->isCurrentModerationContext(
                                 generation, liveChatID, actingAccountID))
                {
                    return;
                }
                if (!result)
                {
                    if (retryUnauthorized(result.error()))
                    {
                        return;
                    }
                    self->rejectModerationAccess(result.error());
                    self->addSystemMessage(
                        u"Failed to delete YouTube message: "_s +
                        result.error().message);
                    return;
                }
                self->receiveMessages(
                    {YouTubeMessage{
                        .id = messageID,
                        .liveChatId = liveChatID,
                        .kind = YouTubeMessageKind::Tombstone,
                        .publishedAt = QDateTime::currentDateTimeUtc(),
                        .targetMessageID = messageID,
                    }},
                    false);
                const auto account =
                    getApp()->getAccounts()->youtube.current();
                if (account->channelID() != actingAccountID ||
                    !account->hasCredentials())
                {
                    return;
                }
                if (confirmsModeratorAccess)
                {
                    self->setObservedSelfRole(YouTubeAuthor{
                        .channelId = actingAccountID,
                        .isModerator = true,
                        .roleMetadataKnown = true,
                    });
                }
            };

        if (targetAuthorID.isEmpty() || targetText.trimmed().isEmpty())
        {
            YouTubeApi::deleteMessage(messageID, accessToken, completed);
            return;
        }

        YouTubeApi::getRecentLiveChatMessages(
            liveChatID, accessToken,
            [weak, messageID, liveChatID, actingAccountID, generation,
             accessToken, targetAuthorID, targetText, targetPublishedAt,
             retryUnauthorized,
             completed](Expected<std::vector<YouTubeMessage>, YouTubeApiError>
                            recent) mutable {
                auto current = weak.lock();
                if (!current || !current->isCurrentModerationContext(
                                    generation, liveChatID, actingAccountID))
                {
                    return;
                }
                if (!recent)
                {
                    if (retryUnauthorized(recent.error()))
                    {
                        return;
                    }
                    completed(makeUnexpected(recent.error()));
                    return;
                }

                const auto canonicalID =
                    youtube::detail::findMatchingLiveChatMessageID(
                        *recent, targetAuthorID, targetText,
                        targetPublishedAt);
                if (canonicalID)
                {
                    YouTubeApi::deleteMessage(*canonicalID, accessToken,
                                              completed);
                    return;
                }

                YouTubeApi::deleteMessage(
                    messageID, accessToken,
                    [completed](Expected<void, YouTubeApiError> result) {
                        if (!result &&
                            youtube::detail::shouldResolveLiveChatMessageDelete(
                                result.error()))
                        {
                            completed(makeUnexpected(YouTubeApiError{
                                .message =
                                    u"YouTube could not match this message in recent chat."_s,
                                .reason = u"liveChatMessageNotFound"_s,
                                .httpStatus = 404,
                            }));
                            return;
                        }
                        completed(std::move(result));
                    });
            });
    });
}

void YouTubeChannel::moderateUser(const QString &channelID,
                                  std::optional<std::chrono::seconds> duration)
{
    if (channelID.isEmpty() || this->liveChatID_.isEmpty() ||
        this->pendingModerationChannelIDs_.contains(channelID))
    {
        return;
    }
    const auto liveChatID = this->liveChatID_;
    const auto generation = this->moderationGeneration_;
    const auto actingAccount = getApp()->getAccounts()->youtube.current();
    const auto actingAccountID = actingAccount->channelID();
    const YouTubeAuthor actingAuthor{
        .channelId = actingAccountID,
        .handle = actingAccount->handle(),
        .displayName = actingAccount->displayName(),
        .avatarUrl = actingAccount->avatarUrl(),
        .isModerator = true,
        .roleMetadataKnown = true,
    };
    this->pendingModerationChannelIDs_.insert(channelID);
    this->moderationStateChanged.invoke();
    this->withAccessToken(
        [weak = this->weakFromThis(), channelID, duration, liveChatID,
         actingAccountID, generation, actingAuthor](
            const QString &accessToken, UnauthorizedRetry retryUnauthorized) {
            auto self = weak.lock();
            if (!self || !self->isCurrentModerationContext(
                             generation, liveChatID, actingAccountID))
            {
                return;
            }
            auto markSuccess = [weak, channelID, duration, liveChatID,
                                actingAccountID, generation, actingAuthor] {
                auto current = weak.lock();
                if (!current || !current->isCurrentModerationContext(
                                    generation, liveChatID, actingAccountID))
                {
                    return;
                }
                current->receiveMessages(
                    {YouTubeMessage{
                        .id =
                            u"youtube-local-moderation:"_s +
                            QUuid::createUuid().toString(QUuid::WithoutBraces),
                        .liveChatId = liveChatID,
                        .kind = YouTubeMessageKind::UserBanned,
                        .publishedAt = QDateTime::currentDateTimeUtc(),
                        .author = actingAuthor,
                        .ban =
                            YouTubeBanDetails{
                                .targetChannelId = channelID,
                                .targetDisplayName =
                                    current->displayNameForUser(channelID),
                                .kind = duration ? YouTubeBanKind::Temporary
                                                 : YouTubeBanKind::Permanent,
                                .duration = duration.value_or(
                                    std::chrono::seconds::zero()),
                            },
                        .localEcho = true,
                    }},
                    false);
                const auto account = getApp()->getAccounts()->youtube.current();
                if (account->channelID() != actingAccountID ||
                    !account->hasCredentials())
                {
                    return;
                }
                current->setObservedSelfRole(actingAuthor);
            };
            YouTubeApi::banUser(
                liveChatID, channelID, duration, accessToken,
                [weak, channelID, liveChatID, actingAccountID, generation,
                 markSuccess, retryUnauthorized = std::move(retryUnauthorized)](
                    Expected<QString, YouTubeApiError> result) {
                    auto current = weak.lock();
                    if (!current ||
                        !current->isCurrentModerationContext(
                            generation, liveChatID, actingAccountID))
                    {
                        return;
                    }
                    if (!result)
                    {
                        if (retryUnauthorized(result.error()))
                        {
                            return;
                        }
                        current->pendingModerationChannelIDs_.erase(channelID);
                        current->rejectModerationAccess(result.error());
                        current->addSystemMessage(
                            u"YouTube moderation failed: "_s +
                            result.error().message);
                        current->moderationStateChanged.invoke();
                        return;
                    }
                    current->pendingModerationChannelIDs_.erase(channelID);
                    current->banIDsByChannel_[channelID] = *result;
                    current->banCreatedAtByChannel_[channelID] =
                        std::chrono::steady_clock::now();
                    markSuccess();
                    current->moderationStateChanged.invoke();
                });
        },
        [weak = this->weakFromThis(), channelID, liveChatID, actingAccountID,
         generation](const QString &error) {
            if (auto self = weak.lock();
                self && self->isCurrentModerationContext(generation, liveChatID,
                                                         actingAccountID))
            {
                self->pendingModerationChannelIDs_.erase(channelID);
                self->moderationStateChanged.invoke();
                self->addSystemMessage(u"YouTube moderation failed: "_s +
                                       error);
            }
        });
}

bool YouTubeChannel::canUnbanUser(const QString &channelID) const
{
    if (channelID.isEmpty())
    {
        return false;
    }
    return this->banIDsByChannel_.contains(channelID) &&
           !this->pendingModerationChannelIDs_.contains(channelID);
}

void YouTubeChannel::unbanUser(const QString &channelID)
{
    if (this->pendingModerationChannelIDs_.contains(channelID))
    {
        return;
    }
    const auto it = this->banIDsByChannel_.find(channelID);
    if (it == this->banIDsByChannel_.end())
    {
        this->addSystemMessage(
            u"YouTube only exposes the ID of bans created during this "
            u"Moltorino session, so this user cannot be unbanned here."_s);
        return;
    }
    const auto banID = it->second;
    const auto createdAt = this->banCreatedAtByChannel_.find(channelID);
    const auto banCreatedAt =
        createdAt == this->banCreatedAtByChannel_.end()
            ? std::chrono::steady_clock::time_point{}
            : createdAt->second;
    const auto liveChatID = this->liveChatID_;
    const auto generation = this->moderationGeneration_;
    const auto actingAccountID =
        getApp()->getAccounts()->youtube.current()->channelID();
    this->pendingModerationChannelIDs_.insert(channelID);
    this->moderationStateChanged.invoke();
    this->withAccessToken(
        [weak = this->weakFromThis(), channelID, banID, banCreatedAt,
         liveChatID, actingAccountID, generation](
            const QString &accessToken, UnauthorizedRetry retryUnauthorized) {
            auto current = weak.lock();
            if (!current || !current->isCurrentModerationContext(
                                generation, liveChatID, actingAccountID))
            {
                return;
            }
            auto finishUnban = [weak, channelID, banID, liveChatID,
                                actingAccountID, generation] {
                auto self = weak.lock();
                if (!self || !self->isCurrentModerationContext(
                                 generation, liveChatID, actingAccountID))
                {
                    return;
                }
                self->pendingModerationChannelIDs_.erase(channelID);
                const auto current = self->banIDsByChannel_.find(channelID);
                if (current != self->banIDsByChannel_.end() &&
                    current->second == banID)
                {
                    self->banIDsByChannel_.erase(current);
                    self->banCreatedAtByChannel_.erase(channelID);
                }
                self->restoreMessagesForUser(channelID);
                auto target = self->displayNameForUser(channelID);
                if (target.isEmpty())
                {
                    target = channelID;
                }
                self->addSystemMessage(u"Unhid "_s + target +
                                       u" from this YouTube channel."_s);

                const auto account =
                    getApp()->getAccounts()->youtube.current();
                if (account->channelID() == actingAccountID &&
                    account->hasCredentials())
                {
                    self->setObservedSelfRole(YouTubeAuthor{
                        .channelId = actingAccountID,
                        .isModerator = true,
                        .roleMetadataKnown = true,
                    });
                }
                self->moderationStateChanged.invoke();
            };
            auto completed = [weak, channelID, liveChatID, actingAccountID,
                              generation, finishUnban,
                              retryUnauthorized = std::move(retryUnauthorized)](
                                 Expected<void, YouTubeApiError> result) {
                auto self = weak.lock();
                if (!self || !self->isCurrentModerationContext(
                                 generation, liveChatID, actingAccountID))
                {
                    return;
                }
                if (!result)
                {
                    if (retryUnauthorized(result.error()))
                    {
                        return;
                    }
                    if (youtube::detail::shouldRetryLiveChatBanDelete(
                            result.error()))
                    {
                        finishUnban();
                        return;
                    }
                    self->pendingModerationChannelIDs_.erase(channelID);
                    self->moderationStateChanged.invoke();
                    self->rejectModerationAccess(result.error());
                    self->addSystemMessage(u"YouTube unban failed: "_s +
                                           result.error().message);
                    return;
                }
                finishUnban();
            };
            auto request = [weak, channelID, liveChatID, actingAccountID,
                            generation, banID, accessToken,
                            completed = std::move(completed)]() mutable {
                auto self = weak.lock();
                if (!self || !self->isCurrentModerationContext(
                                 generation, liveChatID, actingAccountID))
                {
                    return;
                }
                const auto account = getApp()->getAccounts()->youtube.current();
                if (account->channelID() != actingAccountID ||
                    !account->hasCredentials())
                {
                    self->pendingModerationChannelIDs_.erase(channelID);
                    self->moderationStateChanged.invoke();
                    return;
                }
                YouTubeApi::unbanUser(banID, accessToken, std::move(completed));
            };

            const auto readyAt = banCreatedAt + BAN_DELETE_SETTLE_TIME;
            const auto now = std::chrono::steady_clock::now();
            if (banCreatedAt.time_since_epoch().count() != 0 && now < readyAt)
            {
                QTimer::singleShot(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        readyAt - now),
                    [request = std::move(request)]() mutable {
                        request();
                    });
                return;
            }
            request();
        },
        [weak = this->weakFromThis(), channelID, liveChatID, actingAccountID,
         generation](const QString &error) {
            if (auto self = weak.lock();
                self && self->isCurrentModerationContext(generation, liveChatID,
                                                         actingAccountID))
            {
                self->pendingModerationChannelIDs_.erase(channelID);
                self->moderationStateChanged.invoke();
                self->addSystemMessage(u"YouTube unban failed: "_s + error);
            }
        });
}

bool YouTubeChannel::canModerateTarget(const YouTubeAuthor &author) const
{
    if (!this->hasModRights() || author.channelId.isEmpty() ||
        author.isOwner || author.isModerator ||
        this->pendingModerationChannelIDs_.contains(author.channelId))
    {
        return false;
    }

    const auto account = getApp()->getAccounts()->youtube.current();
    return !account->isAnonymous() &&
           author.channelId != account->channelID();
}

bool YouTubeChannel::isCurrentModerationContext(std::uint64_t generation,
                                                const QString &liveChatID,
                                                const QString &accountID) const
{
    return this->moderationGeneration_ == generation &&
           this->liveChatID_ == liveChatID &&
           getApp()->getAccounts()->youtube.current()->channelID() == accountID;
}

void YouTubeChannel::rejectModerationAccess(const YouTubeApiError &error)
{
    const auto reason = error.reason.trimmed();
    const bool permissionDenied =
        error.httpStatus == 403 &&
        (reason.compare(u"forbidden"_s, Qt::CaseInsensitive) == 0 ||
         reason.compare(u"insufficientPermissions"_s,
                        Qt::CaseInsensitive) == 0);
    if (!permissionDenied || this->isBroadcaster())
    {
        return;
    }

    const bool changed = !this->selfRoleKnown_ || this->observedMod_ ||
                         this->observedOwner_;
    this->selfRoleKnown_ = true;
    this->observedMod_ = false;
    this->observedOwner_ = false;
    if (changed)
    {
        this->userStateChanged.invoke();
        this->moderationStateChanged.invoke();
    }
}

void YouTubeChannel::addLoginMessage()
{
    auto builder = MessageBuilder();
    builder->flags.set(MessageFlag::System,
                       MessageFlag::DoNotTriggerNotification);
    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(u"Connect a YouTube account in "_s,
                                 MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(u"Accounts"_s, MessageElementFlag::Text,
                              MessageColor::Link)
        ->setLink({Link::OpenAccountsPage, {}});
    builder.emplace<TextElement>(u" to send messages."_s,
                                 MessageElementFlag::Text,
                                 MessageColor::System);
    this->addMessage(builder.release(), MessageContext::Original);
}

void YouTubeChannel::withAccessToken(AuthenticatedAction action,
                                     AuthenticationFailure failure)
{
    if (!action)
    {
        return;
    }

    const auto account = getApp()->getAccounts()->youtube.current();
    if (account->isAnonymous())
    {
        if (failure)
        {
            failure(u"The selected YouTube account is no longer connected."_s);
        }
        else
        {
            this->addLoginMessage();
        }
        return;
    }

    struct AuthState {
        std::weak_ptr<YouTubeChannel> channel;
        QString accountID;
        AuthenticatedAction action;
        AuthenticationFailure failure;
        int unauthorizedRetries = 0;
        bool failureReported = false;
        bool selectionChanged = false;
        pajlada::Signals::ScopedConnection accountChanged;

        void reportFailure(const std::shared_ptr<YouTubeChannel> &channel,
                           QString error)
        {
            if (this->failureReported)
            {
                return;
            }
            this->failureReported = true;
            if (this->failure)
            {
                auto callback = std::move(this->failure);
                callback(error);
            }
            else
            {
                channel->addSystemMessage(std::move(error));
            }
        }
    };

    auto state = std::make_shared<AuthState>(AuthState{
        .channel = this->weakFromThis(),
        .accountID = account->channelID(),
        .action = std::move(action),
        .failure = std::move(failure),
    });
    state->accountChanged =
        getApp()->getAccounts()->youtube.currentChanged.connect(
            [weakState = std::weak_ptr(state)] {
                if (auto pending = weakState.lock())
                {
                    pending->selectionChanged = true;
                }
            });
    auto attempt = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakAttempt = attempt;
    *attempt = [state, weakAttempt] {
        const auto keepAlive = weakAttempt.lock();
        auto channel = state->channel.lock();
        if (!keepAlive || !channel)
        {
            return;
        }
        auto *manager = &getApp()->getAccounts()->youtube;
        if (state->selectionChanged ||
            manager->current()->channelID() != state->accountID)
        {
            state->reportFailure(
                channel,
                u"The selected YouTube account changed before the action "
                u"could finish."_s);
            return;
        }

        manager->getAccessToken([state, keepAlive](
                                    ExpectedStr<QString> result) mutable {
            auto channel = state->channel.lock();
            if (!channel)
            {
                return;
            }
            auto *manager = &getApp()->getAccounts()->youtube;
            if (state->selectionChanged ||
                manager->current()->channelID() != state->accountID)
            {
                state->reportFailure(
                    channel, u"The selected YouTube account changed before the "
                             u"action could finish."_s);
                return;
            }
            if (!result)
            {
                state->reportFailure(
                    channel,
                    u"YouTube account needs attention: "_s + result.error());
                return;
            }

            const auto accessToken = *result;
            auto retryUnauthorized = [state, keepAlive, accessToken](
                                         const YouTubeApiError &error) -> bool {
                if (!error.isUnauthorized())
                {
                    return false;
                }
                auto channel = state->channel.lock();
                if (!channel)
                {
                    return true;
                }
                auto *manager = &getApp()->getAccounts()->youtube;
                if (state->selectionChanged ||
                    manager->current()->channelID() != state->accountID)
                {
                    state->reportFailure(
                        channel,
                        u"The selected YouTube account changed before the "
                        u"action could finish."_s);
                    return true;
                }

                if (state->unauthorizedRetries >= 1)
                {
                    if (manager->requireReconnect(state->accountID,
                                                  accessToken))
                    {
                        state->reportFailure(
                            channel,
                            u"Google rejected the refreshed YouTube "
                            u"authorization. Reconnect this account in "
                            u"Settings."_s);
                        return true;
                    }
                    return false;
                }

                std::ignore = manager->invalidateAccessToken(state->accountID,
                                                             accessToken);
                ++state->unauthorizedRetries;
                (*keepAlive)();
                return true;
            };
            state->action(accessToken, std::move(retryUnauthorized));
        });
    };
    (*attempt)();
}

}
