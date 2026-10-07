#include "providers/youtube/YouTubeMessageBuilder.hpp"

#include "Application.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeEmotes.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/Variant.hpp"

#include <QColor>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QStringBuilder>

#include <memory>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

constexpr qsizetype MAX_CACHED_YOUTUBE_AUTHORS = 4096;
constexpr qsizetype MAX_CACHED_YOUTUBE_MEMBER_BADGES = 256;
const QColor YOUTUBE_SUPPORT_ACCENT{u"#ff3d3d"_s};
const QColor YOUTUBE_SUPPORT_HIGHLIGHT{255, 61, 61, 100};

struct YouTubeAuthorCache {
    QMutex mutex;
    QHash<QString, std::shared_ptr<const YouTubeAuthor>> authorsByMessageID;
    QQueue<QString> messageInsertionOrder;
    QHash<QString, std::shared_ptr<const YouTubeAuthor>> authorsByChannelID;
    QQueue<QString> channelInsertionOrder;
    QHash<QString, EmotePtr> memberBadges;
    QQueue<QString> memberBadgeInsertionOrder;
};

YouTubeAuthorCache &authorCache()
{
    static YouTubeAuthorCache cache;
    return cache;
}

QString channelAuthorKey(QStringView sourceChannel, QStringView channelID)
{
    if (sourceChannel.isEmpty() || channelID.isEmpty())
    {
        return {};
    }
    return sourceChannel.toString() + u'\n' + channelID.toString();
}

QDateTime messageTime(const YouTubeMessage &message)
{
    if (message.publishedAt.isValid())
    {
        return message.publishedAt.toLocalTime();
    }
    return QDateTime::currentDateTime();
}

QString authorName(const YouTubeAuthor &author)
{
    const auto handle = visibleYouTubeName(author.handle);
    if (!handle.isEmpty())
    {
        return handle;
    }
    const auto displayName = visibleYouTubeName(author.displayName);
    if (!displayName.isEmpty())
    {
        return displayName;
    }
    if (!author.channelId.isEmpty())
    {
        return author.channelId;
    }
    return u"Unknown user"_s;
}

void mergeAuthor(YouTubeAuthor &existing, const YouTubeAuthor &author)
{
    auto updated = author;
    if (updated.channelId.isEmpty())
    {
        updated.channelId = existing.channelId;
    }
    if (updated.displayName.trimmed().isEmpty())
    {
        updated.displayName = existing.displayName;
    }
    if (updated.handle.trimmed().isEmpty())
    {
        updated.handle = existing.handle;
    }
    if (updated.avatarUrl.isEmpty())
    {
        updated.avatarUrl = existing.avatarUrl;
    }
    if ((!updated.roleMetadataKnown || updated.isMember) &&
        updated.membershipBadgeUrl.isEmpty())
    {
        updated.membershipBadgeUrl = existing.membershipBadgeUrl;
        if (updated.membershipBadgeTooltip.isEmpty())
        {
            updated.membershipBadgeTooltip =
                existing.membershipBadgeTooltip;
        }
    }
    if (!updated.roleMetadataKnown)
    {
        updated.isOwner = existing.isOwner;
        updated.isModerator = existing.isModerator;
        updated.isMember = existing.isMember;
        updated.roleMetadataKnown = existing.roleMetadataKnown;
    }
    updated.isVerified = updated.isVerified || existing.isVerified;
    existing = std::move(updated);
}

std::shared_ptr<const YouTubeAuthor> mergedAuthor(
    const std::shared_ptr<const YouTubeAuthor> &existing,
    const YouTubeAuthor &update)
{
    if (!existing)
    {
        return std::make_shared<const YouTubeAuthor>(update);
    }
    auto merged = *existing;
    mergeAuthor(merged, update);
    return *existing == merged
               ? existing
               : std::make_shared<const YouTubeAuthor>(std::move(merged));
}

EmotePtr makeYouTubeRoleBadge(QStringView resource1x, QStringView resource2x,
                              QStringView tooltip)
{
    const auto name = tooltip.toString();
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{name},
        .images =
            ImageSet{
                Image::fromUrl(Url{resource1x.toString()}, 1.0,
                               QSize{18, 18}),
                Image::fromUrl(Url{resource2x.toString()}, 0.5,
                               QSize{36, 36}),
            },
        .tooltip = Tooltip{name},
    });
}

const EmotePtr &youtubeOwnerBadge()
{
    static const auto badge = makeYouTubeRoleBadge(
        u":/badges/youtube-owner.svg", u":/badges/youtube-owner-36.svg",
        u"YouTube channel owner");
    return badge;
}

const EmotePtr &youtubeModeratorBadge()
{
    static const auto badge = makeYouTubeRoleBadge(
        u":/badges/youtube-moderator.svg",
        u":/badges/youtube-moderator-36.svg", u"YouTube moderator");
    return badge;
}

const EmotePtr &youtubeVerifiedBadge()
{
    static const auto badge = makeYouTubeRoleBadge(
        u":/badges/youtube-verified.svg",
        u":/badges/youtube-verified-36.svg", u"Verified YouTube channel");
    return badge;
}

EmotePtr youtubeMemberBadge(const YouTubeAuthor &author)
{
    const auto badgeUrl = author.membershipBadgeUrl;
    if (badgeUrl.isEmpty())
    {
        static const auto fallback = makeYouTubeRoleBadge(
            u":/badges/youtube-member.svg",
            u":/badges/youtube-member-36.svg", u"YouTube member");
        return fallback;
    }

    const auto tooltip = author.membershipBadgeTooltip.trimmed().isEmpty()
                             ? u"YouTube member"_s
                             : author.membershipBadgeTooltip.trimmed();
    const auto cacheKey = badgeUrl + QChar(0) + tooltip;
    auto &cache = authorCache();
    QMutexLocker locker(&cache.mutex);
    if (const auto existing = cache.memberBadges.constFind(cacheKey);
        existing != cache.memberBadges.cend())
    {
        return *existing;
    }

    locker.unlock();
    auto badge = std::make_shared<const Emote>(Emote{
        .name = EmoteName{u"youtube:member"_s},
        .images = ImageSet(
            Image::fromAutoscaledUrl(Url{badgeUrl}, 18)),
        .tooltip = Tooltip{tooltip},
        .id = EmoteId{badgeUrl},
    });
    locker.relock();
    if (const auto existing = cache.memberBadges.constFind(cacheKey);
        existing != cache.memberBadges.cend())
    {
        return *existing;
    }
    cache.memberBadges.insert(cacheKey, badge);
    cache.memberBadgeInsertionOrder.enqueue(cacheKey);
    while (cache.memberBadges.size() > MAX_CACHED_YOUTUBE_MEMBER_BADGES)
    {
        cache.memberBadges.remove(
            cache.memberBadgeInsertionOrder.dequeue());
    }
    return badge;
}

void appendRoleBadge(YouTubeMessageBuilder &builder, const EmotePtr &badge,
                     MessageElementFlag flag)
{
    auto *element = builder.emplace<BadgeElement>(badge, flag);
    element->setTooltip(badge->tooltip.string);
}

HighlightAlert processHighlights(YouTubeMessageBuilder &builder,
                                 const YouTubeMessage &source)
{
    if (getSettings()->isBlacklistedUser(builder->loginName))
    {
        return {};
    }

    MessageParseArgs args;

    args.isSubscriptionMessage =
        source.kind == YouTubeMessageKind::MembershipMilestone;
    auto [highlighted, result] = getApp()->getHighlights()->check(
        args, {}, builder->loginName, source.text, builder->flags,
        MessagePlatform::YouTube, builder->userID, builder->channelName);
    if (!highlighted)
    {
        return {};
    }

    builder->flags.set(MessageFlag::Highlighted);
    builder->highlightColor = result.color;
    if (!result.matches.empty())
    {
        builder->highlightMatches =
            std::make_shared<const std::vector<HighlightMatch>>(
                std::move(result.matches));
    }
    if (result.showInMentions)
    {
        builder->flags.set(MessageFlag::ShowInMentions);
    }

    HighlightAlert alert{
        .customSound = result.customSoundUrl.value_or(QUrl{}),
        .playSound = result.playSound,
        .windowAlert = result.alert,
    };
    if (source.historical || source.localEcho)
    {
        alert.playSound = false;
        alert.windowAlert = false;
    }
    return alert;
}

}

namespace chatterino {

YouTubeMessageBuilder::YouTubeMessageBuilder(YouTubeChannel *channel,
                                             const QDateTime &time,
                                             bool isSystemMessage)
    : channel_(channel)
{
    this->message().platform = MessagePlatform::YouTube;
    this->message().serverReceivedTime = time;
    this->message().parseTime = QTime::currentTime();

    this->message().flags.set(MessageFlag::DoNotLog);
    if (isSystemMessage)
    {
        this->message().flags.set(MessageFlag::System,
                                  MessageFlag::DoNotTriggerNotification);
    }
}

std::pair<MessagePtrMut, HighlightAlert> YouTubeMessageBuilder::makeMessage(
    YouTubeChannel *channel, const YouTubeMessage &message)
{
    if (channel == nullptr || message.isIgnored())
    {
        return {};
    }

    if (message.kind == YouTubeMessageKind::Text ||
        message.kind == YouTubeMessageKind::SuperChat ||
        (message.kind == YouTubeMessageKind::MembershipMilestone &&
         !message.text.trimmed().isEmpty()))
    {
        return makeChatMessage(channel, message);
    }

    return {makeSystemEvent(channel, message), {}};
}

std::optional<YouTubeAuthor> YouTubeMessageBuilder::cachedAuthorForMessage(
    const QString &messageID)
{
    if (messageID.isEmpty())
    {
        return std::nullopt;
    }

    auto &cache = authorCache();
    const QMutexLocker locker(&cache.mutex);
    const auto it = cache.authorsByMessageID.constFind(messageID);
    if (it == cache.authorsByMessageID.cend())
    {
        return std::nullopt;
    }
    return **it;
}

std::optional<YouTubeAuthor> YouTubeMessageBuilder::cachedAuthorForChannel(
    const QString &sourceChannel, const QString &channelID)
{
    const auto key = channelAuthorKey(sourceChannel, channelID);
    if (key.isEmpty())
    {
        return std::nullopt;
    }

    auto &cache = authorCache();
    const QMutexLocker locker(&cache.mutex);
    const auto it = cache.authorsByChannelID.constFind(key);
    if (it == cache.authorsByChannelID.cend())
    {
        return std::nullopt;
    }
    return **it;
}

void YouTubeMessageBuilder::rememberAuthor(const QString &sourceChannel,
                                           const QString &messageID,
                                           const YouTubeAuthor &author)
{
    if (messageID.isEmpty() && author.channelId.isEmpty())
    {
        return;
    }

    auto &cache = authorCache();
    const QMutexLocker locker(&cache.mutex);
    const auto channelKey = channelAuthorKey(sourceChannel, author.channelId);
    std::shared_ptr<const YouTubeAuthor> messageAuthor;
    if (!messageID.isEmpty())
    {
        auto existing = cache.authorsByMessageID.find(messageID);
        if (existing == cache.authorsByMessageID.end())
        {
            if (!channelKey.isEmpty())
            {
                const auto channelAuthor =
                    cache.authorsByChannelID.constFind(channelKey);
                if (channelAuthor != cache.authorsByChannelID.cend() &&
                    *channelAuthor && **channelAuthor == author)
                {
                    messageAuthor = *channelAuthor;
                }
            }
            if (!messageAuthor)
            {
                messageAuthor = std::make_shared<const YouTubeAuthor>(author);
            }
            cache.authorsByMessageID.insert(messageID, messageAuthor);
            cache.messageInsertionOrder.enqueue(messageID);
        }
        else
        {
            *existing = mergedAuthor(*existing, author);
            messageAuthor = *existing;
        }
    }

    if (!channelKey.isEmpty())
    {
        auto existing = cache.authorsByChannelID.find(channelKey);
        if (existing == cache.authorsByChannelID.end())
        {
            auto channelAuthor = messageAuthor;
            if (!channelAuthor || *channelAuthor != author)
            {
                channelAuthor = std::make_shared<const YouTubeAuthor>(author);
            }
            cache.authorsByChannelID.insert(channelKey,
                                            std::move(channelAuthor));
            cache.channelInsertionOrder.enqueue(channelKey);
        }
        else
        {
            auto channelAuthor = mergedAuthor(*existing, author);
            if (messageAuthor && *messageAuthor == *channelAuthor)
            {
                channelAuthor = std::move(messageAuthor);
            }
            *existing = std::move(channelAuthor);
        }
    }

    while (cache.authorsByMessageID.size() > MAX_CACHED_YOUTUBE_AUTHORS)
    {
        cache.authorsByMessageID.remove(
            cache.messageInsertionOrder.dequeue());
    }
    while (cache.authorsByChannelID.size() > MAX_CACHED_YOUTUBE_AUTHORS)
    {
        cache.authorsByChannelID.remove(
            cache.channelInsertionOrder.dequeue());
    }
}

std::pair<MessagePtrMut, HighlightAlert> YouTubeMessageBuilder::makeChatMessage(
    YouTubeChannel *channel, const YouTubeMessage &source)
{
    YouTubeMessageBuilder builder(channel, messageTime(source), false);
    builder.setCommonFields(source);

    const auto displayName = authorName(source.author);
    const auto handle = visibleYouTubeName(source.author.handle);
    builder->loginName = handle.isEmpty() ? displayName : handle;
    builder->displayName = displayName;
    builder->userID = source.author.channelId;
    rememberAuthor(channel->getName(), source.id, source.author);

    const auto colorSeed = source.author.channelId.isEmpty()
                               ? displayName
                               : source.author.channelId;
    builder->usernameColor = getRandomColor(colorSeed);
    channel->setUserColor(builder->loginName, builder->usernameColor);
    channel->addRecentChatter(builder->loginName);
    channel->setObservedSelfRole(source.author);

    builder.appendChannelName();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    if (!source.id.isEmpty() && !source.author.channelId.isEmpty())
    {
        const bool targetIsOwner = source.author.isOwner;
        const bool targetIsModerator = source.author.isModerator;
        builder.emplace<TwitchModerationElement>(
            [weak = channel->weakFromThis(), targetIsOwner,
             targetIsModerator](const QString &targetChannelID) {
                const auto current = weak.lock();
                if (!current)
                {
                    return false;
                }

                YouTubeAuthor target;
                target.channelId = targetChannelID;
                target.isOwner = targetIsOwner;
                target.isModerator = targetIsModerator;
                return current->hasModRights() &&
                       current->canModerateTarget(target);
            },
            source.author.channelId);
    }
    builder.appendRoleBadges(source.author);
    builder.appendUsername(source.author);
    if (source.kind == YouTubeMessageKind::SuperChat)
    {
        MessageElementFlags amountFlags{MessageElementFlag::Text};
        if (!source.text.trimmed().isEmpty())
        {
            amountFlags.set(MessageElementFlag::IgnoreExactMatch);
        }
        builder.emplace<TextElement>(source.amountDisplayString, amountFlags,
                                     MessageColor(YOUTUBE_SUPPORT_ACCENT),
                                     FontStyle::ChatMediumBold);
    }
    else if (source.kind == YouTubeMessageKind::MembershipMilestone &&
             !source.eventText.trimmed().isEmpty())
    {
        builder.emplace<TextElement>(
            source.eventText.trimmed() + u" ·"_s,
            MessageElementFlags{MessageElementFlag::Text,
                                MessageElementFlag::IgnoreExactMatch},
            MessageColor::System, FontStyle::ChatMediumBold);
    }
    if (source.runs.empty())
    {
        builder.appendUserText(source.text);
    }
    else
    {
        builder.appendUserRuns(source.runs);
    }

    builder
        .emplace<CircularImageElement>(
            Image::fromResourcePixmap(getResources().buttons.replyDark, 0.15),
            2, Qt::gray, MessageElementFlag::ReplyButton)
        ->setLink({Link::ReplyToMessage, source.id});

    builder->messageText = source.text;
    if (source.kind == YouTubeMessageKind::SuperChat &&
        builder->messageText.trimmed().isEmpty())
    {
        builder->messageText = source.amountDisplayString;
    }
    builder->searchText = displayName;
    if (!handle.isEmpty() &&
        handle.compare(displayName, Qt::CaseInsensitive) != 0)
    {
        builder->searchText += u" @"_s + handle;
    }
    builder->searchText += u' ';
    builder->searchText += source.author.channelId;
    builder->searchText += u": "_s;
    if (!source.amountDisplayString.isEmpty())
    {
        builder->searchText += source.amountDisplayString + u' ';
    }
    if (!source.eventText.isEmpty())
    {
        builder->searchText += source.eventText + u' ';
    }
    builder->searchText += source.text;

    auto alert = processHighlights(builder, source);
    if (source.kind == YouTubeMessageKind::SuperChat &&
        !builder->flags.has(MessageFlag::Highlighted))
    {
        builder->flags.set(MessageFlag::Highlighted);
        builder->highlightColor =
            std::make_shared<QColor>(YOUTUBE_SUPPORT_HIGHLIGHT);
    }
    if (source.kind == YouTubeMessageKind::MembershipMilestone)
    {
        builder->flags.set(MessageFlag::Subscription);
    }
    return {builder.release(), alert};
}

MessagePtrMut YouTubeMessageBuilder::makeSystemEvent(
    YouTubeChannel *channel, const YouTubeMessage &source)
{
    YouTubeMessageBuilder builder(channel, messageTime(source), true);
    builder.setCommonFields(source);
    builder.appendChannelName();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    rememberAuthor(channel->getName(), source.id, source.author);

    QString text;
    switch (source.kind)
    {
        case YouTubeMessageKind::Tombstone: {
            builder->userID = source.author.channelId;
            builder->displayName = authorName(source.author);
            builder->loginName = visibleYouTubeName(source.author.handle);
            if (builder->loginName.isEmpty())
            {
                builder->loginName = builder->displayName;
            }
            text = source.text.trimmed();
            if (text.isEmpty())
            {
                text = u"A YouTube message was deleted."_s;
            }
            builder.appendOrEmplaceText(text, MessageColor::System);
            break;
        }

        case YouTubeMessageKind::UserBanned: {
            if (!source.ban || source.ban->targetChannelId.isEmpty() ||
                source.ban->kind == YouTubeBanKind::None)
            {
                return nullptr;
            }
            builder->flags.set(MessageFlag::Timeout,
                               MessageFlag::ModerationAction);

            const auto moderator = authorName(source.author);
            if (!source.author.channelId.isEmpty() ||
                !source.author.displayName.isEmpty())
            {
                builder.appendLinkedUser(moderator, source.author.channelId,
                                         text);
            }

            const auto &ban = *source.ban;
            builder->userID = ban.targetChannelId;
            auto target = visibleYouTubeName(ban.targetDisplayName);
            if (target.trimmed().isEmpty())
            {
                target = ban.targetChannelId.isEmpty() ? u"a user"_s
                                                       : ban.targetChannelId;
            }
            builder->timeoutUser = target;
            rememberAuthor(channel->getName(), {}, YouTubeAuthor{
                                   .channelId = ban.targetChannelId,
                                   .displayName = target,
                               });

            if (ban.kind == YouTubeBanKind::Temporary)
            {
                builder.emplaceSystemTextAndUpdate(
                    text.isEmpty() ? u"A moderator put"_s : u"put"_s, text);
                builder.appendLinkedUser(target, ban.targetChannelId, text);
                builder.emplaceSystemTextAndUpdate(u"in timeout"_s, text);
                if (ban.duration.count() > 0)
                {
                    builder.emplaceSystemTextAndUpdate(u"for"_s, text);
                    builder.emplaceSystemTextAndUpdate(formatTime(ban.duration),
                                                       text);
                }
                builder->elements.back()->setTrailingSpace(false);
                if (text.endsWith(u' '))
                {
                    text.chop(1);
                }
                builder.emplaceSystemTextAndUpdate(u"."_s, text);
            }
            else
            {
                builder.emplaceSystemTextAndUpdate(
                    text.isEmpty() ? u"A moderator permanently hid"_s
                                   : u"permanently hid"_s,
                    text);
                builder.appendLinkedUser(target, ban.targetChannelId, text);
                builder.emplaceSystemTextAndUpdate(u"from this channel."_s,
                                                   text);
            }
            break;
        }

        case YouTubeMessageKind::ChatEnded:
            text = u"YouTube chat has ended."_s;
            builder.appendOrEmplaceText(text, MessageColor::System);
            break;

        case YouTubeMessageKind::AuthorMessagesDeleted: {
            if (source.targetAuthorChannelID.isEmpty())
            {
                return nullptr;
            }
            builder->flags.set(MessageFlag::ModerationAction);
            builder->userID = source.targetAuthorChannelID;
            auto target =
                visibleYouTubeName(source.targetAuthorDisplayName);
            if (target.isEmpty())
            {
                target = u"a user"_s;
            }
            builder->timeoutUser = target;
            rememberAuthor(channel->getName(), {}, YouTubeAuthor{
                                   .channelId = source.targetAuthorChannelID,
                                   .displayName = target,
                               });
            builder.emplaceSystemTextAndUpdate(
                u"A moderator removed messages from"_s, text);
            builder.appendLinkedUser(target, source.targetAuthorChannelID,
                                     text);
            builder->elements.back()->setTrailingSpace(false);
            if (text.endsWith(u' '))
            {
                text.chop(1);
            }
            builder.emplaceSystemTextAndUpdate(u"."_s, text);
            break;
        }

        case YouTubeMessageKind::NewMembership:
        case YouTubeMessageKind::MembershipMilestone:
        case YouTubeMessageKind::MembershipGift: {
            builder->flags.set(MessageFlag::Subscription);
            builder->userID = source.author.channelId;
            builder->displayName = authorName(source.author);
            builder->loginName = visibleYouTubeName(source.author.handle);
            if (builder->loginName.isEmpty())
            {
                builder->loginName = builder->displayName;
            }
            text = source.eventText.trimmed();
            if (text.isEmpty())
            {
                return nullptr;
            }
            const auto name = builder->displayName.trimmed();
            const bool startsWithName =
                !name.isEmpty() && text.startsWith(name) &&
                (text.size() == name.size() || text.at(name.size()).isSpace());
            if (startsWithName)
            {
                builder.appendLinkedUser(name, source.author.channelId,
                                         builder->messageText);
                const auto remainder = text.sliced(name.size()).trimmed();
                if (!remainder.isEmpty())
                {
                    builder.appendOrEmplaceText(remainder,
                                                MessageColor::System);
                    builder->messageText += remainder;
                }
            }
            else
            {
                builder.appendOrEmplaceText(text, MessageColor::System);
                builder->messageText = text;
            }
            break;
        }

        case YouTubeMessageKind::Text:
        case YouTubeMessageKind::SuperChat:
        case YouTubeMessageKind::Ignored:
            return nullptr;
    }

    if (builder->messageText.isEmpty())
    {
        builder->messageText = text.trimmed();
    }
    builder->searchText = builder->messageText;
    return builder.release();
}

void YouTubeMessageBuilder::setCommonFields(const YouTubeMessage &source)
{
    this->message().id = source.id;
    this->message().channelName = this->channel_->getName();
    if (source.historical || source.localEcho)
    {
        this->message().flags.set(MessageFlag::DoNotTriggerNotification);
    }
}

void YouTubeMessageBuilder::appendChannelName()
{
    const auto stableName = this->channel_->getName();
    const auto displayName = this->channel_->getDisplayName();
    this->emplace<ChannelNameElement>(u'#' + displayName)
        ->setLink({Link::JumpToChannel, u":youtube:" % stableName});
}

void YouTubeMessageBuilder::appendRoleBadges(const YouTubeAuthor &author)
{
    if (author.isOwner)
    {
        appendRoleBadge(*this, youtubeOwnerBadge(),
                        MessageElementFlag::BadgeChannelAuthority);
    }
    else if (author.isModerator)
    {
        appendRoleBadge(*this, youtubeModeratorBadge(),
                        MessageElementFlag::BadgeChannelAuthority);
    }

    if (author.isMember)
    {
        appendRoleBadge(*this, youtubeMemberBadge(author),
                        MessageElementFlag::BadgeSubscription);
    }

    if (author.isVerified)
    {
        appendRoleBadge(*this, youtubeVerifiedBadge(),
                        MessageElementFlag::BadgeGlobalAuthority);
    }
}

void YouTubeMessageBuilder::appendUsername(const YouTubeAuthor &author)
{
    const auto name = authorName(author);
    const auto handle = visibleYouTubeName(author.handle);
    auto *element = this->emplace<TextElement>(
        name + u':', MessageElementFlag::Username,
        this->message().usernameColor, FontStyle::ChatMediumBold);
    element->setLink(
        {Link::UserInfo,
         handle.isEmpty() ? u"id:"_s + author.channelId : handle});
}

void YouTubeMessageBuilder::appendUserText(const QString &text)
{
    auto appendPlainText = [this](QStringView text, bool adjacentRight) {
        const auto elementCount = this->message().elements.size();
        for (const auto &part : getApp()->getEmotes()->getEmojis()->parse(text))
        {
            std::visit(variant::Overloaded{
                           [this](const EmotePtr &emoji) {
                               this->emplace<EmoteElement>(
                                   emoji, MessageElementFlag::EmojiAll);
                           },
                           [this](QStringView plainText) {
                               this->addWordFromUserMessage(
                                   plainText, this->channel_, true);
                           },
                       },
                       part);
        }
        if (adjacentRight && this->message().elements.size() > elementCount)
        {
            this->message().elements.back()->setTrailingSpace(false);
        }
    };

    for (const auto word : QStringView{text}.tokenize(u' ', Qt::SkipEmptyParts))
    {
        auto remaining = word;
        while (!remaining.isEmpty())
        {
            qsizetype foundAt = -1;
            qsizetype foundLength = 0;
            EmotePtr foundEmote;
            qsizetype searchFrom = 0;
            while (searchFrom < remaining.size())
            {
                const auto opening = remaining.indexOf(u':', searchFrom);
                if (opening < 0)
                {
                    break;
                }
                const auto closing = remaining.indexOf(u':', opening + 1);
                if (closing < 0)
                {
                    break;
                }
                const auto length = closing - opening + 1;
                if (length > 2 && length <= 80)
                {
                    foundEmote = this->channel_->youtubeEmote(
                        remaining.sliced(opening, length));
                    if (foundEmote)
                    {
                        foundAt = opening;
                        foundLength = length;
                        break;
                    }
                }
                searchFrom = opening + 1;
            }

            if (!foundEmote)
            {
                appendPlainText(remaining, false);
                break;
            }

            if (foundAt > 0)
            {
                appendPlainText(remaining.first(foundAt), true);
            }

            auto *element = this->emplace<EmoteElement>(
                foundEmote, MessageElementFlag::Emote, this->textColor(), false,
                youtube::CUSTOM_EMOJI_HORIZONTAL_PADDING);
            remaining = remaining.sliced(foundAt + foundLength);
            if (!remaining.isEmpty())
            {
                element->setTrailingSpace(false);
            }
        }
    }
}

void YouTubeMessageBuilder::appendUserRuns(
    const std::vector<YouTubeMessageRun> &runs)
{
    const YouTubeMessageRun *previousRun = nullptr;
    for (const auto &run : runs)
    {
        if (run.text.isEmpty())
        {
            continue;
        }

        if (previousRun &&
            shouldJoinYouTubeMessageRuns(*previousRun, run) &&
            !this->message().elements.empty())
        {
            this->message().elements.back()->setTrailingSpace(false);
        }

        if (run.kind == YouTubeMessageRun::Kind::Text ||
            !run.customEmoji || run.emojiImageUrl.isEmpty())
        {
            this->appendUserText(run.text);
        }
        else
        {
            EmotePtr emote;
            for (const auto &shortcut : run.emojiShortcuts)
            {
                emote = this->channel_->youtubeEmote(shortcut);
                if (emote)
                {
                    break;
                }
            }
            if (!emote)
            {
                const auto name = run.emojiShortcuts.isEmpty()
                                      ? run.text
                                      : run.emojiShortcuts.front();
                emote = YouTubeEmotes::makeCustomEmoji(
                    name, run.emojiImageUrl, run.emojiID);
            }
            this->emplace<EmoteElement>(emote, MessageElementFlag::Emote,
                                        this->textColor(), false,
                                        youtube::CUSTOM_EMOJI_HORIZONTAL_PADDING);
        }

        previousRun = &run;
    }
}

void YouTubeMessageBuilder::appendLinkedUser(const QString &displayName,
                                             const QString &channelId,
                                             QString &plainText)
{
    auto *element = this->emplace<TextElement>(
        displayName, MessageElementFlag::Username, MessageColor::System,
        FontStyle::ChatMediumBold);
    if (!channelId.isEmpty())
    {
        element->setLink({Link::UserInfo, u"id:"_s + channelId});
    }
    plainText.append(displayName);
    plainText.append(u' ');
}

}
