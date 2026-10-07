#include "providers/tiktok/TikTokMessageBuilder.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/tiktok/TikTokBadge.hpp"
#include "providers/tiktok/TikTokChannel.hpp"
#include "providers/tiktok/TikTokEmotes.hpp"
#include "providers/tiktok/TikTokText.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"

#include <algorithm>
#include <variant>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

namespace {
const auto TIKTOK_SUPPORT_HIGHLIGHT =
    std::make_shared<QColor>(238, 57, 97, 100);

QString giftLabel(const TikTokEvent &event)
{
    const auto count = std::clamp<quint64>(event.value, 1, 1000000);
    const auto name = event.gift ? event.gift->name : QString{};
    if (name.isEmpty())
    {
        return count == 1 ? u"a gift"_s : u"%1 gifts"_s.arg(count);
    }
    return count == 1 ? name : u"%1 × %2"_s.arg(name).arg(count);
}

const TikTokAuthor *giftRecipient(const TikTokEvent &event,
                                  const TikTokChannel &channel)
{
    if (!event.gift)
    {
        return nullptr;
    }
    const auto &recipient = event.gift->recipient;
    if (recipient.broadcaster ||
        recipient.handle.compare(channel.getName(), Qt::CaseInsensitive) == 0 ||
        (recipient.displayName.isEmpty() && recipient.handle.isEmpty()))
    {
        return nullptr;
    }
    return &recipient;
}

QString authorName(const TikTokAuthor &author)
{
    return author.displayName.isEmpty() ? author.handle : author.displayName;
}

QString eventText(const TikTokEvent &event, const TikTokChannel &channel)
{
    if (event.kind == TikTokEvent::Kind::Gift)
    {
        const auto label = giftLabel(event);
        if (const auto *recipient = giftRecipient(event, channel))
        {
            return u"sent %1 to %2"_s.arg(label, authorName(*recipient));
        }
        return u"sent %1"_s.arg(label);
    }
    if (event.kind == TikTokEvent::Kind::Subscription)
    {
        return event.value > 1 ? u"subscribed for %1 months"_s.arg(event.value)
                               : u"subscribed"_s;
    }
    return event.text;
}

void appendText(MessageBuilder &builder, TikTokChannel *channel,
                QStringView text)
{
    const auto builtins = tikTokBuiltinEmotes();
    for (const auto word : text.tokenize(u' ', Qt::SkipEmptyParts))
    {
        auto remaining = word;
        while (!remaining.isEmpty())
        {
            qsizetype foundAt = -1;
            qsizetype foundLength = 0;
            EmotePtr found;
            for (qsizetype pos = remaining.indexOf(u'['); pos >= 0;
                 pos = remaining.indexOf(u'[', pos + 1))
            {
                const auto end = remaining.indexOf(u']', pos + 1);
                if (end < 0)
                {
                    break;
                }
                const auto it = builtins->find(
                    EmoteName{remaining.sliced(pos, end - pos + 1).toString()});
                if (it != builtins->end())
                {
                    foundAt = pos;
                    foundLength = end - pos + 1;
                    found = it->second;
                    break;
                }
            }
            const auto plain = found ? remaining.first(foundAt) : remaining;
            bool firstPart = true;
            for (const auto &part :
                 getApp()->getEmotes()->getEmojis()->parse(plain))
            {
                if (!firstPart && !builder->elements.empty())
                {
                    builder->elements.back()->setTrailingSpace(false);
                }
                firstPart = false;
                if (const auto *emoji = std::get_if<EmotePtr>(&part))
                {
                    builder.emplace<EmoteElement>(*emoji,
                                                  MessageElementFlag::EmojiAll);
                }
                else
                {
                    builder.addWordFromUserMessage(std::get<QStringView>(part),
                                                   channel);
                }
            }
            if (!found)
            {
                break;
            }
            if (!plain.isEmpty() && !builder->elements.empty())
            {
                builder->elements.back()->setTrailingSpace(false);
            }
            auto *element =
                builder.emplace<EmoteElement>(found, MessageElementFlag::Emote);
            remaining = remaining.sliced(foundAt + foundLength);
            element->setTrailingSpace(remaining.isEmpty());
        }
    }
}
}

TikTokUsernameElement::TikTokUsernameElement(
    const QString &text, MessageElementFlags flags, MessageColor color,
    FontStyle font, QString avatarUrl)
    : TextElement(text, flags, color, font)
    , avatarUrl_(std::move(avatarUrl))
{
}

std::unique_ptr<MessageElement> TikTokUsernameElement::clone() const
{
    auto element = std::make_unique<TikTokUsernameElement>(
        QString{}, this->getFlags(), this->color(), this->fontStyle(),
        this->avatarUrl_);
    element->text_ = this->text_;
    element->hasWords_ = this->hasWords_;
    element->hasExplicitWordBoundaries_ = this->hasExplicitWordBoundaries_;
    element->cloneFrom(*this);
    return element;
}

const QString &TikTokUsernameElement::avatarUrl() const
{
    return this->avatarUrl_;
}

std::pair<MessagePtrMut, HighlightAlert> makeTikTokMessage(
    TikTokChannel *channel, const TikTokEvent &event)
{
    const bool gift = event.kind == TikTokEvent::Kind::Gift;
    const bool subscription = event.kind == TikTokEvent::Kind::Subscription;
    const bool support = gift || subscription;
    const auto text = eventText(event, *channel);
    if (text.isEmpty() &&
        std::ranges::none_of(event.emotes, [](const auto &emote) {
            return isTikTokImageUrl(QUrl(emote.imageUrl));
        }))
    {
        return {};
    }
    MessageBuilder builder;
    builder->platform = MessagePlatform::TikTok;
    builder->channelName = channel->getName();
    builder->id = event.id;
    builder->userID = event.author.id;
    builder->displayName = event.author.displayName.isEmpty()
                               ? event.author.handle
                               : event.author.displayName;
    if (builder->displayName.isEmpty())
    {
        builder->displayName = u"TikTok user"_s;
    }
    builder->loginName = event.author.handle.isEmpty() ? builder->displayName
                                                       : event.author.handle;
    builder->usernameColor = getRandomColor(event.author.id);
    builder->serverReceivedTime = event.time.isValid()
                                      ? event.time.toLocalTime()
                                      : QDateTime::currentDateTime();
    builder->parseTime = QTime::currentTime();
    builder->messageText = text;
    builder->searchText = builder->displayName;
    if (!event.author.handle.isEmpty() &&
        event.author.handle.compare(builder->displayName,
                                    Qt::CaseInsensitive) != 0)
    {
        builder->searchText += u" @"_s + event.author.handle;
    }
    builder->searchText += u' ' + event.author.id + u": "_s + text;
    if (event.historical)
    {
        builder->flags.set(MessageFlag::RecentMessage,
                           MessageFlag::DoNotTriggerNotification);
    }
    if (!event.author.handle.isEmpty())
    {
        channel->addRecentChatter(event.author.handle);
    }
    channel->setUserColor(builder->loginName, builder->usernameColor);

    builder
        .emplace<ChannelNameElement>(u'#' + channel->getDisplayName(),
                                     channel->channelAvatar())
        ->setLink({Link::JumpToChannel, u":tiktok:"_s + channel->getName()});
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    appendTikTokBadges(builder, event.author);
    if (subscription)
    {
        builder->flags.set(MessageFlag::Subscription);
    }
    if (support)
    {
        builder->flags.set(MessageFlag::InvalidReplyTarget);
    }
    builder
        .emplace<TikTokUsernameElement>(
            support ? builder->displayName : builder->displayName + u':',
            MessageElementFlags{MessageElementFlag::Username,
                                MessageElementFlag::NoUsernamePaint},
            support ? MessageColor::Text : MessageColor(builder->usernameColor),
            FontStyle::ChatMediumBold, event.author.avatarUrl)
        ->setLink({Link::UserInfo, event.author.handle.isEmpty()
                                       ? u"id:"_s + event.author.id
                                       : event.author.handle});

    if (gift)
    {
        builder.emplace<TextElement>(u"sent"_s, MessageElementFlag::Text,
                                     MessageColor::Text);
        if (event.gift)
        {
            if (auto image =
                    tikTokEmote(event.gift->name, event.gift->imageUrl, 18))
            {
                builder.emplace<TikTokBadgeElement>(
                    TikTokBadge{.imageUrl = event.gift->imageUrl,
                                .label = event.gift->name},
                    image, MessageElementFlags{MessageElementFlag::EmoteImage});
            }
        }
        builder.emplace<TextElement>(giftLabel(event), MessageElementFlag::Text,
                                     MessageColor::Text,
                                     FontStyle::ChatMediumBold);
        if (const auto *recipient = giftRecipient(event, *channel))
        {
            builder.emplace<TextElement>(u"to"_s, MessageElementFlag::Text,
                                         MessageColor::Text);
            auto *name = builder.emplace<TextElement>(
                authorName(*recipient),
                MessageElementFlags{MessageElementFlag::Text,
                                    MessageElementFlag::NoUsernamePaint},
                MessageColor::Text, FontStyle::ChatMediumBold);
            if (!recipient->handle.isEmpty() || !recipient->id.isEmpty())
            {
                name->setLink({Link::UserInfo, recipient->handle.isEmpty()
                                                   ? u"id:"_s + recipient->id
                                                   : recipient->handle});
            }
        }
    }
    else if (subscription)
    {
        builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                     MessageColor::Text,
                                     FontStyle::ChatMediumBold);
    }
    else
    {
        qsizetype position = 0;
        qsizetype preceding = 0;
        for (const auto &emote : event.emotes)
        {
            const auto offset =
                emote.index == -1 ? std::optional<qsizetype>{event.text.size()}
                                  : tiktok::livetext::incomingIndexToUtf16(
                                        event.text, emote.index, preceding++);
            if (!offset)
            {
                continue;
            }
            auto image = tikTokEmote(u"[emote]", emote.imageUrl);
            if (!image)
            {
                continue;
            }
            const auto index = *offset;
            if (index < position || index > event.text.size() ||
                (index > 0 && index < event.text.size() &&
                 event.text[index].isLowSurrogate()))
            {
                continue;
            }
            appendText(
                builder, channel,
                QStringView(event.text).sliced(position, index - position));
            if (emote.index >= 0 && index > 0 &&
                !event.text[index - 1].isSpace() && !builder->elements.empty())
            {
                builder->elements.back()->setTrailingSpace(false);
            }
            builder.emplace<EmoteElement>(image, MessageElementFlag::Emote)
                ->setTrailingSpace(emote.index < 0 ||
                                   index >= event.text.size() ||
                                   event.text[index].isSpace());
            position = index;
            if (event.text.isEmpty())
            {
                builder->messageText += u"[emote] "_s;
            }
        }
        appendText(builder, channel, QStringView(event.text).sliced(position));
        if (event.text.isEmpty())
        {
            builder->searchText += builder->messageText;
        }
    }

    HighlightAlert alert;
    if (!getSettings()->isBlacklistedUser(builder->loginName))
    {
        MessageParseArgs args;
        auto [highlighted, result] = getApp()->getHighlights()->check(
            args, {}, builder->loginName, builder->messageText, builder->flags,
            MessagePlatform::TikTok, builder->userID, builder->channelName);
        if (highlighted)
        {
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
            alert = {result.customSoundUrl.value_or(QUrl{}),
                     result.playSound && !event.historical,
                     result.alert && !event.historical};
        }
    }

    if (gift && !builder->flags.has(MessageFlag::Highlighted))
    {
        builder->flags.set(MessageFlag::Highlighted);
        builder->highlightColor = TIKTOK_SUPPORT_HIGHLIGHT;
    }
    return {builder.release(), alert};
}
}
