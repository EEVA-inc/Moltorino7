#include "controllers/recording/ChatRecordingMessage.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrc.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeTypes.hpp"
#include "util/FormatTime.hpp"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace chatterino::recording {
namespace {
std::atomic_bool recordingEnabled{false};
thread_local LiveMessageScope *liveScope = nullptr;

QString appendAsset(QJsonArray &assets, QSet<QString> &ids,
                    const QJsonObject &asset)
{
    const auto id = asset.value("id").toString();
    if (!ids.contains(id))
    {
        ids.insert(id);
        assets.append(asset);
    }
    return id;
}

QJsonObject asset(const Emote &emote, const QJsonObject &source,
                  const QString &type)
{
    auto image = emote.images.getImage2();
    if (image->isEmpty())
    {
        image = emote.images.getImage1();
    }
    if (image->isEmpty())
    {
        image = emote.images.getImage3();
    }
    if (image->isEmpty())
    {
        image = emote.images.getImage4();
    }
    QJsonObject result{
        {"type", type},
        {"name", emote.name.string},
        {"providerId", emote.id.string},
        {"url", image->url().string},
        {"imageScale", std::max(1, static_cast<int>(std::lround(
                                       1.0 / std::max(0.01, image->scale()))))},
        {"zeroWidth", emote.zeroWidth},
        {"modifierFlags", static_cast<double>(emote.modifierFlags)},
        {"source", source.value("key")}};

    result.insert("logicalHeight", std::max(1, image->height()));
    result.insert(
        "id", "m_" + stableKey({{"source", source.value("key")},
                                {"type", type},
                                {"name", emote.name.string},
                                {"providerId", emote.id.string},
                                {"url", image->url().string},
                                {"zeroWidth", emote.zeroWidth},
                                {"modifierFlags",
                                 static_cast<double>(emote.modifierFlags)}}));
    return result;
}
}

QString stableKey(const QJsonObject &object)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(object).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}

MessagePtrMut makeSavedMessage(const QString &path, qint64 messages,
                               double durationSeconds, qint64 missingImages)
{
    MessageBuilder builder;
    builder->flags.set(MessageFlag::System, MessageFlag::DoNotLog,
                       MessageFlag::DoNotTriggerNotification);
    builder->serverReceivedTime = QDateTime::currentDateTimeUtc();
    builder.emplace<TimestampElement>();
    const auto add = [&](const QString &text, MessageColor color,
                         FontStyle style = FontStyle::ChatMedium) {
        if (!builder->messageText.isEmpty())
        {
            builder->messageText += ' ';
        }
        builder->messageText += text;
        return builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                            color, style);
    };
    add("Recording saved", MessageColor::Text, FontStyle::ChatMediumBold);
    add(QString::fromUtf8("·"), MessageColor::System);
    add(QString(messages == 1 ? "%1 message" : "%1 messages")
            .arg(QLocale().toString(messages)),
        MessageColor::System);
    if (durationSeconds >= 0)
    {
        add(QString::fromUtf8("·"), MessageColor::System);
        const auto seconds = static_cast<qint64>(std::round(durationSeconds));
        add(seconds == 0 ? QString("0s")
                         : formatTime(std::chrono::seconds{seconds}),
            MessageColor::System);
    }
    add(QString::fromUtf8("·"), MessageColor::System);
    add("Open folder", MessageColor::Link)
        ->setLink(
            {Link::Url, QUrl::fromLocalFile(QFileInfo(path).absolutePath())
                            .toString(QUrl::FullyEncoded)})
        ->setTooltip(QFileInfo(path).fileName());
    if (missingImages > 0)
    {
        add(QString::fromUtf8("·"), MessageColor::System);
        add(QString(missingImages == 1 ? "%1 image unavailable"
                                       : "%1 images unavailable")
                .arg(QLocale().toString(missingImages)),
            MessageColor::System);
    }
    builder->searchText = builder->messageText;
    return builder.release();
}

bool isSupportedSource(const Channel &channel)
{
    return dynamic_cast<const TwitchChannel *>(&channel) ||
           dynamic_cast<const KickChannel *>(&channel) ||
           dynamic_cast<const YouTubeChannel *>(&channel);
}

QJsonObject describeSource(const Channel &channel)
{
    QString platform;
    QString id;
    if (const auto *twitch = dynamic_cast<const TwitchChannel *>(&channel))
    {
        platform = "twitch";
        id = twitch->roomId();
    }
    else if (const auto *kick = dynamic_cast<const KickChannel *>(&channel))
    {
        platform = "kick";
        id = QString::number(kick->userID());
    }
    else if (const auto *youtube =
                 dynamic_cast<const YouTubeChannel *>(&channel))
    {
        platform = "youtube";
        id = youtube->channelID();
    }
    else
    {
        return {};
    }
    if (id.isEmpty() || id == "0")
    {
        id = channel.getName();
    }
    return {{"platform", platform},
            {"id", id},
            {"key", platform + ":" + id},
            {"login", channel.getName()},
            {"displayName", channel.getDisplayName()},
            {"broadcastId", channel.getCurrentStreamID()}};
}

QJsonObject normalizeMessage(const Channel &channel, const Message &message,
                             QString originalText, QJsonObject metadata)
{
    if (message.flags.hasAny({MessageFlag::RecentMessage, MessageFlag::Whisper,
                              MessageFlag::AutoMod,
                              MessageFlag::AutoModOffendingMessage,
                              MessageFlag::RestrictedMessage}))
    {
        return {};
    }
    const auto source = describeSource(channel);
    if (source.isEmpty())
    {
        return {};
    }
    if (originalText.isNull())
    {
        originalText = message.messageText;
    }
    QJsonArray assets;
    QSet<QString> assetIds;
    QHash<const Emote *, QString> emoteIds;
    QJsonArray spans;
    QJsonArray badges;
    qsizetype cursor = 0;
    auto addEmote = [&](const EmotePtr &emote) {
        if (!emote || emote->name.string.isEmpty())
        {
            return;
        }
        const auto &name = emote->name.string;
        auto position = originalText.indexOf(name, cursor);
        if (source.value("platform") == "twitch")
        {
            while (position >= 0 &&
                   ((position > 0 && !originalText[position - 1].isSpace()) ||
                    (position + name.size() < originalText.size() &&
                     !originalText[position + name.size()].isSpace())))
            {
                position = originalText.indexOf(name, position + 1);
            }
        }
        if (position < 0)
        {
            return;
        }
        auto found = emoteIds.constFind(emote.get());
        QString id;
        if (found == emoteIds.cend())
        {
            id = appendAsset(assets, assetIds, asset(*emote, source, "emote"));
            emoteIds.insert(emote.get(), id);
        }
        else
        {
            id = found.value();
        }
        spans.append(QJsonObject{{"start", static_cast<int>(position)},
                                 {"length", name.size()},
                                 {"assetId", id}});
        cursor = position + name.size();
    };
    for (const auto &element : message.elements)
    {
        if (element->getFlags().hasAny(MessageElementFlag::RepliedMessage,
                                       MessageElementFlag::BadgeSharedChannel))
        {
            continue;
        }
        if (const auto *badge =
                dynamic_cast<const BadgeElement *>(element.get()))
        {
            if (const auto emote = badge->getEmote())
            {
                auto item = asset(*emote, source, "badge");
                item.insert("badgeSet", badge->twitchBadgeSetID().value_or(""));
                item.insert("badgeVersion",
                            badge->twitchBadgeVersion().value_or(""));
                badges.append(appendAsset(assets, assetIds, item));
            }
        }
        else if (const auto *emote =
                     dynamic_cast<const EmoteElement *>(element.get()))
        {
            addEmote(emote->getEmote());
        }
        else if (const auto *layers =
                     dynamic_cast<const LayeredEmoteElement *>(element.get()))
        {
            for (const auto &layer : layers->getEmotes())
            {
                addEmote(layer.ptr);
            }
            for (const auto &modifier : layers->getModifiers())
            {
                addEmote(modifier);
            }
        }
    }
    QJsonArray nativeBadges;
    for (const auto &badge : message.twitchBadges)
    {
        nativeBadges.append(
            QJsonObject{{"_id", badge.key_}, {"version", badge.value_}});
    }
    metadata.insert("action", message.flags.has(MessageFlag::Action));
    metadata.insert("bits", static_cast<double>(message.bits));
    if (!message.sharedChatSourceId.isEmpty())
    {
        metadata.insert("sharedChatSourceId", message.sharedChatSourceId);
    }
    return {{"recordType",
             message.flags.has(MessageFlag::System) ? "event" : "message"},
            {"id", message.id},
            {"source", source},
            {"createdAt",
             message.serverReceivedTime.toUTC().toString(Qt::ISODateWithMs)},
            {"text", originalText},
            {"author", QJsonObject{{"id", message.userID},
                                   {"login", message.loginName},
                                   {"displayName", message.displayName},
                                   {"color", message.usernameColor.isValid()
                                                 ? message.usernameColor.name()
                                                 : QString{}}}},
            {"metadata", metadata},
            {"spans", spans},
            {"assets", assets},
            {"badges", badges},
            {"nativeBadges", nativeBadges}};
}

QJsonObject normalizeTwitch(const Channel &channel, const Message &message,
                            const QString &originalText,
                            const QVariantMap &tags)
{
    if (tags.contains("historical"))
    {
        return {};
    }
    QJsonObject metadata;
    for (const auto &key : {"emotes", "gifs", "msg-id", "reply-parent-msg-id",
                            "reply-thread-parent-msg-id", "source-room-id",
                            "source-id", "room-id", "badge-info"})
    {
        if (tags.contains(key))
        {
            metadata.insert(key, tags.value(key).toString());
        }
    }
    auto record = normalizeMessage(channel, message, originalText, metadata);
    if (record.isEmpty())
    {
        return record;
    }

    if (tags.contains("id"))
    {
        record.insert("id", tags.value("id").toString());
    }
    bool timestampOk = false;
    const auto timestamp = tags.value("tmi-sent-ts").toLongLong(&timestampOk);
    if (timestampOk)
    {
        record.insert("createdAt",
                      QDateTime::fromMSecsSinceEpoch(timestamp, Qt::UTC)
                          .toString(Qt::ISODateWithMs));
    }
    auto author = record.value("author").toObject();
    if (tags.contains("user-id"))
    {
        author.insert("id", tags.value("user-id").toString());
    }
    if (tags.contains("login"))
    {
        author.insert("login", tags.value("login").toString());
    }
    if (tags.contains("display-name"))
    {
        author.insert("displayName", tags.value("display-name").toString());
    }
    if (tags.contains("color"))
    {
        author.insert("color", tags.value("color").toString());
    }
    record.insert("author", author);
    auto assets = record.value("assets").toArray();
    QSet<QString> assetIds;
    for (const auto &asset : assets)
    {
        assetIds.insert(asset.toObject().value("id").toString());
    }
    QHash<const Emote *, QString> emoteIds;
    std::vector<QJsonObject> spans;

    for (const auto &emote : parseTwitchEmotes(tags, originalText, 0))
    {
        if (emote.ptr)
        {
            auto found = emoteIds.constFind(emote.ptr.get());
            QString id;
            if (found == emoteIds.cend())
            {
                id = appendAsset(
                    assets, assetIds,
                    asset(*emote.ptr, record.value("source").toObject(),
                          "emote"));
                emoteIds.insert(emote.ptr.get(), id);
            }
            else
            {
                id = found.value();
            }
            spans.push_back({{"start", emote.start},
                             {"length", emote.end - emote.start + 1},
                             {"assetId", id}});
        }
    }
    for (const auto &value : record.value("spans").toArray())
    {
        const auto span = value.toObject();
        bool nativeAsset = false;
        for (const auto &candidate : assets)
        {
            const auto item = candidate.toObject();
            nativeAsset |= item.value("id") == span.value("assetId") &&
                           item.value("url").toString().contains(
                               "static-cdn.jtvnw.net/emoticons/");
        }
        if (nativeAsset)
        {
            continue;
        }
        const int start = span.value("start").toInt();
        const int end = start + span.value("length").toInt();
        const bool overlaps =
            std::ranges::any_of(spans, [&](const auto &native) {
                const int nativeStart = native.value("start").toInt();
                return nativeStart < end &&
                       nativeStart + native.value("length").toInt() > start;
            });
        if (!overlaps)
        {
            spans.push_back(span);
        }
    }
    std::ranges::sort(spans, [](const auto &a, const auto &b) {
        return a.value("start").toInt() < b.value("start").toInt();
    });
    QJsonArray ordered;
    for (const auto &span : spans)
    {
        ordered.append(span);
    }
    record.insert("assets", assets);
    record.insert("spans", ordered);
    return record;
}

QJsonObject normalizeYouTube(const Channel &channel, const Message &message,
                             const YouTubeMessage &source)
{
    if (source.historical || source.localEcho)
    {
        return {};
    }
    QJsonObject details{{"kind", static_cast<int>(source.kind)},
                        {"eventText", source.eventText},
                        {"amount", source.amountDisplayString},
                        {"membershipLevel", source.membershipLevelName},
                        {"memberMonths", source.memberMonths},
                        {"giftCount", source.giftCount},
                        {"replacesExisting", source.replacesExisting},
                        {"targetMessageId", source.targetMessageID},
                        {"targetAuthorId", source.targetAuthorChannelID}};
    auto record = normalizeMessage(channel, message, source.text, details);
    if (record.isEmpty())
    {
        return {};
    }
    record.insert("id", source.id);
    record.insert("createdAt",
                  source.publishedAt.toUTC().toString(Qt::ISODateWithMs));
    auto channelSource = record.value("source").toObject();
    channelSource.insert("broadcastId", source.liveChatId);
    record.insert("source", channelSource);
    auto author = record.value("author").toObject();
    author.insert("id", source.author.channelId);
    author.insert("login", source.author.handle);
    author.insert("displayName", source.author.displayName);
    author.insert("owner", source.author.isOwner);
    author.insert("moderator", source.author.isModerator);
    author.insert("member", source.author.isMember);
    record.insert("author", author);
    auto assets = record.value("assets").toArray();
    QSet<QString> assetIds;
    for (const auto &asset : assets)
    {
        assetIds.insert(asset.toObject().value("id").toString());
    }
    auto spans = record.value("spans").toArray();
    int cursor = 0;
    for (const auto &run : source.runs)
    {
        const auto position = source.text.indexOf(run.text, cursor);
        if (position < 0)
        {
            continue;
        }
        cursor = position + run.text.size();
        if (!run.customEmoji || run.emojiImageUrl.isEmpty() ||
            run.text.isEmpty())
        {
            continue;
        }
        bool covered = false;
        for (const auto &span : spans)
        {
            covered |= span.toObject().value("start").toInt() == position;
        }
        if (covered)
        {
            continue;
        }
        QJsonObject data{{"type", "emote"},
                         {"name", run.text},
                         {"providerId", run.emojiID},
                         {"url", run.emojiImageUrl},
                         {"imageScale", 1},
                         {"logicalHeight", 28},
                         {"source", channelSource.value("key")},
                         {"zeroWidth", false}};
        data.insert("id", "m_" + stableKey(data));
        const auto id = appendAsset(assets, assetIds, data);
        spans.append(QJsonObject{
            {"start", position}, {"length", run.text.size()}, {"assetId", id}});
    }
    std::vector<QJsonObject> orderedSpans;
    for (const auto &span : spans)
    {
        orderedSpans.push_back(span.toObject());
    }
    std::ranges::sort(orderedSpans, [](const auto &a, const auto &b) {
        return a.value("start").toInt() < b.value("start").toInt();
    });
    spans = {};
    for (const auto &span : orderedSpans)
    {
        spans.append(span);
    }
    record.insert("assets", assets);
    record.insert("spans", spans);
    if (source.isUserChatMessage())
    {
        record.insert("recordType", "message");
    }
    if (source.text.isEmpty())
    {
        record.insert("text", source.eventText.isEmpty() ? message.messageText
                                                         : source.eventText);
    }
    return record;
}

LiveMessageScope::LiveMessageScope(const Message *message,
                                   std::function<QJsonObject()> factory)
    : message_(message)
    , factory_(std::move(factory))
    , previous_(liveScope)
{
    liveScope = this;
}

LiveMessageScope::~LiveMessageScope()
{
    liveScope = this->previous_;
}

bool LiveMessageScope::enabled()
{
    return recordingEnabled.load(std::memory_order_relaxed);
}

void LiveMessageScope::setEnabled(bool enabled)
{
    recordingEnabled.store(enabled, std::memory_order_relaxed);
}

const QJsonObject *LiveMessageScope::current(const Message *message)
{
    if (!liveScope || liveScope->message_ != message)
    {
        return nullptr;
    }
    if (liveScope->factory_)
    {
        liveScope->record_ = liveScope->factory_();
        liveScope->factory_ = {};
    }
    return liveScope->record_.isEmpty() ? nullptr : &liveScope->record_;
}

void publicEvent(const Channel &channel, QJsonObject event)
{
    if (LiveMessageScope::enabled())
    {
        if (auto *app = tryGetApp())
        {
            if (auto *recordings = app->getChatRecordings())
            {
                recordings->captureEvent(channel, std::move(event));
            }
        }
    }
}
}
