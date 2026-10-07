// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/MessageBuilder.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "util/Helpers.hpp"
#include "util/QCompareTransparent.hpp"

#include <QStringBuilder>

#include <algorithm>
#include <limits>
#include <set>

namespace {

using namespace chatterino;
using namespace chatterino::eventsub;
using namespace chatterino::literals;

/// <MODERATOR> turned {on/off} <MODE> mode. [<DURATION>]
void makeModeMessage(EventSubMessageBuilder &builder,
                     const lib::payload::channel_moderate::v2::Event &event,
                     const QString &mode, bool on, const QString &duration = {})
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"turned"_s, text);
    QString op = on ? u"on"_s : u"off"_s;
    builder.emplaceSystemTextAndUpdate(op, text);
    builder.emplaceSystemTextAndUpdate(mode, text);
    builder.emplaceSystemTextAndUpdate(u"mode."_s, text);

    if (!duration.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(duration, text);
    }

    builder.setMessageAndSearchText(text);
}

QString stringifyAutomodReason(const lib::automod::AutomodReason &reason,
                               QStringView /* message */)
{
    return reason.category.qt() % u" level " % QString::number(reason.level);
}

QString stringifyAutomodReason(const lib::automod::BlockedTermReason &reason,
                               QStringView message)
{
    if (reason.termsFound.empty())
    {
        return u"blocked term usage"_s;
    }

    std::set<QString, QCompareCaseInsensitive> hitTerms;
    for (const auto &term : reason.termsFound)
    {
        if (term.boundary.startPos < 0 ||
            term.boundary.endPos < term.boundary.startPos ||
            term.boundary.endPos == std::numeric_limits<int>::max())
        {
            continue;
        }
        const auto hitTerm = codepointSlice(message, term.boundary.startPos,
                                            term.boundary.endPos + 1);
        if (hitTerm.isEmpty())
        {
            continue;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        hitTerms.emplace(hitTerm);
#else
        hitTerms.emplace(hitTerm.toString());
#endif
    }

    if (hitTerms.empty())
    {
        return u"blocked term usage"_s;
    }

    QString msg = [&] {
        if (hitTerms.size() == 1)
        {
            return u"matches 1 blocked term"_s;
        }
        return u"matches %1 blocked terms"_s.arg(hitTerms.size());
    }();

    if (getSettings()->streamerModeHideBlockedTermText &&
        getApp()->getStreamerMode()->isEnabled())
    {
        return msg;
    }

    bool first = true;
    for (const auto &hitTerm : hitTerms)
    {
        if (first)
        {
            msg.append(u" \"");
            first = false;
        }
        else
        {
            msg.append(u"\", \"");
        }

        msg.append(hitTerm);
    }
    msg.append(u'"');

    return msg;
}

// XXX: this is a duplicate from messages/MessageBuilder.cpp
EmotePtr makeAutoModBadge()
{
    return std::make_shared<Emote>(Emote{
        .name = EmoteName{},
        .images =
            ImageSet{Image::fromResourcePixmap(getResources().twitch.automod)},
        .tooltip = Tooltip{"AutoMod"},
        .homePage =
            Url{"https://dashboard.twitch.tv/settings/moderation/automod"},
    });
}

QString localizedDisplayName(
    const lib::payload::automod_message_hold::v2::Event &event)
{
    QString displayName = event.userName.qt();
    bool hasLocalizedName =
        displayName.compare(event.userLogin.qt(), Qt::CaseInsensitive) != 0;

    switch (getSettings()->usernameDisplayMode.getValue())
    {
        case UsernameDisplayMode::Username: {
            if (hasLocalizedName)
            {
                displayName = event.userLogin.qt();
            }
            break;
        }
        case UsernameDisplayMode::LocalizedName: {
            break;
        }
        case UsernameDisplayMode::UsernameAndLocalizedName: {
            if (hasLocalizedName)
            {
                displayName =
                    event.userLogin.qt() % '(' % event.userName.qt() % ')';
            }
            break;
        }
        default:
            break;
    }
    return displayName;
}

QString localizedDisplayName(const automod::ReviewItem &item)
{
    QString displayName =
        item.userName.isEmpty() ? item.userLogin : item.userName;
    const bool hasLocalizedName =
        displayName.compare(item.userLogin, Qt::CaseInsensitive) != 0;

    switch (getSettings()->usernameDisplayMode.getValue())
    {
        case UsernameDisplayMode::Username:
            return item.userLogin;
        case UsernameDisplayMode::UsernameAndLocalizedName:
            if (hasLocalizedName)
            {
                return item.userLogin % u'(' % displayName % u')';
            }
            break;
        case UsernameDisplayMode::LocalizedName:
        default:
            break;
    }
    return displayName;
}

void appendHeldMessageText(EventSubMessageBuilder &builder,
                           TwitchChannel *channel, const QString &message)
{
    for (const auto &word : message.split(u' '))
    {
        if (word.isEmpty())
        {
            continue;
        }
        builder.addWordFromUserMessage(word, channel);
    }
}

qsizetype utf16OffsetForCodepoint(QStringView text, int codepointOffset)
{
    if (codepointOffset < 0)
    {
        return -1;
    }
    qsizetype utf16 = 0;
    int codepoints = 0;
    while (utf16 < text.size() && codepoints < codepointOffset)
    {
        const auto first = text.at(utf16);
        if (first.isHighSurrogate() && utf16 + 1 < text.size() &&
            text.at(utf16 + 1).isLowSurrogate())
        {
            utf16 += 2;
        }
        else
        {
            ++utf16;
        }
        ++codepoints;
    }
    return codepoints == codepointOffset ? utf16 : -1;
}

void appendTwitchMatch(automod::HoldData &data,
                       const lib::automod::Boundary &boundary,
                       const QString &ruleDetails)
{
    if (boundary.endPos == std::numeric_limits<int>::max())
    {
        return;
    }
    const auto start =
        utf16OffsetForCodepoint(data.messageText, boundary.startPos);
    const auto end =
        utf16OffsetForCodepoint(data.messageText, boundary.endPos + 1);
    if (start < 0 || end <= start)
    {
        return;
    }
    const auto fragment = data.messageText.mid(start, end - start);
    if (!fragment.isEmpty() &&
        !data.matchedFragments.contains(fragment, Qt::CaseInsensitive))
    {
        data.matchedFragments.push_back(fragment);
    }
    data.twitchMatches.push_back({
        .start = start,
        .length = end - start,
        .color = defaultAutoModMatchColor(),
        .ruleName = ruleDetails,
        .pattern = fragment,
        .source = HighlightMatchSource::AutoMod,
        .style = HighlightMatchStyle::Fill,
    });
}

void mergeAdjacentTwitchMatches(automod::HoldData &data)
{
    if (data.twitchMatches.size() < 2)
    {
        return;
    }

    std::ranges::sort(data.twitchMatches, {}, &HighlightMatch::start);
    std::vector<HighlightMatch> merged;
    merged.reserve(data.twitchMatches.size());
    for (auto &match : data.twitchMatches)
    {
        if (!merged.empty())
        {
            auto &previous = merged.back();
            const auto previousEnd = previous.start + previous.length;
            const auto matchEnd = match.start + match.length;
            const auto gapLength = match.start - previousEnd;
            const bool whitespaceGap =
                gapLength >= 0 && data.messageText.mid(previousEnd, gapLength)
                                      .trimmed()
                                      .isEmpty();
            const bool sameAppearance = previous.color == match.color &&
                                        previous.ruleName == match.ruleName &&
                                        previous.source == match.source &&
                                        previous.style == match.style &&
                                        previous.paintID == match.paintID;
            if (sameAppearance && (match.start <= previousEnd || whitespaceGap))
            {
                previous.length =
                    std::max(previousEnd, matchEnd) - previous.start;
                previous.pattern =
                    data.messageText.mid(previous.start, previous.length);
                continue;
            }
        }
        merged.emplace_back(std::move(match));
    }
    data.twitchMatches = std::move(merged);
}

QString displayAutoModCategory(QString category)
{
    category.replace(u'_', u' ');
    if (!category.isEmpty())
    {
        category[0] = category.at(0).toUpper();
    }
    return category;
}

void fillAutoModReason(automod::HoldData &data,
                       const lib::automod::AutomodReason &reason)
{
    data.blockedTerm = false;
    data.reasonCategory = displayAutoModCategory(reason.category.qt());
    data.reasonLevel = reason.level;
    data.reasonSummary =
        data.reasonCategory % u" · Level " % QString::number(reason.level);
    const auto details = u"Twitch AutoMod\n"_s % data.reasonSummary;
    for (const auto &boundary : reason.boundaries)
    {
        appendTwitchMatch(data, boundary, details);
    }
    mergeAdjacentTwitchMatches(data);
}

void fillAutoModReason(automod::HoldData &data,
                       const lib::automod::BlockedTermReason &reason)
{
    data.blockedTerm = true;
    data.reasonCategory = u"Blocked term"_s;
    for (const auto &found : reason.termsFound)
    {
        const auto owner =
            QString::fromStdString(found.ownerBroadcasterUserLogin);
        auto details = u"Twitch AutoMod\nBlocked term"_s;
        if (!owner.isEmpty() &&
            owner.compare(data.broadcasterLogin, Qt::CaseInsensitive) != 0)
        {
            details += u"\nShared from #"_s % owner;
        }
        appendTwitchMatch(data, found.boundary, details);
        if (data.termOwnerLogin.isEmpty() && !owner.isEmpty())
        {
            data.termOwnerLogin = owner;
        }
    }
    mergeAdjacentTwitchMatches(data);
    const auto count = data.matchedFragments.size();
    data.reasonSummary = count == 1 ? u"Matched 1 blocked term"_s
                                    : u"Matched %1 blocked terms"_s.arg(count);
    if (count == 0)
    {
        data.reasonSummary = u"Blocked term usage"_s;
    }
}

}  // namespace

namespace chatterino::eventsub {

EventSubMessageBuilder::EventSubMessageBuilder(TwitchChannel *channel,
                                               const QDateTime &time)
    : channel(channel)
{
    this->emplace<TimestampElement>(time.time());
    this->message().flags.set(MessageFlag::System, MessageFlag::EventSub);
    this->message().serverReceivedTime = time;
}

EventSubMessageBuilder::EventSubMessageBuilder(TwitchChannel *channel)
    : channel(channel)
{
    this->message().flags.set(MessageFlag::EventSub);
}

EventSubMessageBuilder::~EventSubMessageBuilder() = default;

void EventSubMessageBuilder::appendUser(const lib::String &userName,
                                        const lib::String &userLogin,
                                        QString &text, bool trailingSpace)
{
    auto login = userLogin.qt();
    auto *el = this->emplace<MentionElement>(
        userName.qt(), login, MessageColor::System,
        this->channel->getUserColor(login));
    text.append(login);

    if (trailingSpace)
    {
        text.append(u' ');
    }
    else
    {
        el->setTrailingSpace(false);
    }
}

void EventSubMessageBuilder::setMessageAndSearchText(const QString &text)
{
    assert(this->message().messageText.isNull());
    assert(this->message().searchText.isNull());

    this->message().messageText = text;
    this->message().searchText = text;
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Vip &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has added", text);
    builder.appendUser(action.userName, action.userLogin, text);
    builder.emplaceSystemTextAndUpdate("as a VIP of this channel.", text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unvip &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has removed", text);
    builder.appendUser(action.userName, action.userLogin, text);
    builder.emplaceSystemTextAndUpdate("as a VIP of this channel.", text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Warn &action)
{
    builder->flags.set(MessageFlag::ModerationAction);
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has warned", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    QStringList reasons;

    if (!action.reason.qt().isEmpty())
    {
        reasons.append(action.reason.qt());
    }

    for (const auto &rule : action.chatRulesCited)
    {
        if (!rule.qt().isEmpty())
        {
            reasons.append(rule.qt());
        }
    }

    if (reasons.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.emplaceSystemTextAndUpdate(reasons.join(", "), text);
    }

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unban &action)
{
    builder->flags.set(MessageFlag::ModerationAction, MessageFlag::Untimeout);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("unbanned", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(".", text);

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Untimeout &action)
{
    builder->flags.set(MessageFlag::ModerationAction, MessageFlag::Untimeout);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("untimedout", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(".", text);

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Delete &action)
{
    builder->flags.set(MessageFlag::DoNotTriggerNotification,
                       MessageFlag::ModerationAction);

    QString text;
    const bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("deleted a message from", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(":", text);

    auto limit = getSettings()->deletedMessageLengthLimit.getValue();
    if (limit > 0 && action.messageBody.view().length() > limit)
    {
        builder
            .emplace<TextElement>(action.messageBody.qt().left(limit) + "…",
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::JumpToMessage, action.messageID.qt()});

        text.append(action.messageBody.qt().left(limit) + "…");
    }
    else
    {
        builder
            .emplace<TextElement>(action.messageBody.qt(),
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::JumpToMessage, action.messageID.qt()});

        text.append(action.messageBody.qt());
    }

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Followers &action)
{
    QString duration;
    if (action.followDurationMinutes > 0)
    {
        duration = u"(%1 minutes)"_s.arg(action.followDurationMinutes);
    }
    makeModeMessage(builder, event, u"followers-only"_s, true, duration);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::FollowersOff & /*action*/)
{
    makeModeMessage(builder, event, u"followers-only"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::EmoteOnly & /*action*/)
{
    makeModeMessage(builder, event, u"emote-only"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::EmoteOnlyOff & /*action*/)
{
    makeModeMessage(builder, event, u"emote-only"_s, false);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Slow &action)
{
    makeModeMessage(builder, event, u"slow"_s, true,
                    u"(%1 seconds)"_s.arg(action.waitTimeSeconds));
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::SlowOff & /*action*/)
{
    makeModeMessage(builder, event, u"slow"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Subscribers & /*action*/)
{
    makeModeMessage(builder, event, u"subscribers-only"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::SubscribersOff & /*action*/)
{
    makeModeMessage(builder, event, u"subscribers-only"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Uniquechat & /*action*/)
{
    makeModeMessage(builder, event, u"unique-chat"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::UniquechatOff & /*action*/)
{
    makeModeMessage(builder, event, u"unique-chat"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::AutomodTerms &action)
{
    builder->flags.set(MessageFlag::ModerationAction);

    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    if (action.action == "add")
    {
        builder.emplaceSystemTextAndUpdate(u"added"_s, text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(u"removed"_s, text);
    }

    QString terms;
    for (size_t i = 0; i < action.terms.size(); i++)
    {
        if (i != 0)
        {
            if (i == action.terms.size() - 1)
            {
                if (action.terms.size() == 2)
                {
                    terms.append(u" and ");
                }
                else
                {
                    terms.append(u", and ");
                }
            }
            else
            {
                terms.append(u", ");
            }
        }
        terms.append(u'"');
        terms.append(action.terms[i].qt());
        terms.append(u'"');
    }
    builder.emplaceSystemTextAndUpdate(terms, text);
    builder.emplaceSystemTextAndUpdate(u"as"_s, text);
    if (action.terms.size() == 1)
    {
        builder.emplaceSystemTextAndUpdate(u"a"_s, text);
    }
    builder.emplaceSystemTextAndUpdate(action.list.qt(), text);
    if (action.terms.size() == 1)
    {
        builder.emplaceSystemTextAndUpdate(u"term"_s, text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(u"terms"_s, text);
    }
    builder.emplaceSystemTextAndUpdate(u"on AutoMod."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Mod &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"modded"_s, text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unmod &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"unmodded"_s, text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Raid &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("initiated a raid to", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unraid &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("canceled the raid to", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::UnbanRequest &action)
{
    builder->flags.set(MessageFlag::ModerationAction);

    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    if (action.isApproved)
    {
        builder.emplaceSystemTextAndUpdate("approved", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate("denied", text);
    }
    builder.appendOrEmplaceSystemTextAndUpdate("the unban request from", text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    if (action.moderatorMessage.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.appendOrEmplaceSystemTextAndUpdate(action.moderatorMessage.qt(),
                                                   text);
    }

    builder.setMessageAndSearchText(text);
}

MessagePtr makeAutomodHoldMessageHeader(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::automod_message_hold::v2::Event &event)
{
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::ModerationAction,
                       MessageFlag::AutoMod,
                       MessageFlag::AutoModOffendingMessageHeader);
    builder->flags.set(
        MessageFlag::AutoModBlockedTerm,
        std::holds_alternative<lib::automod::BlockedTermReason>(event.reason));

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);
    // AutoMod header message
    auto reason = std::visit(
        [&](const auto &r) {
            return stringifyAutomodReason(r, event.message.text.qt());
        },
        event.reason);
    builder.emplace<TextElement>(u"Held a message for reason: " % reason %
                                     u". Allow will post it in chat. ",
                                 MessageElementFlag::Text, MessageColor::Text);
    // Allow link button
    builder
        .emplace<TextElement>("Allow", MessageElementFlag::Text,
                              MessageColor(QColor(0, 255, 0)),
                              FontStyle::ChatMediumBold)
        ->setLink({Link::AutoModAllow, event.messageID.qt()});
    // Deny link button
    builder
        .emplace<TextElement>(" Deny", MessageElementFlag::Text,
                              MessageColor(QColor(255, 0, 0)),
                              FontStyle::ChatMediumBold)
        ->setLink({Link::AutoModDeny, event.messageID.qt()});

    builder.setMessageAndSearchText(
        u"AutoMod: Held a message for reason: " % reason %
        u". Allow will post it in chat. Allow Deny");

    return builder.release();
}

MessagePtr makeAutomodHoldMessageBody(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::automod_message_hold::v2::Event &event)
{
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->flags.set(MessageFlag::PubSub, MessageFlag::ModerationAction,
                       MessageFlag::AutoMod,
                       MessageFlag::AutoModOffendingMessage);
    builder->flags.set(
        MessageFlag::AutoModBlockedTerm,
        std::holds_alternative<lib::automod::BlockedTermReason>(event.reason));

    // Builder for offender's message
    builder->channelName = event.broadcasterUserLogin.qt();
    builder
        .emplace<ChannelNameElement>(u'#' + event.broadcasterUserLogin.qt(),
                                     channel->channelAvatar())
        ->setLink({Link::JumpToChannel, event.broadcasterUserLogin.qt()});
    builder.emplace<TimestampElement>(time.time());
    builder.emplace<TwitchModerationElement>(true, false, false,
                                             channel->weakFromThis());
    builder->loginName = event.userLogin.qt();

    auto displayName = localizedDisplayName(event);
    // sender username
    builder
        .emplace<MentionElement>(displayName + ':', event.userLogin.qt(),
                                 MessageColor::Text,
                                 channel->getUserColor(event.userLogin.qt()))
        ->addFlags(MessageElementFlag::Username);

    // sender's message caught by AutoMod
    appendHeldMessageText(builder, channel, event.message.text.qt());

    builder.setMessageAndSearchText(displayName % u": " %
                                    event.message.text.qt());

    return builder.release();
}

MessagePtr makeAutomodMessageUpdateHeader(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::automod_message_update::v2::Event &event)
{
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::Disabled, MessageFlag::PubSub,
                       MessageFlag::ModerationAction, MessageFlag::AutoMod,
                       MessageFlag::AutoModOffendingMessageHeader);
    builder->flags.set(
        MessageFlag::AutoModBlockedTerm,
        std::holds_alternative<lib::automod::BlockedTermReason>(event.reason));

    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);

    const auto reason = std::visit(
        [&](const auto &r) {
            return stringifyAutomodReason(r, event.message.text.qt());
        },
        event.reason);
    QString text = u"AutoMod: Held a message for reason: " % reason % u". ";
    builder.emplace<TextElement>(u"Held a message for reason: " % reason % u".",
                                 MessageElementFlag::Text, MessageColor::Text);

    const auto status = QString::fromStdString(event.status).toLower();
    const auto moderatorLogin =
        QString::fromStdString(event.moderatorUserLogin);
    auto moderatorName = QString::fromStdString(event.moderatorUserName);
    if (moderatorName.isEmpty())
    {
        moderatorName = moderatorLogin;
    }

    if (status == u"approved" || status == u"denied")
    {
        const auto verb = status == u"approved" ? u"Approved"_s : u"Denied"_s;
        builder.emplace<TextElement>(verb % u" by", MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatMediumBold);
        text += verb % u" by ";
        if (!moderatorName.isEmpty())
        {
            builder.emplace<MentionElement>(
                moderatorName, moderatorLogin, MessageColor::System,
                channel->getUserColor(moderatorLogin));
            text += moderatorName;
        }
        else
        {
            builder.emplace<TextElement>(u"a moderator"_s,
                                         MessageElementFlag::Text,
                                         MessageColor::System);
            text += u"a moderator";
        }
        builder->elements.back()->setTrailingSpace(false);
        builder.emplace<TextElement>(u"."_s, MessageElementFlag::Text,
                                     MessageColor::System);
        text += u'.';
    }
    else if (status == u"expired")
    {
        const auto resolution = u"Expired before a moderator acted."_s;
        builder.emplace<TextElement>(resolution, MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatMediumBold);
        text += resolution;
    }
    else
    {
        const auto resolution = u"Resolved with status: " %
                                QString::fromStdString(event.status) % u'.';
        builder.emplace<TextElement>(resolution, MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatMediumBold);
        text += resolution;
    }

    builder.setMessageAndSearchText(text);
    return builder.release();
}

automod::HoldData makeAutoModReviewHoldData(
    QString notificationID, const QDateTime &time,
    const lib::payload::automod_message_hold::v2::Event &event)
{
    automod::HoldData data;
    data.notificationID = std::move(notificationID);
    data.broadcasterID = event.broadcasterUserID.qt();
    data.broadcasterLogin = event.broadcasterUserLogin.qt();
    data.broadcasterName = event.broadcasterUserName.qt();
    data.userID = event.userID.qt();
    data.userLogin = event.userLogin.qt();
    data.userName = event.userName.qt();
    data.messageID = event.messageID.qt();
    data.messageText = event.message.text.qt();
    data.receivedAt = time;
    std::visit(
        [&](const auto &reason) {
            fillAutoModReason(data, reason);
        },
        event.reason);
    return data;
}

automod::UpdateData makeAutoModReviewUpdateData(
    QString notificationID, const QDateTime &time,
    const lib::payload::automod_message_update::v2::Event &event)
{
    automod::UpdateData data;
    data.notificationID = std::move(notificationID);
    data.broadcasterID = event.broadcasterUserID.qt();
    data.broadcasterLogin = event.broadcasterUserLogin.qt();
    data.broadcasterName = event.broadcasterUserName.qt();
    data.userID = QString::fromStdString(event.userID);
    data.userLogin = QString::fromStdString(event.userLogin);
    data.userName = QString::fromStdString(event.userName);
    data.messageID = event.messageID.qt();
    data.messageText = event.message.text.qt();
    data.receivedAt = time;
    data.status = QString::fromStdString(event.status);
    data.moderatorID = QString::fromStdString(event.moderatorUserID);
    data.moderatorLogin = QString::fromStdString(event.moderatorUserLogin);
    data.moderatorName = QString::fromStdString(event.moderatorUserName);
    std::visit(
        [&](const auto &reason) {
            fillAutoModReason(data, reason);
        },
        event.reason);
    return data;
}

MessagePtr makeAutoModReviewMessage(TwitchChannel *channel,
                                    const automod::ReviewItem &item,
                                    AutoModReviewPresentation presentation)
{
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = item.receivedAt;
    builder->id = u"automod_" % item.messageID;
    builder->loginName = item.userLogin;
    builder->displayName = item.userName;
    builder->userID = item.userID;
    builder->channelName = item.broadcasterLogin;
    builder->flags.set(MessageFlag::PubSub, MessageFlag::EventSub,
                       MessageFlag::ModerationAction, MessageFlag::AutoMod,
                       MessageFlag::AutoModOffendingMessageHeader,
                       MessageFlag::AutoModOffendingMessage,
                       MessageFlag::InvalidReplyTarget);
    builder->flags.set(MessageFlag::AutoModBlockedTerm, item.blockedTerm);
    builder->autoModReview = std::make_shared<const automod::ReviewItem>(item);
    builder->highlightMatches = item.highlightMatches;
    const MessageElementFlags metadataTextFlags{
        MessageElementFlag::Text, MessageElementFlag::IgnoreExactMatch};

    if (presentation == AutoModReviewPresentation::ChatLegacy)
    {
        if (channel != nullptr)
        {
            builder
                .emplace<ChannelNameElement>(u'#' + item.broadcasterLogin,
                                             channel->channelAvatar())
                ->setLink({Link::JumpToChannel, item.broadcasterLogin});
        }
        else
        {
            builder
                .emplace<TextElement>(
                    u'#' + item.broadcasterLogin,
                    MessageElementFlags{MessageElementFlag::ChannelName,
                                        MessageElementFlag::IgnoreExactMatch},
                    MessageColor::Link)
                ->setLink({Link::JumpToChannel, item.broadcasterLogin});
        }
        builder.emplace<TimestampElement>(item.receivedAt.time());
        builder.emplace<BadgeElement>(
            makeAutoModBadge(), MessageElementFlag::BadgeChannelAuthority);
        builder
            .emplace<TextElement>(u"AutoMod:"_s, metadataTextFlags,
                                  QColor(0, 0, 255), FontStyle::ChatMediumBold)
            ->addFlags(MessageElementFlag::Username);
        builder.emplace<TextElement>(
            u"Held a message for "_s % item.reasonSummary % u'.',
            metadataTextFlags, MessageColor::Text);

        const bool pending = item.state == automod::ReviewState::Approving ||
                             item.state == automod::ReviewState::Denying ||
                             item.secondaryInProgress;
        const bool showActionRow =
            item.actionsAllowed && item.state == automod::ReviewState::Open;
        if (showActionRow)
        {
            builder
                .emplace<TextElement>(u"Allow"_s, metadataTextFlags,
                                      MessageColor(QColor(0, 255, 0)),
                                      FontStyle::ChatMediumBold)
                ->setLink({Link::AutoModReviewApprove, item.key})
                ->setTooltip(u"Allow this message (A)"_s);
            builder
                .emplace<TextElement>(u"Deny"_s, metadataTextFlags,
                                      MessageColor(QColor(255, 0, 0)),
                                      FontStyle::ChatMediumBold)
                ->setLink({Link::AutoModReviewDeny, item.key})
                ->setTooltip(u"Deny this message (D)"_s);
        }
        else if (pending)
        {
            builder.emplace<TextElement>(
                automod::reviewStateLabel(item), metadataTextFlags,
                MessageColor::System, FontStyle::ChatMediumBold);
        }
        else if (item.state == automod::ReviewState::Open &&
                 !item.actionsAllowed)
        {
            builder.emplace<TextElement>(u"Read only"_s, metadataTextFlags,
                                         MessageColor::System,
                                         FontStyle::ChatMedium);
        }
        else if (item.actionsAllowed &&
                 (item.failedAction != automod::ReviewAction::None ||
                  item.secondaryFailed))
        {
            const auto failedAction =
                item.secondaryFailed ? item.secondaryAction : item.failedAction;
            auto failureLabel = automod::reviewActionLabel(failedAction);
            if (!failureLabel.isEmpty())
            {
                failureLabel[0] = failureLabel[0].toUpper();
            }
            failureLabel += u" failed"_s;
            auto failureText = failureLabel;
            if (!item.statusDetail.isEmpty())
            {
                failureText += u" · "_s % item.statusDetail;
            }
            builder
                .emplace<TextElement>(failureText, metadataTextFlags,
                                      MessageColor::System,
                                      FontStyle::ChatMedium)
                ->setTooltip(item.statusDetail);
        }
        else if (item.state == automod::ReviewState::Approved ||
                 item.state == automod::ReviewState::Denied)
        {
            QString resolution = item.state == automod::ReviewState::Approved
                                     ? u"Approved"_s
                                     : u"Denied"_s;
            const auto moderator = item.moderatorName.isEmpty()
                                       ? item.moderatorLogin
                                       : item.moderatorName;
            if (!moderator.isEmpty())
            {
                resolution += u" by " % moderator;
            }
            builder.emplace<TextElement>(resolution, metadataTextFlags,
                                         MessageColor::System,
                                         FontStyle::ChatMedium);
        }
        else
        {
            const auto status = item.statusDetail.isEmpty()
                                    ? automod::reviewStateLabel(item)
                                    : item.statusDetail;
            builder.emplace<TextElement>(status, metadataTextFlags,
                                         MessageColor::System,
                                         FontStyle::ChatMedium);
        }

        builder.emplace<LinebreakElement>(metadataTextFlags);
        if (channel != nullptr)
        {
            builder.emplace<TwitchModerationElement>(true, false, false,
                                                     channel->weakFromThis());
        }
        else
        {
            builder.emplace<TwitchModerationElement>(true, false, false);
        }
        const auto displayName = localizedDisplayName(item);
        builder
            .emplace<MentionElement>(
                displayName + ':', item.userLogin, MessageColor::Text,
                channel != nullptr ? channel->getUserColor(item.userLogin)
                                   : QColor{})
            ->addFlags({MessageElementFlag::Username,
                        MessageElementFlag::IgnoreExactMatch});
        appendHeldMessageText(builder, channel, item.messageText);

        builder->messageText = item.messageText;
        builder->searchText = item.broadcasterLogin % u' ' % item.userLogin %
                              u' ' % item.messageText % u' ' %
                              item.reasonSummary % u' ' %
                              automod::reviewStateLabel(item);
        return builder.release();
    }

    const bool isReviewQueue =
        presentation == AutoModReviewPresentation::ReviewQueue;

    const auto reviewReason = [&item] {
        if (!item.reasonCategory.isEmpty())
        {
            auto reason = item.reasonCategory;
            if (item.reasonLevel > 0)
            {
                reason +=
                    u" \u00b7 Level "_s % QString::number(item.reasonLevel);
            }
            return reason;
        }

        return QString{};
    }();
    if (isReviewQueue)
    {
        builder
            .emplace<TextElement>(
                u'#' + item.broadcasterLogin,
                MessageElementFlags{MessageElementFlag::ChannelName,
                                    MessageElementFlag::IgnoreExactMatch},
                MessageColor::Link, FontStyle::ChatSmall)
            ->setLink({Link::JumpToChannel, item.broadcasterLogin});
        if (getSettings()->autoModReviewShowReason && !reviewReason.isEmpty())
        {
            builder.emplace<TextElement>(u"\u00b7"_s, metadataTextFlags,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
            builder.emplace<TextElement>(reviewReason, metadataTextFlags,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
        }
        builder.emplace<LinebreakElement>(metadataTextFlags);
        builder.emplace<TimestampElement>(item.receivedAt.time());
    }
    else
    {
        if (getSettings()->autoModReviewShowReason && !reviewReason.isEmpty())
        {
            builder.emplace<TextElement>(reviewReason, metadataTextFlags,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
            builder.emplace<LinebreakElement>(metadataTextFlags);
        }

        builder.emplace<TimestampElement>(item.receivedAt.time());
    }
    const auto displayName = localizedDisplayName(item);
    builder
        .emplace<MentionElement>(
            displayName + ':', item.userLogin, MessageColor::Text,
            channel != nullptr ? channel->getUserColor(item.userLogin)
                               : QColor{})
        ->addFlags({MessageElementFlag::Username,
                    MessageElementFlag::IgnoreExactMatch});
    appendHeldMessageText(builder, channel, item.messageText);

    const MessageElementFlags expandedFlags{
        MessageElementFlag::Text, MessageElementFlag::AutoModReviewExpanded,
        MessageElementFlag::IgnoreExactMatch};

    if (getSettings()->autoModReviewShowContext && !item.context.isEmpty())
    {
        builder.emplace<LinebreakElement>(expandedFlags);
        builder.emplace<TextElement>(
            isReviewQueue ? u"Earlier messages"_s : u"Recent local context"_s,
            expandedFlags, MessageColor::System,
            isReviewQueue ? FontStyle::ChatSmall : FontStyle::ChatMediumBold);
        const auto addContext = [&builder, &expandedFlags, isReviewQueue](
                                    const automod::ContextMessage &context) {
            builder.emplace<LinebreakElement>(expandedFlags);
            if (!isReviewQueue)
            {
                builder.emplace<TextElement>(
                    context.receivedAt.isValid()
                        ? context.receivedAt.time().toString(u"HH:mm"_s)
                        : u"Earlier"_s,
                    expandedFlags, MessageColor::System);
            }
            builder.emplace<TextElement>(
                context.displayName + ':', expandedFlags,
                isReviewQueue ? MessageColor::Link : MessageColor::System,
                isReviewQueue ? FontStyle::ChatSmall
                              : FontStyle::ChatMediumBold);
            builder.emplace<TextElement>(
                context.text, expandedFlags, MessageColor::Text,
                isReviewQueue ? FontStyle::ChatSmall : FontStyle::ChatMedium);
        };
        if (getSettings()->autoModReviewContextOrder != 1)
        {
            for (auto it = item.context.crbegin(); it != item.context.crend();
                 ++it)
            {
                addContext(*it);
            }
        }
        else
        {
            for (const auto &context : item.context)
            {
                addContext(context);
            }
        }
    }
    builder.emplace<LinebreakElement>(metadataTextFlags);
    const bool showActionRow =
        item.actionsAllowed && item.state == automod::ReviewState::Open;
    const bool pending = item.state == automod::ReviewState::Approving ||
                         item.state == automod::ReviewState::Denying ||
                         item.secondaryInProgress;
    if (showActionRow)
    {
        const auto timeoutValue = [&item](int seconds) {
            return item.key % u'\n' % QString::number(seconds);
        };
        builder
            .emplace<TextElement>(u"Approve"_s, metadataTextFlags,
                                  MessageColor(QColor(0, 255, 0)),
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewApprove, item.key})
            ->setTooltip(u"Approve this message (A)"_s);
        builder
            .emplace<TextElement>(u"Deny"_s, metadataTextFlags,
                                  MessageColor(QColor(255, 0, 0)),
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewDeny, item.key})
            ->setTooltip(u"Deny this message (D)"_s);
        builder.emplace<TextElement>(u"·"_s, metadataTextFlags,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);
        const auto addTimeout = [&](int seconds, QString tooltip) {
            builder
                .emplace<AutoModActionElement>(AutoModActionKind::Timeout,
                                               seconds)
                ->setLink({Link::AutoModReviewTimeout, timeoutValue(seconds)})
                ->setTooltip(std::move(tooltip));
        };
        addTimeout(30, u"Deny and timeout for 30 seconds"_s);
        addTimeout(60, u"Deny and timeout for 1 minute"_s);
        addTimeout(5 * 60, u"Deny and timeout for 5 minutes"_s);
        addTimeout(10 * 60, u"Deny and timeout for 10 minutes"_s);
        addTimeout(60 * 60, u"Deny and timeout for 1 hour"_s);
        addTimeout(24 * 60 * 60, u"Deny and timeout for 1 day"_s);
        builder.emplace<AutoModActionElement>(AutoModActionKind::Ban)
            ->setLink({Link::AutoModReviewBan, item.key})
            ->setTooltip(u"Deny and permanently ban this user (B)"_s);
    }
    else if (pending)
    {
        builder.emplace<TextElement>(automod::reviewStateLabel(item),
                                     metadataTextFlags, MessageColor::System,
                                     FontStyle::ChatSmall);
    }
    else if (item.state == automod::ReviewState::Open && !item.actionsAllowed)
    {
        builder.emplace<TextElement>(u"Read only"_s, metadataTextFlags,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);
    }
    else if (item.actionsAllowed &&
             (item.failedAction != automod::ReviewAction::None ||
              item.secondaryFailed))
    {
        const auto failedAction =
            item.secondaryFailed ? item.secondaryAction : item.failedAction;
        auto failureLabel = automod::reviewActionLabel(failedAction);
        if (!failureLabel.isEmpty())
        {
            failureLabel[0] = failureLabel[0].toUpper();
        }
        failureLabel += u" failed"_s;
        auto failureText = failureLabel;
        if (!item.statusDetail.isEmpty())
        {
            failureText += u" · "_s % item.statusDetail;
        }
        builder
            .emplace<TextElement>(failureText, metadataTextFlags,
                                  MessageColor::System, FontStyle::ChatSmall)
            ->setTooltip(item.statusDetail);
    }
    else if (item.state == automod::ReviewState::Approved ||
             item.state == automod::ReviewState::Denied)
    {
        QString resolution = item.state == automod::ReviewState::Approved
                                 ? u"Approved"_s
                                 : u"Denied"_s;
        const auto moderator = item.moderatorName.isEmpty()
                                   ? item.moderatorLogin
                                   : item.moderatorName;
        if (!moderator.isEmpty())
        {
            resolution += u" by " % moderator;
        }
        builder.emplace<TextElement>(resolution, metadataTextFlags,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);
    }
    else if (!item.statusDetail.isEmpty())
    {
        builder.emplace<TextElement>(item.statusDetail, metadataTextFlags,
                                     MessageColor::System,
                                     FontStyle::ChatSmall);
    }

    if (isReviewQueue &&
        getSettings()->autoModReviewShowShortcutHints.getValue())
    {
        builder.emplace<LinebreakElement>(expandedFlags);
        const auto addHint = [&builder, &expandedFlags](
                                 QString key, QString action, bool separator) {
            builder.emplace<TextElement>(std::move(key), expandedFlags,
                                         MessageColor::Link,
                                         FontStyle::ChatSmall);
            builder.emplace<TextElement>(std::move(action), expandedFlags,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
            if (separator)
            {
                builder.emplace<TextElement>(u"·"_s, expandedFlags,
                                             MessageColor::System,
                                             FontStyle::ChatSmall);
            }
        };
        addHint(u"↑/↓"_s, u"Select"_s, true);
        if (showActionRow)
        {
            addHint(u"A"_s, u"Approve"_s, true);
            addHint(u"D"_s, u"Deny"_s, true);
            addHint(u"T"_s, u"Timeout"_s, true);
            addHint(u"B"_s, u"Ban"_s, true);
        }
        addHint(u"Enter"_s, u"Usercard"_s, false);
    }

    builder->messageText = item.messageText;
    builder->searchText = item.broadcasterLogin % u' ' % item.userLogin % u' ' %
                          item.messageText % u' ' % item.reasonSummary % u' ' %
                          automod::reviewStateLabel(item);
    return builder.release();
}

MessagePtr makeSuspiciousUserMessageHeader(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_message::v1::Event &event)
{
    EventSubMessageBuilder builder(channel);

    // Builder for low trust user message with explanation
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->serverReceivedTime = time;
    builder->flags.set(MessageFlag::LowTrustUsers);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);

    // Suspicious user header message
    QString prefix = u"Suspicious User:"_s;
    builder.emplace<TextElement>(prefix, MessageElementFlag::Text,
                                 MessageColor(QColor(0, 0, 255)),
                                 FontStyle::ChatMediumBold);

    QString headerMessage;
    if (event.lowTrustStatus == lib::suspicious_users::Status::Restricted)
    {
        headerMessage = u"Restricted"_s;
        builder->flags.set(MessageFlag::RestrictedMessage);
    }
    else
    {
        headerMessage = u"Monitored"_s;
        builder->flags.set(MessageFlag::MonitoredMessage);
    }

    auto hasType = [&](lib::suspicious_users::Type type) {
        return std::ranges::find(event.types, type) != event.types.end();
    };

    if (hasType(lib::suspicious_users::Type::BanEvaderDetector))
    {
        QString evader;
        if (event.banEvasionEvaluation ==
            lib::suspicious_users::BanEvasionEvaluation::Likely)
        {
            evader = u"likely"_s;
        }
        else
        {
            evader = u"possible"_s;
        }

        headerMessage += QStringLiteral(". Detected as ") % evader %
                         QStringLiteral(" ban evader");
    }

    if (hasType(lib::suspicious_users::Type::SharedChannelBan))
    {
        headerMessage += QStringLiteral(". Banned in ") %
                         QString::number(event.sharedBanChannelIds.size()) %
                         QStringLiteral(" shared channels");
    }

    builder.emplace<TextElement>(headerMessage, MessageElementFlag::Text,
                                 MessageColor::Text);

    builder.setMessageAndSearchText(prefix % u" " % headerMessage);

    return builder.release();
}

MessagePtr makeSuspiciousUserMessageBody(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_message::v1::Event &event)
{
    EventSubMessageBuilder builder(channel);
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->serverReceivedTime = time;
    if (event.lowTrustStatus == lib::suspicious_users::Status::Restricted)
    {
        builder->flags.set(MessageFlag::RestrictedMessage);
    }
    else
    {
        builder->flags.set(MessageFlag::MonitoredMessage);
    }

    builder
        .emplace<ChannelNameElement>(u'#' + event.broadcasterUserLogin.qt(),
                                     channel->channelAvatar())
        ->setLink({Link::JumpToChannel, event.broadcasterUserLogin.qt()});
    builder.emplace<TimestampElement>(time.time());
    builder.emplace<TwitchModerationElement>(true, false, false,
                                             channel->weakFromThis());
    builder->loginName = event.userLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::LowTrustUsers);

    // sender username
    builder
        .emplace<MentionElement>(event.userName.qt() + ":",
                                 event.userLogin.qt(), MessageColor::Text,
                                 channel->getUserColor(event.userLogin.qt()))
        ->addFlags(MessageElementFlag::Username);

    // sender's message caught by AutoMod
    // XXX: add the structured message here
    builder.emplace<TextElement>(event.message.text.qt(),
                                 MessageElementFlag::Text, MessageColor::Text);

    builder.setMessageAndSearchText(event.userName.qt() % u": " %
                                    event.message.text.qt());

    return builder.release();
}

MessagePtr makeSuspiciousUserUpdate(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_update::v1::Event &event)
{
    EventSubMessageBuilder builder(channel, time);
    builder->flags.set(MessageFlag::DoNotTriggerNotification,
                       MessageFlag::ModerationAction);
    builder->loginName = event.moderatorUserLogin.qt();

    QString text;
    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);

    switch (event.lowTrustStatus)
    {
        case lib::suspicious_users::Status::None: {
            builder.emplaceSystemTextAndUpdate(u"removed"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"from the suspicious user list."_s, text);
        }
        break;

        case lib::suspicious_users::Status::ActiveMonitoring: {
            builder.emplaceSystemTextAndUpdate(u"added"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"as a monitored suspicious chatter."_s, text);
        }
        break;

        case lib::suspicious_users::Status::Restricted: {
            builder.emplaceSystemTextAndUpdate(u"added"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"as a restricted suspicious chatter."_s, text);
        }
        break;
    }

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtr makeUserMessageHeldMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_chat_user_message_hold::v1::Event &event)
{
    QString text("AutoMod: Hey! Your message is being checked by mods and has "
                 "not been sent.");
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::AutoMod);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);
    builder.emplace<TextElement>(
        "Hey! Your message is being checked by mods and has not been sent.",
        MessageElementFlag::Text, MessageColor::Text);

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtr makeUserMessageUpdateMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_chat_user_message_update::v1::Event &event)
{
    using lib::payload::channel_chat_user_message_update::v1::Status;

    QString text("AutoMod: ");
    EventSubMessageBuilder builder(channel);
    builder->serverReceivedTime = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::AutoMod);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);

    switch (event.status)
    {
        case Status::Approved:
            text += "Mods have accepted your message.";
            builder.emplace<TextElement>("Mods have accepted your message.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;

        case Status::Denied:
            text += "Mods have denied your message.";
            builder.emplace<TextElement>("Mods have denied your message.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;

        case Status::Invalid:
            text += "Your message was lost in the void.";
            builder.emplace<TextElement>("Your message was lost in the void.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;
    }

    builder.setMessageAndSearchText(text);

    return builder.release();
}

}  // namespace chatterino::eventsub
