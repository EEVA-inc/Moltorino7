#include "providers/youtube/YouTubeInnertubeParser.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

constexpr qsizetype MAX_ACTIONS = 5'000;
constexpr qsizetype MAX_REPLAY_DEPTH = 4;
constexpr qsizetype MAX_RUNS = 512;
constexpr qsizetype MAX_BADGES = 32;
constexpr qsizetype MAX_SHORTCUTS = 32;
constexpr qsizetype MAX_CONTINUATIONS = 16;
constexpr qsizetype MAX_MESSAGE_CHARS = 16 * 1024;
constexpr qsizetype MAX_TOTAL_MESSAGE_CHARS = 4 * 1024 * 1024;
constexpr qsizetype MAX_NAME_CHARS = 256;
constexpr qsizetype MAX_ID_CHARS = 512;
constexpr qsizetype MAX_EMOJI_ID_CHARS = 512;
constexpr qsizetype MAX_SHORTCUT_CHARS = 80;
constexpr qsizetype MAX_CONTINUATION_CHARS = 64 * 1024;
constexpr qsizetype MAX_CLICK_TRACKING_CHARS = 16 * 1024;
constexpr qint64 MIN_TIMEOUT_MS = 250;
constexpr qint64 MAX_TIMEOUT_MS = 2 * 60 * 1000;
constexpr double MAX_SAFE_JSON_INTEGER = 9'007'199'254'740'991.0;

bool containsControl(QStringView value)
{
    return std::ranges::any_of(value, [](QChar character) {
        return character.category() == QChar::Other_Control;
    });
}

std::optional<QString> boundedString(const QJsonValue &value, qsizetype maximum,
                                     bool rejectControls = false)
{
    if (!value.isString())
    {
        return std::nullopt;
    }

    auto text = value.toString();
    if (text.size() > maximum || (rejectControls && containsControl(text)))
    {
        return std::nullopt;
    }
    return text;
}

std::optional<QString> requiredID(const QJsonValue &value)
{
    auto id = boundedString(value, MAX_ID_CHARS, true);
    if (!id || id->isEmpty())
    {
        return std::nullopt;
    }
    return id;
}

QString plainText(const QJsonValue &value, qsizetype maximum)
{
    if (!value.isObject())
    {
        return {};
    }

    const auto object = value.toObject();
    if (const auto simple =
            boundedString(object.value(u"simpleText"_s), maximum))
    {
        return *simple;
    }

    const auto runsValue = object.value(u"runs"_s);
    if (!runsValue.isArray())
    {
        return {};
    }

    QString result;
    const auto runs = runsValue.toArray();
    if (runs.size() > MAX_RUNS)
    {
        return {};
    }
    for (const auto &runValue : runs)
    {
        if (!runValue.isObject())
        {
            continue;
        }
        const auto run = runValue.toObject();
        const auto text = boundedString(run.value(u"text"_s), maximum);
        if (!text || result.size() + text->size() > maximum)
        {
            return {};
        }
        result.append(*text);
    }
    return result;
}

QString bestThumbnail(const QJsonValue &value)
{
    if (!value.isObject())
    {
        return {};
    }
    const auto thumbnailsValue = value.toObject().value(u"thumbnails"_s);
    if (!thumbnailsValue.isArray())
    {
        return {};
    }

    QString best;
    qint64 bestArea = -1;
    const auto thumbnails = thumbnailsValue.toArray();
    for (const auto &thumbnailValue : thumbnails)
    {
        if (!thumbnailValue.isObject())
        {
            continue;
        }
        const auto thumbnail = thumbnailValue.toObject();
        const auto url = boundedString(thumbnail.value(u"url"_s), 8'192);
        if (!url || !isTrustedYouTubeImageUrl(*url))
        {
            continue;
        }

        const auto width =
            std::clamp(thumbnail.value(u"width"_s).toInt(), 1, 4'096);
        const auto height =
            std::clamp(thumbnail.value(u"height"_s).toInt(), 1, 4'096);
        const auto area = static_cast<qint64>(width) * height;
        if (area >= bestArea)
        {
            bestArea = area;
            best = *url;
        }
    }
    return best;
}

QDateTime parseTimestamp(const QJsonValue &value)
{
    qulonglong microseconds = 0;
    bool ok = false;
    if (value.isString())
    {
        microseconds = value.toString().toULongLong(&ok);
    }
    else if (value.isDouble())
    {
        const auto raw = value.toDouble();
        if (std::isfinite(raw) && std::floor(raw) == raw && raw >= 0.0 &&
            raw <= MAX_SAFE_JSON_INTEGER)
        {
            microseconds = static_cast<qulonglong>(raw);
            ok = true;
        }
    }

    if (!ok || microseconds / 1'000 >
                   static_cast<qulonglong>(std::numeric_limits<qint64>::max()))
    {
        return {};
    }

    const auto utc = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(microseconds / 1'000), Qt::UTC);
    return utc.isValid() ? utc.toLocalTime() : QDateTime{};
}

QString emojiAccessibilityLabel(const QJsonObject &emoji)
{
    const auto value = emoji.value(u"image"_s)
                           .toObject()
                           .value(u"accessibility"_s)
                           .toObject()
                           .value(u"accessibilityData"_s)
                           .toObject()
                           .value(u"label"_s);
    return boundedString(value, MAX_SHORTCUT_CHARS).value_or(QString{});
}

std::optional<YouTubeMessageRun> parseEmojiRun(const QJsonObject &emoji)
{
    YouTubeMessageRun result;
    result.kind = YouTubeMessageRun::Kind::Emoji;
    result.customEmoji = emoji.value(u"isCustomEmoji"_s).toBool();

    if (const auto id =
            boundedString(emoji.value(u"emojiId"_s), MAX_EMOJI_ID_CHARS, true))
    {
        result.emojiID = *id;
    }

    const auto shortcutsValue = emoji.value(u"shortcuts"_s);
    if (shortcutsValue.isArray())
    {
        const auto shortcuts = shortcutsValue.toArray();
        const auto count = std::min(shortcuts.size(), MAX_SHORTCUTS);
        result.emojiShortcuts.reserve(count);
        for (qsizetype index = 0; index < count; ++index)
        {
            const auto shortcut =
                boundedString(shortcuts.at(index), MAX_SHORTCUT_CHARS, true);
            if (shortcut && !shortcut->isEmpty())
            {
                result.emojiShortcuts.emplace_back(*shortcut);
            }
        }
    }

    result.emojiImageUrl = bestThumbnail(emoji.value(u"image"_s));
    const auto label = emojiAccessibilityLabel(emoji);
    if (result.customEmoji)
    {
        result.text = result.emojiShortcuts.isEmpty()
                          ? label
                          : result.emojiShortcuts.front();
        if (result.text.isEmpty())
        {
            result.text = result.emojiID;
        }
    }
    else
    {
        result.text = result.emojiID;
        if (result.text.isEmpty() && !result.emojiShortcuts.isEmpty())
        {
            result.text = result.emojiShortcuts.front();
        }
        if (result.text.isEmpty())
        {
            result.text = label;
        }
    }

    if (result.text.isEmpty() || result.text.size() > MAX_SHORTCUT_CHARS)
    {
        return std::nullopt;
    }
    return result;
}

bool parseMessageRuns(const QJsonValue &value, YouTubeMessage &message)
{
    if (!value.isObject())
    {
        return false;
    }
    const auto runsValue = value.toObject().value(u"runs"_s);
    if (!runsValue.isArray())
    {
        return false;
    }

    const auto runs = runsValue.toArray();
    if (runs.isEmpty() || runs.size() > MAX_RUNS)
    {
        return false;
    }
    message.runs.reserve(runs.size());

    for (const auto &runValue : runs)
    {
        if (!runValue.isObject())
        {
            continue;
        }
        const auto run = runValue.toObject();
        if (run.contains(u"text"_s))
        {
            const auto text =
                boundedString(run.value(u"text"_s), MAX_MESSAGE_CHARS);
            if (!text || message.text.size() + text->size() > MAX_MESSAGE_CHARS)
            {
                return false;
            }
            if (!text->isEmpty())
            {
                YouTubeMessageRun parsed;
                parsed.kind = YouTubeMessageRun::Kind::Text;
                parsed.text = *text;
                message.text.append(*text);
                message.runs.emplace_back(std::move(parsed));
            }
            continue;
        }

        const auto emojiValue = run.value(u"emoji"_s);
        if (!emojiValue.isObject())
        {
            continue;
        }
        auto parsed = parseEmojiRun(emojiValue.toObject());
        if (!parsed ||
            message.text.size() + parsed->text.size() > MAX_MESSAGE_CHARS)
        {
            return false;
        }
        message.text.append(parsed->text);
        message.runs.emplace_back(std::move(*parsed));
    }

    return !message.runs.empty() && !message.text.trimmed().isEmpty();
}

bool parseOptionalMessageRuns(const QJsonValue &value,
                              YouTubeMessage &message)
{
    if (value.isUndefined() || value.isNull())
    {
        return true;
    }

    if (!value.isObject())
    {
        return false;
    }
    const auto runs = value.toObject().value(u"runs"_s);
    if (runs.isUndefined() || runs.isNull() ||
        (runs.isArray() && runs.toArray().isEmpty()))
    {
        return true;
    }

    YouTubeMessage parsed;
    if (!parseMessageRuns(value, parsed))
    {
        return false;
    }

    message.text = std::move(parsed.text);
    message.runs = std::move(parsed.runs);
    return true;
}

void parseAuthorBadges(const QJsonValue &value, YouTubeAuthor &author)
{
    if (!value.isArray())
    {
        return;
    }
    const auto badges = value.toArray();
    const auto count = std::min(badges.size(), MAX_BADGES);
    for (qsizetype index = 0; index < count; ++index)
    {
        if (!badges.at(index).isObject())
        {
            continue;
        }
        const auto renderer = badges.at(index)
                                  .toObject()
                                  .value(u"liveChatAuthorBadgeRenderer"_s)
                                  .toObject();
        const auto icon = renderer.value(u"icon"_s)
                              .toObject()
                              .value(u"iconType"_s)
                              .toString()
                              .toUpper();
        const auto tooltip =
            boundedString(renderer.value(u"tooltip"_s), MAX_NAME_CHARS)
                .value_or(QString{});
        const auto label =
            boundedString(renderer.value(u"accessibility"_s)
                              .toObject()
                              .value(u"accessibilityData"_s)
                              .toObject()
                              .value(u"label"_s),
                          MAX_NAME_CHARS)
                .value_or(QString{});
        const auto fallback = (tooltip + u' ' + label).toLower();
        const auto membershipBadge =
            bestThumbnail(renderer.value(u"customThumbnail"_s));

        author.isOwner = author.isOwner || icon == u"OWNER"_s ||
                         fallback.contains(u"channel owner"_s);
        author.isModerator = author.isModerator || icon == u"MODERATOR"_s ||
                             fallback.contains(u"moderator"_s);
        author.isVerified = author.isVerified || icon == u"VERIFIED"_s ||
                            fallback.contains(u"verified"_s);
        if (!membershipBadge.isEmpty())
        {
            author.isMember = true;
            author.membershipBadgeUrl = membershipBadge;
            author.membershipBadgeTooltip =
                tooltip.isEmpty() ? label : tooltip;
        }
    }
}

bool parseRendererAuthor(const QJsonObject &renderer, YouTubeAuthor &author)
{
    const auto authorID =
        requiredID(renderer.value(u"authorExternalChannelId"_s));
    if (!authorID)
    {
        return false;
    }

    author.channelId = *authorID;
    applyYouTubeAuthorName(
        author, plainText(renderer.value(u"authorName"_s), MAX_NAME_CHARS));
    author.avatarUrl = bestThumbnail(renderer.value(u"authorPhoto"_s));
    parseAuthorBadges(renderer.value(u"authorBadges"_s), author);

    author.roleMetadataKnown = true;
    return true;
}

std::optional<YouTubeMessage> parseTextRenderer(const QJsonObject &renderer,
                                                QStringView liveChatID)
{
    const auto id = requiredID(renderer.value(u"id"_s));
    if (!id)
    {
        return std::nullopt;
    }

    YouTubeMessage message;
    message.id = *id;
    message.liveChatId = liveChatID.toString();
    message.kind = YouTubeMessageKind::Text;
    message.publishedAt = parseTimestamp(renderer.value(u"timestampUsec"_s));
    if (!parseRendererAuthor(renderer, message.author))
    {
        return std::nullopt;
    }

    if (!parseMessageRuns(renderer.value(u"message"_s), message))
    {
        return std::nullopt;
    }
    return message;
}

std::optional<YouTubeMessage> parsePaidRenderer(const QJsonObject &renderer,
                                                QStringView liveChatID)
{
    const auto id = requiredID(renderer.value(u"id"_s));
    if (!id)
    {
        return std::nullopt;
    }

    YouTubeMessage message;
    message.id = *id;
    message.liveChatId = liveChatID.toString();
    message.kind = YouTubeMessageKind::SuperChat;
    message.publishedAt = parseTimestamp(renderer.value(u"timestampUsec"_s));
    if (!parseRendererAuthor(renderer, message.author))
    {
        return std::nullopt;
    }
    message.amountDisplayString =
        plainText(renderer.value(u"purchaseAmountText"_s), 128).trimmed();
    if (message.amountDisplayString.isEmpty())
    {
        return std::nullopt;
    }

    if (!parseOptionalMessageRuns(renderer.value(u"message"_s), message))
    {
        return std::nullopt;
    }
    return message;
}

std::optional<YouTubeMessage> parseMembershipRenderer(
    const QJsonObject &renderer, QStringView liveChatID)
{
    const auto id = requiredID(renderer.value(u"id"_s));
    if (!id)
    {
        return std::nullopt;
    }

    YouTubeMessage message;
    message.id = *id;
    message.liveChatId = liveChatID.toString();
    message.kind = YouTubeMessageKind::MembershipMilestone;
    message.publishedAt = parseTimestamp(renderer.value(u"timestampUsec"_s));
    if (!parseRendererAuthor(renderer, message.author))
    {
        return std::nullopt;
    }
    message.author.isMember = true;
    message.eventText =
        plainText(renderer.value(u"headerPrimaryText"_s), MAX_MESSAGE_CHARS)
            .trimmed();
    if (message.eventText.isEmpty())
    {
        message.eventText =
            plainText(renderer.value(u"headerSubtext"_s), MAX_MESSAGE_CHARS)
                .trimmed();
    }
    if (!parseOptionalMessageRuns(renderer.value(u"message"_s), message))
    {
        return std::nullopt;
    }
    if (message.eventText.isEmpty() && message.text.trimmed().isEmpty())
    {
        return std::nullopt;
    }
    return message;
}

std::optional<YouTubeMessage> parseGiftPurchaseRenderer(
    const QJsonObject &renderer, QStringView liveChatID)
{
    const auto id = requiredID(renderer.value(u"id"_s));
    if (!id)
    {
        return std::nullopt;
    }

    const auto header =
        renderer.value(u"header"_s)
            .toObject()
            .value(u"liveChatSponsorshipsHeaderRenderer"_s)
            .toObject();
    if (header.isEmpty())
    {
        return std::nullopt;
    }

    auto authorRenderer = header;
    if (const auto authorID =
            renderer.value(u"authorExternalChannelId"_s);
        !authorID.isUndefined() && !authorID.isNull())
    {
        authorRenderer.insert(u"authorExternalChannelId"_s, authorID);
    }

    YouTubeMessage message;
    message.id = *id;
    message.liveChatId = liveChatID.toString();
    message.kind = YouTubeMessageKind::MembershipGift;
    message.publishedAt = parseTimestamp(renderer.value(u"timestampUsec"_s));
    if (!parseRendererAuthor(authorRenderer, message.author))
    {
        return std::nullopt;
    }
    message.eventText =
        plainText(header.value(u"primaryText"_s), MAX_MESSAGE_CHARS).trimmed();
    if (message.eventText.isEmpty())
    {
        return std::nullopt;
    }
    return message;
}

std::optional<YouTubeMessage> parseSupportedRenderer(
    const QJsonObject &item, QStringView liveChatID)
{
    if (const auto renderer = item.value(u"liveChatTextMessageRenderer"_s);
        renderer.isObject())
    {
        return parseTextRenderer(renderer.toObject(), liveChatID);
    }
    if (const auto renderer = item.value(u"liveChatPaidMessageRenderer"_s);
        renderer.isObject())
    {
        return parsePaidRenderer(renderer.toObject(), liveChatID);
    }
    if (const auto renderer = item.value(u"liveChatMembershipItemRenderer"_s);
        renderer.isObject())
    {
        return parseMembershipRenderer(renderer.toObject(), liveChatID);
    }
    if (const auto renderer = item.value(
            u"liveChatSponsorshipsGiftPurchaseAnnouncementRenderer"_s);
        renderer.isObject())
    {
        return parseGiftPurchaseRenderer(renderer.toObject(), liveChatID);
    }
    return std::nullopt;
}

std::optional<YouTubeMessage> parseTargetDeletion(const QJsonObject &action,
                                                  QStringView liveChatID)
{
    const auto target = requiredID(action.value(u"targetItemId"_s));
    if (!target)
    {
        return std::nullopt;
    }

    YouTubeMessage message;
    message.id = *target;
    message.liveChatId = liveChatID.toString();
    message.kind = YouTubeMessageKind::Tombstone;
    message.targetMessageID = *target;
    message.text =
        plainText(action.value(u"deletedStateMessage"_s), MAX_MESSAGE_CHARS);
    return message;
}

std::optional<YouTubeMessage> parseAuthorDeletion(const QJsonObject &action,
                                                  QStringView liveChatID)
{
    const auto target = requiredID(action.value(u"externalChannelId"_s));
    if (!target)
    {
        return std::nullopt;
    }

    YouTubeMessage message;
    message.liveChatId = liveChatID.toString();
    message.kind = YouTubeMessageKind::AuthorMessagesDeleted;
    message.targetAuthorChannelID = *target;
    message.text =
        plainText(action.value(u"deletedStateMessage"_s), MAX_MESSAGE_CHARS);
    return message;
}

struct ActionParser {
    QString liveChatID;
    std::vector<YouTubeMessage> messages;
    qsizetype remaining = MAX_ACTIONS;
    qsizetype totalMessageChars = 0;
    QString fatalError;

    void append(YouTubeMessage message)
    {
        const auto messageChars = message.text.size() +
                                  message.eventText.size() +
                                  message.amountDisplayString.size();
        if (messageChars >
            MAX_TOTAL_MESSAGE_CHARS - this->totalMessageChars)
        {
            this->fatalError = u"YouTube returned too much live chat text."_s;
            return;
        }
        this->totalMessageChars += messageChars;
        this->messages.emplace_back(std::move(message));
    }

    void parse(const QJsonObject &action, qsizetype depth)
    {
        if (!this->fatalError.isEmpty())
        {
            return;
        }
        if (depth > MAX_REPLAY_DEPTH || this->remaining <= 0)
        {
            this->fatalError =
                u"YouTube returned too many nested live chat actions."_s;
            return;
        }
        --this->remaining;

        const auto replayValue = action.value(u"replayChatItemAction"_s);
        if (replayValue.isObject())
        {
            const auto nestedValue = replayValue.toObject().value(u"actions"_s);
            if (!nestedValue.isArray())
            {
                return;
            }
            const auto nested = nestedValue.toArray();
            if (nested.size() > this->remaining)
            {
                this->fatalError =
                    u"YouTube returned too many live chat actions."_s;
                return;
            }
            for (const auto &nestedActionValue : nested)
            {
                if (nestedActionValue.isObject())
                {
                    this->parse(nestedActionValue.toObject(), depth + 1);
                }
            }
            return;
        }

        const auto addValue = action.value(u"addChatItemAction"_s);
        if (addValue.isObject())
        {
            const auto item =
                addValue.toObject().value(u"item"_s).toObject();
            if (auto message =
                    parseSupportedRenderer(item, this->liveChatID))
            {
                this->append(std::move(*message));
            }
            return;
        }

        const auto replaceValue = action.value(u"replaceChatItemAction"_s);
        if (replaceValue.isObject())
        {
            const auto replace = replaceValue.toObject();
            const auto item =
                replace.value(u"replacementItem"_s).toObject();
            if (auto message =
                    parseSupportedRenderer(item, this->liveChatID))
            {
                message->replacesExisting = true;
                const auto target =
                    requiredID(replace.value(u"targetItemId"_s));
                message->targetMessageID = target.value_or(message->id);
                this->append(std::move(*message));
            }
            return;
        }

        static constexpr std::array<QStringView, 2> TARGET_DELETIONS{
            u"markChatItemAsDeletedAction",
            u"removeChatItemAction",
        };
        for (const auto actionType : TARGET_DELETIONS)
        {
            const auto value = action.value(actionType);
            if (value.isObject())
            {
                if (auto message =
                        parseTargetDeletion(value.toObject(), this->liveChatID))
                {
                    this->append(std::move(*message));
                }
                return;
            }
        }

        static constexpr std::array<QStringView, 2> AUTHOR_DELETIONS{
            u"markChatItemsByAuthorAsDeletedAction",
            u"removeChatItemByAuthorAction",
        };
        for (const auto actionType : AUTHOR_DELETIONS)
        {
            const auto value = action.value(actionType);
            if (value.isObject())
            {
                if (auto message =
                        parseAuthorDeletion(value.toObject(), this->liveChatID))
                {
                    this->append(std::move(*message));
                }
                return;
            }
        }

    }
};

std::optional<qint64> parseInteger(const QJsonValue &value)
{
    if (value.isString())
    {
        bool ok = false;
        const auto number = value.toString().toLongLong(&ok);
        if (!ok)
        {
            return std::nullopt;
        }
        return number;
    }
    if (!value.isDouble())
    {
        return std::nullopt;
    }

    const auto raw = value.toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw ||
        raw < -MAX_SAFE_JSON_INTEGER || raw > MAX_SAFE_JSON_INTEGER)
    {
        return std::nullopt;
    }
    return static_cast<qint64>(raw);
}

struct ParsedContinuation {
    QString token;
    QString clickTrackingParams;
    std::optional<std::chrono::milliseconds> timeout;
};

std::optional<ParsedContinuation> parseContinuation(
    const QJsonObject &continuation)
{
    static constexpr std::array<QStringView, 4> TYPES{
        u"invalidationContinuationData",
        u"timedContinuationData",
        u"reloadContinuationData",
        u"liveChatReplayContinuationData",
    };

    for (const auto type : TYPES)
    {
        const auto value = continuation.value(type);
        if (!value.isObject())
        {
            continue;
        }
        const auto data = value.toObject();
        const auto token = boundedString(data.value(u"continuation"_s),
                                         MAX_CONTINUATION_CHARS, true);
        if (!token || token->isEmpty())
        {
            return std::nullopt;
        }

        ParsedContinuation parsed;
        parsed.token = *token;
        if (const auto tracking =
                boundedString(data.value(u"clickTrackingParams"_s),
                              MAX_CLICK_TRACKING_CHARS, true))
        {
            parsed.clickTrackingParams = *tracking;
        }
        if (parsed.clickTrackingParams.isEmpty())
        {
            if (const auto tracking =
                    boundedString(data.value(u"trackingParams"_s),
                                  MAX_CLICK_TRACKING_CHARS, true))
            {
                parsed.clickTrackingParams = *tracking;
            }
        }
        if (const auto timeout = parseInteger(data.value(u"timeoutMs"_s));
            timeout && *timeout >= 0)
        {
            parsed.timeout = std::chrono::milliseconds{
                std::clamp(*timeout, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS)};
        }
        return parsed;
    }
    return std::nullopt;
}

QString responseError(const QJsonObject &response)
{
    const auto errorValue = response.value(u"error"_s);
    if (errorValue.isObject())
    {
        if (const auto message =
                boundedString(errorValue.toObject().value(u"message"_s), 1'024))
        {
            return message->trimmed();
        }
    }
    return {};
}

}

namespace chatterino {

YouTubeInnertubeParseResult parseYouTubeInnertubeChat(
    const QJsonObject &response, QStringView liveChatID)
{
    YouTubeInnertubeParseResult result;
    if (liveChatID.isEmpty() || liveChatID.size() > MAX_ID_CHARS ||
        containsControl(liveChatID))
    {
        result.error = u"The YouTube live chat identity is invalid."_s;
        return result;
    }

    const auto continuationContents = response.value(u"continuationContents"_s);
    if (!continuationContents.isObject())
    {
        result.error = responseError(response);
        if (result.error.isEmpty())
        {
            result.error =
                u"YouTube returned no live chat continuation data."_s;
        }
        return result;
    }
    const auto liveChatValue =
        continuationContents.toObject().value(u"liveChatContinuation"_s);
    if (!liveChatValue.isObject())
    {
        result.error = u"YouTube returned malformed live chat data."_s;
        return result;
    }
    const auto liveChat = liveChatValue.toObject();

    ActionParser parser{liveChatID.toString()};
    const auto actionsValue = liveChat.value(u"actions"_s);
    if (!actionsValue.isUndefined() && !actionsValue.isNull() &&
        !actionsValue.isArray())
    {
        result.error = u"YouTube returned malformed live chat actions."_s;
        return result;
    }
    if (actionsValue.isArray())
    {
        const auto actions = actionsValue.toArray();
        if (actions.size() > MAX_ACTIONS)
        {
            result.error = u"YouTube returned too many live chat actions."_s;
            return result;
        }
        for (const auto &actionValue : actions)
        {
            if (actionValue.isObject())
            {
                parser.parse(actionValue.toObject(), 0);
            }
        }
    }
    if (!parser.fatalError.isEmpty())
    {
        result.error = std::move(parser.fatalError);
        return result;
    }

    const auto continuationsValue = liveChat.value(u"continuations"_s);
    if (continuationsValue.isUndefined() || continuationsValue.isNull())
    {
        result.messages = std::move(parser.messages);
        result.ended = true;
        result.valid = true;
        return result;
    }
    if (!continuationsValue.isArray())
    {
        result.error = u"YouTube returned malformed continuation metadata."_s;
        return result;
    }

    const auto continuations = continuationsValue.toArray();
    if (continuations.size() > MAX_CONTINUATIONS)
    {
        result.error = u"YouTube returned too many continuation tokens."_s;
        return result;
    }
    if (continuations.isEmpty())
    {
        result.messages = std::move(parser.messages);
        result.ended = true;
        result.valid = true;
        return result;
    }

    for (const auto &continuationValue : continuations)
    {
        if (!continuationValue.isObject())
        {
            continue;
        }
        if (auto continuation = parseContinuation(continuationValue.toObject()))
        {
            result.nextContinuation = std::move(continuation->token);
            result.clickTrackingParams =
                std::move(continuation->clickTrackingParams);
            result.serverTimeout = continuation->timeout;
            result.messages = std::move(parser.messages);
            result.valid = true;
            return result;
        }
    }

    result.error = u"YouTube returned an unsupported live chat continuation."_s;
    return result;
}

}
