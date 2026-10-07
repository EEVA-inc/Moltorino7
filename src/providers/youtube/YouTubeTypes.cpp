#include "providers/youtube/YouTubeTypes.hpp"

#include <QJsonValue>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

using namespace chatterino;

std::chrono::seconds parseDuration(const QJsonValue &value)
{
    qint64 seconds = 0;
    bool ok = false;

    if (value.isString())
    {
        seconds = value.toString().toLongLong(&ok);
    }
    else if (value.isDouble())
    {
        seconds = value.toInteger();
        ok = seconds > 0;
    }

    if (!ok || seconds <= 0)
    {
        return {};
    }

    const auto maxSeconds =
        static_cast<qint64>(std::chrono::seconds::max().count());
    return std::chrono::seconds{std::min(seconds, maxSeconds)};
}

QDateTime parsePublishedAt(const QString &value)
{
    auto publishedAt = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!publishedAt.isValid())
    {
        publishedAt = QDateTime::fromString(value, Qt::ISODate);
    }
    return publishedAt.isValid() ? publishedAt.toLocalTime() : QDateTime{};
}

QString normalizedMessageText(QStringView text)
{
    return text.toString().normalized(QString::NormalizationForm_C);
}

bool isRecentPendingMatch(
    const YouTubePendingSend &pending, const QDateTime &publishedAt,
    std::chrono::steady_clock::time_point now)
{
    if (publishedAt.isValid() && pending.sentAt.isValid())
    {
        const auto window = YouTubePendingSendTracker::MATCH_WINDOW.count();
        if (publishedAt < pending.sentAt.addSecs(-window) ||
            publishedAt > pending.sentAt.addSecs(window))
        {
            return false;
        }
    }
    if (pending.trackedAt.time_since_epoch().count() == 0)
    {
        return true;
    }

    return now >= pending.trackedAt &&
           now - pending.trackedAt <= YouTubePendingSendTracker::MATCH_WINDOW;
}

}

namespace chatterino {

QString visibleYouTubeName(QString name)
{
    name = name.trimmed();
    if (name.startsWith(u'@'))
    {
        name.remove(0, 1);
    }
    return name;
}

QString youtubeChannelUrl(const QString &channelID)
{
    if (channelID.isEmpty())
    {
        return {};
    }

    return QStringLiteral("https://www.youtube.com/channel/") +
           QString::fromUtf8(QUrl::toPercentEncoding(channelID));
}

bool isTrustedYouTubeImageUrl(const QString &value)
{
    const QUrl url(value);
    if (!url.isValid() || url.scheme() != QStringLiteral("https"))
    {
        return false;
    }

    const auto host = url.host().toLower();
    return host == QStringLiteral("yt3.ggpht.com") ||
           host.endsWith(QStringLiteral(".ggpht.com")) ||
           host.endsWith(QStringLiteral(".googleusercontent.com"));
}

void applyYouTubeAuthorName(YouTubeAuthor &author, QString name)
{
    name = name.trimmed();
    if (name.startsWith(u'@'))
    {
        author.handle = name.sliced(1);
        author.displayName = author.handle;
        return;
    }
    author.displayName = std::move(name);
}

bool shouldJoinYouTubeMessageRuns(const YouTubeMessageRun &left,
                                  const YouTubeMessageRun &right) noexcept
{
    if (left.text.isEmpty() || right.text.isEmpty() ||
        left.text.back().isSpace() || right.text.front().isSpace())
    {
        return false;
    }

    const auto isRenderedCustomEmoji = [](const YouTubeMessageRun &run) {
        return run.kind == YouTubeMessageRun::Kind::Emoji &&
               run.customEmoji && !run.emojiImageUrl.isEmpty();
    };
    if (left.kind == YouTubeMessageRun::Kind::Emoji ||
        right.kind == YouTubeMessageRun::Kind::Emoji)
    {
        return (left.kind != YouTubeMessageRun::Kind::Emoji ||
                isRenderedCustomEmoji(left)) &&
               (right.kind != YouTubeMessageRun::Kind::Emoji ||
                isRenderedCustomEmoji(right));
    }

    const auto isCompactWritingScript = [](QChar character) {
        switch (character.script())
        {
            case QChar::Script_Han:
            case QChar::Script_Hiragana:
            case QChar::Script_Katakana:
            case QChar::Script_Hangul:
            case QChar::Script_Bopomofo:
            case QChar::Script_Yi:
            case QChar::Script_Thai:
            case QChar::Script_Lao:
            case QChar::Script_Khmer:
            case QChar::Script_Myanmar:
                return true;
            default:
                return false;
        }
    };

    if (isCompactWritingScript(left.text.back()) ||
        isCompactWritingScript(right.text.front()))
    {
        return true;
    }

    return !(left.text.back().isLetterOrNumber() &&
             right.text.front().isLetterOrNumber());
}

void YouTubePendingSendTracker::track(YouTubePendingSend send)
{
    if (send.localMessageID.isEmpty() || send.liveChatID.isEmpty() ||
        send.author.channelId.isEmpty())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::erase_if(this->pending_, [&send](const auto &pending) {
        return pending.localMessageID == send.localMessageID;
    });

    if (send.visibleMessageID.isEmpty())
    {
        send.visibleMessageID = send.localMessageID;
    }
    send.text = normalizedMessageText(send.text);
    if (!send.sentAt.isValid())
    {
        send.sentAt = QDateTime::currentDateTimeUtc();
    }
    if (send.trackedAt.time_since_epoch().count() == 0)
    {
        send.trackedAt = now;
    }

    while (this->pending_.size() >= MAX_PENDING_SENDS)
    {
        this->pending_.pop_front();
    }
    this->pending_.emplace_back(std::move(send));
}

std::optional<QString> YouTubePendingSendTracker::acknowledge(
    QStringView localMessageID, QString acknowledgedMessageID)
{
    const auto pending = std::ranges::find_if(
        this->pending_, [localMessageID](const auto &candidate) {
            return candidate.localMessageID == localMessageID;
        });
    if (pending == this->pending_.end())
    {
        return std::nullopt;
    }

    auto previousVisibleID = pending->visibleMessageID;
    if (!acknowledgedMessageID.isEmpty())
    {
        pending->acknowledgedMessageID = acknowledgedMessageID;
        pending->visibleMessageID = std::move(acknowledgedMessageID);
    }
    return previousVisibleID;
}

std::optional<YouTubePendingSend> YouTubePendingSendTracker::takeMatching(
    const YouTubeMessage &message,
    std::chrono::steady_clock::time_point now)
{
    if (message.kind != YouTubeMessageKind::Text ||
        message.liveChatId.isEmpty())
    {
        return std::nullopt;
    }

    auto pending = this->pending_.end();
    if (!message.id.isEmpty())
    {
        pending = std::ranges::find_if(
            this->pending_, [&message](const auto &candidate) {
                return candidate.liveChatID == message.liveChatId &&
                       !candidate.acknowledgedMessageID.isEmpty() &&
                       candidate.acknowledgedMessageID == message.id;
            });
    }

    if (pending == this->pending_.end() &&
        !message.author.channelId.isEmpty())
    {
        const auto normalizedText = normalizedMessageText(message.text);
        pending = std::ranges::find_if(
            this->pending_,
            [&message, &normalizedText, now](const auto &candidate) {
                return candidate.liveChatID == message.liveChatId &&
                       candidate.author.channelId ==
                           message.author.channelId &&
                       candidate.text == normalizedText &&
                       isRecentPendingMatch(candidate, message.publishedAt,
                                            now);
            });
    }

    if (pending == this->pending_.end())
    {
        return std::nullopt;
    }

    auto matched = std::move(*pending);
    this->pending_.erase(pending);
    return matched;
}

std::optional<YouTubePendingSend> YouTubePendingSendTracker::takeByLocalID(
    QStringView localMessageID)
{
    const auto pending = std::ranges::find_if(
        this->pending_, [localMessageID](const auto &candidate) {
            return candidate.localMessageID == localMessageID;
        });
    if (pending == this->pending_.end())
    {
        return std::nullopt;
    }

    auto matched = std::move(*pending);
    this->pending_.erase(pending);
    return matched;
}

std::optional<YouTubePendingSend> YouTubePendingSendTracker::takeByMessageID(
    QStringView messageID)
{
    const auto pending = std::ranges::find_if(
        this->pending_, [messageID](const auto &candidate) {
            return candidate.localMessageID == messageID ||
                   candidate.visibleMessageID == messageID ||
                   candidate.acknowledgedMessageID == messageID;
        });
    if (pending == this->pending_.end())
    {
        return std::nullopt;
    }

    auto matched = std::move(*pending);
    this->pending_.erase(pending);
    return matched;
}

std::vector<QString>
YouTubePendingSendTracker::unacknowledgedLocalMessageIDs() const
{
    std::vector<QString> ids;
    ids.reserve(this->pending_.size());
    for (const auto &pending : this->pending_)
    {
        if (pending.acknowledgedMessageID.isEmpty())
        {
            ids.emplace_back(pending.localMessageID);
        }
    }
    return ids;
}

void YouTubePendingSendTracker::clear()
{
    this->pending_.clear();
}

std::size_t YouTubePendingSendTracker::size() const noexcept
{
    return this->pending_.size();
}

YouTubeMessageKind classifyYouTubeMessageType(QStringView sourceType) noexcept
{
    if (sourceType == u"textMessageEvent")
    {
        return YouTubeMessageKind::Text;
    }
    if (sourceType == u"superChatEvent")
    {
        return YouTubeMessageKind::SuperChat;
    }
    if (sourceType == u"newSponsorEvent")
    {
        return YouTubeMessageKind::NewMembership;
    }
    if (sourceType == u"memberMilestoneChatEvent")
    {
        return YouTubeMessageKind::MembershipMilestone;
    }
    if (sourceType == u"membershipGiftingEvent")
    {
        return YouTubeMessageKind::MembershipGift;
    }
    if (sourceType == u"tombstone" || sourceType == u"messageDeletedEvent" ||
        sourceType == u"messageRetractedEvent")
    {
        return YouTubeMessageKind::Tombstone;
    }
    if (sourceType == u"userBannedEvent")
    {
        return YouTubeMessageKind::UserBanned;
    }
    if (sourceType == u"chatEndedEvent")
    {
        return YouTubeMessageKind::ChatEnded;
    }
    return YouTubeMessageKind::Ignored;
}

YouTubeMessage parseYouTubeMessage(const QJsonObject &object)
{
    YouTubeMessage message;
    message.id = object.value(QStringLiteral("id")).toString();

    const auto snippet = object.value(QStringLiteral("snippet")).toObject();
    const auto authorDetails =
        object.value(QStringLiteral("authorDetails")).toObject();

    const auto sourceType = snippet.value(QStringLiteral("type")).toString();
    message.kind = classifyYouTubeMessageType(sourceType);
    message.liveChatId = snippet.value(QStringLiteral("liveChatId")).toString();
    message.text = snippet.value(QStringLiteral("displayMessage")).toString();
    if (message.text.isEmpty() && message.kind == YouTubeMessageKind::Text)
    {
        message.text = snippet.value(QStringLiteral("textMessageDetails"))
                           .toObject()
                           .value(QStringLiteral("messageText"))
                           .toString();
    }
    message.publishedAt = parsePublishedAt(
        snippet.value(QStringLiteral("publishedAt")).toString());

    message.author.channelId =
        authorDetails.value(QStringLiteral("channelId")).toString();
    if (message.author.channelId.isEmpty())
    {
        message.author.channelId =
            snippet.value(QStringLiteral("authorChannelId")).toString();
    }
    applyYouTubeAuthorName(
        message.author,
        authorDetails.value(QStringLiteral("displayName")).toString());
    message.author.avatarUrl =
        authorDetails.value(QStringLiteral("profileImageUrl")).toString();
    message.author.isOwner =
        authorDetails.value(QStringLiteral("isChatOwner")).toBool();
    message.author.isModerator =
        authorDetails.value(QStringLiteral("isChatModerator")).toBool();
    message.author.isVerified =
        authorDetails.value(QStringLiteral("isVerified")).toBool();
    message.author.isMember =
        authorDetails.value(QStringLiteral("isChatSponsor")).toBool();
    message.author.roleMetadataKnown =
        authorDetails.contains(QStringLiteral("isChatOwner")) ||
        authorDetails.contains(QStringLiteral("isChatModerator")) ||
        authorDetails.contains(QStringLiteral("isChatSponsor"));
    auto eventAuthorName = visibleYouTubeName(message.author.displayName);
    if (eventAuthorName.isEmpty())
    {
        eventAuthorName = message.author.channelId.isEmpty()
                              ? QStringLiteral("A viewer")
                              : message.author.channelId;
    }

    switch (message.kind)
    {
        case YouTubeMessageKind::SuperChat: {
            const auto details =
                snippet.value(QStringLiteral("superChatDetails")).toObject();
            message.amountDisplayString =
                details.value(QStringLiteral("amountDisplayString"))
                    .toString();
            message.text =
                details.value(QStringLiteral("userComment")).toString();
            if (message.amountDisplayString.isEmpty())
            {
                message.kind = YouTubeMessageKind::Ignored;
            }
            break;
        }
        case YouTubeMessageKind::NewMembership: {
            const auto displayText = message.text.trimmed();
            const auto details =
                snippet.value(QStringLiteral("newSponsorDetails")).toObject();
            message.membershipLevelName =
                details.value(QStringLiteral("memberLevelName")).toString();
            if (!displayText.isEmpty())
            {
                message.eventText = displayText;
            }
            else if (details.value(QStringLiteral("isUpgrade")).toBool())
            {
                message.eventText = message.membershipLevelName.isEmpty()
                                        ? QStringLiteral(
                                              "%1 upgraded their membership")
                                              .arg(eventAuthorName)
                                        : QStringLiteral(
                                              "%1 upgraded to %2")
                                              .arg(eventAuthorName,
                                                   message.membershipLevelName);
            }
            else
            {
                message.eventText = message.membershipLevelName.isEmpty()
                                        ? QStringLiteral(
                                              "%1 became a member")
                                              .arg(eventAuthorName)
                                        : QStringLiteral("%1 joined as %2")
                                              .arg(eventAuthorName,
                                                   message.membershipLevelName);
            }
            message.text.clear();
            message.author.isMember = true;
            break;
        }
        case YouTubeMessageKind::MembershipMilestone: {
            const auto details =
                snippet.value(QStringLiteral("memberMilestoneChatDetails"))
                    .toObject();
            message.text =
                details.value(QStringLiteral("userComment")).toString();
            message.memberMonths =
                details.value(QStringLiteral("memberMonth")).toInt();
            message.membershipLevelName =
                details.value(QStringLiteral("memberLevelName")).toString();
            if (message.memberMonths > 0)
            {
                message.eventText =
                    QStringLiteral("Member for %1 month%2")
                        .arg(message.memberMonths)
                        .arg(message.memberMonths == 1 ? QString{}
                                                       : QStringLiteral("s"));
            }
            else
            {
                message.eventText = QStringLiteral("Member milestone");
            }
            message.author.isMember = true;
            break;
        }
        case YouTubeMessageKind::MembershipGift: {
            const auto displayText = message.text.trimmed();
            const auto details =
                snippet.value(QStringLiteral("membershipGiftingDetails"))
                    .toObject();
            message.giftCount =
                details.value(QStringLiteral("giftMembershipsCount")).toInt();
            message.membershipLevelName =
                details.value(QStringLiteral("giftMembershipsLevelName"))
                    .toString();
            if (!displayText.isEmpty())
            {
                message.eventText = displayText;
            }
            else if (message.giftCount > 0)
            {
                message.eventText =
                    QStringLiteral("%1 gifted %2 membership%3")
                        .arg(eventAuthorName)
                        .arg(message.giftCount)
                        .arg(message.giftCount == 1 ? QString{}
                                                    : QStringLiteral("s"));
            }
            else
            {
                message.eventText =
                    QStringLiteral("%1 gifted memberships")
                        .arg(eventAuthorName);
            }
            message.text.clear();
            break;
        }
        default:
            break;
    }

    if (message.kind == YouTubeMessageKind::Tombstone)
    {
        if (sourceType == QStringLiteral("messageDeletedEvent"))
        {
            message.id = snippet.value(QStringLiteral("messageDeletedDetails"))
                             .toObject()
                             .value(QStringLiteral("deletedMessageId"))
                             .toString();
        }
        else if (sourceType == QStringLiteral("messageRetractedEvent"))
        {
            message.id =
                snippet.value(QStringLiteral("messageRetractedDetails"))
                    .toObject()
                    .value(QStringLiteral("retractedMessageId"))
                    .toString();
        }

        if (message.id.isEmpty())
        {
            message.kind = YouTubeMessageKind::Ignored;
        }
    }

    if (message.kind == YouTubeMessageKind::UserBanned)
    {
        const auto details =
            snippet.value(QStringLiteral("userBannedDetails")).toObject();
        const auto target =
            details.value(QStringLiteral("bannedUserDetails")).toObject();

        YouTubeBanDetails ban;
        ban.targetChannelId =
            target.value(QStringLiteral("channelId")).toString();
        ban.targetDisplayName =
            target.value(QStringLiteral("displayName")).toString();

        const auto banType =
            details.value(QStringLiteral("banType")).toString();
        if (banType == QStringLiteral("temporary"))
        {
            ban.kind = YouTubeBanKind::Temporary;
            ban.duration = parseDuration(
                details.value(QStringLiteral("banDurationSeconds")));
        }
        else if (banType == QStringLiteral("permanent"))
        {
            ban.kind = YouTubeBanKind::Permanent;
        }

        if (ban.targetChannelId.isEmpty() || ban.kind == YouTubeBanKind::None)
        {
            message.kind = YouTubeMessageKind::Ignored;
        }
        else
        {
            message.ban = std::move(ban);
        }
    }

    return message;
}

bool YouTubeMessage::isUserChatMessage() const noexcept
{
    return this->kind == YouTubeMessageKind::Text ||
           this->kind == YouTubeMessageKind::SuperChat ||
           (this->kind == YouTubeMessageKind::MembershipMilestone &&
            !QStringView{this->text}.trimmed().isEmpty());
}

bool YouTubeMessageDeduper::accept(const YouTubeMessage &message)
{
    QString key;
    if (message.replacesExisting)
    {
        if (message.targetMessageID.isEmpty() || message.id.isEmpty())
        {
            return true;
        }
        key.reserve(9 + message.targetMessageID.size() + message.id.size());
        key = QStringLiteral("replace");
        key.append(QChar(u'\0'));
        key.append(message.targetMessageID);
        key.append(QChar(u'\0'));
        key.append(message.id);
    }
    else
    {
        if (message.id.isEmpty())
        {
            return true;
        }
        key = QString::number(static_cast<int>(message.kind));
        key.append(QChar(u'\0'));
        key.append(message.id);
    }

    if (this->seen_.contains(QStringView{key}))
    {
        return false;
    }
    this->order_.emplace_back(std::move(key));
    this->seen_.emplace(this->order_.back());
    while (this->order_.size() > MAX_ENTRIES)
    {
        this->seen_.erase(QStringView{this->order_.front()});
        this->order_.pop_front();
    }
    return true;
}

std::size_t YouTubeMessageDeduper::size() const noexcept
{
    return this->order_.size();
}

YouTubeReplayBuffer::YouTubeReplayBuffer(std::size_t limit)
    : limit_(limit)
{
}

void YouTubeReplayBuffer::append(YouTubeMessage message)
{
    if (this->limit_ == 0 || message.isIgnored())
    {
        return;
    }

    const auto targetID =
        message.replacesExisting && !message.targetMessageID.isEmpty()
            ? QStringView{message.targetMessageID}
            : QStringView{message.id};
    if (!targetID.isEmpty())
    {
        const auto existing = this->byID_.find(targetID);
        if (existing != this->byID_.end())
        {
            if (message.kind == YouTubeMessageKind::Tombstone ||
                message.replacesExisting)
            {
                auto *stored = existing->second.first;
                const auto sequence = stored->sequence;
                this->removeID(*stored);
                *stored = this->store(std::move(message));
                stored->sequence = sequence;
                this->addID(*stored);
            }
            return;
        }
    }

    while (this->messages_.size() >= this->limit_)
    {
        this->evictOldest();
    }
    this->messages_.emplace_back(this->store(std::move(message)));
    this->addID(this->messages_.back());
}

std::vector<YouTubeMessage> YouTubeReplayBuffer::snapshot() const
{
    std::vector<YouTubeMessage> result;
    result.reserve(this->messages_.size());
    for (const auto &stored : this->messages_)
    {
        auto message = stored.message;
        if (stored.author)
        {
            message.author = *stored.author;
        }
        result.emplace_back(std::move(message));
    }
    return result;
}

std::size_t YouTubeReplayBuffer::size() const noexcept
{
    return this->messages_.size();
}

std::size_t YouTubeReplayBuffer::limit() const noexcept
{
    return this->limit_;
}

YouTubeReplayBuffer::StoredMessage YouTubeReplayBuffer::store(
    YouTubeMessage message)
{
    auto author = this->internAuthor(std::move(message.author));
    message.author = {};
    return StoredMessage{
        .message = std::move(message),
        .author = std::move(author),
        .sequence = this->nextSequence_++,
    };
}

std::shared_ptr<const YouTubeAuthor> YouTubeReplayBuffer::internAuthor(
    YouTubeAuthor author)
{
    static const YouTubeAuthor emptyAuthor;
    if (author == emptyAuthor)
    {
        return {};
    }
    if (!author.channelId.isEmpty())
    {
        const auto existing = this->authors_.find(author.channelId);
        if (existing != this->authors_.end())
        {
            if (auto shared = existing->second.lock();
                shared && *shared == author)
            {
                return shared;
            }
        }
    }

    auto shared = std::make_shared<const YouTubeAuthor>(std::move(author));
    if (!shared->channelId.isEmpty())
    {
        this->authors_.insert_or_assign(shared->channelId, shared);
        if (this->authors_.size() > this->messages_.size() + 64)
        {
            this->pruneAuthors();
        }
    }
    return shared;
}

void YouTubeReplayBuffer::addID(StoredMessage &message)
{
    if (message.message.id.isEmpty())
    {
        return;
    }
    const QStringView id{message.message.id};
    auto [it, inserted] =
        this->byID_.try_emplace(id, IDRecord{.first = &message, .count = 1});
    if (inserted)
    {
        return;
    }
    ++it->second.count;
    if (message.sequence < it->second.first->sequence)
    {
        it->second.first = &message;
    }
}

void YouTubeReplayBuffer::removeID(StoredMessage &message)
{
    if (message.message.id.isEmpty())
    {
        return;
    }
    const QString id = message.message.id;
    const auto found = this->byID_.find(QStringView{id});
    if (found == this->byID_.end())
    {
        return;
    }
    if (found->second.count == 1)
    {
        this->byID_.erase(found);
        return;
    }
    if (found->second.first != &message)
    {
        --found->second.count;
        return;
    }

    const auto remaining = found->second.count - 1;
    this->byID_.erase(found);
    StoredMessage *first = nullptr;
    for (auto &candidate : this->messages_)
    {
        if (&candidate == &message || candidate.message.id != id)
        {
            continue;
        }
        if (first == nullptr || candidate.sequence < first->sequence)
        {
            first = &candidate;
        }
    }
    if (first != nullptr)
    {
        this->byID_.emplace(QStringView{first->message.id},
                            IDRecord{.first = first, .count = remaining});
    }
}

void YouTubeReplayBuffer::evictOldest()
{
    if (this->messages_.empty())
    {
        return;
    }
    this->removeID(this->messages_.front());
    this->messages_.pop_front();
    if (++this->evictionsSinceAuthorPrune_ >= 64)
    {
        this->evictionsSinceAuthorPrune_ = 0;
        this->pruneAuthors();
    }
}

void YouTubeReplayBuffer::pruneAuthors()
{
    std::erase_if(this->authors_, [](const auto &entry) {
        return entry.second.expired();
    });
}

}
