#include "messages/Message.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "messages/MessageThread.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/DebugCount.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/helper/ScrollbarHighlight.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

namespace chatterino {

using namespace literals;

namespace {

bool isDigit(QChar c)
{
    const auto value = c.unicode();
    return value >= '0' && value <= '9';
}

bool isLowerHexLetter(QChar c)
{
    const auto value = c.unicode();
    return value >= 'a' && value <= 'f';
}

bool isUpperHexLetter(QChar c)
{
    const auto value = c.unicode();
    return value >= 'A' && value <= 'F';
}

bool isUuidVariant(QChar c)
{
    return c == QLatin1Char('8') || c == QLatin1Char('9') ||
           c == QLatin1Char('a') || c == QLatin1Char('b') ||
           c == QLatin1Char('A') || c == QLatin1Char('B');
}

QString highlightMatchSourceName(HighlightMatchSource source)
{
    switch (source)
    {
        case HighlightMatchSource::Phrase:
            return u"phrase"_s;
        case HighlightMatchSource::WordList:
            return u"moderation-list"_s;
        case HighlightMatchSource::AutoMod:
            return u"automod"_s;
    }
    return {};
}

}

Message::Message()
    : parseTime(QTime::currentTime())
{
    DebugCount::increase(DebugObject::Message);
}

Message::~Message()
{
    DebugCount::decrease(DebugObject::Message);
}

ScrollbarHighlight Message::getScrollBarHighlight() const
{
    if (this->isHiddenByClientNonce())
    {
        return {};
    }
    if (this->flags.has(MessageFlag::Highlighted) ||
        this->flags.has(MessageFlag::HighlightedWhisper))
    {
        return {
            this->highlightColor,
        };
    }

    if (this->flags.has(MessageFlag::WatchStreak) &&
        getSettings()->enableWatchStreakHighlight)
    {
        return {
            ColorProvider::instance().color(ColorType::WatchStreak),
        };
    }

    if (this->flags.has(MessageFlag::Subscription) &&
        getSettings()->enableSubHighlight)
    {
        return {
            ColorProvider::instance().color(ColorType::Subscription),
        };
    }

    if (this->flags.has(MessageFlag::RedeemedHighlight) ||
        (this->flags.has(MessageFlag::RedeemedChannelPointReward) &&
         !this->usesTwitchGigantifyPresentation()))
    {
        return {
            ColorProvider::instance().color(ColorType::RedeemedHighlight),
            ScrollbarHighlight::Default,
            true,
        };
    }

    if (this->flags.has(MessageFlag::ElevatedMessage))
    {
        return {
            ColorProvider::instance().color(
                ColorType::ElevatedMessageHighlight),
            ScrollbarHighlight::Default,
            false,
            false,
            true,
        };
    }

    if (this->flags.has(MessageFlag::FirstMessage))
    {
        return {
            ColorProvider::instance().color(ColorType::FirstMessageHighlight),
            ScrollbarHighlight::Default,
            false,
            true,
        };
    }

    if (this->flags.has(MessageFlag::AutoModOffendingMessage) ||
        this->flags.has(MessageFlag::AutoModOffendingMessageHeader))
    {
        return {
            ColorProvider::instance().color(ColorType::AutomodHighlight),
        };
    }

    if (this->flags.has(MessageFlag::ChatWarning))
    {
        return {
            ColorProvider::instance().color(ColorType::AutomodHighlight),
        };
    }

    if (this->flags.has(MessageFlag::Announcement) &&
        getSettings()->enableAnnouncementHighlight)
    {
        return {
            ColorProvider::instance().color(colorTypeFromHelixAnnouncementColor(
                this->announcementColor,
                getSettings()->enableColoredAnnouncementHighlight)),
        };
    }

    if (this->flags.has(MessageFlag::UncategorizedNotification))
    {
        return {
            ColorProvider::instance().color(ColorType::Subscription),
        };
    }

    return {};
}

std::shared_ptr<Message> Message::clone() const
{
    auto cloned = std::make_shared<Message>();
    cloned->flags = this->flags;
    cloned->parseTime = this->parseTime;
    cloned->id = this->id;
    cloned->searchText = this->searchText;
    cloned->messageText = this->messageText;
    cloned->loginName = this->loginName;
    cloned->displayName = this->displayName;
    cloned->localizedName = this->localizedName;
    cloned->userID = this->userID;
    cloned->timeoutUser = this->timeoutUser;
    cloned->channelName = this->channelName;
    cloned->usernameColor = this->usernameColor;
    cloned->serverReceivedTime = this->serverReceivedTime;
    cloned->twitchBadges = this->twitchBadges;
    if (this->sharedChatSourceBadges)
    {
        cloned->sharedChatSourceBadges =
            std::make_unique<const std::vector<TwitchBadge>>(
                *this->sharedChatSourceBadges);
    }
    cloned->twitchBadgeInfos = this->twitchBadgeInfos;
    cloned->externalBadges = this->externalBadges;
    cloned->highlightColor = this->highlightColor;
    cloned->highlightMatches = this->highlightMatches;
    cloned->autoModReview = this->autoModReview;
    cloned->replyThread = this->replyThread;
    cloned->replyParent = this->replyParent;
    cloned->translatedFrom = this->translatedFrom;
    cloned->count = this->count;
    cloned->reward = this->reward;
    cloned->sharedChatSourceId = this->sharedChatSourceId;
    cloned->platform = this->platform;
    cloned->clientDetection = this->clientDetection;
    cloned->emoteOnly = this->emoteOnly;
    cloned->bits = this->bits;
    cloned->announcementColor = this->announcementColor;
    std::ranges::transform(this->elements, std::back_inserter(cloned->elements),
                           [](const auto &element) {
                               return element->clone();
                           });
    return cloned;
}

QJsonObject Message::toJson() const
{
    QJsonObject msg{
        {"flags"_L1, qmagicenum::enumFlagsName(this->flags.value())},
        {"id"_L1, this->id},
        {"searchText"_L1, this->searchText},
        {"messageText"_L1, this->messageText},
        {"loginName"_L1, this->loginName},
        {"displayName"_L1, this->displayName},
        {"localizedName"_L1, this->localizedName},
        {"userID"_L1, this->userID},
        {"timeoutUser"_L1, this->timeoutUser},
        {"channelName"_L1, this->channelName},
        {"usernameColor"_L1, this->usernameColor.name(QColor::HexArgb)},
        {"count"_L1, static_cast<qint64>(this->count)},
        {"serverReceivedTime"_L1,
         this->serverReceivedTime.toString(Qt::ISODate)},
        {"frozen"_L1, this->frozen},
    };

    QJsonArray twitchBadges;
    for (const auto &badge : this->twitchBadges)
    {
        twitchBadges.append(badge.key_);
    }
    msg["twitchBadges"_L1] = twitchBadges;

    QJsonObject twitchBadgeInfos;
    for (const auto &[key, value] : this->twitchBadgeInfos)
    {
        twitchBadgeInfos.insert(key, value);
    }
    msg["twitchBadgeInfos"_L1] = twitchBadgeInfos;

    msg["externalBadges"_L1] = QJsonArray::fromStringList(this->externalBadges);

    if (this->highlightColor)
    {
        msg["highlightColor"_L1] = this->highlightColor->name(QColor::HexArgb);
    }

    if (this->highlightMatches && !this->highlightMatches->empty())
    {
        QJsonArray matches;
        for (const auto &match : *this->highlightMatches)
        {
            matches.append(QJsonObject{
                {"start"_L1, static_cast<qint64>(match.start)},
                {"length"_L1, static_cast<qint64>(match.length)},
                {"color"_L1, match.color.name(QColor::HexArgb)},
                {"rule"_L1, match.ruleName},
                {"pattern"_L1, match.pattern},
                {"source"_L1, highlightMatchSourceName(match.source)},
                {"style"_L1, highlightMatchStyleName(match.style)},
                {"paintID"_L1, match.paintID},
            });
        }
        msg["highlightMatches"_L1] = matches;
    }

    if (this->replyThread)
    {
        msg["replyThread"_L1] = this->replyThread->toJson();
    }

    if (this->replyParent)
    {
        msg["replyParent"_L1] = this->replyParent->id;
    }

    if (this->reward)
    {
        msg["reward"_L1] = this->reward->toJson();
    }

    if (this->bits > 0)
    {
        msg["bits"_L1] = static_cast<qint64>(this->bits);
    }

    if (this->flags.has(MessageFlag::Announcement))
    {
        msg["announcementColor"_L1] =
            qmagicenum::enumNameString(this->announcementColor);
    }

    if (!getApp()->isTest())
    {
        msg["parseTime"_L1] = this->parseTime.toString(Qt::ISODate);
    }

    QJsonArray elements;
    for (const auto &element : this->elements)
    {
        elements.append(element->toJson());
    }
    msg["elements"_L1] = elements;

    if (this->platform != MessagePlatform::AnyOrTwitch)
    {
        msg["platform"_L1] = qmagicenum::enumNameString(this->platform);
    }

    return msg;
}

QString Message::clientDetectionStatusToString(ClientDetectionStatus status)
{
    switch (status)
    {
        case ClientDetectionStatus::Web:
            return QStringLiteral("Web");
        case ClientDetectionStatus::Android:
            return QStringLiteral("Android");
        case ClientDetectionStatus::IOS:
            return QStringLiteral("iOS");
        case ClientDetectionStatus::Abnormal:
            return QStringLiteral("Abnormal");
        case ClientDetectionStatus::Unknown:
        default:
            return QStringLiteral("Unknown");
    }
}

bool Message::isHiddenByClientNonce() const
{
    return this->flags.has(MessageFlag::ExtendedClientNonce) &&
           !getSettings()->extendedClientNonceParsing;
}

bool Message::usesTwitchGigantifyPresentation() const
{
    return this->flags.has(MessageFlag::GigantifiedEmote) &&
           getSettings()->enableGigantifyEmotes;
}

Message::ClientDetectionStatus Message::classifyClientNonce(
    const QString &nonce)
{
    using Status = ClientDetectionStatus;

    if (nonce.isEmpty())
    {
        return Status::Abnormal;
    }

    if (nonce.size() == 32)
    {
        const bool web = std::all_of(nonce.cbegin(), nonce.cend(), [](QChar c) {
            return isDigit(c) || isLowerHexLetter(c);
        });
        return web ? Status::Web : Status::Abnormal;
    }

    if (nonce.size() != 36 || nonce.at(8) != QLatin1Char('-') ||
        nonce.at(13) != QLatin1Char('-') ||
        nonce.at(18) != QLatin1Char('-') ||
        nonce.at(23) != QLatin1Char('-') ||
        nonce.at(14) != QLatin1Char('4') || !isUuidVariant(nonce.at(19)))
    {
        return Status::Abnormal;
    }

    bool sawLower = false;
    bool sawUpper = false;
    for (qsizetype index = 0; index < nonce.size(); ++index)
    {
        if (index == 8 || index == 13 || index == 18 || index == 23)
        {
            continue;
        }

        const auto c = nonce.at(index);
        if (isDigit(c))
        {
            continue;
        }
        if (isLowerHexLetter(c))
        {
            sawLower = true;
            continue;
        }
        if (isUpperHexLetter(c))
        {
            sawUpper = true;
            continue;
        }

        return Status::Abnormal;
    }

    if (sawLower && sawUpper)
    {
        return Status::Abnormal;
    }
    return sawUpper ? Status::IOS : Status::Android;
}

Message::ReplyStatus Message::isReplyable() const
{
    if (this->loginName.isEmpty())
    {

        return ReplyStatus::NotReplyable;
    }

    constexpr int oneDayInSeconds = 24 * 60 * 60;
    bool messageReplyable = true;
    if (this->flags.hasAny({MessageFlag::System, MessageFlag::Subscription,
                            MessageFlag::Timeout, MessageFlag::Whisper,
                            MessageFlag::ModerationAction,
                            MessageFlag::InvalidReplyTarget}) ||
        this->serverReceivedTime.secsTo(QDateTime::currentDateTime()) >
            oneDayInSeconds)
    {
        messageReplyable = false;
    }

    if (this->replyThread != nullptr)
    {
        if (const auto &rootPtr = this->replyThread->root(); rootPtr != nullptr)
        {
            assert(this != rootPtr.get());
            if (rootPtr->isReplyable() == ReplyStatus::NotReplyable)
            {

                return ReplyStatus::NotReplyableDueToThread;
            }

            return messageReplyable ? ReplyStatus::ReplyableWithThread
                                    : ReplyStatus::NotReplyableWithThread;
        }
    }

    return messageReplyable ? ReplyStatus::Replyable
                            : ReplyStatus::NotReplyable;
}

}
