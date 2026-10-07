// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ChannelView.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/automod/AutoModReviewController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/builtin/twitch/Pin.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/filters/FilterSet.hpp"
#include "controllers/ignores/HiddenUserController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "controllers/recording/ChatRecordingMessage.hpp"
#include "debug/Benchmark.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageThread.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/tiktok/TikTokChannel.hpp"
#include "providers/tiktok/TikTokChatServer.hpp"
#include "providers/tiktok/TikTokMessageBuilder.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/translation/Translator.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeMessageBuilder.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/ThemeCustomization.hpp"
#include "singletons/ThemeVideo.hpp"
#include "singletons/ThemeVideoDecoder.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/DistanceBetweenPoints.hpp"
#include "util/Helpers.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/MultiChannel.hpp"
#include "util/QMagicEnum.hpp"
#include "util/Twitch.hpp"
#include "util/Variant.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/dialogs/ModerationReportDialog.hpp"
#include "widgets/dialogs/ReplyThreadPopup.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ScrollbarHighlight.hpp"
#include "widgets/helper/SearchPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/TooltipWidget.hpp"
#include "widgets/Window.hpp"

#include <magic_enum/magic_enum_flags.hpp>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCursor>
#include <QDate>
#include <QDebug>
#include <QDesktopServices>
#include <QEasingCurve>
#include <QFileInfo>
#include <QGestureEvent>
#include <QGraphicsBlurEffect>
#include <QHash>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QScopeGuard>
#include <QSet>
#include <QStringBuilder>
#include <QTimer>
#include <QTransform>
#include <QUrl>
#include <QUuid>
#include <QVariantAnimation>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <variant>

namespace {

constexpr size_t TOOLTIP_EMOTE_ENTRIES_LIMIT = 7;

using namespace chatterino;
using namespace Qt::StringLiterals;

bool isImageLayoutElement(const MessageLayoutElement *element)
{
    return element != nullptr &&
           (dynamic_cast<const ImageLayoutElement *>(element) != nullptr ||
            dynamic_cast<const LayeredImageLayoutElement *>(element) !=
                nullptr);
}

ImagePtr hoverImageForLayoutElement(const MessageLayoutElement *element)
{
    const auto *imageElement =
        dynamic_cast<const ImageLayoutElement *>(element);
    return imageElement != nullptr ? imageElement->getHoverImage() : ImagePtr{};
}

MessagePtr messageForIdentity(const MessageLayoutPtr &layout)
{
    return layout != nullptr ? layout->getMessagePtr() : MessagePtr{};
}

MessagePtr messageForIdentity(const MessagePtr &message)
{
    return message;
}

template <typename T>
QSet<const Message *> messageIdentityPointers(const std::vector<T> &items)
{
    QSet<const Message *> pointers;
    pointers.reserve(static_cast<qsizetype>(items.size()));
    for (const auto &item : items)
    {
        if (const auto message = messageForIdentity(item))
        {
            pointers.insert(message.get());
        }
    }
    return pointers;
}

template <typename T>
std::optional<std::pair<size_t, size_t>> nearestRetainedMessage(
    const std::vector<T> &frozen, const std::vector<T> &live,
    size_t frozenIndex)
{
    QHash<const Message *, size_t> livePointers;
    QMultiHash<QString, size_t> liveIDs;
    livePointers.reserve(static_cast<qsizetype>(live.size()));
    liveIDs.reserve(static_cast<qsizetype>(live.size()));
    for (size_t index = 0; index < live.size(); ++index)
    {
        const auto message = messageForIdentity(live[index]);
        if (message == nullptr)
        {
            continue;
        }
        livePointers.insert(message.get(), index);
        if (!message->id.isEmpty())
        {
            liveIDs.insert(message->id, index);
        }
    }

    const auto liveIndexFor = [&](size_t index) -> std::optional<size_t> {
        if (index >= frozen.size())
        {
            return std::nullopt;
        }
        const auto message = messageForIdentity(frozen[index]);
        if (message == nullptr)
        {
            return std::nullopt;
        }
        if (const auto found = livePointers.constFind(message.get());
            found != livePointers.cend())
        {
            return found.value();
        }
        if (!message->id.isEmpty())
        {
            std::optional<size_t> matchedIndex;
            const auto [first, last] = liveIDs.equal_range(message->id);
            for (auto found = first; found != last; ++found)
            {
                const auto candidate = messageForIdentity(live[found.value()]);
                if (candidate->channelName == message->channelName &&
                    candidate->platform == message->platform &&
                    (!matchedIndex || found.value() < *matchedIndex))
                {
                    matchedIndex = found.value();
                }
            }
            return matchedIndex;
        }
        return std::nullopt;
    };

    for (size_t index = frozenIndex; index < frozen.size(); ++index)
    {
        if (const auto liveIndex = liveIndexFor(index))
        {
            return std::pair{index, *liveIndex};
        }
    }
    for (size_t index = frozenIndex; index > 0; --index)
    {
        const auto candidate = index - 1;
        if (const auto liveIndex = liveIndexFor(candidate))
        {
            return std::pair{candidate, *liveIndex};
        }
    }
    return std::nullopt;
}

template <typename T>
std::optional<size_t> equivalentMessageIndex(const std::vector<T> &messages,
                                             const MessagePtr &message)
{
    if (!message)
    {
        return std::nullopt;
    }
    for (size_t index = 0; index < messages.size(); ++index)
    {
        const auto candidate = messageForIdentity(messages[index]);
        if (candidate == message ||
            (candidate && !message->id.isEmpty() &&
             candidate->id == message->id &&
             candidate->channelName == message->channelName &&
             candidate->platform == message->platform))
        {
            return index;
        }
    }
    return std::nullopt;
}

struct SavedSelectionIdentity {
    Selection value;
    MessagePtr startMessage;
    MessagePtr endMessage;
};

template <typename T>
SavedSelectionIdentity saveSelectionIdentity(const Selection &selection,
                                             const std::vector<T> &messages)
{
    SavedSelectionIdentity saved{.value = selection};
    if (selection.isEmpty() ||
        selection.start.messageIndex >= messages.size() ||
        selection.end.messageIndex >= messages.size())
    {
        return saved;
    }

    saved.startMessage =
        messageForIdentity(messages[selection.start.messageIndex]);
    saved.endMessage = messageForIdentity(messages[selection.end.messageIndex]);
    return saved;
}

template <typename T>
void restoreSelectionIdentity(const SavedSelectionIdentity &saved,
                              const std::vector<T> &messages,
                              Selection &selection)
{
    if (saved.value.isEmpty())
    {
        selection = {};
        return;
    }

    const auto startIndex =
        equivalentMessageIndex(messages, saved.startMessage);
    const auto endIndex = equivalentMessageIndex(messages, saved.endMessage);
    if (!startIndex || !endIndex)
    {
        selection = {};
        return;
    }

    auto start = saved.value.start;
    auto end = saved.value.end;
    start.messageIndex = *startIndex;
    end.messageIndex = *endIndex;
    selection = {start, end};
}

constexpr int SCROLLBAR_PADDING = 8;
constexpr int MAX_AUTO_TRANSLATIONS_IN_FLIGHT_PER_CHANNEL = 5;
constexpr int MAX_AUTO_TRANSLATIONS_IN_FLIGHT = 16;
constexpr size_t MAX_SYNCHRONOUS_VIRTUALIZED_LAYOUTS = 10'000;

bool hasAlternateBreak(const std::vector<size_t> &breaksFromBottom,
                       size_t distanceFromBottom)
{
    return std::ranges::binary_search(breaksFromBottom, distanceFromBottom);
}

std::vector<bool> restoredAlternateRows(
    size_t count, bool nextAlternateBackground,
    const std::vector<size_t> &breaksFromBottom)
{
    std::vector<bool> rows(count);
    bool laterState = nextAlternateBackground;
    for (size_t distance = 0; distance < count; ++distance)
    {
        if (!hasAlternateBreak(breaksFromBottom, distance))
        {
            laterState = !laterState;
        }
        rows[count - 1 - distance] = laterState;
    }
    return rows;
}

std::vector<size_t> captureAlternateBreaks(
    const std::vector<MessageLayoutPtr> &layouts, bool nextAlternateBackground)
{
    std::vector<size_t> breaks;
    bool laterState = nextAlternateBackground;
    for (size_t distance = 0; distance < layouts.size(); ++distance)
    {
        const auto &layout = layouts[layouts.size() - 1 - distance];
        const bool rowState =
            layout->flags.has(MessageLayoutFlag::AlternateBackground);
        if (rowState == laterState)
        {
            breaks.push_back(distance);
        }
        laterState = rowState;
    }
    return breaks;
}

struct AutoModTimeoutLink {
    QString key;
    int seconds = 0;
};

std::optional<AutoModTimeoutLink> parseAutoModTimeoutLink(QStringView value)
{
    const auto separator = value.lastIndexOf(u'\n');
    if (separator <= 0)
    {
        return std::nullopt;
    }
    bool ok = false;
    const auto seconds = value.sliced(separator + 1).toInt(&ok);
    if (!ok || seconds <= 0)
    {
        return std::nullopt;
    }
    return AutoModTimeoutLink{
        .key = value.first(separator).toString(),
        .seconds = seconds,
    };
}

bool isAutoModReviewActionLink(Link::Type type)
{
    switch (type)
    {
        case Link::AutoModReviewApprove:
        case Link::AutoModReviewDeny:
        case Link::AutoModReviewTimeout:
        case Link::AutoModReviewBan:
        case Link::AutoModReviewRetry:
            return true;
        default:
            return false;
    }
}

float normalizedBadgeScale(float value)
{
    if (!std::isfinite(value))
    {
        return 1.F;
    }
    return std::clamp(value, 0.5F, 2.F);
}

QString wallpaperKey(const ThemeCustomizationProfile &profile)
{
    const QFileInfo info(profile.wallpaperSource);
    const auto stamp =
        info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
    return QStringLiteral("moltorino-wallpaper:%1:%2:%3")
        .arg(profile.wallpaperSource)
        .arg(profile.wallpaperBlur)
        .arg(stamp);
}

std::shared_ptr<QPixmap> loadWallpaper(const ThemeCustomizationProfile &profile)
{
    if (!profile.hasWallpaper() || profile.wallpaperOpacity <= 0)
    {
        return {};
    }
    const auto key = wallpaperKey(profile);

    static QHash<QString, std::weak_ptr<QPixmap>> cache;
    for (auto it = cache.begin(); it != cache.end();)
    {
        it = it.value().expired() ? cache.erase(it) : std::next(it);
    }
    if (auto cached = cache.value(key).lock())
    {
        return cached;
    }

    QImage image = loadThemeWallpaper(profile.wallpaperSource);
    if (image.isNull())
    {
        return {};
    }
    image = blurThemeWallpaper(std::move(image), profile.wallpaperBlur);
    auto cached =
        std::make_shared<QPixmap>(QPixmap::fromImage(std::move(image)));
    cache.insert(key, cached);
    return cached;
}

void openYouTubeExternalUrl(const QString &url)
{
    if (url.isEmpty())
    {
        return;
    }

    if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
    {
        openLinkIncognito(url);
    }
    else
    {
        QDesktopServices::openUrl(QUrl(url));
    }
}

YouTubeAuthor youtubeAuthorForUserLink(const MessagePtr &message,
                                       const Link &link)
{
    YouTubeAuthor author;
    if (!message || message->platform != MessagePlatform::YouTube)
    {
        return author;
    }

    QString linkedChannelID;
    constexpr QStringView ID_PREFIX = u"id:";
    if (link.value.startsWith(ID_PREFIX))
    {
        linkedChannelID = link.value.sliced(ID_PREFIX.size()).trimmed();
    }
    else
    {
        linkedChannelID = message->userID;
    }

    if (linkedChannelID == message->userID)
    {
        if (const auto cached =
                YouTubeMessageBuilder::cachedAuthorForMessage(message->id);
            cached && cached->channelId == linkedChannelID)
        {
            author = *cached;
        }
    }

    if (!linkedChannelID.isEmpty())
    {
        if (const auto cached = YouTubeMessageBuilder::cachedAuthorForChannel(
                message->channelName, linkedChannelID))
        {
            author = *cached;
        }
        author.channelId = linkedChannelID;
    }

    const bool isMessageAuthor =
        !author.channelId.isEmpty() && author.channelId == message->userID;
    if (author.displayName.isEmpty() && isMessageAuthor)
    {
        author.displayName = message->displayName;
    }
    if (author.handle.isEmpty() && isMessageAuthor)
    {
        author.handle = message->loginName;
    }
    if (author.handle.isEmpty() && !link.value.startsWith(ID_PREFIX))
    {
        author.handle = visibleYouTubeName(link.value);
    }
    return author;
}

void populateYouTubeModerationMenu(
    QMenu *menu, const std::shared_ptr<YouTubeChannel> &channel,
    const YouTubeAuthor &targetAuthor)
{
    if (!channel || !channel->canModerateTarget(targetAuthor))
    {
        return;
    }
    const auto &targetUserID = targetAuthor.channelId;
    auto *timeoutMenu = menu->addMenu("&Timeout user");
    const auto addTimeout = [timeoutMenu, channel, targetUserID](
                                const QString &label,
                                std::chrono::seconds duration) {
        timeoutMenu->addAction(label, [channel, targetUserID, duration] {
            channel->moderateUser(targetUserID, duration);
        });
    };
    addTimeout("30 seconds", std::chrono::seconds{30});
    addTimeout("5 minutes", std::chrono::minutes{5});
    addTimeout("10 minutes", std::chrono::minutes{10});
    addTimeout("1 hour", std::chrono::hours{1});
    addTimeout("1 day", std::chrono::hours{24});
    timeoutMenu->setEnabled(!targetUserID.isEmpty());

    auto *banAction = menu->addAction("&Ban user", [channel, targetUserID] {
        channel->moderateUser(targetUserID, std::nullopt);
    });
    banAction->setEnabled(!targetUserID.isEmpty());

    auto *unbanAction = menu->addAction("&Unban user", [channel, targetUserID] {
        channel->unbanUser(targetUserID);
    });
    unbanAction->setEnabled(channel->canUnbanUser(targetUserID));
}

QString messageTextForTranslation(const MessagePtr &message)
{
    if (message == nullptr)
    {
        return {};
    }

    return trimTextForTranslation(message->messageText);
}

void addTranslationFailedMessage(const ChannelPtr &channel)
{
    if (channel != nullptr)
    {
        channel->addSystemMessage(
            "Translation failed. Try switching providers in Settings.");
    }
}

struct TranslationRequestText {
    QString text;
    QHash<QString, EmotePtr> placeholderEmotes;
};

MessagePtr originalMessageForTranslation(const MessagePtr &message)
{
    if (message == nullptr)
    {
        return nullptr;
    }

    return message->translatedFrom != nullptr ? message->translatedFrom
                                              : message;
}

QString translationTooltip(const TranslationResult &translation,
                           const QString &targetLanguage,
                           const QString &targetLanguageName)
{
    const auto detectedLanguage =
        normalizedLanguageCode(translation.detectedLanguage);
    const auto detectedLanguageName =
        translation.detectedLanguage.isEmpty()
            ? QString{}
            : translationLanguageName(translation.detectedLanguage);

    if (detectedLanguageName.isEmpty() || detectedLanguage == targetLanguage)
    {
        return QStringLiteral("Translated to %1").arg(targetLanguageName);
    }

    return QStringLiteral("Translated from %1 to %2")
        .arg(detectedLanguageName, targetLanguageName);
}

bool isTranslatableContentElement(const MessageElement &element,
                                  const Message &message)
{
    const auto flags = element.getFlags();
    if (flags.hasAny({MessageElementFlag::RepliedMessage,
                      MessageElementFlag::RepeatedMessageCounter,
                      MessageElementFlag::ReplyButton,
                      MessageElementFlag::ModeratorTools,
                      MessageElementFlag::Timestamp,
                      MessageElementFlag::Badges,
                      MessageElementFlag::Username,
                      MessageElementFlag::KickUsername,
                      MessageElementFlag::ChannelName}))
    {
        return false;
    }
    if (message.flags.has(MessageFlag::AutoModOffendingMessage) &&
        flags.has(MessageElementFlag::Mention))
    {
        return false;
    }

    return flags.hasAny({MessageElementFlag::Text, MessageElementFlag::Emote,
                         MessageElementFlag::EmojiAll,
                         MessageElementFlag::BitsStatic,
                         MessageElementFlag::BitsAnimated,
                         MessageElementFlag::BitsAmount,
                         MessageElementFlag::Mention});
}

bool shouldPreserveAfterTranslatedContent(const MessageElement &element)
{
    return element.getFlags().has(MessageElementFlag::ReplyButton);
}

QHash<QString, EmotePtr> emotesFromOriginalMessage(const MessagePtr &message)
{
    QHash<QString, EmotePtr> emotes;
    if (message == nullptr)
    {
        return emotes;
    }

    for (const auto &element : message->elements)
    {
        const auto flags = element->getFlags();
        if (flags.has(MessageElementFlag::EmojiImage))
        {
            continue;
        }

        if (const auto *emoteElement =
                dynamic_cast<const EmoteElement *>(element.get()))
        {
            if (const auto emote = emoteElement->getEmote(); emote != nullptr)
            {
                emotes.insert(emote->getCopyString(), emote);
            }
            continue;
        }

        if (const auto *layeredElement =
                dynamic_cast<const LayeredEmoteElement *>(element.get()))
        {
            for (const auto &emoteLayer : layeredElement->getUniqueEmotes())
            {
                if (emoteLayer.ptr != nullptr &&
                    !emoteLayer.flags.has(MessageElementFlag::EmojiImage))
                {
                    emotes.insert(emoteLayer.ptr->getCopyString(),
                                  emoteLayer.ptr);
                }
            }
        }
    }

    return emotes;
}

bool shouldProtectEmoteForTranslation(const QString &name)
{
    return !name.trimmed().isEmpty();
}

TranslationRequestText prepareTranslationRequestText(
    const MessagePtr &message, const QHash<QString, EmotePtr> &originalEmotes)
{
    auto text = messageTextForTranslation(message);
    if (text.isEmpty())
    {
        return {};
    }

    auto words = text.split(' ');
    QHash<QString, EmotePtr> placeholderEmotes;
    int placeholderIndex = 0;

    for (auto &word : words)
    {
        const auto emoteIt = originalEmotes.constFind(word);
        if (emoteIt == originalEmotes.cend() ||
            !shouldProtectEmoteForTranslation(word))
        {
            continue;
        }

        const auto placeholder = QStringLiteral("MOLTOEMOTE%1").arg(
            placeholderIndex++, 4, 10, QLatin1Char('0'));
        placeholderEmotes.insert(placeholder, *emoteIt);
        word = placeholder;
    }

    return {
        .text = words.join(' '),
        .placeholderEmotes = placeholderEmotes,
    };
}

QString expandTranslationPlaceholders(
    QString text, const QHash<QString, EmotePtr> &placeholderEmotes)
{
    auto placeholders = placeholderEmotes.keys();
    std::sort(placeholders.begin(), placeholders.end(),
              [](const QString &a, const QString &b) {
                  return a.size() > b.size();
              });

    for (const auto &placeholder : placeholders)
    {
        if (const auto it = placeholderEmotes.constFind(placeholder);
            it != placeholderEmotes.cend() && *it != nullptr)
        {
            text.replace(placeholder, (*it)->getCopyString());
        }
    }

    return text;
}

void appendTranslatedTextPart(MessageBuilder &builder, QStringView text,
                              TwitchChannel *channel,
                              const QHash<QString, EmotePtr> &placeholderEmotes,
                              const QHash<QString, EmotePtr> &originalEmotes)
{
    const auto textString = text.toString();
    if (const auto it = placeholderEmotes.constFind(textString);
        it != placeholderEmotes.cend())
    {
        builder.appendEmote(*it);
        return;
    }

    if (const auto it = originalEmotes.constFind(textString);
        it != originalEmotes.cend())
    {
        builder.appendEmote(*it);
        return;
    }

    builder.addWordFromUserMessage(text, channel);
}

void appendTranslatedContent(std::vector<std::unique_ptr<MessageElement>> &out,
                             const QString &translatedText,
                             const QString &tooltip, TwitchChannel *channel,
                             const QHash<QString, EmotePtr> &placeholderEmotes,
                             const QHash<QString, EmotePtr> &originalEmotes)
{
    MessageBuilder builder;

    for (const auto &word : translatedText.split(' '))
    {
        if (word.isEmpty())
        {
            continue;
        }

        for (const auto &variant :
             getApp()->getEmotes()->getEmojis()->parse(word))
        {
            std::visit(
                variant::Overloaded{
                    [&](const EmotePtr &emote) {
                        builder.emplace<EmoteElement>(
                            emote, MessageElementFlag::EmojiAll);
                    },
                    [&](QStringView text) {
                        appendTranslatedTextPart(builder, text, channel,
                                                 placeholderEmotes,
                                                 originalEmotes);
                    },
                },
                variant);
        }
    }

    if (getSettings()->showTranslatedMessageIndicator)
    {
        builder
            .emplace<TextElement>(QStringLiteral("(translated)"),
                                  MessageElementFlag::Text,
                                  MessageColor::System)
            ->setTooltip(tooltip);
    }

    auto content = builder.release();
    for (auto &element : content->elements)
    {
        out.push_back(std::move(element));
    }
}

MessagePtrMut makeTranslatedMessage(const MessagePtr &message,
                                    const TranslationResult &translation,
                                    const QString &targetLanguage,
                                    const QString &targetLanguageName,
                                    Channel *channel,
                                    const QHash<QString, EmotePtr>
                                        &placeholderEmotes)
{
    auto sourceMessage = originalMessageForTranslation(message);
    if (sourceMessage == nullptr)
    {
        return nullptr;
    }

    auto translated = sourceMessage->clone();
    const auto translatedText = expandTranslationPlaceholders(
        translation.translatedText, placeholderEmotes);
    translated->translatedFrom = sourceMessage;
    translated->messageText = translatedText;
    translated->searchText = sourceMessage->searchText + QStringLiteral(" ") +
                             translatedText;

    const auto contentStart = std::ranges::find_if(
        sourceMessage->elements, [&](const auto &element) {
            return isTranslatableContentElement(*element, *sourceMessage);
        });
    if (contentStart == sourceMessage->elements.end())
    {
        return nullptr;
    }

    std::vector<std::unique_ptr<MessageElement>> elements;
    elements.reserve(sourceMessage->elements.size() + 2);

    const auto tooltip =
        translationTooltip(translation, targetLanguage, targetLanguageName);
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel);
    const auto originalEmotes = emotesFromOriginalMessage(sourceMessage);

    for (auto it = sourceMessage->elements.begin();
         it != sourceMessage->elements.end(); ++it)
    {
        if (it < contentStart)
        {
            elements.push_back((*it)->clone());
            continue;
        }

        if (it == contentStart)
        {
            appendTranslatedContent(elements, translation.translatedText,
                                    tooltip, twitchChannel, placeholderEmotes,
                                    originalEmotes);
            continue;
        }

        if (shouldPreserveAfterTranslatedContent(**it))
        {
            elements.push_back((*it)->clone());
        }
    }

    translated->elements = std::move(elements);
    return translated;
}

bool isAutoTranslatableChannel(const ChannelPtr &channel)
{
    if (channel == nullptr)
    {
        return false;
    }

    const auto type = channel->getType();
    return type == Channel::Type::Twitch || type == Channel::Type::Kick ||
           type == Channel::Type::YouTube || type == Channel::Type::TikTok;
}

bool isAutoTranslatableMessage(const MessagePtr &message)
{
    if (message == nullptr || message->translatedFrom != nullptr ||
        messageTextForTranslation(message).isEmpty())
    {
        return false;
    }

    QString currentUser;
    switch (message->platform)
    {
        case MessagePlatform::AnyOrTwitch:
            currentUser =
                getApp()->getAccounts()->twitch.getCurrent()->getUserName();
            break;
        case MessagePlatform::Kick: {
            const auto account = getApp()->getAccounts()->kick.current();
            if (!account->isAnonymous())
            {
                currentUser = account->username();
            }
        }
        break;
        case MessagePlatform::YouTube: {
            const auto account = getApp()->getAccounts()->youtube.current();
            if (!account->isAnonymous() && !message->userID.isEmpty() &&
                message->userID == account->channelID())
            {
                return false;
            }
        }
        break;
        case MessagePlatform::TikTok: {
            const auto account = getApp()->getAccounts()->tiktok.current();
            if ((!account->isAnonymous() && !message->userID.isEmpty() &&
                 message->userID == account->userID()) ||
                message->flags.has(MessageFlag::InvalidReplyTarget))
            {
                return false;
            }
        }
        break;
    }

    if (!currentUser.isEmpty() &&
        message->loginName.compare(currentUser, Qt::CaseInsensitive) == 0)
    {
        return false;
    }

    return !message->flags.hasAny({
        MessageFlag::System,
        MessageFlag::Timeout,
        MessageFlag::PubSub,
        MessageFlag::Whisper,
        MessageFlag::Debug,
        MessageFlag::AutoMod,
        MessageFlag::ModerationAction,
        MessageFlag::ConnectedMessage,
        MessageFlag::DisconnectedMessage,
        MessageFlag::ClearChat,
    });
}

QString autoTranslationChannelKey(const ChannelPtr &channel)
{
    return channel == nullptr ? QString{} : channel->getName().toLower();
}

QString autoTranslationRequestKey(const ChannelPtr &channel,
                                  const MessagePtr &message)
{
    const auto messageKey =
        message == nullptr || message->id.isEmpty()
            ? QString::number(reinterpret_cast<quintptr>(message.get()), 16)
            : message->id;

    return autoTranslationChannelKey(channel) % u':' % messageKey;
}

QSet<QString> &autoTranslationInFlightRequests()
{
    static QSet<QString> requests;
    return requests;
}

QHash<QString, int> &autoTranslationInFlightByChannel()
{
    static QHash<QString, int> requests;
    return requests;
}

bool shouldApplyAutomaticTranslation(const MessagePtr &message,
                                     const TranslationResult &translation,
                                     const QString &targetLanguage,
                                     const QHash<QString, EmotePtr>
                                         &placeholderEmotes)
{
    const auto detectedLanguage =
        normalizedLanguageCode(translation.detectedLanguage);
    if (!detectedLanguage.isEmpty() && detectedLanguage == targetLanguage)
    {
        return false;
    }

    const auto translatedText =
        expandTranslationPlaceholders(translation.translatedText,
                                      placeholderEmotes)
            .trimmed();
    return translatedText != messageTextForTranslation(message);
}

void translateMessageForChannel(const ChannelPtr &channel,
                                const MessagePtr &message, QObject *caller,
                                bool showErrors,
                                bool skipSameLanguage,
                                std::function<void()> onFinished = {})
{
    if (channel == nullptr)
    {
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    const auto sourceMessage = originalMessageForTranslation(message);
    const auto originalEmotes = emotesFromOriginalMessage(sourceMessage);
    const auto requestText =
        prepareTranslationRequestText(sourceMessage, originalEmotes);
    if (requestText.text.isEmpty())
    {
        if (showErrors)
        {
            channel->addSystemMessage(
                "There is no message text to translate.");
        }
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    const auto targetLanguage = normalizedTranslationTargetLanguage(
        getSettings()->messageTranslationTargetLanguage.getValue());
    const auto targetLanguageName = translationLanguageName(targetLanguage);

    requestTextTranslation(
        requestText.text, targetLanguage, caller,
        [channel, message, targetLanguage, targetLanguageName, showErrors,
         skipSameLanguage,
         placeholderEmotes = requestText.placeholderEmotes](
            const TranslationResult &translation) {
            if (skipSameLanguage &&
                !shouldApplyAutomaticTranslation(message, translation,
                                                 targetLanguage,
                                                 placeholderEmotes))
            {
                return;
            }

            auto translated = makeTranslatedMessage(
                message, translation, targetLanguage, targetLanguageName,
                channel.get(), placeholderEmotes);
            if (translated == nullptr)
            {
                if (showErrors)
                {
                    addTranslationFailedMessage(channel);
                }
                return;
            }

            channel->replaceMessage(message, translated);
        },
        [channel, showErrors](const QString &) {
            if (showErrors)
            {
                addTranslationFailedMessage(channel);
                return;
            }

            static QHash<QString, qint64> lastErrors;
            const auto key = autoTranslationChannelKey(channel);
            const auto now = QDateTime::currentSecsSinceEpoch();
            const auto previous = lastErrors.constFind(key);
            if (previous != lastErrors.cend() && now - *previous < 60)
            {
                return;
            }
            if (lastErrors.size() >= 256)
            {
                lastErrors.erase(lastErrors.begin());
            }
            lastErrors.insert(key, now);
            addTranslationFailedMessage(channel);
        },
        std::move(onFinished));
}

std::shared_ptr<QColor> nukePreviewScrollbarColor()
{
    static const auto color = std::make_shared<QColor>(255, 70, 70);
    return color;
}

bool hostMatches(const QString &host, QStringView domain)
{
    const auto lowerHost = host.toLower();
    const auto domainString = domain.toString();
    return lowerHost == domainString ||
           lowerHost.endsWith(QStringLiteral(".") + domainString);
}

std::optional<QString> chatVaultEmoteUrl(QStringView provider,
                                         const QString &id)
{
    const auto trimmedID = id.trimmed();
    if (trimmedID.isEmpty())
    {
        return std::nullopt;
    }

    return QStringLiteral("https://chatvau.lt/emote/%1/%2")
        .arg(provider.toString(),
             QString::fromLatin1(QUrl::toPercentEncoding(trimmedID)));
}

std::optional<QString> chatVaultEmoteUrlFromKnownUrl(const QString &urlString)
{
    const QUrl url(urlString);
    if (!url.isValid())
    {
        return std::nullopt;
    }

    const auto host = url.host();
    const auto path = url.path().split('/', Qt::SkipEmptyParts);
    if (path.isEmpty())
    {
        return std::nullopt;
    }

    if (hostMatches(host, u"7tv.app") && path.size() >= 2 &&
        (path[0] == QStringLiteral("emotes") ||
         path[0] == QStringLiteral("emote")))
    {
        return chatVaultEmoteUrl(u"7tv", path[1]);
    }

    if (hostMatches(host, u"betterttv.com") && path.size() >= 2 &&
        path[0] == QStringLiteral("emotes"))
    {
        return chatVaultEmoteUrl(u"bttv", path[1]);
    }

    if (hostMatches(host, u"betterttv.net") && path.size() >= 2 &&
        path[0] == QStringLiteral("emote"))
    {
        return chatVaultEmoteUrl(u"bttv", path[1]);
    }

    if (hostMatches(host, u"frankerfacez.com") && path.size() >= 2 &&
        (path[0] == QStringLiteral("emoticon") ||
         path[0] == QStringLiteral("emote")))
    {
        return chatVaultEmoteUrl(u"ffz", path[1].section('-', 0, 0));
    }

    if (host.compare(QStringLiteral("static-cdn.jtvnw.net"),
                     Qt::CaseInsensitive) == 0 &&
        path.size() >= 3 && path[0] == QStringLiteral("emoticons") &&
        (path[1] == QStringLiteral("v2") || path[1] == QStringLiteral("v1")))
    {
        return chatVaultEmoteUrl(u"twitch", path[2]);
    }

    return std::nullopt;
}

std::optional<QString> chatVaultEmoteUrl(const Emote &emote)
{
    if (auto url = chatVaultEmoteUrlFromKnownUrl(emote.homePage.string))
    {
        return url;
    }

    const auto tryImage = [](const ImagePtr &image) -> std::optional<QString> {
        if (image == nullptr || image->isEmpty())
        {
            return std::nullopt;
        }
        return chatVaultEmoteUrlFromKnownUrl(image->url().string);
    };

    if (auto url = tryImage(emote.images.getImage1()))
    {
        return url;
    }
    if (auto url = tryImage(emote.images.getImage2()))
    {
        return url;
    }
    if (auto url = tryImage(emote.images.getImage3()))
    {
        return url;
    }

    return std::nullopt;
}

std::optional<QString> chatVaultBadgeUrlFromKnownUrl(const QString &urlString)
{
    const QUrl url(urlString);
    const auto host = url.host();
    const auto path = url.path().split('/', Qt::SkipEmptyParts);

    if (host.compare(QStringLiteral("static-cdn.jtvnw.net"),
                     Qt::CaseInsensitive) != 0 ||
        path.size() < 3 || path[0] != QStringLiteral("badges") ||
        path[1] != QStringLiteral("v1") || QUuid(path[2]).isNull())
    {
        return std::nullopt;
    }

    return QStringLiteral("https://chatvau.lt/badge/twitch/%1").arg(path[2]);
}

std::optional<QString> chatVaultBadgeUrl(const BadgeElement &badgeElement,
                                         const TwitchChannel *twitchChannel)
{
    const auto setID = badgeElement.twitchBadgeSetID();
    if (setID && !setID->trimmed().isEmpty())
    {
        const auto trimmedSetID = setID->trimmed();
        const auto version =
            badgeElement.twitchBadgeVersion().value_or(QString()).trimmed();
        QString path = trimmedSetID;
        if (!version.isEmpty())
        {
            path += QLatin1Char('/') + version;
        }

        static const QSet<QString> globalBadges = {
            "lead_moderator", "moderator", "vip", "broadcaster", "founder",
        };
        static const QSet<QString> channelBadges = {
            "subscriber",
            "bits",
        };

        if (twitchChannel != nullptr && !globalBadges.contains(trimmedSetID))
        {
            const bool isChannelBadge =
                channelBadges.contains(trimmedSetID) ||
                twitchChannel->twitchBadge(trimmedSetID, version).has_value();
            const auto roomID = twitchChannel->roomId();
            if (isChannelBadge && !roomID.isEmpty())
            {
                path += QLatin1Char('/') + roomID;
            }
        }

        return QStringLiteral("https://chatvau.lt/badge/twitch/%1").arg(path);
    }

    const auto &badge = *badgeElement.getEmote();
    const auto tryImage = [](const ImagePtr &image) -> std::optional<QString> {
        if (image == nullptr || image->isEmpty())
        {
            return std::nullopt;
        }
        return chatVaultBadgeUrlFromKnownUrl(image->url().string);
    };

    if (auto url = tryImage(badge.images.getImage1()))
    {
        return url;
    }
    if (auto url = tryImage(badge.images.getImage2()))
    {
        return url;
    }
    return tryImage(badge.images.getImage3());
}

ScrollbarHighlight scrollbarHighlightForMessage(
    const MessagePtr &message, const QSet<QString> &nukePreviewMessageIds)
{
    if (message == nullptr)
    {
        return {};
    }

    if (!message->id.isEmpty() && nukePreviewMessageIds.contains(message->id))
    {
        return {nukePreviewScrollbarColor()};
    }

    return message->getScrollBarHighlight();
}

void addEmoteContextMenuItems(
    QMenu *menu, const Emote &emote, QStringView kind,
    std::optional<QString> chatVaultUrl = std::nullopt)
{
    auto *openAction = menu->addAction("&Open");
    auto *openMenu = new QMenu(menu);
    openAction->setMenu(openMenu);

    auto *copyAction = menu->addAction("&Copy");
    auto *copyMenu = new QMenu(menu);
    copyAction->setMenu(copyMenu);

    // Scale of the smallest image
    std::optional<qreal> baseScale;
    QSet<QString> linkedImageUrls;
    const auto isMoltorinoBadge =
        kind == u"badge" &&
        emote.name.string.startsWith(QStringLiteral("moltorino:"));
    // Add copy and open links for images
    auto addImageLink = [&](const ImagePtr &image, const QString &label = {}) {
        if (image == nullptr || image->isEmpty() ||
            image->url().string.isEmpty())
        {
            return;
        }

        const auto urlString = image->url().string;
        if (linkedImageUrls.contains(urlString))
        {
            return;
        }
        linkedImageUrls.insert(urlString);

        auto factor = label;
        if (factor.isEmpty())
        {
            if (!baseScale)
            {
                baseScale = image->scale();
            }

            factor =
                QString::number(static_cast<int>(*baseScale / image->scale()));
        }

        copyMenu->addAction("&" + factor + "x link", [url = image->url()] {
            crossPlatformCopy(url.string);
        });
        openMenu->addAction("&" + factor + "x link", [url = image->url()] {
            QDesktopServices::openUrl(QUrl(url.string));
        });
    };

    addImageLink(emote.images.getImage1(),
                 isMoltorinoBadge ? QStringLiteral("1") : QString{});
    addImageLink(emote.images.getImage2(),
                 isMoltorinoBadge ? QStringLiteral("2") : QString{});
    addImageLink(emote.images.getImage3(),
                 isMoltorinoBadge ? QStringLiteral("3") : QString{});
    addImageLink(emote.images.getImage4(),
                 isMoltorinoBadge ? QStringLiteral("4") : QString{});

    bool openPageSection = false;
    auto ensureOpenPageSection = [&] {
        if (!openPageSection)
        {
            openMenu->addSeparator();
            openPageSection = true;
        }
    };

    // Copy and open emote page link
    if (!emote.homePage.string.isEmpty())
    {
        copyMenu->addSeparator();
        ensureOpenPageSection();

        copyMenu->addAction(u"Copy &" % kind % u" link",
                            [url = emote.homePage] {
                                crossPlatformCopy(url.string);
                            });
        openMenu->addAction(u"Open &" % kind % u" link",
                            [url = emote.homePage] {
                                QDesktopServices::openUrl(QUrl(url.string));
                            });
    }

    if (!chatVaultUrl && kind != u"badge")
    {
        chatVaultUrl = chatVaultEmoteUrl(emote);
    }
    if (chatVaultUrl)
    {
        ensureOpenPageSection();
        openMenu->addAction("Open in &ChatVault", [url = *chatVaultUrl] {
            QDesktopServices::openUrl(QUrl(url));
        });
    }
}

void addImageContextMenuItems(QMenu *menu,
                              const MessageLayoutElement *hoveredElement,
                              const TwitchChannel *twitchChannel)
{
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &creator = hoveredElement->getCreator();
    auto creatorFlags = creator.getFlags();

    // Badge actions
    if (creatorFlags.hasAny({MessageElementFlag::Badges}))
    {
        if (const auto *badgeElement =
                dynamic_cast<const BadgeElement *>(&creator))
        {
            addEmoteContextMenuItems(
                menu, *badgeElement->getEmote(), u"badge",
                chatVaultBadgeUrl(*badgeElement, twitchChannel));
        }
    }

    // Emote actions
    if (creatorFlags.hasAny(
            {MessageElementFlag::EmoteImage, MessageElementFlag::EmojiImage}))
    {
        if (const auto *emoteElement =
                dynamic_cast<const EmoteElement *>(&creator))
        {
            addEmoteContextMenuItems(menu, *emoteElement->getEmote(), u"emote");
        }
        else if (const auto *layeredElement =
                     dynamic_cast<const LayeredEmoteElement *>(&creator))
        {
            // Give each emote its own submenu
            for (auto &emote : layeredElement->getUniqueEmotes())
            {
                auto *emoteAction = menu->addAction(emote.ptr->name.string);
                auto *emoteMenu = new QMenu(menu);
                emoteAction->setMenu(emoteMenu);
                addEmoteContextMenuItems(emoteMenu, *emote.ptr, u"emote");
            }
        }
    }

    // add seperator
    if (!menu->actions().empty())
    {
        menu->addSeparator();
    }
}

void addLinkContextMenuItems(QMenu *menu,
                             const MessageLayoutElement *hoveredElement)
{
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &link = hoveredElement->getLink();

    if (link.type != Link::Url)
    {
        return;
    }

    // Link copy
    QString url = link.value;

    // open link
    menu->addAction("&Open link", [url] {
        QDesktopServices::openUrl(QUrl(url));
    });
    // open link default
    if (supportsIncognitoLinks())
    {
        menu->addAction("Open link &incognito", [url] {
            openLinkIncognito(url);
        });
    }
    menu->addAction("&Copy link", [url] {
        crossPlatformCopy(url);
    });

    menu->addSeparator();
}

void addHiddenContextMenuItems(QMenu *menu,
                               const MessageLayoutElement * /*hoveredElement*/,
                               const MessageLayoutPtr &layout,
                               QMouseEvent *event)
{
    if (!layout)
    {
        return;
    }

    if (event->modifiers() != Qt::ShiftModifier)
    {
        // NOTE: We currently require the modifier to be ONLY shift - we might want to check if shift is among the modifiers instead
        return;
    }

    if (!layout->getMessage()->id.isEmpty())
    {
        menu->addAction("Copy message &ID",
                        [messageID = layout->getMessage()->id] {
                            crossPlatformCopy(messageID);
                        });
    }

    auto message = layout->getMessagePtr();

    if (message)
    {
        menu->addAction("Copy message &JSON", [message] {
            auto jsonString = QJsonDocument{message->toJson()}.toJson(
                QJsonDocument::Indented);
            crossPlatformCopy(QString::fromUtf8(jsonString));
        });
    }
}

// Current function: https://www.desmos.com/calculator/vdyamchjwh
qreal highlightEasingFunction(qreal progress)
{
    if (progress <= 0.1)
    {
        return 1.0 - pow(10.0 * progress, 3.0);
    }
    return 1.0 + pow((20.0 / 9.0) * (0.5 * progress - 0.5), 3.0);
}

float getTooltipScale(EmoteTooltipScale emoteTooltipScale)
{
    switch (emoteTooltipScale)
    {
        case EmoteTooltipScale::Small:
            return 0.5F;
        case EmoteTooltipScale::Medium:
            return 1.0F;
        case EmoteTooltipScale::Large:
            return 1.5F;
        case EmoteTooltipScale::Huge:
            return 2.0F;

        default:
            return 1.0F;
    }
}

}  // namespace

namespace chatterino {

ChannelView::ChannelView(QWidget *parent, Context context, size_t messagesLimit)
    : ChannelView(InternalCtor{}, parent, nullptr, context, messagesLimit)
{
}

ChannelView::ChannelView(QWidget *parent, Split *split, Context context,
                         size_t messagesLimit)
    : ChannelView(InternalCtor{}, parent, split, context, messagesLimit)
{
    assert(parent != nullptr && split != nullptr &&
           "This constructor should only be used with non-null values (see "
           "documentation)");
}

ChannelView::ChannelView(InternalCtor /*tag*/, QWidget *parent, Split *split,
                         Context context, size_t messagesLimit)
    : BaseWidget(parent)
    , channel_(Channel::getEmpty())
    , split_(split)
    , scrollBar_(new Scrollbar(messagesLimit, this))
    , highlightAnimation_(this)
    , context_(context)
    , messagesLimit_(messagesLimit)
    , messages_(messagesLimit)
{
    this->setMouseTracking(true);

    this->initializeLayout();
    this->initializeScrollbar();
    this->initializeSignals();

    this->cursors_.neutral = QCursor(getResources().scrolling.neutralScroll);
    this->cursors_.up = QCursor(getResources().scrolling.upScroll);
    this->cursors_.down = QCursor(getResources().scrolling.downScroll);

    this->pauseTimer_.setSingleShot(true);
    QObject::connect(&this->pauseTimer_, &QTimer::timeout, this, [this] {
        // remove elements that are finite
        std::erase_if(this->pauses_, [](const auto &p) {
            return p.second.has_value();
        });

        this->updatePauses();
    });

    // This shortcut is not used in splits, it's used in views that
    // don't have a SplitInput like the SearchPopup or EmotePopup.
    // See SplitInput::installKeyPressedEvent for the copy event
    // from views with a SplitInput.
    auto *shortcut = new QShortcut(QKeySequence::StandardKey::Copy, this);
    QObject::connect(shortcut, &QShortcut::activated, [this] {
        this->copySelectedText();
    });

    this->clickTimer_.setSingleShot(true);
    this->clickTimer_.setInterval(500);

    this->scrollTimer_.setInterval(20);
    QObject::connect(&this->scrollTimer_, &QTimer::timeout, this, [this] {
        this->scrollUpdateRequested();
    });

    this->grabGesture(Qt::PanGesture);

    // TODO: Figure out if we need this, and if so, why
    // StrongFocus means we can focus this event through clicking it
    // and tabbing to it from another widget. I don't currently know
    // of any place where you can, or where it would make sense,
    // to tab to a ChannelVieChannelView
    this->setFocusPolicy(Qt::FocusPolicy::ClickFocus);

    this->setupHighlightAnimationColors();
    this->highlightAnimation_.setDuration(1500);
    auto curve = QEasingCurve();
    curve.setCustomType(highlightEasingFunction);
    this->highlightAnimation_.setEasingCurve(curve);
    QObject::connect(&this->highlightAnimation_,
                     &QVariantAnimation::valueChanged, this, [this] {
                         this->queueUpdate();
                     });

    this->messageColors_.applyTheme(getTheme(), this->isOverlay_,
                                    getSettings()->overlayBackgroundOpacity);
    this->messagePreferences_.connectSettings(getSettings(),
                                              this->signalHolder_);

    const auto refilterAutoMod = [this](auto, auto) {
        if (this->underlyingChannel_ &&
            this->underlyingChannel_->getType() == Channel::Type::TwitchAutomod)
        {
            this->refilterMessages();
        }
    };
    getSettings()->autoModReviewQueueOrder.connect(refilterAutoMod,
                                                   this->signalHolder_);
    getSettings()->autoModReviewScrollPosition.connect(
        [this](int position, auto) {
            if (!this->underlyingChannel_ ||
                this->underlyingChannel_->getType() !=
                    Channel::Type::TwitchAutomod)
            {
                return;
            }
            const bool followTop =
                position == 1 ||
                (position != 2 && this->newestFirstAutoModOrder_);
            if (followTop == this->followTop_)
            {
                return;
            }
            const bool wasFollowing =
                !this->revealAutoModSelectionOnMaterialize_ &&
                (this->messageLayoutsMaterialized_
                     ? this->isAtFollowedEdge()
                     : (this->followTop_ ? this->virtualizedAtTop_
                                         : this->virtualizedAtBottom_));
            this->followTop_ = followTop;
            if (wasFollowing)
            {
                this->virtualizedAtTop_ = this->followTop_;
                this->virtualizedAtBottom_ = !this->followTop_;
                this->revealAutoModSelectionOnMaterialize_ = false;
                if (this->messageLayoutsMaterialized_)
                {
                    this->scrollToFollowedEdge(false);
                }
            }
            this->queueLayout();
        },
        this->signalHolder_);

    if (auto *review = getApp()->getAutoModReview())
    {
        getSettings()->autoModReviewShowContext.connect(
            [this](bool enabled, auto) {
                if (!this->underlyingChannel_ ||
                    this->underlyingChannel_->getType() !=
                        Channel::Type::TwitchAutomod)
                {
                    return;
                }

                if (enabled && !this->autoModReviewSelectedKey_.isEmpty())
                {
                    if (auto *review = getApp()->getAutoModReview())
                    {
                        review->ensureContext(this->autoModReviewSelectedKey_);
                    }
                }
                this->queueLayout();
                this->queueUpdate();
            },
            this->signalHolder_);

        this->signalHolder_.managedConnect(
            review->itemAdded, [this](const QString &) {
                if (this->autoModReviewSelectedKey_.isEmpty() &&
                    this->isVisible() && this->underlyingChannel_ &&
                    this->underlyingChannel_->getType() ==
                        Channel::Type::TwitchAutomod)
                {
                    this->selectAutoModReview(1, true);
                }
            });
        this->signalHolder_.managedConnect(
            review->itemChanged, [this, review](const QString &key) {
                if (!this->underlyingChannel_ ||
                    this->underlyingChannel_->getType() !=
                        Channel::Type::TwitchAutomod)
                {
                    return;
                }
                if (!this->isVisible())
                {
                    this->autoModReviewDirty_ = true;
                    return;
                }
                const bool selected = this->autoModReviewSelectedKey_ == key;
                const auto *item = review->find(key);
                if (item == nullptr)
                {
                    return;
                }

                const auto &snapshot = this->getMessagesSnapshot();
                const bool currentlyVisible =
                    std::ranges::any_of(snapshot, [&key](const auto &layout) {
                        const auto &message = layout->getMessagePtr();
                        return message && message->autoModReview &&
                               message->autoModReview->key == key;
                    });
                const auto currentMessage =
                    this->underlyingChannel_->findMessageByID(
                        QStringLiteral("automod_") + item->messageID);
                const bool shouldBeVisible =
                    currentMessage &&
                    this->shouldIncludeMessage(currentMessage);

                if (currentlyVisible != shouldBeVisible)
                {
                    this->refilterMessages();
                }
                if (selected &&
                    item->state != automod::ReviewState::Approving &&
                    item->state != automod::ReviewState::Denying &&
                    !item->secondaryInProgress && !automod::isActionable(*item))
                {
                    if (!this->selectAutoModReview(1, true))
                    {
                        this->setAutoModReviewSelectedKey({});
                    }
                }
            });
        this->signalHolder_.managedConnect(
            review->itemRemoved, [this](const QString &key) {
                if (this->autoModReviewSelectedKey_ == key)
                {
                    this->autoModReviewSelectedKey_.clear();
                }
                if (this->underlyingChannel_ &&
                    this->underlyingChannel_->getType() ==
                        Channel::Type::TwitchAutomod)
                {
                    if (!this->isVisible())
                    {
                        this->autoModReviewDirty_ = true;
                        return;
                    }
                    this->refilterMessages();
                    if (this->autoModReviewSelectedKey_.isEmpty())
                    {
                        this->selectAutoModReview(1, false);
                    }
                }
            });
    }
}

ChannelView::~ChannelView()
{
    this->releaseVideoWallpaper();

    delete this->tooltipWidget_;
}

TooltipWidget *ChannelView::ensureTooltipWidget()
{
    if (this->tooltipWidget_ == nullptr)
    {
        this->tooltipWidget_ = new TooltipWidget(this);

        this->tooltipWidget_->setOverrideScale(this->scale());
    }
    return this->tooltipWidget_;
}

void ChannelView::clearPendingLinkInfo()
{
    if (!this->pendingLinkInfo_)
    {
        return;
    }

    QObject::disconnect(this->pendingLinkInfo_.data(), &LinkInfo::stateChanged,
                        this, &ChannelView::pendingLinkInfoStateChanged);
    this->pendingLinkInfo_.clear();
}

void ChannelView::hideTooltip()
{
    this->clearPendingLinkInfo();
    if (this->tooltipWidget_ != nullptr)
    {
        this->tooltipWidget_->hide();
    }
}

void ChannelView::releaseTooltip()
{
    this->clearPendingLinkInfo();
    delete std::exchange(this->tooltipWidget_, nullptr);
}

void ChannelView::initializeLayout()
{
    this->goToBottom_ = new LabelButton("More messages below", this);
    this->goToBottom_->setStyleSheet(
        "background-color: rgba(0,0,0,0.66); color: #FFF;");
    this->goToBottom_->setVisible(false);

    QObject::connect(this->goToBottom_, &Button::leftClicked, this, [this] {
        QTimer::singleShot(180, this, [this] {
            this->scrollToFollowedEdge(
                getSettings()->enableSmoothScrollingNewMessages.getValue());
        });
    });
}

void ChannelView::initializeScrollbar()
{
    // We can safely ignore the scroll bar's signal connection since the scroll bar will
    // always be destroyed before the ChannelView
    std::ignore = this->scrollBar_->getCurrentValueChanged().connect([this] {
        if (this->isVisible())
        {
            this->performLayout(true);
            this->queueUpdate();
        }
        else
        {
            this->layoutQueued_ = true;
        }
    });
}

void ChannelView::initializeSignals()
{
    if (auto *hiddenUsers = getApp()->getHiddenUsers())
    {
        this->signalHolder_.managedConnect(hiddenUsers->changed, [this] {
            this->refilterMessages();
        });
    }

    this->signalHolder_.managedConnect(getApp()->getWindows()->wordFlagsChanged,
                                       [this] {
                                           this->queueLayout();
                                           this->update();
                                       });

    getSettings()->showLastMessageIndicator.connect(
        [this](auto, auto) {
            this->update();
        },
        this->signalHolder_);

    getSettings()->extendedClientNonceParsing.connect(
        [this](auto, auto) {
            this->refreshScrollbarHighlights();
        },
        this->signalHolder_, false);

    getSettings()->continuousSplitBackground.connect(
        [this] {
            this->update();
        },
        this->signalHolder_, false);

    const auto repaintAnimation = [this](const QRegion &region) {
        if (!this->isVisible() || region.isEmpty())
        {
            return;
        }

        if (this->hoverAnimateOnly_ && !this->animatedPicker_)
        {
            if (auto layout = this->hoveredMessageLayout_.lock())
            {
                layout->invalidateBuffer();
            }
            else
            {
                this->animationRegion_ = {};
                this->selfTimedAnimationRegion_ = {};
                return;
            }
        }

        this->update(region);
    };
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this, repaintAnimation] {
            repaintAnimation(this->animationRegion_);
        });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->twitchGifRepaintRequested,
        [this, repaintAnimation] {
            repaintAnimation(this->selfTimedAnimationRegion_);
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->layoutRequested, [&](Channel *channel) {
            if (this->isVisible() &&
                (channel == nullptr ||
                 this->underlyingChannel_.get() == channel))
            {
                this->queueLayout();
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->invalidateBuffersRequested,
        [this](Channel *channel) {
            if (this->isVisible() &&
                (channel == nullptr ||
                 this->underlyingChannel_.get() == channel))
            {
                this->invalidateBuffers();
            }
        });

    this->signalHolder_.managedConnect(getApp()->getFonts()->fontChanged,
                                       [this] {
                                           this->queueLayout();
                                       });
}

void ChannelView::setHoverAnimateOnly(bool value, bool animatedPreviews)
{
    if (this->hoverAnimateOnly_ == value &&
        this->animatedPicker_ == animatedPreviews)
    {
        return;
    }

    if (!value)
    {
        this->releaseHoveredAnimation();
        if (auto layout = this->hoveredMessageLayout_.lock())
        {
            layout->invalidateBuffer();
        }
        if (!this->hoveredElementRect_.isEmpty())
        {
            this->update(this->hoveredElementRect_);
        }
        this->hoveredLayoutElement_ = nullptr;
        this->hoveredMessageLayout_.reset();
        this->hoveredElementRect_ = {};
        this->animationRegion_ = {};
        this->selfTimedAnimationRegion_ = {};
    }
    this->hoverAnimateOnly_ = value;
    this->animatedPicker_ = value && animatedPreviews;
}

void ChannelView::updateHoveredAnimationImage(
    const MessageLayoutElement *layoutElement)
{
    auto next = hoverImageForLayoutElement(layoutElement);
    auto current = this->hoveredAnimationImage_.lock();
    if (current != next)
    {
        if (current != nullptr)
        {
            current->releasePickerFrames();
        }
        this->hoveredAnimationImage_ = next;
    }
}

void ChannelView::releaseHoveredAnimation()
{
    if (auto image = this->hoveredAnimationImage_.lock())
    {
        image->releasePickerFrames();
    }
    this->hoveredAnimationImage_.reset();
}

void ChannelView::clearHoveredAnimation()
{
    if (!this->hoverAnimateOnly_)
    {
        return;
    }
    this->releaseHoveredAnimation();
    if (auto layout = this->hoveredMessageLayout_.lock())
    {
        layout->invalidateBuffer();
    }
    if (!this->hoveredElementRect_.isEmpty())
    {
        this->update(this->hoveredElementRect_);
    }
    this->hoveredLayoutElement_ = nullptr;
    this->hoveredMessageLayout_.reset();
    this->hoveredElementRect_ = {};
    if (!this->animatedPicker_)
    {
        this->animationRegion_ = {};
        this->selfTimedAnimationRegion_ = {};
    }
}

Scrollbar *ChannelView::scrollbar()
{
    return this->scrollBar_;
}

Split *ChannelView::findParentSplit() const
{
    if (auto *split = dynamic_cast<Split *>(this->parentWidget()))
    {
        return split;
    }

    auto *searchPopup = dynamic_cast<SearchPopup *>(this->parentWidget());
    return searchPopup == nullptr
               ? nullptr
               : dynamic_cast<Split *>(searchPopup->parentWidget());
}

bool ChannelView::pausable() const
{
    return this->pausable_;
}

void ChannelView::setPausable(bool value)
{
    this->pausable_ = value;
}

bool ChannelView::paused() const
{
    /// No elements in the map -> not paused
    return this->pausable() && !this->pauses_.empty();
}

void ChannelView::pause(PauseReason reason, std::optional<uint> msecs)
{
    if (!this->pausable())
    {
        return;
    }
    bool wasUnpaused = !this->paused();

    if (wasUnpaused && this->messageLayoutsMaterialized_)
    {
        this->snapshot_ = this->messages_.getSnapshot();
    }

    if (msecs)
    {
        /// Msecs has a value
        auto timePoint = SteadyClock::now() + std::chrono::milliseconds(*msecs);
        auto it = this->pauses_.find(reason);

        if (it == this->pauses_.end())
        {
            /// No value found so we insert a new one.
            this->pauses_[reason] = timePoint;
        }
        else
        {
            /// If the new time point is newer then we override.
            auto &previousTimePoint = it->second;
            if (previousTimePoint.has_value() &&
                previousTimePoint.value() < timePoint)
            {
                previousTimePoint = timePoint;
            }
        }
    }
    else
    {
        /// Msecs is none -> pause is infinite.
        /// We just override the value.
        this->pauses_[reason] = std::nullopt;
    }

    this->updatePauses();

    if (wasUnpaused)
    {
        this->update();
    }
}

void ChannelView::unpause(PauseReason reason)
{
    if (this->pauses_.erase(reason) > 0)
    {
        this->updatePauses();
    }
}

void ChannelView::updatePauses()
{
    using namespace std::chrono;

    if (this->pauses_.empty())
    {
        const bool followEdge = this->isAtFollowedEdge();
        this->unpaused();

        /// No pauses so we can stop the timer
        this->pauseEnd_ = std::nullopt;
        this->pauseTimer_.stop();

        const auto desiredBeforeBoundsUpdate =
            this->scrollBar_->getDesiredValue();
        this->scrollBar_->offsetMaximum(this->pauseScrollMaximumOffset_);
        this->scrollBar_->offsetMinimum(this->pauseScrollMinimumOffset_);
        const auto desiredAfterPendingChanges =
            desiredBeforeBoundsUpdate + this->pauseScrollValueOffset_;

        this->measureScrollbarForScrollRestore();
        if (followEdge)
        {
            this->scrollToFollowedEdge(false);
        }
        else
        {
            this->scrollBar_->setDesiredValue(desiredAfterPendingChanges,
                                              false);
        }
        this->pauseScrollMinimumOffset_ = 0;
        this->pauseScrollMaximumOffset_ = 0;
        this->pauseScrollValueOffset_ = 0;

        this->queueLayout();
        // make sure we re-render
        this->update();
        if (!this->isVisible())
        {
            this->releaseHiddenMessageLayouts();
        }
    }
    else if (std::any_of(this->pauses_.begin(), this->pauses_.end(),
                         [](auto &&value) {
                             return !value.second;
                         }))
    {
        /// Some of the pauses are infinite
        this->pauseEnd_ = std::nullopt;
        this->pauseTimer_.stop();
    }
    else
    {
        /// Get the maximum pause
        auto pauseEnd =
            std::max_element(this->pauses_.begin(), this->pauses_.end(),
                             [](auto &&a, auto &&b) {
                                 return a.second < b.second;
                             })
                ->second.value();

        if (pauseEnd != this->pauseEnd_)
        {
            /// Start the timer
            this->pauseEnd_ = pauseEnd;
            auto duration =
                duration_cast<milliseconds>(pauseEnd - SteadyClock::now());
            this->pauseTimer_.start(std::max(duration, 0ms));
        }
    }
}

void ChannelView::unpaused()
{
    /// Move selection
    const auto liveLayouts =
        this->pauseSelectionNeedsRemap_ || this->pauseLastReadNeedsRemap_
            ? this->messages_.getSnapshot()
            : std::vector<MessageLayoutPtr>{};
    const auto remapSelectionByMessage = [this,
                                          &liveLayouts](Selection &selection) {
        if (selection.isEmpty())
        {
            return;
        }
        const auto saved = saveSelectionIdentity(selection, this->snapshot_);
        restoreSelectionIdentity(saved, liveLayouts, selection);
    };

    const auto applySelectionOffset = [this](Selection &selection) {
        if (this->pauseSelectionIndexOffset_ < 0)
        {
            selection.shiftMessageIndex(
                static_cast<size_t>(-this->pauseSelectionIndexOffset_));
            return;
        }
        const auto offset =
            static_cast<size_t>(this->pauseSelectionIndexOffset_);
        selection.start.messageIndex += offset;
        selection.end.messageIndex += offset;
        selection.selectionMin.messageIndex += offset;
        selection.selectionMax.messageIndex += offset;
        if (selection.selectionMax.messageIndex >= this->messages_.size())
        {
            selection = {};
        }
    };
    if (this->pauseSelectionNeedsRemap_)
    {
        remapSelectionByMessage(this->selection_);
        remapSelectionByMessage(this->doubleClickSelection_);
    }
    else
    {
        applySelectionOffset(this->selection_);
        applySelectionOffset(this->doubleClickSelection_);
    }

    if (this->pauseLastReadNeedsRemap_ && this->lastReadMessage_ != nullptr)
    {
        const auto target = this->pauseLastReadTarget_ != nullptr
                                ? this->pauseLastReadTarget_
                                : this->lastReadMessage_->getMessagePtr();
        const auto index = equivalentMessageIndex(liveLayouts, target);
        this->lastReadMessage_ =
            index ? liveLayouts[*index] : MessageLayoutPtr{};
    }

    this->pauseSelectionIndexOffset_ = 0;
    this->pauseSelectionNeedsRemap_ = false;
    this->pauseLastReadNeedsRemap_ = false;
    this->pauseLastReadTarget_.reset();
}

void ChannelView::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();

    this->setupHighlightAnimationColors();
    this->messageColors_.applyTheme(getTheme(), this->isOverlay_,
                                    getSettings()->overlayBackgroundOpacity);
    this->wallpaperCacheKey_.clear();
    this->wallpaperPixmap_ = {};
    this->releaseVideoWallpaper();
    this->invalidateBuffers();
    this->updateRoundedChildMasks();
}

void ChannelView::updateColorTheme()
{
    this->themeChangedEvent();
}

void ChannelView::setIsOverlay(bool isOverlay)
{
    this->isOverlay_ = isOverlay;
    this->themeChangedEvent();
}

void ChannelView::setTransparentBackground(bool transparent)
{
    this->transparentBackground_ = transparent;
    if (transparent)
    {
        this->messageColors_.regularBg = Qt::transparent;
        this->messageColors_.alternateBg = Qt::transparent;
        this->messageColors_.channelBackground = Qt::transparent;
        this->messageColors_.hasTransparency = true;
    }
    this->update();
}

bool ChannelView::getTransparentBackground() const
{
    return this->transparentBackground_;
}

void ChannelView::setOverrideImageScale(std::optional<float> value)
{
    this->overrideImageScale_ = value;
    this->queueLayout();
}

std::optional<float> ChannelView::getOverrideImageScale() const
{
    return this->overrideImageScale_;
}

void ChannelView::setOverrideEmoteScale(std::optional<float> value)
{
    this->overrideEmoteScale_ = value;
    this->queueLayout();
}

std::optional<float> ChannelView::getOverrideEmoteScale() const
{
    return this->overrideEmoteScale_;
}

void ChannelView::setOverrideBadgeScale(std::optional<float> value)
{
    this->overrideBadgeScale_ = value;
    this->queueLayout();
}

std::optional<float> ChannelView::getOverrideBadgeScale() const
{
    return this->overrideBadgeScale_;
}

float ChannelView::effectiveBadgeScale() const
{
    if (this->overrideBadgeScale_)
    {
        return *this->overrideBadgeScale_;
    }

    return this->scale() *
           normalizedBadgeScale(getSettings()->badgeScale.getValue());
}

void ChannelView::setCenterBadges(bool value)
{
    this->centerBadges_ = value;
    this->queueLayout();
}

void ChannelView::setupHighlightAnimationColors()
{
    this->highlightAnimation_.setStartValue(
        this->theme->messages.highlightAnimationStart);
    this->highlightAnimation_.setEndValue(
        this->theme->messages.highlightAnimationEnd);
}

void ChannelView::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    if (this->tooltipWidget_ != nullptr)
    {
        this->tooltipWidget_->setOverrideScale(scale);
    }

    if (this->goToBottom_)
    {
        auto factor = this->scale();
#ifdef Q_OS_MACOS
        factor = scale * 80.F /
                 std::max<float>(
                     0.01, this->logicalDpiX() * this->devicePixelRatioF());
#endif
        this->goToBottom_->setFont(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, factor));
    }

    this->updateScrollWidgetGeometries();

    this->queueLayout();
}

void ChannelView::updateScrollWidgetGeometries()
{
    if (this->scrollBar_)
    {
        const auto scrollbarWidth = int(16 * this->scale());
        this->scrollBar_->setGeometry(this->width() - scrollbarWidth, 0,
                                      scrollbarWidth, this->height());
    }

    if (this->goToBottom_)
    {
        const auto goToBottomHeight = int(this->scale() * 26);
        this->goToBottom_->setGeometry(0, this->height() - goToBottomHeight,
                                       this->width(), goToBottomHeight);
    }

    this->updateRoundedChildMasks();
}

void ChannelView::queueUpdate()
{
    this->update();
}

void ChannelView::queueUpdate(const QRect &area)
{
    this->update(area);
}

void ChannelView::invalidateBuffers()
{
    this->bufferInvalidationQueued_ = true;
    this->queueLayout();
    this->update();
}

void ChannelView::queueLayout()
{
    if (this->isVisible())
    {
        this->performLayout();
    }
    else
    {
        this->layoutQueued_ = true;
    }
}

void ChannelView::showEvent(QShowEvent * /*event*/)
{
    const bool isAutoMod =
        this->underlyingChannel_ &&
        this->underlyingChannel_->getType() == Channel::Type::TwitchAutomod;
    if (isAutoMod && this->autoModReviewDirty_)
    {
        this->autoModReviewDirty_ = false;
        this->refilterMessages();
    }

    this->materializeMessageLayouts();

    if (this->layoutQueued_)
    {
        this->performLayout(false, true);
    }
    if (!isAutoMod)
    {
        return;
    }

    const auto &snapshot = this->getMessagesSnapshot();
    const bool selectedVisible =
        !this->autoModReviewSelectedKey_.isEmpty() &&
        std::ranges::any_of(snapshot, [this](const auto &layout) {
            const auto &message = layout->getMessagePtr();
            return message && message->autoModReview &&
                   message->autoModReview->key ==
                       this->autoModReviewSelectedKey_;
        });
    if (!selectedVisible)
    {
        this->setAutoModReviewSelectedKey({});
    }
    if (this->autoModReviewSelectedKey_.isEmpty())
    {
        this->selectAutoModReview(1, true);
    }
}

void ChannelView::keyPressEvent(QKeyEvent *event)
{
    if (this->handleAutoModReviewKey(event))
    {
        event->accept();
        return;
    }
    BaseWidget::keyPressEvent(event);
}

void ChannelView::performLayout(bool causedByScrollbar, bool causedByShow)
{
    // BenchmarkGuard benchmark("layout");

    const bool hadAnimatedRegion = !this->animationRegion_.isEmpty() ||
                                   !this->selfTimedAnimationRegion_.isEmpty();
    this->animationRegion_ = {};
    this->selfTimedAnimationRegion_ = {};

    if (this->hoverAnimateOnly_)
    {
        this->hoveredLayoutElement_ = nullptr;
        this->hoveredElementRect_ = {};
    }
    this->layoutQueued_ = false;

    /// Get messages and check if there are at least 1
    const auto &messages = this->getMessagesSnapshot();

    this->followingScrollEdge_ = this->isAtFollowedEdge() ||
                                 (!this->showScrollBar_ && !causedByScrollbar);

    /// Layout visible messages
    this->layoutVisibleMessages(messages);

    /// Update scrollbar
    this->updateScrollbar(messages, causedByScrollbar, causedByShow);

    this->goToBottom_->setText(this->followTop_ ? u"More messages above"_s
                                                : u"More messages below"_s);
    this->goToBottom_->setVisible(this->enableScrollingToBottom_ &&
                                  this->showScrollBar_ &&
                                  !this->isAtFollowedEdge());

    if (hadAnimatedRegion)
    {
        this->queueUpdate();
    }

    if (this->hoverAnimateOnly_ && this->isVisible())
    {
        const auto cursorPos = this->mapFromGlobal(QCursor::pos());
        if (this->rect().contains(cursorPos))
        {
            std::shared_ptr<MessageLayout> hoveredLayout;
            QPointF relativePos;
            int messageIndex = 0;
            if (this->tryGetMessageAt(cursorPos, hoveredLayout, relativePos,
                                      messageIndex))
            {
                auto *element = hoveredLayout->getElementAt(relativePos);
                if (!isImageLayoutElement(element))
                {
                    element = nullptr;
                }

                if (element != nullptr)
                {
                    this->hoveredLayoutElement_ = element;
                    this->updateHoveredAnimationImage(element);
                    this->hoveredMessageLayout_ = hoveredLayout;
                    const auto elementRect = element->getRect().toAlignedRect();
                    const auto messageY =
                        cursorPos.y() - static_cast<int>(relativePos.y());
                    this->hoveredElementRect_ =
                        elementRect.translated(0, messageY);
                    const auto hoverRegion =
                        element->hasAnimatedContent()
                            ? QRegion(this->hoveredElementRect_)
                                  .intersected(this->rect())
                            : QRegion{};
                    if (!this->animatedPicker_)
                    {
                        if (element->usesOwnAnimationTimer())
                        {
                            this->animationRegion_ = {};
                            this->selfTimedAnimationRegion_ = hoverRegion;
                        }
                        else
                        {
                            this->animationRegion_ = hoverRegion;
                            this->selfTimedAnimationRegion_ = {};
                        }
                    }
                    this->update(this->hoveredElementRect_);
                }
                else
                {
                    this->releaseHoveredAnimation();
                    this->hoveredMessageLayout_.reset();
                }
            }
            else
            {
                this->releaseHoveredAnimation();
                this->hoveredMessageLayout_.reset();
            }
        }
        else
        {
            this->releaseHoveredAnimation();
            this->hoveredMessageLayout_.reset();
        }
    }
    if (this->context_ == Context::ReplyThread)
    {
        this->layoutChanged.invoke();
    }
}

int ChannelView::contentHeight(int maximum)
{
    int height = 8;
    const auto width = this->getLayoutWidth();
    const auto [selectedChannel, mcFlags] = this->getMultiChannelInfo();
    const auto flags = this->getFlags() | mcFlags;
    for (const auto &message : this->getMessagesSnapshot())
    {
        if (height >= maximum)
        {
            break;
        }
        message->layoutForMeasurement(this->makeLayoutContext(
            *message->getMessage(), flags, selectedChannel, width));
        height += message->getHeight();
    }
    return std::min(height, maximum);
}

void ChannelView::layoutVisibleMessages(
    const std::vector<MessageLayoutPtr> &messages)
{
    const auto start = size_t(this->scrollBar_->getRelativeCurrentValue());
    const auto layoutWidth = this->getLayoutWidth();
    const auto flags = this->getFlags();
    auto redrawRequired = false;

    if (messages.size() > start)
    {
        auto y = this->verticalOffset_ -
                 (messages[start]->getHeight() *
                  (fmod(this->scrollBar_->getRelativeCurrentValue(), 1)));

        auto [selectedChannel, mcFlags] = this->getMultiChannelInfo();
        auto layoutFlags = flags | mcFlags;

        for (auto i = start; i < messages.size() && y <= this->height(); i++)
        {
            const auto &message = messages[i];

            redrawRequired |= message->layout(
                this->makeLayoutContext(*message->getMessage(), layoutFlags,
                                        selectedChannel, layoutWidth),
                this->bufferInvalidationQueued_);

            y += message->getHeight();

            if (y > this->height())
            {
                break;
            }
        }
        this->bufferInvalidationQueued_ = false;
    }

    if (redrawRequired)
    {
        this->queueUpdate();
    }
}

void ChannelView::updateScrollbar(const std::vector<MessageLayoutPtr> &messages,
                                  bool causedByScrollbar, bool causedByShow)
{
    if (messages.size() == 0)
    {
        this->scrollBar_->setVisible(false);
        this->scrollBar_->setPageSize(0);
        this->scrollBar_->scrollToTop(false);
        this->showScrollBar_ = false;
        this->followingScrollEdge_ = true;
        return;
    }

    /// Layout the messages at the bottom
    qreal h = this->height() - 8;
    auto flags = this->getFlags();
    auto layoutWidth = this->getLayoutWidth();
    auto showScrollbar = false;
    auto [selectedChannel, mcFlags] = this->getMultiChannelInfo();
    flags = flags | mcFlags;

    // convert i to int since it checks >= 0
    for (auto i = int(messages.size()) - 1; i >= 0; i--)
    {
        auto *message = messages[i].get();

        message->layoutForMeasurement(this->makeLayoutContext(
            *message->getMessage(), flags, selectedChannel, layoutWidth));

        h -= message->getHeight();

        if (h < 0)  // break condition
        {
            this->scrollBar_->setPageSize(
                static_cast<qreal>(messages.size() - i) +
                (h / std::max(1, message->getHeight())));

            showScrollbar = true;
            break;
        }
    }

    /// Update scrollbar values
    this->scrollBar_->setVisible(showScrollbar);

    if (!showScrollbar)
    {
        this->scrollBar_->setPageSize(this->scrollBar_->getMaximum() -
                                      this->scrollBar_->getMinimum());
        this->scrollBar_->scrollToTop(false);
        this->followingScrollEdge_ = true;
    }
    this->showScrollBar_ = showScrollbar;

    if (this->enableScrollingToBottom_ && this->followingScrollEdge_ &&
        showScrollbar && !causedByScrollbar)
    {
        this->scrollToFollowedEdge(
            !causedByShow &&
            getSettings()->enableSmoothScrollingNewMessages.getValue());
    }
}

void ChannelView::setVerticalOffset(int offset)
{
    if (this->verticalOffset_ != offset)
    {
        this->verticalOffset_ = offset;
        this->queueLayout();
    }
}

void ChannelView::clearMessages()
{
    this->setAutoModReviewSelectedKey({});
    this->recordingNotices_.clear();
    this->lastClearedMessage_.reset();

    if (this->underlyingChannel_ != nullptr)
    {
        const auto source = this->underlyingChannel_->getMessageSnapshot();
        if (!source.empty())
        {
            this->lastClearedMessage_ = source.back();
        }
    }
    const bool wasMaterialized = this->messageLayoutsMaterialized_;

    this->messageLayoutsMaterialized_ = true;
    this->resetMessageState();
    if (this->channel_ != nullptr)
    {
        this->channel_->clearMessages();
    }
    this->messageLayoutsMaterialized_ = wasMaterialized;

    if (!wasMaterialized)
    {
        this->messages_.releaseStorage();
        this->scrollBar_->releaseHighlightsStorage();
    }
    else if (!this->isVisible())
    {
        this->releaseHiddenMessageLayouts();
    }
}

void ChannelView::measureScrollbarForScrollRestore()
{
    this->updateScrollbar(this->getMessagesSnapshot(), true, false);
}

void ChannelView::resetMessageState()
{
    if (this->hoverAnimateOnly_)
    {
        this->releaseHoveredAnimation();
        this->hoveredLayoutElement_ = nullptr;
        this->hoveredMessageLayout_.reset();
        this->hoveredElementRect_ = {};
        this->animationRegion_ = {};
        this->selfTimedAnimationRegion_ = {};
    }

    this->clearMessageCaches();

    // Clear all stored messages in this chat widget
    this->messages_.clear();
    std::vector<MessageLayoutPtr>().swap(this->snapshot_);
    this->lastReadMessage_.reset();
    this->highlightedMessage_ = nullptr;
    this->virtualizedScrollAnchor_.reset();
    this->virtualizedLastReadMessage_.reset();
    std::vector<MessagePtr>().swap(this->virtualizedExpandedMessages_);
    std::vector<size_t>().swap(this->virtualizedAlternateBreaksFromBottom_);
    this->virtualizedScrollFraction_ = 0;
    this->virtualizedDistanceFromBottom_ = 0;
    this->virtualizedAtBottom_ = !this->followTop_;
    this->virtualizedAtTop_ = this->followTop_;
    this->pauseScrollMinimumOffset_ = 0;
    this->pauseScrollMaximumOffset_ = 0;
    this->pauseScrollValueOffset_ = 0;
    this->pauseSelectionIndexOffset_ = 0;
    this->pauseSelectionNeedsRemap_ = false;
    this->pauseLastReadNeedsRemap_ = false;
    this->pauseLastReadTarget_.reset();
    this->selection_ = {};
    this->doubleClickSelection_ = {};
    this->nukePreviewMessageIds_.clear();
    this->revealAutoModSelectionOnMaterialize_ = false;
    this->scrollBar_->clearHighlights();
    this->scrollBar_->resetBounds();
    this->scrollBar_->setPageSize(0);
    this->scrollBar_->setMaximum(0);
    this->scrollBar_->setMinimum(0);
    this->scrollBar_->setVisible(false);
    this->showScrollBar_ = false;
    this->followingScrollEdge_ = true;
    this->scrollToFollowedEdge(false);
    this->queueLayout();
    this->update();

    this->lastMessageHasAlternateBackground_ = false;
    this->lastMessageHasAlternateBackgroundReverse_ = true;
}

bool ChannelView::isLogicallyAtBottom() const
{
    if (!this->showScrollBar_ || this->scrollBar_->getBottom() <=
                                     this->scrollBar_->getMinimum() + 0.0001)
    {
        return !this->followTop_;
    }
    return this->scrollBar_->isAtBottom();
}

bool ChannelView::isLogicallyAtTop() const
{
    if (!this->showScrollBar_ || this->scrollBar_->getBottom() <=
                                     this->scrollBar_->getMinimum() + 0.0001)
    {
        return this->followTop_;
    }
    return this->scrollBar_->getDesiredValue() <=
           this->scrollBar_->getMinimum() + 0.0001;
}

bool ChannelView::isAtFollowedEdge() const
{
    return this->followTop_ ? this->isLogicallyAtTop()
                            : this->isLogicallyAtBottom();
}

void ChannelView::scrollToFollowedEdge(bool animate)
{
    if (this->followTop_)
    {
        this->scrollBar_->scrollToTop(animate);
    }
    else
    {
        this->scrollBar_->scrollToBottom(animate);
    }
}

bool ChannelView::canReleaseHiddenMessageLayouts() const
{
    return this->split_ != nullptr && this->parentWidget() == this->split_ &&
           this->context_ == Context::None &&
           this->messagesLimit_ <= MAX_SYNCHRONOUS_VIRTUALIZED_LAYOUTS;
}

void ChannelView::materializeMessageLayouts()
{
    if (this->messageLayoutsMaterialized_ || this->channel_ == nullptr)
    {
        return;
    }

    this->messageLayoutsMaterialized_ = true;
    const bool nextAlternateBackground =
        this->lastMessageHasAlternateBackground_;
    this->messages_.clear();
    std::vector<MessageLayoutPtr>().swap(this->snapshot_);
    this->scrollBar_->clearHighlights();

    const auto messages = this->channel_->getMessageSnapshot();
    const auto alternateRows =
        restoredAlternateRows(messages.size(), nextAlternateBackground,
                              this->virtualizedAlternateBreaksFromBottom_);
    const auto expandedMessages =
        messageIdentityPointers(this->virtualizedExpandedMessages_);
    MessageLayoutPtr restoredLastRead;
    size_t restoredAnchorIndex = messages.size();
    std::optional<size_t> selectedAutoModReviewIndex;

    for (size_t index = 0; index < messages.size(); ++index)
    {
        const auto &message = messages[index];
        auto layout = std::make_shared<MessageLayout>(message);
        if (alternateRows[index])
        {
            layout->flags.set(MessageLayoutFlag::AlternateBackground);
        }

        if (this->channel_->shouldIgnoreHighlights())
        {
            layout->flags.set(MessageLayoutFlag::IgnoreHighlights);
        }
        if (expandedMessages.contains(message.get()))
        {
            layout->flags.set(MessageLayoutFlag::Expanded);
        }

        if (message == this->virtualizedLastReadMessage_)
        {
            restoredLastRead = layout;
        }
        if (message == this->virtualizedScrollAnchor_)
        {
            restoredAnchorIndex = index;
        }
        if (this->revealAutoModSelectionOnMaterialize_ &&
            message->autoModReview &&
            message->autoModReview->key == this->autoModReviewSelectedKey_)
        {
            selectedAutoModReviewIndex = index;
        }

        this->messages_.pushBack(layout);
        if (this->showScrollbarHighlights())
        {
            this->scrollBar_->addHighlight(scrollbarHighlightForMessage(
                message, this->nukePreviewMessageIds_));
        }
    }

    if (this->paused())
    {
        this->snapshot_ = this->messages_.getSnapshot();
    }

    this->lastReadMessage_ = std::move(restoredLastRead);
    this->scrollBar_->setMinimum(0);
    this->scrollBar_->setMaximum(static_cast<qreal>(messages.size()));

    this->measureScrollbarForScrollRestore();

    if (selectedAutoModReviewIndex)
    {
        this->scrollBar_->setDesiredValue(
            static_cast<qreal>(*selectedAutoModReviewIndex), false);
    }
    else if (this->virtualizedAtBottom_ || messages.empty())
    {
        this->scrollBar_->scrollToBottom(false);
    }
    else if (this->virtualizedAtTop_)
    {
        this->scrollBar_->scrollToTop(false);
    }
    else
    {
        if (restoredAnchorIndex == messages.size())
        {
            restoredAnchorIndex =
                messages.size() > this->virtualizedDistanceFromBottom_
                    ? messages.size() - this->virtualizedDistanceFromBottom_
                    : 0;
        }
        this->scrollBar_->setDesiredValue(
            static_cast<qreal>(restoredAnchorIndex) +
                this->virtualizedScrollFraction_,
            false);
    }

    this->virtualizedScrollAnchor_.reset();
    this->virtualizedLastReadMessage_.reset();
    std::vector<MessagePtr>().swap(this->virtualizedExpandedMessages_);
    std::vector<size_t>().swap(this->virtualizedAlternateBreaksFromBottom_);
    this->virtualizedScrollFraction_ = 0;
    this->virtualizedDistanceFromBottom_ = 0;
    this->virtualizedAtTop_ = false;
    this->revealAutoModSelectionOnMaterialize_ = false;
    this->layoutQueued_ = true;
}

void ChannelView::releaseHiddenMessageLayouts()
{
    if (!this->messageLayoutsMaterialized_ ||
        !this->canReleaseHiddenMessageLayouts() || this->paused() ||
        this->hasSelection())
    {
        return;
    }

    const auto messages = this->messages_.getSnapshot();
    this->virtualizedAlternateBreaksFromBottom_ = captureAlternateBreaks(
        messages, this->lastMessageHasAlternateBackground_);

    this->virtualizedAtBottom_ = this->isLogicallyAtBottom();
    this->virtualizedAtTop_ = this->isLogicallyAtTop();
    this->virtualizedScrollAnchor_.reset();
    this->virtualizedExpandedMessages_.clear();
    this->virtualizedScrollFraction_ = 0;
    this->virtualizedDistanceFromBottom_ = 0;

    if ((!this->virtualizedAtBottom_ ||
         (this->followTop_ && !this->newestFirstAutoModOrder_)) &&
        !messages.empty())
    {
        const auto relative =
            std::clamp(this->scrollBar_->getDesiredValue() -
                           this->scrollBar_->getMinimum(),
                       qreal(0), static_cast<qreal>(messages.size() - 1));
        const auto index = static_cast<size_t>(std::floor(relative));
        this->virtualizedScrollAnchor_ = messages[index]->getMessagePtr();
        this->virtualizedScrollFraction_ = relative - std::floor(relative);
        this->virtualizedDistanceFromBottom_ = messages.size() - index;
    }

    for (const auto &layout : messages)
    {
        if (layout->flags.has(MessageLayoutFlag::Expanded))
        {
            this->virtualizedExpandedMessages_.push_back(
                layout->getMessagePtr());
        }
    }

    this->virtualizedLastReadMessage_ =
        this->lastReadMessage_ != nullptr
            ? this->lastReadMessage_->getMessagePtr()
            : MessagePtr{};
    this->highlightAnimation_.stop();
    this->highlightedMessage_ = nullptr;
    this->lastReadMessage_.reset();
    this->messagesOnScreen_.clear();
    this->messages_.releaseStorage();
    std::vector<MessageLayoutPtr>().swap(this->snapshot_);
    this->scrollBar_->releaseHighlightsStorage();
    this->messageLayoutsMaterialized_ = false;

    if (messages.size() >= 128)
    {
        requestMemoryPressureRelief();
    }
}

Scrollbar &ChannelView::getScrollBar()
{
    return *this->scrollBar_;
}

QString ChannelView::getSelectedText()
{
    QString result = "";

    std::vector<MessageLayoutPtr> &messagesSnapshot =
        this->getMessagesSnapshot();

    Selection selection = this->selection_;

    if (selection.isEmpty())
    {
        return result;
    }

    const auto numMessages = messagesSnapshot.size();
    const auto indexStart = selection.selectionMin.messageIndex;
    const auto indexEnd = selection.selectionMax.messageIndex;

    if (indexEnd >= numMessages || indexStart >= numMessages)
    {
        // One of our messages is out of bounds
        return result;
    }

    auto flags = this->getFlags();
    auto [selectedChannel, mcFlags] = this->getMultiChannelInfo();
    flags = flags | mcFlags;

    for (auto msg = indexStart; msg <= indexEnd; msg++)
    {
        MessageLayoutPtr layout = messagesSnapshot[msg];
        const bool hadCache = layout->hasCache();
        layout->layout(
            this->makeLayoutContext(*layout->getMessage(), flags,
                                    selectedChannel, this->getLayoutWidth()),
            false);
        auto from = msg == selection.selectionMin.messageIndex
                        ? selection.selectionMin.charIndex
                        : 0;
        auto to = msg == selection.selectionMax.messageIndex
                      ? selection.selectionMax.charIndex
                      : layout->getLastCharacterIndex() + 1;

        layout->addSelectionText(result, from, to);
        if (!hadCache)
        {
            layout->deleteCache();
        }

        if (msg != indexEnd)
        {
            result += '\n';
        }
    }

    return result;
}

bool ChannelView::hasSelection()
{
    return !this->selection_.isEmpty();
}

void ChannelView::clearSelection()
{
    this->selection_ = Selection();
    this->queueLayout();
}

void ChannelView::copySelectedText()
{
    crossPlatformCopy(this->getSelectedText());
}

void ChannelView::setEnableScrollingToBottom(bool value)
{
    this->enableScrollingToBottom_ = value;
}

bool ChannelView::getEnableScrollingToBottom() const
{
    return this->enableScrollingToBottom_;
}

void ChannelView::setOverrideFlags(std::optional<MessageElementFlags> value)
{
    this->overrideFlags_ = value;
    this->queueUpdate();
}

void ChannelView::setCollapseMessages(bool value)
{
    if (this->collapseMessages_ == value)
    {
        return;
    }

    this->collapseMessages_ = value;
    this->invalidateBuffers();
}

void ChannelView::setOverrideSeparateMessages(std::optional<bool> value)
{
    this->overrideSeparateMessages_ = value;
    this->queueUpdate();
}

void ChannelView::setHighlightsEnabled(bool enabled)
{
    if (this->messagePreferences_.showHighlights == enabled)
    {
        return;
    }
    this->messagePreferences_.showHighlights = enabled;
    this->invalidateBuffers();
}

const std::optional<MessageElementFlags> &ChannelView::getOverrideFlags() const
{
    return this->overrideFlags_;
}

std::vector<MessageLayoutPtr> &ChannelView::getMessagesSnapshot()
{
    this->materializeMessageLayouts();

    this->snapshotGuard_.guard();
    if (!this->paused() /*|| this->scrollBar_->isVisible()*/)
    {
        this->snapshot_ = this->messages_.getSnapshot();
    }

    return this->snapshot_;
}

ChannelPtr ChannelView::channel() const
{
    assert(this->channel_ != nullptr);

    return this->channel_;
}

ChannelPtr ChannelView::underlyingChannel() const
{
    return this->underlyingChannel_;
}

ChannelPtr ChannelView::selectedChannel() const
{
    if (auto *mc = dynamic_cast<MultiChannel *>(this->underlyingChannel_.get()))
    {
        const auto *active = mc->activeChannel();
        if (active)
        {
            return active->channel;
        }
    }
    return this->underlyingChannel_;
}

ChannelPtr ChannelView::inferChannel(const Message &msg,
                                     InferChannel mode) const
{
    ChannelPtr base = this->underlyingChannel_;
    switch (mode)
    {
        case InferChannel::UnderlyingOnly:
            break;
        case InferChannel::SourceChannelIfAvailable: {
            if (this->hasSourceChannel())
            {
                base = this->sourceChannel_;
            }
        }
        break;
        case InferChannel::SearchParentIfAvailable: {
            auto *searchPopup =
                dynamic_cast<SearchPopup *>(this->parentWidget());
            if (searchPopup != nullptr)
            {
                auto *split =
                    dynamic_cast<Split *>(searchPopup->parentWidget());
                if (split != nullptr)
                {
                    base = split->getChannel();
                }
            }
        }
        break;
    }

    auto *mc = dynamic_cast<MultiChannel *>(base.get());
    if (!mc)
    {
        return base;
    }

    QStringView nameView = msg.channelName;
    bool kc = nameView.startsWith(u":kick:");
    if (kc)
    {
        nameView = nameView.sliced(sizeof(":kick:") - 1);
    }
    if (nameView.startsWith(u":youtube:"))
    {
        nameView = nameView.sliced(sizeof(":youtube:") - 1);
    }
    if (nameView.startsWith(u":tiktok:"))
    {
        nameView = nameView.sliced(sizeof(":tiktok:") - 1);
    }
    if (nameView.startsWith(u'#'))
    {
        nameView = nameView.sliced(1);
    }

    const auto *active = mc->activeChannel();
    ChannelPtr platformFallback;
    bool platformFallbackIsUnique = true;
    auto matches = [&](const MultiChannel::ChildChannel &chan) {
        if (!platformMatches(msg.platform, chan.platform))
        {
            return false;
        }

        const auto caseSensitivity =
            chan.platform == MultiChannel::Platform::YouTube
                ? Qt::CaseSensitive
                : Qt::CaseInsensitive;
        return nameView.compare(chan.channel->getName(), caseSensitivity) == 0;
    };

    if (active && matches(*active))
    {
        return active->channel;
    }

    for (const auto &chan : mc->channels())
    {
        if (platformMatches(msg.platform, chan.platform))
        {
            if (platformFallback)
            {
                platformFallbackIsUnique = false;
            }
            else
            {
                platformFallback = chan.channel;
            }
        }
        if (matches(chan))
        {
            return chan.channel;
        }
    }

    if (platformFallback && platformFallbackIsUnique)
    {
        return platformFallback;
    }

    return Channel::getEmpty();
}

std::pair<Channel *, MessageElementFlags> ChannelView::getMultiChannelInfo()
    const
{
    Channel *selectedChannel = this->underlyingChannel_.get();
    MessageElementFlags flags{};
    if (auto *mc = dynamic_cast<MultiChannel *>(selectedChannel))
    {
        const auto *active = mc->activeChannel();
        if (active)
        {
            selectedChannel = active->channel.get();
        }
        switch (mc->indicatorMode())
        {
            case MultiChannelIndicatorMode::None:
                break;
            case MultiChannelIndicatorMode::PlatformBadgeIfUnselected:
                flags.set(MessageElementFlag::PlatformBadgeIfUnselected);
                break;
            case MultiChannelIndicatorMode::PlatformBadgeAlways:
                flags.set(MessageElementFlag::PlatformBadgeAlways);
                break;
            case MultiChannelIndicatorMode::ChannelName:
                flags.set(MessageElementFlag::ChannelName);
                break;
            case MultiChannelIndicatorMode::ChannelAvatar:
                flags.set(MessageElementFlag::ChannelAvatar);
                break;
        }
    }
    return {selectedChannel, flags};
}

bool ChannelView::showScrollbarHighlights() const
{
    return this->channel_->getType() != Channel::Type::TwitchMentions;
}

void ChannelView::refreshScrollbarHighlights()
{
    if (!this->messageLayoutsMaterialized_)
    {
        this->scrollBar_->releaseHighlightsStorage();
        this->scrollBar_->update();
        return;
    }

    this->scrollBar_->clearHighlights();
    if (!this->showScrollbarHighlights())
    {
        this->scrollBar_->update();
        return;
    }

    const auto snapshot = this->messages_.getSnapshot();
    for (const auto &layout : snapshot)
    {
        this->scrollBar_->addHighlight(scrollbarHighlightForMessage(
            layout != nullptr ? layout->getMessagePtr() : nullptr,
            this->nukePreviewMessageIds_));
    }

    this->scrollBar_->update();
}

void ChannelView::setChannel(const ChannelPtr &underlyingChannel)
{
    this->lastClearedMessage_.reset();
    this->setChannel(underlyingChannel,
                     underlyingChannel->getMessageSnapshot());
}

void ChannelView::setChannel(const ChannelPtr &underlyingChannel,
                             std::vector<MessagePtr> snapshot)
{
    const bool sourceChanged = this->underlyingChannel_ != underlyingChannel;
    if (sourceChanged)
    {
        /// Clear connections from the last channel
        this->channelConnections_.clear();
    }
    this->proxyConnections_.clear();

    const bool shouldMaterialize =
        !this->canReleaseHiddenMessageLayouts() || this->isVisible() ||
        (this->messageLayoutsMaterialized_ && this->paused());

    this->messageLayoutsMaterialized_ = true;
    this->resetMessageState();
    this->underlyingChannel_ = underlyingChannel;

    /// make copy of channel and expose
    this->channel_ = std::make_unique<Channel>(underlyingChannel->getName(),
                                               underlyingChannel->getType(),
                                               this->messagesLimit_);
    this->messageLayoutsMaterialized_ = shouldMaterialize;
    if (!this->messageLayoutsMaterialized_)
    {
        this->messages_.releaseStorage();
        this->scrollBar_->releaseHighlightsStorage();
    }
    const bool isAutoModChannel =
        underlyingChannel->getType() == Channel::Type::TwitchAutomod;
    const auto newestAutoModFirst = [isAutoModChannel] {
        return isAutoModChannel &&
               getSettings()->autoModReviewQueueOrder.getValue() == 0;
    };
    this->newestFirstAutoModOrder_ = newestAutoModFirst();
    const auto scrollPosition =
        getSettings()->autoModReviewScrollPosition.getValue();
    this->followTop_ =
        isAutoModChannel &&
        (scrollPosition == 1 ||
         (scrollPosition != 2 && this->newestFirstAutoModOrder_));
    if (!this->messageLayoutsMaterialized_)
    {
        this->virtualizedAtBottom_ = !this->followTop_;
        this->virtualizedAtTop_ = this->followTop_;
    }

    //
    // Proxy channel connections
    // Use a proxy channel to keep filtered messages past the time they are removed from their origin channel
    //

    if (sourceChanged)
    {
        this->channelConnections_.managedConnect(
            underlyingChannel->messageAppended,
            [this, newestAutoModFirst](
                MessagePtr &message,
                std::optional<MessageFlags> overridingFlags) {
                if (this->shouldIncludeMessage(message))
                {
                    if (this->split_ && this->parentWidget() == this->split_ &&
                        this->context_ == Context::None &&
                        recording::LiveMessageScope::enabled())
                    {
                        if (auto *recordings = getApp()->getChatRecordings())
                        {
                            recordings->capture(this->split_, message);
                        }
                    }
                    if (newestAutoModFirst())
                    {
                        this->channel_->prependMessage(message);
                        return;
                    }
                    if (this->channel_->lastDate_ != QDate::currentDate())
                    {
                        // Day change message
                        this->channel_->lastDate_ = QDate::currentDate();
                        auto msg = makeSystemMessage(
                            QLocale().toString(QDate::currentDate(),
                                               QLocale::LongFormat),
                            QTime(0, 0));
                        msg->flags.set(MessageFlag::DoNotLog);
                        this->channel_->addMessage(msg,
                                                   MessageContext::Original);
                    }
                    this->channel_->addMessage(message, MessageContext::Repost,
                                               overridingFlags);
                    this->messageAddedToChannel(message);
                    this->maybeAutoTranslateMessage(message);
                }
            });

        this->channelConnections_.managedConnect(
            underlyingChannel->messagesAddedAtStart,
            [this, newestAutoModFirst](std::vector<MessagePtr> &messages) {
                if (newestAutoModFirst())
                {
                    QTimer::singleShot(0, this, [this] {
                        this->refilterMessages();
                    });
                    return;
                }
                std::vector<MessagePtr> filtered;
                std::copy_if(messages.begin(), messages.end(),
                             std::back_inserter(filtered),
                             [this](const auto &msg) {
                                 return this->shouldIncludeMessage(msg);
                             });

                if (!filtered.empty())
                {
                    this->channel_->addMessagesAtStart(filtered);
                }
            });

        this->channelConnections_.managedConnect(
            underlyingChannel->messageReplaced,
            [this](auto index, const auto &prev, const auto &replacement) {
                this->replaceFilteredMessage(index, prev, replacement);
            });

        this->channelConnections_.managedConnect(
            underlyingChannel->filledInMessages,
            [this, newestAutoModFirst](const auto &messages) {
                if (newestAutoModFirst())
                {
                    QTimer::singleShot(0, this, [this] {
                        this->refilterMessages();
                    });
                    return;
                }
                std::vector<MessagePtr> filtered;
                filtered.reserve(messages.size());
                std::copy_if(messages.begin(), messages.end(),
                             std::back_inserter(filtered),
                             [this](const auto &msg) {
                                 return this->shouldIncludeMessage(msg);
                             });
                this->channel_->fillInMissingMessages(filtered);
            });

        this->channelConnections_.managedConnect(
            underlyingChannel->messagesCleared, [this] {
                this->clearMessages();
            });
    }

    for (const auto &notice : this->recordingNotices_)
    {
        if (std::ranges::find(snapshot, notice) == snapshot.end())
        {
            const auto position =
                std::ranges::find_if(snapshot, [&](const auto &message) {
                    return message->serverReceivedTime >
                           notice->serverReceivedTime;
                });
            snapshot.insert(position, notice);
        }
    }

    // Copy over messages from the backing channel to the filtered one
    // and the ui.
    for (auto it = snapshot.crbegin(); it != snapshot.crend(); ++it)
    {
        if (*it && !(*it)->flags.has(MessageFlag::System) &&
            (*it)->serverReceivedTime.isValid())
        {
            this->channel_->lastDate_ =
                (*it)->serverReceivedTime.toLocalTime().date();
            break;
        }
    }

    if (newestAutoModFirst())
    {
        std::reverse(snapshot.begin(), snapshot.end());
    }

    size_t nMessagesAdded = 0;
    for (const auto &msg : snapshot)
    {
        if (std::ranges::find(this->recordingNotices_, msg) ==
                this->recordingNotices_.end() &&
            !this->shouldIncludeMessage(msg))
        {
            continue;
        }

        if (newestAutoModFirst() && nMessagesAdded >= this->messagesLimit_)
        {
            break;
        }

        if (this->messageLayoutsMaterialized_)
        {
            auto messageLayout = std::make_shared<MessageLayout>(msg);

            if (this->lastMessageHasAlternateBackground_)
            {
                messageLayout->flags.set(
                    MessageLayoutFlag::AlternateBackground);
            }
            if (underlyingChannel->shouldIgnoreHighlights())
            {
                messageLayout->flags.set(MessageLayoutFlag::IgnoreHighlights);
            }

            this->messages_.pushBack(messageLayout);
            if (this->showScrollbarHighlights())
            {
                this->scrollBar_->addHighlight(scrollbarHighlightForMessage(
                    msg, this->nukePreviewMessageIds_));
            }
        }
        this->lastMessageHasAlternateBackground_ =
            !this->lastMessageHasAlternateBackground_;

        this->channel_->addMessage(msg, MessageContext::Repost);

        nMessagesAdded++;
    }

    this->scrollBar_->setMaximum(
        this->messageLayoutsMaterialized_
            ? static_cast<qreal>(
                  std::min(nMessagesAdded, this->messages_.limit()))
            : 0);

    if (this->paused() && this->messageLayoutsMaterialized_)
    {
        this->snapshot_ = this->messages_.getSnapshot();
    }

    //
    // Standard channel connections
    //

    // on new message
    this->proxyConnections_.managedConnect(
        this->channel_->messageAppended,
        [this](MessagePtr &message,
               std::optional<MessageFlags> overridingFlags) {
            this->messageAppended(message, overridingFlags);
        });

    this->proxyConnections_.managedConnect(
        this->channel_->messagePrepended,
        [this](MessagePtr &message, const MessagePtr &evicted) {
            this->messagePrepended(message, evicted);
        });

    this->proxyConnections_.managedConnect(
        this->channel_->messagesAddedAtStart,
        [this](std::vector<MessagePtr> &messages) {
            this->messageAddedAtStart(messages);
        });

    // on message replaced
    this->proxyConnections_.managedConnect(
        this->channel_->messageReplaced,
        [this](size_t index, const MessagePtr &prev,
               const MessagePtr &replacement) {
            this->messageReplaced(index, prev, replacement);
        });

    // on messages filled in
    this->proxyConnections_.managedConnect(this->channel_->filledInMessages,
                                           [this](const auto &) {
                                               this->messagesUpdated();
                                           });

    this->updateID();

    this->queueLayout();
    if (!this->isVisible() && !this->scrollBar_->isVisible())
    {
        this->scrollToFollowedEdge();
    }
    this->queueUpdate();

    if (sourceChanged)
    {
        // Notifications
        auto *mc = dynamic_cast<MultiChannel *>(underlyingChannel.get());
        auto handleChan = [this](Channel *chan) {
            auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan);
            if (twitchChannel != nullptr)
            {
                this->channelConnections_.managedConnect(
                    twitchChannel->streamStatusChanged, [this]() {
                        this->liveStatusChanged.invoke();
                    });
            }
            else if (auto *kickChannel = dynamic_cast<KickChannel *>(chan))
            {
                this->channelConnections_.managedConnect(
                    kickChannel->liveStatusChanged, [this] {
                        this->liveStatusChanged.invoke();
                    });
            }
            else if (auto *youtubeChannel =
                         dynamic_cast<YouTubeChannel *>(chan))
            {
                this->channelConnections_.managedConnect(
                    youtubeChannel->liveStatusChanged, [this] {
                        this->liveStatusChanged.invoke();
                    });
            }
            else if (auto *tiktokChannel = dynamic_cast<TikTokChannel *>(chan))
            {
                this->channelConnections_.managedConnect(
                    tiktokChannel->liveStatusChanged, [this] {
                        this->liveStatusChanged.invoke();
                    });
            }
        };

        if (mc)
        {
            for (const auto &child : mc->channels())
            {
                handleChan(child.channel.get());
            }
            this->channelConnections_.managedConnect(
                mc->activeChannelChanged, [this] {
                    this->liveStatusChanged.invoke();
                });
        }
        else
        {
            handleChan(underlyingChannel.get());
        }
    }
}

void ChannelView::setFilters(const QList<QUuid> &ids)
{
    this->channelFilters_ = std::make_shared<FilterSet>(ids);

    this->updateID();
}

QList<QUuid> ChannelView::getFilterIds() const
{
    if (!this->channelFilters_)
    {
        return {};
    }

    return this->channelFilters_->filterIds();
}

FilterSetPtr ChannelView::getFilterSet() const
{
    return this->channelFilters_;
}

bool ChannelView::shouldIncludeMessage(const MessagePtr &m) const
{
    if (m)
    {
        if (auto *hiddenUsers = getApp()->getHiddenUsers();
            hiddenUsers && hiddenUsers->shouldHideMessage(*m))
        {
            return false;
        }
    }
    if (this->messagePredicate_ && !this->messagePredicate_(m))
    {
        return false;
    }
    if (this->channelFilters_)
    {
        if (getSettings()->excludeUserMessagesFromFilter &&
            getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
                m->loginName, Qt::CaseInsensitive) == 0)
        {
            return true;
        }

        return this->channelFilters_->filter(m, this->underlyingChannel_);
    }

    return true;
}

void ChannelView::setMessagePredicate(
    std::function<bool(const MessagePtr &)> predicate)
{
    this->messagePredicate_ = std::move(predicate);
    this->refilterMessages();
}

void ChannelView::addRecordingNotice(const MessagePtr &message)
{
    if (this->recordingNotices_.size() == 4)
    {
        this->recordingNotices_.erase(this->recordingNotices_.begin());
    }
    this->recordingNotices_.push_back(message);
    this->channel_->addMessage(message, MessageContext::Repost);
}

void ChannelView::refilterMessages()
{
    if (!this->underlyingChannel_)
    {
        return;
    }
    this->lastClearedMessage_.reset();
    this->refilterMessages(this->underlyingChannel_->getMessageSnapshot());
}

void ChannelView::replaceFilteredMessage(size_t index,
                                         const MessagePtr &previous,
                                         const MessagePtr &replacement)
{
    if (this->lastClearedMessage_.lock() == previous)
    {
        this->lastClearedMessage_ = replacement;
        return;
    }
    const bool include = this->shouldIncludeMessage(replacement);
    if (include && this->split_ && this->parentWidget() == this->split_ &&
        this->context_ == Context::None &&
        recording::LiveMessageScope::enabled())
    {
        if (auto *recordings = getApp()->getChatRecordings())
        {
            recordings->capture(this->split_, replacement);
        }
    }
    if (include && this->channel_->replaceMessage(index, previous, replacement))
    {
        return;
    }
    if (include && this->shouldIncludeMessage(previous))
    {
        return;
    }

    auto snapshot = this->channel_->getMessageSnapshot();
    if (this->newestFirstAutoModOrder_)
    {
        std::reverse(snapshot.begin(), snapshot.end());
    }
    const auto old = std::ranges::find(snapshot, previous);
    if (!include)
    {
        if (old == snapshot.end())
        {
            return;
        }
        snapshot.erase(old);
    }
    else
    {
        const auto source = this->underlyingChannel_->getMessageSnapshot();
        const auto current = std::ranges::find(source, replacement);
        if (current == source.end())
        {
            return;
        }
        if (const auto cleared = this->lastClearedMessage_.lock())
        {
            const auto boundary = std::ranges::find(source, cleared);
            if (boundary != source.end() && current <= boundary)
            {
                return;
            }
        }
        std::unordered_map<const Message *, size_t> positions;
        positions.reserve(snapshot.size());
        for (size_t i = 0; i < snapshot.size(); ++i)
        {
            positions.emplace(snapshot[i].get(), i);
        }
        auto insertion = snapshot.end();
        for (auto next = current + 1; next != source.end(); ++next)
        {
            const auto neighbor = positions.find(next->get());
            if (neighbor != positions.end())
            {
                insertion = snapshot.begin() + neighbor->second;
                break;
            }
        }
        if (insertion == snapshot.end())
        {
            for (auto prior = current; prior != source.begin();)
            {
                --prior;
                const auto neighbor = positions.find(prior->get());
                if (neighbor != positions.end())
                {
                    insertion = snapshot.begin() + neighbor->second + 1;
                    break;
                }
            }
        }
        snapshot.insert(insertion, replacement);
    }
    this->refilterMessages(std::move(snapshot));
}

void ChannelView::refilterMessages(std::vector<MessagePtr> snapshot)
{
    const auto wasMaterialized = this->messageLayoutsMaterialized_;
    const bool preservePausedSnapshot = this->paused() && wasMaterialized;
    const auto materializedSnapshot = wasMaterialized
                                          ? this->messages_.getSnapshot()
                                          : std::vector<MessageLayoutPtr>{};
    const auto materializedLastRead =
        wasMaterialized && this->lastReadMessage_ != nullptr
            ? this->lastReadMessage_->getMessagePtr()
            : MessagePtr{};
    const auto materializedLastReadTarget =
        this->pauseLastReadTarget_ != nullptr ? this->pauseLastReadTarget_
                                              : materializedLastRead;
    const bool previousNewestFirstAutoModOrder = this->newestFirstAutoModOrder_;
    const bool previouslyFollowedTop = this->followTop_;
    auto pausedSnapshot = preservePausedSnapshot
                              ? this->snapshot_
                              : std::vector<MessageLayoutPtr>{};
    const auto pausedIdentitySnapshot = pausedSnapshot;
    const auto &selectionIdentitySnapshot =
        preservePausedSnapshot ? pausedSnapshot : materializedSnapshot;
    const auto pausedRelative =
        pausedSnapshot.empty()
            ? qreal(0)
            : std::clamp(this->scrollBar_->getDesiredValue() -
                             this->scrollBar_->getMinimum(),
                         qreal(0),
                         static_cast<qreal>(pausedSnapshot.size() - 1));
    const auto pausedAnchorIndex =
        static_cast<size_t>(std::floor(pausedRelative));
    const auto pausedAnchorFraction =
        pausedRelative - std::floor(pausedRelative);
    const auto pausedAnchorMessage =
        pausedSnapshot.empty() || pausedSnapshot[pausedAnchorIndex] == nullptr
            ? MessagePtr{}
            : pausedSnapshot[pausedAnchorIndex]->getMessagePtr();
    const auto savedSelection =
        saveSelectionIdentity(this->selection_, selectionIdentitySnapshot);
    const auto savedDoubleClickSelection = saveSelectionIdentity(
        this->doubleClickSelection_, selectionIdentitySnapshot);
    auto wasAtBottom = wasMaterialized ? this->isLogicallyAtBottom()
                                       : this->virtualizedAtBottom_;
    auto wasAtTop =
        wasMaterialized ? this->isLogicallyAtTop() : this->virtualizedAtTop_;
    const auto relativePosition =
        this->scrollBar_->getDesiredValue() - this->scrollBar_->getMinimum();
    auto virtualizedAnchor = this->virtualizedScrollAnchor_;
    auto virtualizedFraction = this->virtualizedScrollFraction_;
    auto virtualizedDistance = this->virtualizedDistanceFromBottom_;
    auto virtualizedLastRead = this->virtualizedLastReadMessage_;
    auto virtualizedExpanded = this->virtualizedExpandedMessages_;
    auto virtualizedMessages = !wasMaterialized
                                   ? this->channel_->getMessageSnapshot()
                                   : std::vector<MessagePtr>{};
    std::optional<size_t> virtualizedAnchorIndex;
    if (!wasMaterialized && !wasAtBottom && !wasAtTop &&
        !virtualizedMessages.empty())
    {
        virtualizedAnchorIndex =
            equivalentMessageIndex(virtualizedMessages, virtualizedAnchor);
        if (!virtualizedAnchorIndex)
        {
            virtualizedAnchorIndex =
                virtualizedMessages.size() > virtualizedDistance
                    ? virtualizedMessages.size() - virtualizedDistance
                    : 0;
        }
    }
    std::optional<qreal> pausedFrozenPosition;
    bool pausedFrozenFollowBottom = false;
    bool pausedFrozenFollowTop = false;

    if (wasMaterialized)
    {
        const auto &layouts = materializedSnapshot;
        virtualizedAnchor.reset();
        virtualizedFraction = 0;
        virtualizedDistance = 0;
        if ((!wasAtBottom ||
             (this->followTop_ && !this->newestFirstAutoModOrder_)) &&
            !layouts.empty())
        {
            const auto relative =
                std::clamp(relativePosition, qreal(0),
                           static_cast<qreal>(layouts.size() - 1));
            const auto index = static_cast<size_t>(std::floor(relative));
            virtualizedAnchor = layouts[index]->getMessagePtr();
            virtualizedFraction = relative - std::floor(relative);
            virtualizedDistance = layouts.size() - index;
        }
        virtualizedLastRead = this->lastReadMessage_ != nullptr
                                  ? this->lastReadMessage_->getMessagePtr()
                                  : MessagePtr{};
        virtualizedExpanded.clear();
        for (const auto &layout : layouts)
        {
            if (layout->flags.has(MessageLayoutFlag::Expanded))
            {
                virtualizedExpanded.push_back(layout->getMessagePtr());
            }
        }
    }
    const auto selectedKey = this->autoModReviewSelectedKey_;
    const auto nukePreviewIds = this->nukePreviewMessageIds_;
    auto channel = this->underlyingChannel_;
    this->setChannel(channel, std::move(snapshot));
    this->setNukePreviewMessageIds(nukePreviewIds);
    if (!preservePausedSnapshot && !this->messageLayoutsMaterialized_ &&
        !savedSelection.value.isEmpty())
    {
        const auto filteredMessages = this->channel_->getMessageSnapshot();
        if (equivalentMessageIndex(filteredMessages,
                                   savedSelection.startMessage) &&
            equivalentMessageIndex(filteredMessages, savedSelection.endMessage))
        {
            this->materializeMessageLayouts();
        }
    }
    const bool autoModOrderReversed =
        previousNewestFirstAutoModOrder != this->newestFirstAutoModOrder_;
    const bool wasFollowing = previouslyFollowedTop ? wasAtTop : wasAtBottom;
    if (previouslyFollowedTop != this->followTop_ ||
        (autoModOrderReversed && !wasFollowing))
    {
        std::swap(wasAtBottom, wasAtTop);
    }
    if (autoModOrderReversed && preservePausedSnapshot)
    {
        std::reverse(pausedSnapshot.begin(), pausedSnapshot.end());
    }
    const bool materializedFromVirtualized =
        !wasMaterialized && this->messageLayoutsMaterialized_;
    if (preservePausedSnapshot)
    {
        std::vector<MessageLayoutPtr> filteredSnapshot;
        filteredSnapshot.reserve(pausedSnapshot.size());
        bool alternate = false;
        for (auto &layout : pausedSnapshot)
        {
            if (layout == nullptr ||
                !this->shouldIncludeMessage(layout->getMessagePtr()))
            {
                continue;
            }
            if (alternate)
            {
                layout->flags.set(MessageLayoutFlag::AlternateBackground);
            }
            else
            {
                layout->flags.unset(MessageLayoutFlag::AlternateBackground);
            }
            alternate = !alternate;
            filteredSnapshot.push_back(std::move(layout));
        }
        this->snapshot_ = std::move(filteredSnapshot);

        this->lastReadMessage_.reset();
        if (const auto lastReadIndex =
                equivalentMessageIndex(this->snapshot_, materializedLastRead))
        {
            this->lastReadMessage_ = this->snapshot_[*lastReadIndex];
            this->pauseLastReadNeedsRemap_ = true;
            this->pauseLastReadTarget_ = materializedLastReadTarget;
        }

        restoreSelectionIdentity(savedSelection, this->snapshot_,
                                 this->selection_);
        restoreSelectionIdentity(savedDoubleClickSelection, this->snapshot_,
                                 this->doubleClickSelection_);

        const auto currentSize = this->messages_.size();
        const auto frozenSize = this->snapshot_.size();
        this->scrollBar_->setMinimum(0);
        this->scrollBar_->setMaximum(static_cast<qreal>(frozenSize));
        this->pauseScrollMinimumOffset_ = 0;
        this->pauseScrollMaximumOffset_ =
            static_cast<int>(currentSize) - static_cast<int>(frozenSize);
        this->pauseScrollValueOffset_ = 0;

        pausedFrozenFollowBottom = wasAtBottom && !this->snapshot_.empty();
        pausedFrozenFollowTop = wasAtTop && !this->snapshot_.empty();
        if (!pausedFrozenFollowBottom && !pausedFrozenFollowTop)
        {
            if (this->snapshot_.empty())
            {
                pausedFrozenPosition = 0;
            }
            else
            {
                const auto retained = nearestRetainedMessage(
                    pausedIdentitySnapshot, this->snapshot_, pausedAnchorIndex);
                const auto restoredIndex =
                    retained ? retained->second : size_t{0};
                const auto exactAnchor =
                    retained && retained->first == pausedAnchorIndex &&
                    this->snapshot_[restoredIndex]->getMessagePtr() ==
                        pausedAnchorMessage;
                pausedFrozenPosition =
                    static_cast<qreal>(restoredIndex) +
                    (exactAnchor ? pausedAnchorFraction : qreal(0));
            }
        }
    }
    if (!this->messageLayoutsMaterialized_)
    {
        if (!wasAtBottom && !wasAtTop)
        {
            const auto currentMessages = this->channel_->getMessageSnapshot();
            if (virtualizedAnchorIndex)
            {
                if (const auto retained = nearestRetainedMessage(
                        virtualizedMessages, currentMessages,
                        *virtualizedAnchorIndex))
                {
                    const bool exactAnchor =
                        retained->first == *virtualizedAnchorIndex;
                    virtualizedAnchor = currentMessages[retained->second];
                    virtualizedFraction =
                        exactAnchor ? virtualizedFraction : qreal(0);
                    virtualizedDistance =
                        currentMessages.size() - retained->second;
                }
                else
                {
                    virtualizedAnchor.reset();
                    virtualizedFraction = 0;
                    virtualizedDistance = currentMessages.size();
                }
            }
            else
            {
                virtualizedAnchor.reset();
                virtualizedFraction = 0;
                virtualizedDistance = currentMessages.size();
            }
        }
        this->virtualizedAtBottom_ = wasAtBottom;
        this->virtualizedAtTop_ = wasAtTop;
        this->virtualizedScrollAnchor_ = virtualizedAnchor;
        this->virtualizedScrollFraction_ = virtualizedFraction;
        this->virtualizedDistanceFromBottom_ = virtualizedDistance;
        this->virtualizedLastReadMessage_ = virtualizedLastRead;
        this->virtualizedExpandedMessages_ = std::move(virtualizedExpanded);
    }
    else
    {
        const auto layouts = this->messages_.getSnapshot();
        if (!preservePausedSnapshot)
        {
            restoreSelectionIdentity(savedSelection, layouts, this->selection_);
            restoreSelectionIdentity(savedDoubleClickSelection, layouts,
                                     this->doubleClickSelection_);
        }
        if (!preservePausedSnapshot && !materializedFromVirtualized)
        {
            this->lastReadMessage_.reset();
            if (const auto lastReadIndex =
                    equivalentMessageIndex(layouts, materializedLastRead))
            {
                this->lastReadMessage_ = layouts[*lastReadIndex];
            }
        }
        if (!virtualizedExpanded.empty())
        {
            const auto expandedMessages =
                messageIdentityPointers(virtualizedExpanded);
            for (const auto &layout : layouts)
            {
                if (expandedMessages.contains(layout->getMessagePtr().get()))
                {
                    layout->flags.set(MessageLayoutFlag::Expanded);
                }
            }
        }
        if (materializedFromVirtualized)
        {
            this->lastReadMessage_.reset();
            if (const auto lastReadIndex =
                    equivalentMessageIndex(layouts, virtualizedLastRead))
            {
                this->lastReadMessage_ = layouts[*lastReadIndex];
            }

            this->measureScrollbarForScrollRestore();
            if (wasAtBottom || layouts.empty())
            {
                this->scrollBar_->scrollToBottom(false);
            }
            else if (wasAtTop)
            {
                this->scrollBar_->scrollToTop(false);
            }
            else
            {
                const auto anchorIndex =
                    equivalentMessageIndex(layouts, virtualizedAnchor);
                const auto restoredIndex = anchorIndex.value_or(
                    layouts.size() > virtualizedDistance
                        ? layouts.size() - virtualizedDistance
                        : 0);
                this->scrollBar_->setDesiredValue(
                    static_cast<qreal>(restoredIndex) +
                        (anchorIndex ? virtualizedFraction : qreal(0)),
                    false);
            }
            this->queueLayout();
        }
    }
    if (!selectedKey.isEmpty())
    {
        const auto snapshot = this->channel_->getMessageSnapshot();
        const bool selectedStillVisible =
            std::ranges::any_of(snapshot, [&selectedKey](const auto &message) {
                return message && message->autoModReview &&
                       message->autoModReview->key == selectedKey;
            });
        if (selectedStillVisible)
        {
            if (this->messageLayoutsMaterialized_)
            {
                this->measureScrollbarForScrollRestore();
            }
            this->autoModReviewSelectedKey_.clear();
            this->setAutoModReviewSelectedKey(selectedKey);
            if (!preservePausedSnapshot)
            {
                return;
            }
            const auto frozenSelected = std::ranges::find_if(
                this->snapshot_, [&selectedKey](const auto &layout) {
                    const auto &message = layout->getMessagePtr();
                    return message && message->autoModReview &&
                           message->autoModReview->key == selectedKey;
                });
            if (frozenSelected != this->snapshot_.end())
            {
                pausedFrozenFollowBottom = false;
                pausedFrozenFollowTop = false;
                pausedFrozenPosition = static_cast<qreal>(
                    frozenSelected - this->snapshot_.begin());
            }
        }
        else
        {
            this->setAutoModReviewSelectedKey({});
        }
    }
    if (!this->messageLayoutsMaterialized_)
    {
        return;
    }
    if (materializedFromVirtualized && !preservePausedSnapshot)
    {
        return;
    }
    if (preservePausedSnapshot)
    {
        this->pauseSelectionNeedsRemap_ = true;

        this->measureScrollbarForScrollRestore();
        if (pausedFrozenFollowBottom)
        {
            this->scrollBar_->scrollToBottom(false);
        }
        else if (pausedFrozenFollowTop)
        {
            this->scrollBar_->scrollToTop(false);
        }
        else
        {
            this->scrollBar_->setDesiredValue(
                pausedFrozenPosition.value_or(qreal(0)), false);
        }
        this->queueLayout();
        if (!this->snapshot_.empty())
        {
            const auto relative = std::clamp(
                this->scrollBar_->getDesiredValue() -
                    this->scrollBar_->getMinimum(),
                qreal(0), static_cast<qreal>(this->snapshot_.size() - 1));
            const auto frozenIndex = static_cast<size_t>(std::floor(relative));
            const auto currentLayouts = this->messages_.getSnapshot();
            if (const auto retained = nearestRetainedMessage(
                    this->snapshot_, currentLayouts, frozenIndex))
            {
                if (!pausedFrozenFollowBottom && !pausedFrozenFollowTop)
                {
                    const auto exactAnchor = retained->first == frozenIndex;
                    const auto target =
                        static_cast<qreal>(retained->second) +
                        (exactAnchor ? relative - std::floor(relative)
                                     : qreal(0));
                    this->pauseScrollValueOffset_ = target - relative;
                }
            }
        }
        return;
    }

    this->measureScrollbarForScrollRestore();
    if (wasAtBottom)
    {
        this->scrollBar_->scrollToBottom();
    }
    else if (wasAtTop)
    {
        this->scrollBar_->scrollToTop();
    }
    else
    {
        const auto layouts = this->messages_.getSnapshot();
        if (const auto anchorIndex =
                equivalentMessageIndex(layouts, virtualizedAnchor))
        {
            this->scrollBar_->setDesiredValue(
                this->scrollBar_->getMinimum() +
                    static_cast<qreal>(*anchorIndex) + virtualizedFraction,
                false);
        }
        else if (!materializedSnapshot.empty())
        {
            const auto oldIndex = static_cast<size_t>(std::clamp(
                relativePosition, qreal(0),
                static_cast<qreal>(materializedSnapshot.size() - 1)));
            const auto retained =
                nearestRetainedMessage(materializedSnapshot, layouts, oldIndex);
            this->scrollBar_->setDesiredValue(
                this->scrollBar_->getMinimum() +
                    static_cast<qreal>(retained ? retained->second : 0),
                false);
        }
        else
        {
            this->scrollBar_->setDesiredValue(this->scrollBar_->getMinimum() +
                                              relativePosition);
        }
    }
}

const QString &ChannelView::autoModReviewSelectedKey() const
{
    return this->autoModReviewSelectedKey_;
}

void ChannelView::setAutoModReviewSelectedKey(QString key)
{
    if (this->autoModReviewSelectedKey_ == key)
    {
        return;
    }
    this->autoModReviewSelectedKey_ = std::move(key);
    this->queueLayout();
    this->queueUpdate();

    if (!this->autoModReviewSelectedKey_.isEmpty())
    {
        const auto selectedKey = this->autoModReviewSelectedKey_;

        QTimer::singleShot(0, this, [this, selectedKey] {
            if (this->autoModReviewSelectedKey_ != selectedKey)
            {
                return;
            }
            if (getSettings()->autoModReviewShowContext)
            {
                if (auto *review = getApp()->getAutoModReview())
                {
                    review->ensureContext(selectedKey);
                }
            }
        });
    }

    if (!this->messageLayoutsMaterialized_)
    {
        this->revealAutoModSelectionOnMaterialize_ =
            !this->autoModReviewSelectedKey_.isEmpty();
        return;
    }
    this->revealAutoModSelectionOnMaterialize_ = false;

    auto &snapshot = this->getMessagesSnapshot();
    for (size_t index = 0; index < snapshot.size(); ++index)
    {
        const auto &message = snapshot[index]->getMessagePtr();
        if (message && message->autoModReview &&
            message->autoModReview->key == this->autoModReviewSelectedKey_)
        {
            this->scrollBar_->setDesiredValue(this->scrollBar_->getMinimum() +
                                              static_cast<qreal>(index));
            break;
        }
    }
}

bool ChannelView::selectAutoModReview(int direction, bool actionableOnly)
{
    auto &snapshot = this->getMessagesSnapshot();
    if (snapshot.empty())
    {
        return false;
    }

    QVector<QString> keys;
    QSet<QString> seenKeys;
    keys.reserve(static_cast<qsizetype>(snapshot.size()));
    auto *review = getApp()->getAutoModReview();
    for (const auto &layout : snapshot)
    {
        const auto &message = layout->getMessagePtr();
        if (!message || !message->autoModReview ||
            seenKeys.contains(message->autoModReview->key))
        {
            continue;
        }
        const auto *item = review != nullptr
                               ? review->find(message->autoModReview->key)
                               : nullptr;
        if (actionableOnly &&
            (item == nullptr || !automod::isActionable(*item)))
        {
            continue;
        }
        seenKeys.insert(message->autoModReview->key);
        keys.push_back(message->autoModReview->key);
    }
    if (keys.isEmpty())
    {
        return false;
    }

    auto index = keys.indexOf(this->autoModReviewSelectedKey_);
    if (index < 0 && actionableOnly)
    {
        this->setAutoModReviewSelectedKey(this->followTop_ ? keys.front()
                                                           : keys.back());
        return true;
    }
    if (index < 0)
    {
        index = direction < 0 ? 0 : -1;
    }
    const auto count = keys.size();
    index = (index + (direction < 0 ? -1 : 1) + count) % count;
    this->setAutoModReviewSelectedKey(keys.at(index));
    return true;
}

bool ChannelView::handleAutoModReviewKey(QKeyEvent *event)
{
    const auto modifiers = event != nullptr
                               ? event->modifiers() & ~Qt::KeypadModifier
                               : Qt::KeyboardModifiers{};
    if (event == nullptr || this->underlyingChannel_ == nullptr ||
        this->underlyingChannel_->getType() != Channel::Type::TwitchAutomod ||
        modifiers != Qt::NoModifier)
    {
        return false;
    }

    const bool shortcutOverride = event->type() == QEvent::ShortcutOverride;
    switch (event->key())
    {
        case Qt::Key_Up:
            return shortcutOverride || this->selectAutoModReview(-1, false);
        case Qt::Key_Down:
            return shortcutOverride || this->selectAutoModReview(1, false);
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (shortcutOverride)
            {
                return true;
            }
            if (this->autoModReviewSelectedKey_.isEmpty() &&
                !this->selectAutoModReview(1, false))
            {
                return false;
            }
            return this->openSelectedAutoModUsercard();
        case Qt::Key_A:
            return shortcutOverride || this->approveSelectedAutoMod();
        case Qt::Key_D:
            return shortcutOverride || this->denySelectedAutoMod();
        case Qt::Key_T:
            return shortcutOverride || this->timeoutSelectedAutoMod();
        case Qt::Key_B:
            return shortcutOverride || this->banSelectedAutoMod();
        case Qt::Key_Escape:
            if (this->autoModReviewSelectedKey_.isEmpty())
            {
                return false;
            }
            if (!shortcutOverride)
            {
                this->setAutoModReviewSelectedKey({});
            }
            return true;
        default:
            return false;
    }
}

bool ChannelView::approveSelectedAutoMod()
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr
                           ? review->find(this->autoModReviewSelectedKey_)
                           : nullptr;
    if (item == nullptr || !item->actionsAllowed ||
        item->state != automod::ReviewState::Open)
    {
        return false;
    }
    review->approve(this->autoModReviewSelectedKey_);
    return true;
}

bool ChannelView::denySelectedAutoMod()
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr
                           ? review->find(this->autoModReviewSelectedKey_)
                           : nullptr;
    if (item == nullptr || !item->actionsAllowed ||
        item->state != automod::ReviewState::Open)
    {
        return false;
    }
    review->deny(this->autoModReviewSelectedKey_);
    return true;
}

bool ChannelView::retrySelectedAutoMod()
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr
                           ? review->find(this->autoModReviewSelectedKey_)
                           : nullptr;
    if (item == nullptr || (item->failedAction == automod::ReviewAction::None &&
                            !item->secondaryFailed))
    {
        return false;
    }
    review->retry(item->key);
    return true;
}

bool ChannelView::openSelectedAutoModUsercard()
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr
                           ? review->find(this->autoModReviewSelectedKey_)
                           : nullptr;
    if (item == nullptr)
    {
        return false;
    }
    this->showUserInfoPopup(item->userLogin, MessagePlatform::AnyOrTwitch,
                            item->broadcasterLogin);
    return true;
}

QString ChannelView::autoModReviewDetails(const QString &key) const
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr ? review->find(key) : nullptr;
    if (item == nullptr)
    {
        return {};
    }
    QString details = u"AutoMod Review\nChannel: #"_s % item->broadcasterLogin %
                      u"\nUser: " % item->userLogin % u"\nState: " %
                      automod::reviewStateLabel(*item) % u"\nWhy held: " %
                      item->reasonSummary;
    if (!item->matchedFragments.isEmpty())
    {
        details +=
            u"\nMatched by Twitch: " % item->matchedFragments.join(u", "_s);
    }
    if (!item->statusDetail.isEmpty())
    {
        details += u"\nResult: " % item->statusDetail;
    }
    details += u"\n\n" % item->messageText;
    return details;
}

bool ChannelView::copySelectedAutoModDetails()
{
    const auto details =
        this->autoModReviewDetails(this->autoModReviewSelectedKey_);
    if (details.isEmpty())
    {
        return false;
    }
    QApplication::clipboard()->setText(details);
    return true;
}

bool ChannelView::timeoutSelectedAutoMod()
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr
                           ? review->find(this->autoModReviewSelectedKey_)
                           : nullptr;
    if (item == nullptr || !item->actionsAllowed ||
        item->state != automod::ReviewState::Open)
    {
        return false;
    }
    auto menuPosition = QCursor::pos();
    if (!this->rect().contains(this->mapFromGlobal(menuPosition)))
    {
        menuPosition = this->mapToGlobal(this->rect().center());
    }
    this->showAutoModTimeoutMenu(this->autoModReviewSelectedKey_, menuPosition);
    return true;
}

bool ChannelView::banSelectedAutoMod()
{
    return this->confirmAutoModBan(this->autoModReviewSelectedKey_);
}

ChannelPtr ChannelView::sourceChannel() const
{
    return this->sourceChannel_;
}

void ChannelView::setSourceChannel(ChannelPtr sourceChannel)
{
    this->sourceChannel_ = std::move(sourceChannel);
}

bool ChannelView::hasSourceChannel() const
{
    return this->sourceChannel_ != nullptr;
}

void ChannelView::messageAppended(MessagePtr &message,
                                  std::optional<MessageFlags> overridingFlags)
{
    auto *messageFlags = &message->flags;
    if (overridingFlags)
    {
        messageFlags = &*overridingFlags;
    }

    if (this->messageLayoutsMaterialized_)
    {
        auto messageRef = std::make_shared<MessageLayout>(message);

        if (this->lastMessageHasAlternateBackground_)
        {
            messageRef->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        if (this->channel_->shouldIgnoreHighlights())
        {
            messageRef->flags.set(MessageLayoutFlag::IgnoreHighlights);
        }

        if (this->paused())
        {
            this->pauseScrollMaximumOffset_++;
        }
        else
        {
            this->scrollBar_->offsetMaximum(1);
        }

        if (this->messages_.pushBack(messageRef))
        {
            if (this->paused())
            {
                this->pauseScrollMinimumOffset_++;
                --this->pauseSelectionIndexOffset_;
            }
            else
            {
                this->scrollBar_->offsetMinimum(1);
                if (this->followingScrollEdge_ && !this->isVisible())
                {
                    this->scrollToFollowedEdge(false);
                }
                this->selection_.shiftMessageIndex(1);
                this->doubleClickSelection_.shiftMessageIndex(1);
            }
        }

        if (this->showScrollbarHighlights())
        {
            this->scrollBar_->addHighlight(scrollbarHighlightForMessage(
                message, this->nukePreviewMessageIds_));
        }
    }
    else
    {
        if (this->followTop_ && !this->newestFirstAutoModOrder_)
        {
            this->virtualizedAtBottom_ = false;
        }
        if (!this->virtualizedAtBottom_)
        {
            ++this->virtualizedDistanceFromBottom_;
            const auto retained = this->channel_->countMessages();
            if (this->virtualizedDistanceFromBottom_ > retained)
            {
                this->virtualizedScrollAnchor_.reset();
                this->virtualizedScrollFraction_ = 0;
            }
        }

        const auto retained = this->channel_->countMessages();
        for (auto &distance : this->virtualizedAlternateBreaksFromBottom_)
        {
            ++distance;
        }
        std::erase_if(this->virtualizedAlternateBreaksFromBottom_,
                      [retained](size_t distance) {
                          return distance >= retained;
                      });
    }
    this->lastMessageHasAlternateBackground_ =
        !this->lastMessageHasAlternateBackground_;

    if (!messageFlags->has(MessageFlag::DoNotTriggerNotification) &&
        !message->isHiddenByClientNonce())
    {
        if ((messageFlags->has(MessageFlag::Highlighted) &&
             messageFlags->has(MessageFlag::ShowInMentions) &&
             !messageFlags->has(MessageFlag::Subscription) &&
             (getSettings()->highlightMentions ||
              this->channel_->getType() != Channel::Type::TwitchMentions)) ||
            (this->channel_->getType() == Channel::Type::TwitchAutomod &&
             getSettings()->enableAutomodHighlight))
        {
            this->tabHighlightRequested.invoke(
                {.state = HighlightState::Highlighted,
                 .color = message->highlightColor});
        }
        else
        {
            this->tabHighlightRequested.invoke(
                {.state = HighlightState::NewMessage});
        }
    }

    if (!this->autoModReviewSelectedKey_.isEmpty() &&
        this->channel_->getType() == Channel::Type::TwitchAutomod &&
        this->channel_->countMessages() >= this->messagesLimit_)
    {
        const auto retained = this->channel_->getMessageSnapshot();
        if (std::ranges::none_of(retained, [this](const auto &message) {
                return message && message->autoModReview &&
                       message->autoModReview->key ==
                           this->autoModReviewSelectedKey_;
            }))
        {
            this->setAutoModReviewSelectedKey({});
        }
    }

    this->queueLayout();
    if (!this->isVisible())
    {
        this->releaseHiddenMessageLayouts();
    }
}

void ChannelView::messagePrepended(MessagePtr &message,
                                   const MessagePtr &evicted)
{
    if (!this->messageLayoutsMaterialized_)
    {
        if (!this->followTop_)
        {
            this->virtualizedAtTop_ = false;
        }
        if (evicted != nullptr)
        {
            if (!this->virtualizedAtBottom_ && !this->virtualizedAtTop_ &&
                this->virtualizedDistanceFromBottom_ != 0)
            {
                --this->virtualizedDistanceFromBottom_;
                if (this->virtualizedDistanceFromBottom_ == 0)
                {
                    this->virtualizedScrollAnchor_.reset();
                    this->virtualizedScrollFraction_ = 0;
                }
            }

            const bool hadEndpoint = hasAlternateBreak(
                this->virtualizedAlternateBreaksFromBottom_, 0);
            std::erase(this->virtualizedAlternateBreaksFromBottom_, size_t{0});
            for (auto &distance : this->virtualizedAlternateBreaksFromBottom_)
            {
                --distance;
            }
            if (hadEndpoint)
            {
                const auto endpoint = std::ranges::lower_bound(
                    this->virtualizedAlternateBreaksFromBottom_, size_t{0});
                if (endpoint !=
                        this->virtualizedAlternateBreaksFromBottom_.end() &&
                    *endpoint == 0)
                {
                    this->virtualizedAlternateBreaksFromBottom_.erase(endpoint);
                }
                else
                {
                    this->virtualizedAlternateBreaksFromBottom_.insert(endpoint,
                                                                       0);
                }
            }
            this->lastMessageHasAlternateBackground_ =
                !this->lastMessageHasAlternateBackground_;
        }
        else if (this->channel_->countMessages() == 1 &&
                 !this->lastMessageHasAlternateBackground_)
        {
            this->virtualizedAlternateBreaksFromBottom_.push_back(0);
        }
        if (evicted && evicted->autoModReview &&
            evicted->autoModReview->key == this->autoModReviewSelectedKey_)
        {
            this->setAutoModReviewSelectedKey({});
        }
        if (!message->flags.has(MessageFlag::DoNotTriggerNotification) &&
            !message->isHiddenByClientNonce())
        {
            if (getSettings()->enableAutomodHighlight)
            {
                this->tabHighlightRequested.invoke(
                    {.state = HighlightState::Highlighted,
                     .color = message->highlightColor});
            }
            else
            {
                this->tabHighlightRequested.invoke(
                    {.state = HighlightState::NewMessage});
            }
        }
        this->queueLayout();
        return;
    }

    auto messageRef = std::make_shared<MessageLayout>(message);
    const auto previousFirst = this->messages_.first();
    if (previousFirst &&
        !(*previousFirst)->flags.has(MessageLayoutFlag::AlternateBackground))
    {
        messageRef->flags.set(MessageLayoutFlag::AlternateBackground);
    }
    if (this->channel_->shouldIgnoreHighlights())
    {
        messageRef->flags.set(MessageLayoutFlag::IgnoreHighlights);
    }

    const auto previousSize = this->messages_.size();
    const auto wasAtBottom = this->isLogicallyAtBottom();
    const auto wasAtTop = this->isLogicallyAtTop();
    MessageLayoutPtr removedLayout;
    const bool removed = this->messages_.pushFront(messageRef, removedLayout);
    assert(removed == (evicted != nullptr));
    if (removed)
    {
        this->lastMessageHasAlternateBackground_ =
            !this->lastMessageHasAlternateBackground_;
    }

    if (this->paused())
    {
        if (!removed)
        {
            ++this->pauseScrollMaximumOffset_;
        }
        ++this->pauseScrollValueOffset_;
        ++this->pauseSelectionIndexOffset_;
    }
    else if (!removed)
    {
        this->scrollBar_->offsetMaximum(1);
    }
    if (!this->paused() && wasAtTop && this->followTop_)
    {
        this->scrollBar_->scrollToTop(false);
    }
    else if (!this->paused() && wasAtBottom)
    {
        this->scrollBar_->scrollToBottom(false);
    }
    else if (!this->paused())
    {
        this->scrollBar_->offset(1);
    }

    const auto shiftSelectionForward = [previousSize,
                                        removed](Selection &selection) {
        if (removed && previousSize != 0 &&
            selection.selectionMax.messageIndex >= previousSize - 1)
        {
            selection = {};
            return;
        }
        ++selection.start.messageIndex;
        ++selection.end.messageIndex;
        ++selection.selectionMin.messageIndex;
        ++selection.selectionMax.messageIndex;
    };
    if (!this->paused() && previousSize != 0)
    {
        shiftSelectionForward(this->selection_);
        shiftSelectionForward(this->doubleClickSelection_);
    }

    if (this->showScrollbarHighlights())
    {
        this->scrollBar_->prependHighlight(scrollbarHighlightForMessage(
            message, this->nukePreviewMessageIds_));
    }

    if (evicted && evicted->autoModReview &&
        evicted->autoModReview->key == this->autoModReviewSelectedKey_)
    {
        this->setAutoModReviewSelectedKey({});
    }

    if (!message->flags.has(MessageFlag::DoNotTriggerNotification) &&
        !message->isHiddenByClientNonce())
    {
        if (getSettings()->enableAutomodHighlight)
        {
            this->tabHighlightRequested.invoke(
                {.state = HighlightState::Highlighted,
                 .color = message->highlightColor});
        }
        else
        {
            this->tabHighlightRequested.invoke(
                {.state = HighlightState::NewMessage});
        }
    }

    this->queueLayout();
    if (!this->isVisible())
    {
        this->releaseHiddenMessageLayouts();
    }
}

void ChannelView::messageAddedAtStart(std::vector<MessagePtr> &messages)
{
    if (!this->messageLayoutsMaterialized_)
    {
        const auto retained = this->channel_->countMessages();
        const auto added = messages.size();
        const auto previousSize = retained >= added ? retained - added : 0;
        const bool newLastAlternate =
            (added % 2) == 0 ? this->lastMessageHasAlternateBackgroundReverse_
                             : !this->lastMessageHasAlternateBackgroundReverse_;

        if (previousSize == 0)
        {
            if (newLastAlternate == this->lastMessageHasAlternateBackground_)
            {
                this->virtualizedAlternateBreaksFromBottom_.push_back(0);
            }
        }
        else
        {
            const auto previousRows = restoredAlternateRows(
                previousSize, this->lastMessageHasAlternateBackground_,
                this->virtualizedAlternateBreaksFromBottom_);
            if (newLastAlternate == previousRows.front())
            {
                this->virtualizedAlternateBreaksFromBottom_.push_back(
                    previousSize);
            }
        }
        if ((messages.size() % 2) != 0)
        {
            this->lastMessageHasAlternateBackgroundReverse_ =
                !this->lastMessageHasAlternateBackgroundReverse_;
        }
        if (!this->virtualizedAtBottom_ && !messages.empty())
        {
            this->virtualizedAtTop_ = false;
        }
        this->queueLayout();
        return;
    }

    std::vector<MessageLayoutPtr> messageRefs;
    messageRefs.resize(messages.size());

    /// Create message layouts
    for (size_t i = 0; i < messages.size(); i++)
    {
        auto message = messages.at(i);
        auto layout = std::make_shared<MessageLayout>(message);

        // alternate color
        if (!this->lastMessageHasAlternateBackgroundReverse_)
        {
            layout->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        this->lastMessageHasAlternateBackgroundReverse_ =
            !this->lastMessageHasAlternateBackgroundReverse_;

        messageRefs.at(i) = std::move(layout);
    }

    /// Add the messages at the start
    auto addedMessages = this->messages_.pushFront(messageRefs);
    if (!addedMessages.empty())
    {
        const auto shiftSelection = [&addedMessages](Selection &selection) {
            if (selection.isEmpty())
            {
                return;
            }
            const auto offset = addedMessages.size();
            selection.start.messageIndex += offset;
            selection.end.messageIndex += offset;
            selection.selectionMin.messageIndex += offset;
            selection.selectionMax.messageIndex += offset;
        };
        if (this->paused())
        {
            const auto added = static_cast<int>(addedMessages.size());
            this->pauseScrollMaximumOffset_ += added;
            this->pauseScrollValueOffset_ += added;
            this->pauseSelectionIndexOffset_ += added;
        }
        else
        {
            shiftSelection(this->selection_);
            shiftSelection(this->doubleClickSelection_);
            if (this->scrollBar_->isAtBottom())
            {
                this->scrollBar_->scrollToBottom();
            }
            else
            {
                this->scrollBar_->offset(qreal(addedMessages.size()));
            }
        }
        if (!this->paused())
        {
            this->scrollBar_->offsetMaximum(qreal(addedMessages.size()));
        }
    }

    if (this->showScrollbarHighlights())
    {
        std::vector<ScrollbarHighlight> highlights;
        highlights.reserve(messages.size());
        for (const auto &message : messages)
        {
            highlights.push_back(scrollbarHighlightForMessage(
                message, this->nukePreviewMessageIds_));
        }

        this->scrollBar_->addHighlightsAtStart(highlights);
    }

    this->queueLayout();
}

void ChannelView::messageReplaced(size_t hint, const MessagePtr &prev,
                                  const MessagePtr &replacement)
{
    if (!this->messageLayoutsMaterialized_)
    {
        if (this->virtualizedScrollAnchor_ == prev)
        {
            this->virtualizedScrollAnchor_ = replacement;
        }
        if (this->virtualizedLastReadMessage_ == prev)
        {
            this->virtualizedLastReadMessage_ = replacement;
        }
        for (auto &expanded : this->virtualizedExpandedMessages_)
        {
            if (expanded == prev)
            {
                expanded = replacement;
            }
        }
        this->queueLayout();
        return;
    }

    auto optItem = this->messages_.find(hint, [&](const auto &it) {
        return it->getMessagePtr() == prev;
    });
    if (!optItem)
    {
        return;
    }
    const auto &[index, oldItem] = *optItem;

    auto newItem = std::make_shared<MessageLayout>(replacement);

    if (oldItem->flags.has(MessageLayoutFlag::AlternateBackground))
    {
        newItem->flags.set(MessageLayoutFlag::AlternateBackground);
    }
    if (oldItem->flags.has(MessageLayoutFlag::Expanded))
    {
        newItem->flags.set(MessageLayoutFlag::Expanded);
    }
    if (oldItem->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        newItem->flags.set(MessageLayoutFlag::IgnoreHighlights);
    }

    const bool replacesLastRead = this->lastReadMessage_ == oldItem;
    const bool replacesPendingLastRead = this->paused() &&
                                         this->pauseLastReadNeedsRemap_ &&
                                         this->pauseLastReadTarget_ == prev;
    const bool replacesScrollHighlight =
        this->highlightedMessage_ == oldItem.get();

    this->scrollBar_->replaceHighlight(
        index,
        scrollbarHighlightForMessage(replacement, this->nukePreviewMessageIds_));

    this->messages_.replaceItem(index, newItem);
    if (replacesLastRead || replacesPendingLastRead)
    {
        if (this->paused())
        {
            this->pauseLastReadNeedsRemap_ = true;
            this->pauseLastReadTarget_ = replacement;
        }
        else
        {
            this->lastReadMessage_ = newItem;
        }
    }
    if (replacesScrollHighlight && !this->paused())
    {
        this->highlightedMessage_ = newItem.get();
    }
    this->queueLayout();
}

void ChannelView::messagesUpdated()
{
    auto snapshot = this->channel_->getMessageSnapshot();

    if (!this->messageLayoutsMaterialized_)
    {
        this->virtualizedAlternateBreaksFromBottom_.clear();
        this->lastMessageHasAlternateBackground_ = (snapshot.size() % 2) != 0;
        this->lastMessageHasAlternateBackgroundReverse_ = true;
        if (!this->virtualizedAtBottom_)
        {
            if (const auto anchorIndex = equivalentMessageIndex(
                    snapshot, this->virtualizedScrollAnchor_))
            {
                this->virtualizedDistanceFromBottom_ =
                    snapshot.size() - *anchorIndex;
            }
            else
            {
                this->virtualizedScrollAnchor_.reset();
                this->virtualizedScrollFraction_ = 0;
                this->virtualizedDistanceFromBottom_ = snapshot.size();
            }
        }
        this->queueLayout();
        return;
    }

    const bool preservePausedSnapshot = this->paused();
    const auto previousLayouts = this->messages_.getSnapshot();
    const bool followedEdge = this->isAtFollowedEdge();
    const auto savedSelection =
        preservePausedSnapshot
            ? SavedSelectionIdentity{}
            : saveSelectionIdentity(this->selection_, previousLayouts);
    const auto savedDoubleClickSelection =
        preservePausedSnapshot
            ? SavedSelectionIdentity{}
            : saveSelectionIdentity(this->doubleClickSelection_,
                                    previousLayouts);
    const auto lastReadIdentity = this->lastReadMessage_ != nullptr
                                      ? this->lastReadMessage_->getMessagePtr()
                                      : MessagePtr{};
    const auto lastReadRemapTarget = this->pauseLastReadTarget_ != nullptr
                                         ? this->pauseLastReadTarget_
                                         : lastReadIdentity;
    const auto wasAtBottom =
        preservePausedSnapshot && this->isLogicallyAtBottom();
    const auto wasAtTop = preservePausedSnapshot && this->isLogicallyAtTop();
    const auto frozenRelativePosition =
        this->scrollBar_->getDesiredValue() - this->scrollBar_->getMinimum();

    QSet<const Message *> expandedMessages;
    expandedMessages.reserve(static_cast<qsizetype>(this->messages_.size()));
    for (const auto &layout : previousLayouts)
    {
        if (layout->flags.has(MessageLayoutFlag::Expanded))
        {
            expandedMessages.insert(layout->getMessagePtr().get());
        }
    }

    this->messages_.clear();
    this->scrollBar_->clearHighlights();
    if (!preservePausedSnapshot)
    {
        this->scrollBar_->resetBounds();
        this->scrollBar_->setMaximum(qreal(snapshot.size()));
        this->scrollBar_->setMinimum(0);
    }
    this->lastMessageHasAlternateBackground_ = false;
    this->lastMessageHasAlternateBackgroundReverse_ = true;

    for (const auto &msg : snapshot)
    {
        auto messageLayout = std::make_shared<MessageLayout>(msg);

        if (this->lastMessageHasAlternateBackground_)
        {
            messageLayout->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        this->lastMessageHasAlternateBackground_ =
            !this->lastMessageHasAlternateBackground_;

        if (this->channel_->shouldIgnoreHighlights())
        {
            messageLayout->flags.set(MessageLayoutFlag::IgnoreHighlights);
        }
        if (expandedMessages.contains(msg.get()))
        {
            messageLayout->flags.set(MessageLayoutFlag::Expanded);
        }

        this->messages_.pushBack(messageLayout);
        if (this->showScrollbarHighlights())
        {
            this->scrollBar_->addHighlight(scrollbarHighlightForMessage(
                msg, this->nukePreviewMessageIds_));
        }
    }

    if (preservePausedSnapshot)
    {
        const auto frozenSize = this->snapshot_.size();
        const auto currentSize = this->messages_.size();
        this->scrollBar_->setMinimum(0);
        this->scrollBar_->setMaximum(static_cast<qreal>(frozenSize));
        this->pauseScrollMinimumOffset_ = 0;
        this->pauseScrollMaximumOffset_ =
            static_cast<int>(currentSize) - static_cast<int>(frozenSize);
        this->pauseScrollValueOffset_ = 0;
        this->pauseSelectionIndexOffset_ = 0;
        this->pauseSelectionNeedsRemap_ = true;

        this->measureScrollbarForScrollRestore();
        if (this->snapshot_.empty())
        {
            this->scrollBar_->scrollToTop(false);
        }
        else if (wasAtBottom)
        {
            this->scrollBar_->scrollToBottom(false);
        }
        else if (wasAtTop)
        {
            this->scrollBar_->scrollToTop(false);
        }
        else
        {
            this->scrollBar_->setDesiredValue(frozenRelativePosition, false);
        }
        this->queueLayout();
        if (!wasAtBottom && !this->snapshot_.empty())
        {
            const auto relative = std::clamp(
                this->scrollBar_->getDesiredValue() -
                    this->scrollBar_->getMinimum(),
                qreal(0), static_cast<qreal>(this->snapshot_.size() - 1));
            const auto frozenIndex = static_cast<size_t>(std::floor(relative));
            const auto currentLayouts = this->messages_.getSnapshot();
            if (const auto retained = nearestRetainedMessage(
                    this->snapshot_, currentLayouts, frozenIndex))
            {
                const auto exactAnchor = retained->first == frozenIndex;
                const auto target =
                    static_cast<qreal>(retained->second) +
                    (exactAnchor ? relative - std::floor(relative) : qreal(0));
                this->pauseScrollValueOffset_ = target - relative;
            }
        }
        if (lastReadIdentity != nullptr)
        {
            this->pauseLastReadNeedsRemap_ = true;
            this->pauseLastReadTarget_ = lastReadRemapTarget;
        }
        return;
    }

    this->lastReadMessage_.reset();
    const auto currentLayouts = this->messages_.getSnapshot();
    if (const auto index =
            equivalentMessageIndex(currentLayouts, lastReadIdentity))
    {
        this->lastReadMessage_ = currentLayouts[*index];
    }
    restoreSelectionIdentity(savedSelection, currentLayouts, this->selection_);
    restoreSelectionIdentity(savedDoubleClickSelection, currentLayouts,
                             this->doubleClickSelection_);

    this->measureScrollbarForScrollRestore();
    if (followedEdge)
    {
        this->scrollToFollowedEdge(false);
    }
    else if (!previousLayouts.empty())
    {
        const auto relative =
            std::clamp(frozenRelativePosition, qreal(0),
                       static_cast<qreal>(previousLayouts.size() - 1));
        const auto index = static_cast<size_t>(std::floor(relative));
        const auto retained =
            nearestRetainedMessage(previousLayouts, currentLayouts, index);
        const auto position = retained
                                  ? static_cast<qreal>(retained->second) +
                                        (retained->first == index
                                             ? relative - std::floor(relative)
                                             : qreal(0))
                                  : qreal(0);
        this->scrollBar_->setDesiredValue(position, false);
    }

    this->queueLayout();
}

void ChannelView::updateLastReadMessage()
{
    if (!this->messageLayoutsMaterialized_)
    {
        const auto messages = this->channel_->getMessageSnapshot();
        this->virtualizedLastReadMessage_ =
            messages.empty() ? MessagePtr{} : messages.back();
        this->update();
        return;
    }

    if (auto lastMessage = this->messages_.last())
    {
        this->lastReadMessage_ = *lastMessage;
    }

    this->update();
}

void ChannelView::resizeEvent(QResizeEvent * /*event*/)
{
    this->updateScrollWidgetGeometries();

    this->scrollBar_->raise();

    this->queueLayout();

    this->update();
}

void ChannelView::refreshWallpaper()
{
    if (!this->wallpaperCacheKey_.isEmpty())
    {
        return;
    }
    const auto &profile = getTheme()->customization;
    const auto key = wallpaperKey(profile);
    this->wallpaperCacheKey_ = key;
    if (isThemeVideo(profile.wallpaperSource) && profile.wallpaperOpacity > 0 &&
        this->isVisible())
    {
        this->wallpaperVideo_ =
            ThemeVideo::acquire(profile.wallpaperSource, this);
        this->wallpaperVideoConnection_ =
            QObject::connect(this->wallpaperVideo_.get(),
                             &ThemeVideo::frameChanged, this, [this] {
                                 this->update();
                             });
        return;
    }
    this->wallpaperPixmap_ = loadWallpaper(profile);
}

void ChannelView::releaseVideoWallpaper()
{
    QObject::disconnect(this->wallpaperVideoConnection_);
    if (this->wallpaperVideo_)
    {
        this->wallpaperVideo_->detach(this);
    }
    this->wallpaperVideo_.reset();
}

bool ChannelView::ensureChatFramePaths()
{
    const auto &profile = getTheme()->customization;
    if (this->isOverlay_ || this->transparentBackground_ ||
        !profile.roundChat || profile.cornerRadius() <= 0)
    {
        return false;
    }

    const qreal radius = profile.cornerRadius() * this->scale();
    const qreal width =
        profile.chatBorder && profile.chatBorderOpacity > 0
            ? std::clamp(profile.chatBorderWidth, 1, 4) * this->scale()
            : 0.0;
    const auto frameRect = QRectF(this->rect());
    if (!this->chatFrameContentPath_ || !this->chatFrameOutsidePath_ ||
        !this->chatFrameBorderPath_ ||
        this->chatFramePathSize_ != this->size() ||
        !qFuzzyCompare(this->chatFramePathRadius_ + 1, radius + 1) ||
        !qFuzzyCompare(this->chatFramePathWidth_ + 1, width + 1))
    {
        this->chatFramePathSize_ = this->size();
        this->chatFramePathRadius_ = radius;
        this->chatFramePathWidth_ = width;
        this->chatFrameContentPath_ = std::make_unique<QPainterPath>();
        this->chatFrameOutsidePath_ = std::make_unique<QPainterPath>();
        this->chatFrameBorderPath_ = std::make_unique<QPainterPath>();
        this->chatFrameContentPath_->addRoundedRect(frameRect, radius, radius);
        buildThemeChatFramePaths(frameRect, radius, width,
                                 *this->chatFrameOutsidePath_,
                                 *this->chatFrameBorderPath_);
    }

    return true;
}

void ChannelView::updateRoundedChildMasks()
{
    if (!this->scrollBar_)
    {
        return;
    }

    if (!this->ensureChatFramePaths())
    {
        this->scrollBar_->clearMask();
        if (this->goToBottom_)
        {
            this->goToBottom_->clearMask();
        }
        return;
    }

    const qreal inset = std::min(
        this->chatFramePathWidth_,
        std::max<qreal>(0.0, std::min(this->width(), this->height()) / 2.0));
    const auto innerRect =
        QRectF(this->rect()).adjusted(inset, inset, -inset, -inset);
    QPainterPath childClipPath;
    childClipPath.addRoundedRect(
        innerRect, std::max<qreal>(0.0, this->chatFramePathRadius_ - inset),
        std::max<qreal>(0.0, this->chatFramePathRadius_ - inset));

    const auto applyMask = [&childClipPath](QWidget *widget) {
        QTransform transform;
        transform.translate(-widget->x(), -widget->y());
        const auto localPath = transform.map(childClipPath);
        widget->setMask(QRegion(localPath.toFillPolygon().toPolygon()));
    };
    applyMask(this->scrollBar_);
    if (this->goToBottom_)
    {
        applyMask(this->goToBottom_);
    }
}

std::pair<QColor, QColor> ChannelView::chatFrameSurfaces() const
{
    if (this->context_ != Context::None)
    {
        return {getTheme()->window.background, getTheme()->window.background};
    }

    const auto &profile = getTheme()->customization;
    auto topSurface = getTheme()->splits.header.background;
    const bool hasBanner = this->split_ && this->split_->hasVisibleBanner();
    if (hasBanner)
    {
        const QColor bannerSurface(getSettings()->pinBannerBackgroundColor);
        if (bannerSurface.isValid())
        {
            topSurface = bannerSurface;
        }
    }
    if (!hasBanner &&
        profile.foundation == ThemeFoundation::ChatterinoClassic &&
        this->split_ && this->split_->hasFocus())
    {
        topSurface = getTheme()->splits.header.focusedBackground;
    }
    return {topSurface, getTheme()->splits.input.background};
}

void ChannelView::paintChatFrame(QPainter &painter)
{
    if (!this->ensureChatFramePaths())
    {
        return;
    }

    const auto &profile = getTheme()->customization;
    const auto surfaces = this->chatFrameSurfaces();
    paintThemeChatFrame(
        painter, QRectF(this->rect()), this->chatFramePathRadius_,
        this->chatFramePathWidth_, surfaces.first, surfaces.second,
        profile.chatBorderColor,
        profile.chatBorder ? profile.chatBorderOpacity : 0,
        this->chatFrameOutsidePath_.get(), this->chatFrameBorderPath_.get());
}

void ChannelView::paintWallpaper(QPainter &painter)
{
    const auto &profile = getTheme()->customization;
    painter.fillRect(this->rect(), profile.chatBackground);
    this->refreshWallpaper();
    if (!this->wallpaperVideo_ &&
        (!this->wallpaperPixmap_ || this->wallpaperPixmap_->isNull()))
    {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setOpacity(profile.wallpaperOpacity / 100.0);
    QRectF target(this->rect());
    if (getSettings()->continuousSplitBackground && this->split_ != nullptr &&
        this->context_ == Context::None && this->parentWidget() == this->split_)
    {
        if (auto *container =
                qobject_cast<SplitContainer *>(this->split_->parentWidget()))
        {
            painter.translate(-this->mapTo(container, QPoint()));
            target = container->rect();
        }
    }
    if (this->wallpaperVideo_)
    {
        this->wallpaperVideo_->paint(painter, target, profile);
    }
    else if (profile.wallpaperMode == ThemeWallpaperMode::Tile)
    {
        const auto zoom = std::clamp(profile.wallpaperZoom, 100, 300) / 100.0;
        painter.save();
        painter.scale(zoom, zoom);
        painter.drawTiledPixmap(
            QRectF(target.topLeft() / zoom, target.size() / zoom),
            *this->wallpaperPixmap_);
        painter.restore();
    }
    else
    {
        const auto layout = layoutThemeWallpaper(
            this->wallpaperPixmap_->size(), target, profile.wallpaperMode,
            profile.wallpaperFocalX, profile.wallpaperFocalY,
            profile.wallpaperZoom);
        painter.drawPixmap(layout.destination, *this->wallpaperPixmap_,
                           layout.source);
    }
    painter.restore();

    if (profile.wallpaperOpacity > 0 && profile.wallpaperOverlayOpacity > 0)
    {
        auto overlay = profile.wallpaperOverlayColor;
        overlay.setAlphaF(overlay.alphaF() * profile.wallpaperOverlayOpacity /
                          100.0);
        painter.fillRect(this->rect(), overlay);
    }
}

void ChannelView::setSelection(const Selection &newSelection)
{
    if (this->selection_ != newSelection)
    {
        this->selection_ = newSelection;
        this->selectionChanged.invoke();
        this->update();
    }
}

void ChannelView::setSelection(const SelectionItem &start,
                               const SelectionItem &end)
{
    this->setSelection({start, end});
}

MessageElementFlags ChannelView::getFlags() const
{
    auto *app = getApp();

    if (this->overrideFlags_)
    {
        return *this->overrideFlags_;
    }

    MessageElementFlags flags = app->getWindows()->getWordFlags();

    auto *split = this->findParentSplit();

    if (split != nullptr)
    {
        if (split->getModerationMode())
        {
            flags.set(MessageElementFlag::ModeratorTools);
        }
        if (getSettings()->enableRepeatedMessageDetector &&
            (!getSettings()->repeatedMessagesShowOnlyModerationMode ||
             split->getModerationMode()))
        {
            flags.set(MessageElementFlag::RepeatedMessageCounter);
        }
        if (this->underlyingChannel_ ==
                getApp()->getTwitch()->getMentionsChannel() ||
            this->underlyingChannel_ ==
                getApp()->getTwitch()->getLiveChannel() ||
            this->underlyingChannel_ ==
                getApp()->getTwitch()->getAutomodChannel())
        {
            flags.set(MessageElementFlag::ChannelName);
            flags.unset(MessageElementFlag::ChannelPointReward);
        }
    }

    if (this->context_ == Context::UserCard &&
        getSettings()->enableRepeatedMessageDetector &&
        getSettings()->repeatedMessagesShowInUsercards)
    {
        flags.set(MessageElementFlag::RepeatedMessageCounter);
    }

    if (getSettings()->hideMessageTimestampsWhenLive &&
        this->underlyingChannel_ != nullptr &&
        this->underlyingChannel_->isLive())
    {
        flags.unset(MessageElementFlag::Timestamp);
    }

    if (this->sourceChannel_ == getApp()->getTwitch()->getMentionsChannel() ||
        this->sourceChannel_ == getApp()->getTwitch()->getAutomodChannel())
    {
        flags.set(MessageElementFlag::ChannelName);
    }

    if (this->context_ == Context::ReplyThread ||
        getSettings()->hideReplyContext)
    {
        // Don't show inline replies within the ReplyThreadPopup
        // or if they're hidden
        flags.unset(MessageElementFlag::RepliedMessage);
    }

    if (!this->canReplyToMessages())
    {
        flags.unset(MessageElementFlag::ReplyButton);
    }

    return flags;
}

bool ChannelView::scrollToMessage(const MessagePtr &message)
{
    if (!this->mayContainMessage(message))
    {
        return false;
    }

    if (!this->messageLayoutsMaterialized_)
    {
        const auto messages = this->channel_->getMessageSnapshot();
        if (std::ranges::find(messages, message) == messages.end())
        {
            return false;
        }
    }

    auto &messagesSnapshot = this->getMessagesSnapshot();
    if (messagesSnapshot.size() == 0)
    {
        return false;
    }

    // TODO: Figure out if we can somehow binary-search here.
    //       Currently, a message only sometimes stores a QDateTime,
    //       but always a QTime (inaccurate on midnight).
    //
    // We're searching from the bottom since it's more likely for a user
    // wanting to go to a message that recently scrolled out of view.
    size_t messageIdx = messagesSnapshot.size() - 1;
    for (; messageIdx < SIZE_MAX; messageIdx--)
    {
        if (messagesSnapshot[messageIdx]->getMessagePtr() == message)
        {
            break;
        }
    }

    if (messageIdx == SIZE_MAX)
    {
        return false;
    }

    this->scrollToMessageLayout(messagesSnapshot[messageIdx].get(), messageIdx);
    if (this->split_)
    {
        getApp()->getWindows()->select(this->split_);
    }
    return true;
}

bool ChannelView::scrollToMessageId(const QString &messageId)
{
    if (!this->messageLayoutsMaterialized_)
    {
        const auto messages = this->channel_->getMessageSnapshot();
        if (std::ranges::none_of(messages, [&messageId](const auto &message) {
                return message != nullptr && message->id == messageId;
            }))
        {
            return false;
        }
    }

    auto &messagesSnapshot = this->getMessagesSnapshot();
    if (messagesSnapshot.size() == 0)
    {
        return false;
    }

    // We're searching from the bottom since it's more likely for a user
    // wanting to go to a message that recently scrolled out of view.
    size_t messageIdx = messagesSnapshot.size() - 1;
    for (; messageIdx < SIZE_MAX; messageIdx--)
    {
        if (messagesSnapshot[messageIdx]->getMessagePtr()->id == messageId)
        {
            break;
        }
    }

    if (messageIdx == SIZE_MAX)
    {
        return false;
    }

    this->scrollToMessageLayout(messagesSnapshot[messageIdx].get(), messageIdx);
    if (this->split_)
    {
        getApp()->getWindows()->select(this->split_);
    }
    return true;
}

bool ChannelView::containsMessage(const MessagePtr &message) const
{
    if (message == nullptr || this->channel_ == nullptr)
    {
        return false;
    }

    const auto snapshot = this->channel_->getMessageSnapshot();
    return std::ranges::find(snapshot, message) != snapshot.end();
}

bool ChannelView::canReplyToMessage(const MessagePtr &message) const
{
    if (message == nullptr || !this->canReplyToMessages())
    {
        return false;
    }

    const auto source = this->inferChannel(*message);
    if (!source ||
        (!source->isTwitchOrKickChannel() && !source->isYouTubeChannel()) ||
        source->getType() == Channel::Type::TwitchWhispers ||
        source->getType() == Channel::Type::TwitchLive)
    {
        return false;
    }

    if (message->platform == MessagePlatform::YouTube)
    {
        const auto author =
            YouTubeMessageBuilder::cachedAuthorForMessage(message->id);
        const auto handle = author ? visibleYouTubeName(author->handle)
                                   : visibleYouTubeName(message->loginName);
        if (handle.isEmpty())
        {
            return false;
        }
    }

    const auto status = message->isReplyable();
    return status == Message::ReplyStatus::Replyable ||
           status == Message::ReplyStatus::ReplyableWithThread;
}

bool ChannelView::replyToMessage(const MessagePtr &message)
{
    if (!this->canReplyToMessage(message) || !this->setInputReply(message))
    {
        return false;
    }

    getApp()->getWindows()->select(this->split_);
    return true;
}

void ChannelView::setNukePreviewMessageIds(QSet<QString> messageIds)
{
    if (this->nukePreviewMessageIds_ == messageIds)
    {
        return;
    }

    this->nukePreviewMessageIds_ = std::move(messageIds);
    this->refreshScrollbarHighlights();
    this->queueUpdate();
}

void ChannelView::clearNukePreview()
{
    if (this->nukePreviewMessageIds_.isEmpty())
    {
        return;
    }

    this->nukePreviewMessageIds_.clear();
    this->refreshScrollbarHighlights();
    this->queueUpdate();
}

void ChannelView::scrollToMessageLayout(MessageLayout *layout,
                                        size_t messageIdx)
{
    this->highlightedMessage_ = layout;
    this->highlightAnimation_.setCurrentTime(0);
    this->highlightAnimation_.start(QAbstractAnimation::KeepWhenStopped);

    if (this->showScrollBar_)
    {
        this->getScrollBar().setDesiredValue(this->scrollBar_->getMinimum() +
                                             qreal(messageIdx));
    }
}

void ChannelView::paintEvent(QPaintEvent *event)
{
    //    BenchmarkGuard benchmark("paint");

    QPainter painter(this);

    const bool rounded = this->ensureChatFramePaths();
    if (rounded)
    {
        painter.save();
        painter.setClipPath(*this->chatFrameContentPath_);
    }

    // Clip painting strictly to the widget's bounds to hide wrapped text when collapsed
    painter.setClipRect(this->rect(), Qt::IntersectClip);

    if (!this->transparentBackground_)
    {
        if (getTheme()->customization.hasWallpaper() && !this->isOverlay_)
        {
            this->paintWallpaper(painter);
        }
        else
        {
            painter.fillRect(this->rect(),
                             this->messageColors_.channelBackground);
        }
    }

    // draw messages
    this->drawMessages(painter, event->region());

    // draw paused sign
    if (this->paused())
    {
        auto baseSize = 20;
        auto scale = this->scale();
        auto indicatorSize = baseSize * scale;
        auto color = QColor(180, 180, 180, 255);
        auto brush = QBrush(color);

        const auto pausedY = indicatorSize / 4;
        const auto pausedX = 5 * scale;

        QFont font = painter.font();
        font.setPixelSize(indicatorSize);
        painter.setFont(font);

        const QString text = "Paused";
        const QFontMetrics metrics(font);
        const auto textWidth = metrics.horizontalAdvance(text);
        const auto textX = pausedX * 3 + 10 * scale;

        painter.fillRect(QRectF(0, 0, pausedX + textX + textWidth,
                                indicatorSize / 2 + indicatorSize),
                         QBrush(QColor(0, 0, 0, 200), Qt::SolidPattern));

        painter.fillRect(
            QRectF(pausedX, pausedY, indicatorSize / 4, indicatorSize), brush);
        painter.fillRect(
            QRectF(pausedX * 3, pausedY, indicatorSize / 4, indicatorSize),
            brush);

        painter.setPen(color);
        painter.drawText(QRectF(textX, pausedY, textWidth, indicatorSize),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    if (rounded)
    {
        painter.restore();
    }

    this->paintChatFrame(painter);
}

// if overlays is false then it draws the message, if true then it draws things
// such as the grey overlay when a message is disabled
void ChannelView::drawMessages(QPainter &painter, const QRegion &area)
{
    auto &messagesSnapshot = this->getMessagesSnapshot();

    const auto start = size_t(this->scrollBar_->getRelativeCurrentValue());

    if (start >= messagesSnapshot.size())
    {
        this->clearMessageCaches();
        return;
    }

    MessageLayout *end = nullptr;

    auto messagePreferences = this->messagePreferences_;
    if (this->overrideSeparateMessages_.has_value())
    {
        messagePreferences.separateMessages = *this->overrideSeparateMessages_;
    }
    const auto &appearance = getTheme()->customization;
    if (appearance.useThemeMessageRows)
    {
        messagePreferences.alternateMessages = appearance.alternateMessageRows;
    }

    MessagePaintContext ctx = {
        .painter = painter,
        .selection = this->selection_,
        .colorProvider = ColorProvider::instance(),
        .messageColors = this->messageColors_,
        .preferences = messagePreferences,

        .canvasWidth = this->width(),
        .isWindowFocused = this->window() == QApplication::activeWindow(),
        .isMentions = this->underlyingChannel_ ==
                      getApp()->getTwitch()->getMentionsChannel(),

        .y = this->verticalOffset_ -
             static_cast<int>(
                 messagesSnapshot[start]->getHeight() *
                 (fmod(this->scrollBar_->getRelativeCurrentValue(), 1))),
        .messageIndex = start,
        .isLastReadMessage = false,
        .isCollapsed = this->collapseMessages_,

        .paintMessageShadow =
            appearance.messageShadow && !this->hoverAnimateOnly_,
        .messageShadowEmotes = appearance.messageShadowEmotes,
        .messageShadowColor = appearance.messageShadowColor,
        .messageShadowOpacity = appearance.messageShadowOpacity,
        .messageShadowOffset = appearance.messageShadowOffset(),
        .messageShadowBlur = appearance.messageShadowBlur,
        .highlightOpacityAdjustment = appearance.highlightOpacityAdjustment,
        .hoveredElement = this->hoveredLayoutElement_,
        .hoverAnimateOnly = this->hoverAnimateOnly_,
    };
    bool showLastMessageIndicator = getSettings()->showLastMessageIndicator;

    QRegion animationRegion;
    QRegion selfTimedAnimationRegion;
    for (; ctx.messageIndex < messagesSnapshot.size(); ++ctx.messageIndex)
    {
        MessageLayout *layout = messagesSnapshot[ctx.messageIndex].get();

        if (showLastMessageIndicator)
        {
            ctx.isLastReadMessage = this->lastReadMessage_.get() == layout;
        }
        else
        {
            ctx.isLastReadMessage = false;
        }

        const QRect messageRect{0, ctx.y, layout->getWidth(),
                                layout->getHeight()};
        if (area.intersects(messageRect))
        {
            auto paintResult = layout->paint(ctx);
            const auto &message = layout->getMessagePtr();
            if (message != nullptr &&
                this->nukePreviewMessageIds_.contains(message->id))
            {
                const QRect previewRect{
                    0,
                    ctx.y,
                    layout->getWidth(),
                    layout->getHeight(),
                };
                painter.fillRect(previewRect, QColor(255, 70, 70, 38));
                painter.fillRect(
                    QRect{0, ctx.y, std::max(2, int(3 * this->scale())),
                          layout->getHeight()},
                    QColor(255, 70, 70, 145));
            }
            animationRegion += paintResult.animatedRegion;
            selfTimedAnimationRegion += paintResult.selfTimedAnimatedRegion;

            if (this->highlightedMessage_ == layout)
            {
                painter.fillRect(
                    QRect{
                        0,
                        ctx.y,
                        layout->getWidth(),
                        layout->getHeight(),
                    },
                    this->highlightAnimation_.currentValue().value<QColor>());
                if (this->highlightAnimation_.state() ==
                    QVariantAnimation::Stopped)
                {
                    this->highlightedMessage_ = nullptr;
                }
            }
        }

        ctx.y += layout->getHeight();

        end = layout;
        if (ctx.y > this->height())
        {
            break;
        }
    }

    this->animationRegion_ =
        (this->animationRegion_.subtracted(area) + animationRegion)
            .intersected(this->rect());
    this->selfTimedAnimationRegion_ =
        (this->selfTimedAnimationRegion_.subtracted(area) +
         selfTimedAnimationRegion)
            .intersected(this->rect());
#ifdef FOURTF
    if (!QRegion(this->rect()).subtracted(area).isEmpty())
    {
        // shows the updated area on partial repaints
        painter.setPen(Qt::red);
        const auto bounds = area.boundingRect();
        painter.drawRect(bounds.x(), bounds.y(), bounds.width() - 1,
                         bounds.height() - 1);
    }
#endif

    if (end == nullptr)
    {
        this->clearMessageCaches();
        return;
    }

    this->updateMessageCaches(messagesSnapshot, start, end);
}

MessageLayoutContext ChannelView::makeLayoutContext(const Message &message,
                                                    MessageElementFlags flags,
                                                    Channel *selectedChannel,
                                                    int width) const
{
    const bool isAutoModReviewChannel =
        this->underlyingChannel_ != nullptr &&
        this->underlyingChannel_->getType() == Channel::Type::TwitchAutomod;

    return {
        .messageColors = this->messageColors_,
        .flags = flags,
        .width = width,
        .scale = this->scale(),
        .imageScale = this->overrideImageScale_.value_or(
            this->scale() * static_cast<float>(this->devicePixelRatio())),
        .emoteScale = this->overrideEmoteScale_.value_or(this->scale()),
        .badgeScale = this->effectiveBadgeScale(),
        .centerBadges = this->centerBadges_,
        .selectedChannel = selectedChannel,
        .message = message,
        .autoModReviewExpanded =
            message.autoModReview != nullptr && isAutoModReviewChannel &&
            message.autoModReview->key == this->autoModReviewSelectedKey_,
        .autoModReviewChannel = isAutoModReviewChannel,
        .preferences = &this->messagePreferences_,
    };
}

void ChannelView::updateMessageCaches(
    const std::vector<MessageLayoutPtr> &messagesSnapshot, size_t start,
    MessageLayout *end)
{
    // remove messages that are on screen
    // the messages that are left at the end get their buffers reset
    for (size_t i = start; i < messagesSnapshot.size(); ++i)
    {
        auto it = this->messagesOnScreen_.find(messagesSnapshot[i]);
        if (it != this->messagesOnScreen_.end())
        {
            this->messagesOnScreen_.erase(it);
        }

        if (messagesSnapshot[i].get() == end)
        {
            break;
        }
    }

    this->clearMessageCaches();

    // add all messages on screen to the map
    for (size_t i = start; i < messagesSnapshot.size(); ++i)
    {
        const std::shared_ptr<MessageLayout> &layout = messagesSnapshot[i];

        this->messagesOnScreen_.insert(layout);

        if (layout.get() == end)
        {
            break;
        }
    }
}

void ChannelView::clearMessageCaches()
{
    // delete the message buffers that aren't on screen
    for (const auto &layout : this->messagesOnScreen_)
    {
        layout->deleteCache();
    }
    this->messagesOnScreen_.clear();
}

void ChannelView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0)
    {
        // Ignore any scrolls where no vertical scrolling has taken place
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier))
    {
        // Ignore any scrolls where ctrl is held down - it is used for zoom
        event->ignore();
        return;
    }

    if (this->scrollBar_->isVisible())
    {
        float mouseMultiplier = getSettings()->mouseScrollMultiplier;

        // This ensures snapshot won't be indexed out of bounds when scrolling really fast
        qreal desired = std::max<qreal>(0, this->scrollBar_->getDesiredValue());
        qreal delta = event->angleDelta().y() * qreal(1.5) * mouseMultiplier;

        auto &snapshot = this->getMessagesSnapshot();
        int snapshotLength = int(snapshot.size());
        int i = std::min<int>(int(desired - this->scrollBar_->getMinimum()),
                              snapshotLength - 1);

        auto flags = this->getFlags();
        auto [selectedChannel, mcFlags] = this->getMultiChannelInfo();
        flags = flags | mcFlags;

        if (delta > 0)
        {
            qreal scrollFactor = fmod(desired, 1);
            qreal currentScrollLeft = std::max<qreal>(
                0.01, int(scrollFactor * snapshot[i]->getHeight()));

            for (; i >= 0; i--)
            {
                if (delta < currentScrollLeft)
                {
                    desired -= scrollFactor * (delta / currentScrollLeft);
                    break;
                }
                else
                {
                    delta -= currentScrollLeft;
                    desired -= scrollFactor;
                }

                if (i == 0)
                {
                    desired = 0;
                }
                else
                {
                    snapshot[i - 1]->layoutForMeasurement(
                        this->makeLayoutContext(*snapshot[i - 1]->getMessage(),
                                                flags, selectedChannel,
                                                this->getLayoutWidth()));
                    scrollFactor = 1;
                    currentScrollLeft = snapshot[i - 1]->getHeight();
                }
            }
        }
        else
        {
            delta = -delta;
            qreal scrollFactor = 1 - fmod(desired, 1);
            qreal currentScrollLeft = std::max<qreal>(
                0.01, int(scrollFactor * snapshot[i]->getHeight()));

            for (; i < snapshotLength; i++)
            {
                if (delta < currentScrollLeft)
                {
                    desired +=
                        scrollFactor * (qreal(delta) / currentScrollLeft);
                    break;
                }
                else
                {
                    delta -= currentScrollLeft;
                    desired += scrollFactor;
                }

                if (i == snapshotLength - 1)
                {
                    desired = snapshot.size();
                }
                else
                {
                    snapshot[i + 1]->layoutForMeasurement(
                        this->makeLayoutContext(*snapshot[i + 1]->getMessage(),
                                                flags, selectedChannel,
                                                this->getLayoutWidth()));

                    scrollFactor = 1;
                    currentScrollLeft = snapshot[i + 1]->getHeight();
                }
            }
        }

        this->scrollBar_->setDesiredValue(desired, true);
    }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void ChannelView::enterEvent(QEnterEvent * /*event*/)
#else
void ChannelView::enterEvent(QEvent * /*event*/)
#endif
{
}

void ChannelView::leaveEvent(QEvent * /*event*/)
{
    this->hideTooltip();
    this->clearHoveredAnimation();

    this->unpause(PauseReason::Mouse);
}

bool ChannelView::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride &&
        this->handleAutoModReviewKey(static_cast<QKeyEvent *>(event)))
    {
        event->accept();
        return true;
    }
    if (this->animatedPicker_ && (event->type() == QEvent::WindowActivate ||
                                  event->type() == QEvent::WindowDeactivate))
    {
        this->update();
    }
    if (event->type() == QEvent::Gesture)
    {
        if (const auto *gestureEvent = dynamic_cast<QGestureEvent *>(event))
        {
            return this->gestureEvent(gestureEvent);
        }
    }

    return BaseWidget::event(event);
}

bool ChannelView::gestureEvent(const QGestureEvent *event)
{
    if (QGesture *pan = event->gesture(Qt::PanGesture))
    {
        if (const auto *gesture = dynamic_cast<QPanGesture *>(pan))
        {
            switch (gesture->state())
            {
                case Qt::GestureStarted: {
                    this->isPanning_ = true;
                    // Remove any selections and hide tooltip while panning
                    this->clearSelection();
                    this->hideTooltip();
                    if (this->isScrolling_)
                    {
                        this->disableScrolling();
                    }
                }
                break;

                case Qt::GestureUpdated: {
                    if (this->scrollBar_->isVisible())
                    {
                        this->scrollBar_->offset(-gesture->delta().y() * 0.1);
                    }
                }
                break;

                case Qt::GestureFinished:
                case Qt::GestureCanceled:
                default: {
                    this->clearSelection();
                    this->isPanning_ = false;
                }
                break;
            }

            return true;
        }
    }

    return false;
}

void ChannelView::mouseMoveEvent(QMouseEvent *event)
{
    if (this->isPanning_)
    {
        // Don't do any text selection, hovering, etc while panning
        return;
    }

    /// Pause on hover
    if (float pauseTime = getSettings()->pauseOnHoverDuration;
        pauseTime > 0.001F)
    {
        this->pause(PauseReason::Mouse,
                    static_cast<uint32_t>(pauseTime * 1000.F));
    }
    else if (pauseTime < -0.5F)
    {
        this->pause(PauseReason::Mouse);
    }

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    // no message under cursor
    if (!this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex))
    {
        this->clearHoveredAnimation();
        this->setCursor(Qt::ArrowCursor);
        this->hideTooltip();
        return;
    }

    if (this->isScrolling_)
    {
        this->currentMousePosition_ = event->globalPosition();
    }

    // check for word underneath cursor
    const MessageLayoutElement *hoverLayoutElement =
        layout->getElementAt(relativePos);

    if (this->hoverAnimateOnly_)
    {
        if (!isImageLayoutElement(hoverLayoutElement))
        {
            hoverLayoutElement = nullptr;
        }

        if (this->hoveredLayoutElement_ != hoverLayoutElement)
        {
            this->updateHoveredAnimationImage(hoverLayoutElement);
            if (auto hoveredLayout = this->hoveredMessageLayout_.lock())
            {
                hoveredLayout->invalidateBuffer();
            }
            if (!this->hoveredElementRect_.isEmpty())
            {
                this->update(this->hoveredElementRect_);
            }
            this->hoveredLayoutElement_ = hoverLayoutElement;
            if (hoverLayoutElement != nullptr)
            {
                this->hoveredMessageLayout_ = layout;
                layout->invalidateBuffer();
                const auto elemRect =
                    hoverLayoutElement->getRect().toAlignedRect();
                const auto messageY =
                    event->pos().y() - static_cast<int>(relativePos.y());
                this->hoveredElementRect_ = elemRect.translated(0, messageY);
                const auto hoverRegion =
                    hoverLayoutElement->hasAnimatedContent()
                        ? QRegion(this->hoveredElementRect_)
                              .intersected(this->rect())
                        : QRegion{};
                if (!this->animatedPicker_)
                {
                    if (hoverLayoutElement->usesOwnAnimationTimer())
                    {
                        this->animationRegion_ = {};
                        this->selfTimedAnimationRegion_ = hoverRegion;
                    }
                    else
                    {
                        this->animationRegion_ = hoverRegion;
                        this->selfTimedAnimationRegion_ = {};
                    }
                }
                this->update(this->hoveredElementRect_);
            }
            else
            {
                this->hoveredMessageLayout_.reset();
                this->hoveredElementRect_ = {};
                if (!this->animatedPicker_)
                {
                    this->animationRegion_ = {};
                    this->selfTimedAnimationRegion_ = {};
                }
            }
        }
    }

    // selecting single characters
    if (this->isLeftMouseDown_)
    {
        auto index = layout->getSelectionIndex(relativePos);
        this->setSelection(this->selection_.start,
                           SelectionItem(messageIndex, index));
    }

    // selecting whole words
    if (this->isDoubleClick_ && hoverLayoutElement)
    {
        auto [wordStart, wordEnd] =
            layout->getWordBounds(hoverLayoutElement, relativePos);
        auto hoveredWord = Selection{SelectionItem(messageIndex, wordStart),
                                     SelectionItem(messageIndex, wordEnd)};
        // combined selection spanning from initially selected word to hoveredWord
        auto selectUnion = this->doubleClickSelection_ | hoveredWord;

        this->setSelection(selectUnion);
    }

    // message under cursor is collapsed
    if (layout->flags.has(MessageLayoutFlag::Collapsed))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->hideTooltip();
        return;
    }

    if (hoverLayoutElement == nullptr)
    {
        this->setCursor(Qt::ArrowCursor);
        this->hideTooltip();
        return;
    }

    auto *element = &hoverLayoutElement->getCreator();
    bool isLinkValid = hoverLayoutElement->getLink().isValid();
    const auto *emoteElement = dynamic_cast<const EmoteElement *>(element);
    const auto *layeredEmoteElement =
        dynamic_cast<const LayeredEmoteElement *>(element);
    bool isNotEmote = emoteElement == nullptr && layeredEmoteElement == nullptr;

    QString tooltip = hoverLayoutElement->getFragmentTooltip(relativePos);
    const bool isFragmentTooltip = !tooltip.isEmpty();
    if (tooltip.isEmpty())
    {
        tooltip = element->getTooltip();
    }
    bool isPaintTooltip = false;
    if (tooltip.isEmpty())
    {
        if (const auto *textElement =
                dynamic_cast<const TextLayoutElement *>(hoverLayoutElement))
        {
            tooltip = textElement->getPaintTooltip();
            isPaintTooltip = !tooltip.isEmpty();
        }
    }

    if (tooltip.isEmpty() ||
        (!isFragmentTooltip && !isPaintTooltip && isLinkValid && isNotEmote &&
         !getSettings()->linkInfoTooltip))
    {
        this->hideTooltip();
    }
    else
    {
        auto *tooltipWidget = this->ensureTooltipWidget();
        const auto *badgeElement = dynamic_cast<const BadgeElement *>(element);

        if (isFragmentTooltip)
        {
            this->clearPendingLinkInfo();
            tooltipWidget->setOne(TooltipEntry{
                .image = nullptr,
                .text = tooltip,
            });
        }
        else if (badgeElement || emoteElement || layeredEmoteElement)
        {
            this->clearPendingLinkInfo();
            auto showThumbnailSetting =
                getSettings()->emotesTooltipPreview.getEnum();

            bool showThumbnail =
                showThumbnailSetting == ThumbnailPreviewMode::AlwaysShow ||
                (showThumbnailSetting == ThumbnailPreviewMode::ShowOnShift &&
                 event->modifiers() == Qt::ShiftModifier);

            if (this->hoverAnimateOnly_)
            {
                showThumbnail = false;
            }

            if (emoteElement)
            {
                const auto &emote = emoteElement->getEmote();
                if (emote->modifierPlacement != EmoteModifierPlacement::None &&
                    !getSettings()->isEmoteModifierEnabled(emote->name.string))
                {
                    tooltip += "<br>Effect disabled";
                }
                auto scale = getSettings()->emoteTooltipScale.getEnum();
                const auto image = showThumbnail
                                       ? emoteElement->getImageForTooltip()
                                       : nullptr;
                auto imageScale = getTooltipScale(scale);
                if (image && emoteElement->isTwitchGif())
                {
                    const auto physicalSize = image->size() / image->scale();
                    const auto longestSide =
                        std::max(physicalSize.width(), physicalSize.height());
                    if (longestSide > 0)
                    {
                        imageScale *=
                            EmoteElement::TWITCH_GIF_LOGICAL_SIZE / longestSide;
                    }
                }
                tooltipWidget->setOne(
                    TooltipEntry::scaled(image, tooltip, imageScale));
            }
            else if (layeredEmoteElement)
            {
                const auto &layeredEmotes = layeredEmoteElement->getEmotes();
                // Should never be empty but ensure it
                if (!layeredEmotes.empty())
                {
                    std::vector<TooltipEntry> entries;
                    entries.reserve(std::min(layeredEmotes.size(),
                                             TOOLTIP_EMOTE_ENTRIES_LIMIT));

                    const auto &emoteTooltips =
                        layeredEmoteElement->getEmoteTooltips();
                    QStringList modifiers;
                    for (const auto &modifier :
                         layeredEmoteElement->getModifiers())
                    {
                        if (getSettings()->isEmoteModifierEnabled(
                                modifier->name.string))
                        {
                            modifiers.append(
                                modifier->name.string.toHtmlEscaped());
                        }
                    }
                    const auto modifierTooltip =
                        modifiers.isEmpty()
                            ? QString{}
                            : "<br>Modifiers: " + modifiers.join(", ");

                    // Someone performing some tomfoolery could put an emote with tens,
                    // if not hundreds of zero-width emotes on a single emote. If the
                    // tooltip may take up more than three rows, truncate everything else.
                    bool truncating = false;
                    size_t upperLimit = layeredEmotes.size();
                    if (layeredEmotes.size() > TOOLTIP_EMOTE_ENTRIES_LIMIT)
                    {
                        upperLimit = TOOLTIP_EMOTE_ENTRIES_LIMIT - 1;
                        truncating = true;
                    }

                    for (size_t i = 0; i < upperLimit; ++i)
                    {
                        const auto &emote = layeredEmotes[i].ptr;
                        if (i == 0)
                        {
                            // First entry gets a large image and full description
                            auto scale =
                                getSettings()->emoteTooltipScale.getEnum();
                            entries.push_back(TooltipEntry::scaled(
                                showThumbnail ? emote->images.getImage(3.0)
                                              : nullptr,
                                emoteTooltips[i] + modifierTooltip,
                                getTooltipScale(scale)));
                        }
                        else
                        {
                            // Every other entry gets a small image and just the emote name
                            auto scale =
                                getSettings()->emoteTooltipScale.getEnum();
                            entries.push_back(TooltipEntry::scaled(
                                showThumbnail ? emote->images.getImage(1.0)
                                              : nullptr,
                                emote->name.string, getTooltipScale(scale)));
                        }
                    }

                    if (truncating)
                    {
                        entries.push_back({nullptr, "..."});
                    }

                    auto style = layeredEmotes.size() > 2
                                     ? TooltipStyle::Grid
                                     : TooltipStyle::Vertical;
                    tooltipWidget->set(entries, style);
                }
            }
            else if (badgeElement)
            {
                auto scale = getSettings()->emoteTooltipScale.getEnum();
                const bool moltorino = badgeElement->getFlags().has(
                    MessageElementFlag::BadgeMoltorino);
                tooltipWidget->setOne(TooltipEntry::scaled(
                    showThumbnail
                        ? badgeElement->getEmote()->images.getImage(
                              3.0, moltorino ? ImageSet::ScaleMode::Exact
                                             : ImageSet::ScaleMode::Emote)
                        : nullptr,
                    tooltip, getTooltipScale(scale), 72));
            }
        }
        else if (auto *linkElement = dynamic_cast<LinkElement *>(element))
        {
            auto thumbnailSize = getSettings()->thumbnailSize;
            if (linkElement)
            {
                if (linkElement->linkInfo()->isPending())
                {
                    getApp()->getLinkResolver()->resolve(
                        linkElement->linkInfo());
                }
                this->setLinkInfoTooltip(linkElement->linkInfo());
            }
        }
        else
        {
            this->clearPendingLinkInfo();
            tooltipWidget->setOne(TooltipEntry{
                .image = nullptr,
                .text = tooltip,
            });
        }

        tooltipWidget->moveTo(
            event->globalPosition().toPoint() + QPoint(16, 16),
            widgets::BoundsChecking::CursorPosition);
        tooltipWidget->setWordWrap(isLinkValid && !isPaintTooltip &&
                                   !isFragmentTooltip);
        tooltipWidget->show();
    }

    // check if word has a link
    if (isLinkValid)
    {
        this->setCursor(Qt::PointingHandCursor);
    }
    else
    {
        this->setCursor(Qt::ArrowCursor);
    }
}

void ChannelView::mousePressEvent(QMouseEvent *event)
{
    this->mouseDown.invoke(event);

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    if (!this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex))
    {
        this->setCursor(Qt::ArrowCursor);
        auto &messagesSnapshot = this->getMessagesSnapshot();
        if (messagesSnapshot.size() == 0)
        {
            return;
        }

        // Start selection at the last message at its last index
        if (event->button() == Qt::LeftButton)
        {
            auto lastMessageIndex = messagesSnapshot.size() - 1;
            auto lastMessage = messagesSnapshot[lastMessageIndex];
            auto lastCharacterIndex = lastMessage->getLastCharacterIndex();

            SelectionItem selectionItem(lastMessageIndex, lastCharacterIndex);
            this->setSelection(selectionItem, selectionItem);
        }
        return;
    }

    const auto *pressedElement = layout->getElementAt(relativePos);
    const bool directAutoModAction =
        pressedElement != nullptr &&
        isAutoModReviewActionLink(pressedElement->getLink().type) &&
        !getSettings()->linksDoubleClickOnly;
    if (event->button() == Qt::LeftButton && !directAutoModAction &&
        this->underlyingChannel_ &&
        this->underlyingChannel_->getType() == Channel::Type::TwitchAutomod &&
        layout->getMessage()->autoModReview)
    {
        this->setAutoModReviewSelectedKey(
            layout->getMessage()->autoModReview->key);
        this->setFocus(Qt::MouseFocusReason);
    }

    // check if message is collapsed
    switch (event->button())
    {
        case Qt::LeftButton: {
            if (this->isScrolling_)
            {
                this->disableScrolling();
            }

            this->lastLeftPressPosition_ = event->globalPosition();
            this->isLeftMouseDown_ = true;

            if (layout->flags.has(MessageLayoutFlag::Collapsed))
            {
                return;
            }

            if (getSettings()->linksDoubleClickOnly.getValue())
            {
                this->pause(PauseReason::DoubleClick, 200);
            }

            int index = layout->getSelectionIndex(relativePos);
            auto selectionItem = SelectionItem(messageIndex, index);
            this->setSelection(selectionItem, selectionItem);
        }
        break;

        case Qt::RightButton: {
            if (this->isScrolling_)
            {
                this->disableScrolling();
            }

            this->lastRightPressPosition_ = event->globalPosition();
            this->isRightMouseDown_ = true;
        }
        break;

        case Qt::MiddleButton: {
            const MessageLayoutElement *hoverLayoutElement =
                layout->getElementAt(relativePos);

            if (hoverLayoutElement != nullptr &&
                hoverLayoutElement->getLink().isUrl() &&
                this->isScrolling_ == false)
            {
                break;
            }
            else
            {
                if (this->isScrolling_)
                {
                    this->disableScrolling();
                }
                else if (hoverLayoutElement != nullptr &&
                         hoverLayoutElement->getFlags().has(
                             MessageElementFlag::Username))
                {
                    break;
                }
                else if (this->scrollBar_->isVisible())
                {
                    this->enableScrolling(event->globalPosition());
                }
            }
        }
        break;

        default:;
    }

    this->update();
}

void ChannelView::mouseReleaseEvent(QMouseEvent *event)
{
    // find message
    this->queueLayout();

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    bool foundElement =
        this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex);

    // check if mouse was pressed
    if (event->button() == Qt::LeftButton)
    {
        if (this->isDoubleClick_)
        {
            this->isDoubleClick_ = false;

            if (!this->selection_.isEmpty())
            {
                copyToSelection(this->getSelectedText());
            }

            // Was actually not a wanted triple-click
            if (std::abs(distanceBetweenPoints(this->lastDoubleClickPosition_,
                                               event->globalPosition())) > 10.F)
            {
                this->clickTimer_.stop();
                return;
            }
        }
        else if (this->isLeftMouseDown_)
        {
            this->isLeftMouseDown_ = false;

            if (!this->selection_.isEmpty())
            {
                copyToSelection(this->getSelectedText());
            }

            if (std::abs(distanceBetweenPoints(this->lastLeftPressPosition_,
                                               event->globalPosition())) > 15.F)
            {
                return;
            }

            // Triple-clicking a message selects the whole message
            if (foundElement && this->clickTimer_.isActive() &&
                (std::abs(distanceBetweenPoints(this->lastDoubleClickPosition_,
                                                event->globalPosition())) <
                 10.F))
            {
                this->selectWholeMessage(layout.get(), messageIndex);
                if (!this->selection_.isEmpty())
                {
                    copyToSelection(this->getSelectedText());
                }
                return;
            }
        }
        else
        {
            return;
        }
    }
    else if (event->button() == Qt::RightButton)
    {
        if (this->isRightMouseDown_)
        {
            this->isRightMouseDown_ = false;

            if (std::abs(distanceBetweenPoints(this->lastRightPressPosition_,
                                               event->globalPosition())) > 15.F)
            {
                return;
            }
        }
        else
        {
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton)
    {
        if (this->isScrolling_ && this->scrollBar_->isVisible())
        {
            if (event->globalPosition() == this->lastMiddlePressPosition_)
            {
                this->enableScrolling(event->globalPosition());
            }
            else
            {
                this->disableScrolling();
            }

            return;
        }

        if (foundElement)
        {
            const MessageLayoutElement *hoverLayoutElement =
                layout->getElementAt(relativePos);

            if (hoverLayoutElement == nullptr)
            {
                return;
            }
            if (hoverLayoutElement->getFlags().has(
                    MessageElementFlag::Username))
            {
                const auto &usernameLink = hoverLayoutElement->getLink();
                if (layout->getMessage()->platform == MessagePlatform::TikTok)
                {
                    this->handleLinkClick(event, usernameLink, layout.get());
                    return;
                }
                if (layout->getMessage()->platform == MessagePlatform::YouTube)
                {
                    openYouTubeExternalUrl(
                        youtubeChannelUrl(layout->getMessage()->userID));
                    return;
                }

                const auto userName = usernameLink.value;
                const auto type = this->hasSourceChannel()
                                      ? this->sourceChannel_->getType()
                                      : this->channel_->getType();
                switch (type)
                {
                    case Channel::Type::TwitchWhispers:
                    case Channel::Type::TwitchLive:
                        QDesktopServices::openUrl(
                            QUrl(u"https://www.twitch.tv/" % userName));
                        break;
                    case Channel::Type::TwitchMentions:
                        openTwitchUsercard(layout->getMessage()->channelName,
                                           userName);
                        break;
                    default:
                        openTwitchUsercard(this->channel_->getName(), userName);
                        break;
                }

                return;
            }
            if (hoverLayoutElement->getLink().isUrl() == false)
            {
                return;
            }
        }
    }
    else
    {
        // not left or right button
        return;
    }

    // no message found
    if (!foundElement)
    {
        // No message at clicked position
        return;
    }

    // message under cursor is collapsed
    if (layout->flags.has(MessageLayoutFlag::Collapsed))
    {
        layout->flags.set(MessageLayoutFlag::Expanded);
        layout->flags.set(MessageLayoutFlag::RequiresLayout);

        this->queueLayout();
        return;
    }

    const MessageLayoutElement *hoverLayoutElement =
        layout->getElementAt(relativePos);

    // handle the click
    const QPointer<ChannelView> self(this);
    this->handleMouseClick(event, hoverLayoutElement, layout);

    if (self)
    {
        self->update();
    }
}

void ChannelView::handleMouseClick(QMouseEvent *event,
                                   const MessageLayoutElement *hoveredElement,
                                   MessageLayoutPtr layout)
{
    switch (event->button())
    {
        case Qt::LeftButton: {
            if (hoveredElement == nullptr)
            {
                return;
            }

            const auto link = hoveredElement->getLink();
            this->elementClicked.invoke(hoveredElement, event->modifiers());
            if (!getSettings()->linksDoubleClickOnly)
            {
                this->handleLinkClick(event, link, layout.get());
            }

            // Invoke to signal from EmotePopup.
            if (link.type == Link::InsertText)
            {
                this->linkClicked.invoke(link, event->modifiers());

                if (this->context_ == Context::None)
                {
                    auto *split = dynamic_cast<Split *>(this->parentWidget());
                    if (split)
                    {
                        split->insertTextToInput(link.value);
                    }
                }
            }
        }
        break;
        case Qt::RightButton: {
            // insert user mention to input, only in default context
            if ((this->context_ == Context::None) &&
                (hoveredElement != nullptr))
            {
                auto *split = dynamic_cast<Split *>(this->parentWidget());
                auto insertText = [=](QString text) {
                    if (split)
                    {
                        split->insertTextToInput(text);
                    }
                };
                const auto &link = hoveredElement->getLink();

                if (link.type == Link::UserInfo)
                {
                    // This is terrible because it FPs on messages where the
                    // user mentions themselves
                    bool canReply =
                        QString::compare(link.value,
                                         layout->getMessage()->loginName,
                                         Qt::CaseInsensitive) == 0;
                    UsernameRightClickBehavior action =
                        UsernameRightClickBehavior::Mention;
                    if (canReply)
                    {
                        Qt::KeyboardModifier userSpecifiedModifier =
                            getSettings()->usernameRightClickModifier;

                        if (userSpecifiedModifier ==
                            Qt::KeyboardModifier::NoModifier)
                        {
                            qCWarning(chatterinoCommon)
                                << "sanity check failed: "
                                   "invalid settings detected "
                                   "Settings::usernameRightClickModifier is "
                                   "NoModifier, which should never happen";
                            return;
                        }

                        Qt::KeyboardModifiers modifiers{userSpecifiedModifier};
                        auto isModifierHeld = event->modifiers() == modifiers;

                        if (isModifierHeld)
                        {
                            action = getSettings()
                                         ->usernameRightClickModifierBehavior;
                        }
                        else
                        {
                            action = getSettings()->usernameRightClickBehavior;
                        }
                    }
                    switch (action)
                    {
                        case UsernameRightClickBehavior::Mention: {
                            if (split == nullptr)
                            {
                                return;
                            }

                            if (link.value.startsWith("id:"))
                            {
                                return;
                            }

                            // Insert @username into split input
                            const bool commaMention =
                                getSettings()->mentionUsersWithComma;
                            const bool isFirstWord =
                                split->getInput().isEditFirstWord();
                            auto mentionTarget = link.value;
                            const auto &message = *layout->getMessage();
                            if (canReply &&
                                hoveredElement->getFlags().has(
                                    MessageElementFlag::Username) &&
                                !message.displayName.isEmpty() &&
                                message.displayName.compare(
                                    message.loginName, Qt::CaseInsensitive) ==
                                    0)
                            {
                                mentionTarget = message.displayName;
                            }
                            if (layout->getMessage()->platform ==
                                    MessagePlatform::YouTube &&
                                mentionTarget.startsWith(u'@'))
                            {
                                mentionTarget.remove(0, 1);
                            }
                            auto userMention = formatUserMention(
                                mentionTarget, isFirstWord, commaMention);
                            insertText("@" + userMention + " ");
                        }
                        break;

                        case UsernameRightClickBehavior::Reply: {
                            // Start a new reply if matching user's settings
                            this->setInputReply(layout->getMessagePtr());
                        }
                        break;

                        case UsernameRightClickBehavior::Ignore:
                            break;

                        case UsernameRightClickBehavior::ContextMenu:
                            this->addContextMenuItems(hoveredElement, layout,
                                                      event);
                            break;

                        default: {
                            qCWarning(chatterinoCommon)
                                << "unhandled or corrupted "
                                   "UsernameRightClickBehavior value in "
                                   "ChannelView::handleMouseClick:"
                                << action;
                        }
                        break;  // unreachable
                    }

                    return;
                }

                if (link.type == Link::UserWhisper)
                {
                    insertText("/w " + link.value + " ");
                    return;
                }
            }

            this->addContextMenuItems(hoveredElement, layout, event);
        }
        break;
        case Qt::MiddleButton: {
            if (hoveredElement == nullptr)
            {
                return;
            }

            const auto &link = hoveredElement->getLink();
            if (!getSettings()->linksDoubleClickOnly)
            {
                this->handleLinkClick(event, link, layout.get());
            }
        }
        break;
        default:;
    }
}

void ChannelView::addContextMenuItems(
    const MessageLayoutElement *hoveredElement, MessageLayoutPtr layout,
    QMouseEvent *event)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // Add image options if the element clicked contains an image (e.g. a badge or an emote)
    const auto channel = this->inferChannel(*layout->getMessage());
    const auto *twitchChannel =
        dynamic_cast<const TwitchChannel *>(channel.get());
    addImageContextMenuItems(menu, hoveredElement, twitchChannel);

    // Add link options if the element clicked contains a link
    addLinkContextMenuItems(menu, hoveredElement);

    // Add message options
    this->addMessageContextMenuItems(menu, layout);

    this->addUsernameContextMenuItems(menu, hoveredElement, layout);

    // Add Twitch-specific link options if the element clicked contains a link detected as a Twitch username
    this->addTwitchLinkContextMenuItems(menu, hoveredElement);

    // Add hidden options (e.g. copy message ID) if the user held down Shift
    addHiddenContextMenuItems(menu, hoveredElement, layout, event);

    // Add executable command options
    this->addCommandExecutionContextMenuItems(menu, hoveredElement, layout);

    this->messageMenuCreated.invoke(menu, hoveredElement);

#ifdef CHATTERINO_HAVE_PLUGINS
    auto *pluginSeparator = menu->addSeparator();
    const auto actionCount = menu->actions().size();
    getApp()->getWindows()->channelViewContextMenuRequested.invoke(
        *this, *layout, hoveredElement, *menu);
    if (menu->actions().size() == actionCount)
    {
        menu->removeAction(pluginSeparator);
        pluginSeparator->deleteLater();
    }
#endif

    menu->popup(QCursor::pos());
    menu->raise();
}

void ChannelView::addMessageContextMenuItems(QMenu *menu,
                                             const MessageLayoutPtr &layout)
{
    // Copy actions
    if (!this->selection_.isEmpty())
    {
        menu->addAction("&Copy selection", [this] {
            crossPlatformCopy(this->getSelectedText());
        });
    }

    QString messageCopyString;
    layout->addSelectionText(messageCopyString, 0, INT_MAX,
                             CopyMode::OnlyTextAndEmotes);
    menu->addAction("Copy &message",
                    [copyString = std::move(messageCopyString)] {
                        crossPlatformCopy(copyString);
                    });

    QString fullMessageCopyString;
    layout->addSelectionText(fullMessageCopyString, 0, INT_MAX,
                             CopyMode::EverythingButReplies);
    menu->addAction("Copy &full message",
                    [copyString = std::move(fullMessageCopyString)] {
                        crossPlatformCopy(copyString);
                    });

    auto contextMessage = layout->getMessagePtr();
    if (contextMessage->translatedFrom != nullptr)
    {
        menu->addAction("Show &original", [channel = this->underlyingChannel_,
                                           contextMessage] {
            if (channel != nullptr)
            {
                channel->replaceMessage(contextMessage,
                                        contextMessage->translatedFrom);
            }
        });
    }
    else
    {
        if (getSettings()->showTranslateMessageContextAction)
        {
            auto *translateAction = menu->addAction(
                "&Translate message",
                [this, contextMessage, channel = this->underlyingChannel_] {
                    translateMessageForChannel(channel, contextMessage, this,
                                               true, false);
                });
            translateAction->setEnabled(
                !messageTextForTranslation(contextMessage).isEmpty());
        }
    }

    // Only display reply option where it makes sense
    if (this->canReplyToMessages())
    {
        const auto setReply = [this, channel = this->underlyingChannel_](
                                  const MessagePtr &message) {
            if (this->underlyingChannel_ == channel)
            {
                this->setInputReply(message);
            }
        };
        const auto &messagePtr = layout->getMessagePtr();
        const auto replyStatus =
            !this->canReplyToMessage(messagePtr) && !messagePtr->replyThread
                ? Message::ReplyStatus::NotReplyable
                : messagePtr->isReplyable();
        switch (replyStatus)
        {
            case Message::ReplyStatus::Replyable: {
                menu->addAction("&Reply to message", [setReply, messagePtr] {
                    setReply(messagePtr);
                });
                break;
            }

            case Message::ReplyStatus::NotReplyable: {
                const auto &replyAction = menu->addAction(
                    "&Reply to message", [setReply, messagePtr] {
                        setReply(messagePtr);
                    });
                replyAction->setEnabled(false);
                break;
            }

            case Message::ReplyStatus::ReplyableWithThread: {
                menu->addAction("&Reply to message", [setReply, messagePtr] {
                    setReply(messagePtr);
                });
                menu->addAction("Reply to &original thread",
                                [setReply, messagePtr] {
                                    setReply(messagePtr->replyThread->root());
                                });
                break;
            }

            case Message::ReplyStatus::NotReplyableWithThread: {
                const auto &replyAction = menu->addAction(
                    "&Reply to message", [setReply, messagePtr] {
                        setReply(messagePtr);
                    });
                replyAction->setEnabled(false);

                menu->addAction("Reply to &original thread",
                                [setReply, messagePtr] {
                                    setReply(messagePtr->replyThread->root());
                                });
                break;
            }

            case Message::ReplyStatus::NotReplyableDueToThread: {
                const auto &replyAction = menu->addAction(
                    "&Reply to message", [setReply, messagePtr] {
                        setReply(messagePtr);
                    });
                replyAction->setEnabled(false);

                const auto &replyThreadAction = menu->addAction(
                    "Reply to &original thread", [setReply, messagePtr] {
                        setReply(messagePtr->replyThread->root());
                    });

                replyThreadAction->setEnabled(false);
                break;
            }
        }

        if (const auto threadMessagePtr = layout->getMessagePtr();
            threadMessagePtr->replyThread != nullptr)
        {
            menu->addAction("View &thread", [this, threadMessagePtr] {
                this->showReplyThreadPopup(threadMessagePtr);
            });
        }
    }
    else if (this->context_ == Context::Search)
    {
        const auto messagePtr = layout->getMessagePtr();
        auto *search = dynamic_cast<SearchPopup *>(this->parentWidget());
        auto *replyAction =
            menu->addAction("&Reply to message", [search, messagePtr] {
                if (search != nullptr)
                {
                    search->replyToMessage(messagePtr);
                }
            });
        replyAction->setEnabled(search != nullptr &&
                                search->canReplyToMessage(messagePtr));
    }

    const auto sourceMessage = layout->getMessagePtr();
    auto chan = this->inferChannel(*sourceMessage,
                                   InferChannel::SourceChannelIfAvailable);
    if (this->context_ == Context::Search)
    {
        if (auto *search = dynamic_cast<SearchPopup *>(this->parentWidget()))
        {
            chan = search->sourceChannelForMessage(sourceMessage);
        }
    }

    // Pin / Unpin action (outside Moderate submenu)
    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(chan.get()))
    {
        const bool canUseModerateMenu = twitchChannel->hasModRights();
        if (!layout->getMessage()->id.isEmpty() &&
            twitchChannel->canManagePinnedMessages() &&
            (!getSettings()->movePinToModerateMenu || !canUseModerateMenu))
        {
            auto id = layout->getMessage()->id;
            auto pinnedMessage = twitchChannel->accessPinnedMessage();
            if (pinnedMessage->has_value() && (*pinnedMessage)->messageId == id)
            {
                menu->addAction(
                    "Unpin message",
                    [chan, twitchChannel, id, pinId = (*pinnedMessage)->pinId] {
                        {
                            const auto current =
                                twitchChannel->accessPinnedMessage();
                            if (!current->has_value() ||
                                (*current)->messageId != id ||
                                (*current)->pinId != pinId)
                            {
                                return;
                            }
                        }
                        twitchChannel->unpinMessage();
                    });
            }
            else
            {
                menu->addAction("Pin message", [chan, twitchChannel, id] {
                    twitchChannel->pinMessage(
                        id, commands::normalizePinDuration(
                                getSettings()->defaultPinDuration));
                });
            }
        }
    }

    // Add search action when text is selected and search feature is enabled
    if (!this->selection_.isEmpty() && getSettings()->searchEnabled.getValue())
    {
        QString searchURL = getSettings()->searchEngineUrl.getValue();
        QString searchName = getSettings()->searchEngineName.getValue();

        if (!searchURL.isEmpty())
        {
            QString actionText =
                searchName.isEmpty() ? "&Search" : "&Search with " + searchName;

            if (getSettings()->searchIncognito && supportsIncognitoLinks())
            {
                actionText += " in private mode";
            }

            menu->addAction(actionText, [this, searchURL] {
                QString query = this->getSelectedText().trimmed();
                QString encodedQuery = QUrl::toPercentEncoding(query);
                QString url = searchURL + encodedQuery;

                if (getSettings()->searchIncognito && supportsIncognitoLinks())
                {
                    openLinkIncognito(url);
                }
                else
                {
                    QDesktopServices::openUrl(QUrl(url));
                }
            });
        }
    }

    const auto youtubeChannel = std::dynamic_pointer_cast<YouTubeChannel>(chan);
    YouTubeAuthor youtubeTarget;
    const auto messageID = layout->getMessage()->id;
    const auto targetUserID = layout->getMessage()->userID;
    if (const auto cached =
            YouTubeMessageBuilder::cachedAuthorForMessage(messageID))
    {
        youtubeTarget = *cached;
    }
    youtubeTarget.channelId = targetUserID;
    const bool canModerateYouTubeMessage =
        youtubeChannel != nullptr && youtubeChannel->canUseModerationTools() &&
        !layout->getMessage()->flags.has(MessageFlag::System) &&
        (!messageID.isEmpty() || !targetUserID.isEmpty());
    const bool canModerateTwitchOrKickMessage = youtubeChannel == nullptr &&
                                                !messageID.isEmpty() &&
                                                chan->hasModRights();

    if (canModerateTwitchOrKickMessage || canModerateYouTubeMessage)
    {
        menu->addSeparator();
        auto *moderateAction = menu->addAction("Mo&derate");
        auto *moderateMenu = new QMenu(menu);
        moderateAction->setMenu(moderateMenu);

        auto id = messageID;

        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            if (twitchChannel->canManagePinnedMessages() &&
                getSettings()->movePinToModerateMenu)
            {
                auto pinnedMessage = twitchChannel->accessPinnedMessage();
                if (pinnedMessage->has_value() &&
                    (*pinnedMessage)->messageId == id)
                {
                    moderateMenu->addAction(
                        "Unpin message", [chan, twitchChannel, id,
                                          pinId = (*pinnedMessage)->pinId] {
                            {
                                const auto current =
                                    twitchChannel->accessPinnedMessage();
                                if (!current->has_value() ||
                                    (*current)->messageId != id ||
                                    (*current)->pinId != pinId)
                                {
                                    return;
                                }
                            }
                            twitchChannel->unpinMessage();
                        });
                }
                else
                {
                    moderateMenu->addAction(
                        "Pin message", [chan, twitchChannel, id] {
                            twitchChannel->pinMessage(
                                id, commands::normalizePinDuration(
                                        getSettings()->defaultPinDuration));
                        });
                }
            }
        }

        auto *deleteAction = moderateMenu->addAction(
            "&Delete message", [chan, id] {
                auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
                if (twitchChannel)
                {
                    twitchChannel->deleteMessagesAs(
                        id, getApp()->getAccounts()->twitch.getCurrent().get());
                }
                else if (auto *kc = dynamic_cast<KickChannel *>(chan.get()))
                {
                    kc->deleteMessage(id);
                }
                else if (auto *youtube = dynamic_cast<YouTubeChannel *>(chan.get()))
                {
                    youtube->deleteMessage(id);
                }
            });
        deleteAction->setEnabled(!id.isEmpty());

        if (youtubeChannel != nullptr)
        {
            if (youtubeChannel->canModerateTarget(youtubeTarget))
            {
                moderateMenu->addSeparator();
                populateYouTubeModerationMenu(moderateMenu, youtubeChannel,
                                              youtubeTarget);
            }
        }
    }

    bool isSearch = this->context_ == Context::Search;
    bool isReplyOrUserCard = (this->context_ == Context::ReplyThread ||
                              this->context_ == Context::UserCard) &&
                             this->split_ != nullptr;
    bool isMentions =
        this->channel()->getType() == Channel::Type::TwitchMentions;
    bool isAutomod = this->channel()->getType() == Channel::Type::TwitchAutomod;
    if (isSearch || isMentions || isReplyOrUserCard || isAutomod)
    {
        const auto &messagePtr = layout->getMessagePtr();
        menu->addAction("&Go to message", [this, messagePtr, isSearch,
                                           isMentions, isReplyOrUserCard,
                                           isAutomod] {
            if (isSearch)
            {
                if (const auto &search =
                        dynamic_cast<SearchPopup *>(this->parentWidget()))
                {
                    search->goToMessage(messagePtr);
                }
            }
            else if (isMentions || isAutomod)
            {
                getApp()->getWindows()->scrollToMessage(messagePtr);
            }
            else if (isReplyOrUserCard)
            {
                // If the thread is in the mentions or automod channel,
                // we need to find the original split.
                const auto type = this->split_->getChannel()->getType();
                if (type == Channel::Type::TwitchMentions ||
                    type == Channel::Type::TwitchAutomod)
                {
                    getApp()->getWindows()->scrollToMessage(messagePtr);
                }
                else
                {
                    this->split_->getChannelView().scrollToMessage(messagePtr);
                }
            }
        });
    }
}

void ChannelView::translateMessage(const MessagePtr &message)
{
    translateMessageForChannel(this->underlyingChannel_, message, this, true,
                               false);
}

void ChannelView::maybeAutoTranslateMessage(const MessagePtr &message)
{
    if (this->context_ != Context::None || this->split_ == nullptr)
    {
        return;
    }

    auto channel = this->inferChannel(*message, InferChannel::UnderlyingOnly);
    if (!isAutoTranslatableChannel(channel) ||
        !getSettings()->isAutoTranslateChannel(channel->getName()) ||
        !isAutoTranslatableMessage(message))
    {
        return;
    }

    const auto channelKey = autoTranslationChannelKey(channel);
    auto &inFlightByChannel = autoTranslationInFlightByChannel();
    if (inFlightByChannel.value(channelKey) >=
        MAX_AUTO_TRANSLATIONS_IN_FLIGHT_PER_CHANNEL)
    {
        return;
    }

    const auto requestKey = autoTranslationRequestKey(channel, message);
    auto &inFlightRequests = autoTranslationInFlightRequests();
    if (inFlightRequests.contains(requestKey) ||
        inFlightRequests.size() >= MAX_AUTO_TRANSLATIONS_IN_FLIGHT)
    {
        return;
    }

    inFlightRequests.insert(requestKey);
    inFlightByChannel.insert(channelKey,
                             inFlightByChannel.value(channelKey) + 1);

    translateMessageForChannel(
        channel, message, nullptr, false, true, [channelKey, requestKey] {
            auto &requests = autoTranslationInFlightRequests();
            requests.remove(requestKey);

            auto &counts = autoTranslationInFlightByChannel();
            const auto remaining = counts.value(channelKey) - 1;
            if (remaining <= 0)
            {
                counts.remove(channelKey);
                return;
            }

            counts.insert(channelKey, remaining);
        });
}

void ChannelView::addTwitchLinkContextMenuItems(
    QMenu *menu, const MessageLayoutElement *hoveredElement)
{
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &link = hoveredElement->getLink();

    if (link.type != Link::Url)
    {
        return;
    }

    static QRegularExpression twitchChannelRegex(
        R"(^(?:https?:\/\/)?(?:www\.|go\.)?twitch\.tv\/(?:popout\/)?(?<username>[a-z0-9_]{3,}))",
        QRegularExpression::CaseInsensitiveOption);
    static QSet<QString> ignoredUsernames{
        "directory",      //
        "downloads",      //
        "drops",          //
        "friends",        //
        "inventory",      //
        "jobs",           //
        "login",          //
        "messages",       //
        "payments",       //
        "profile",        //
        "security",       //
        "settings",       //
        "signup",         //
        "subscriptions",  //
        "turbo",          //
        "videos",         //
        "wallet",         //
    };

    auto twitchMatch = twitchChannelRegex.match(link.value);
    auto twitchUsername = twitchMatch.captured("username");
    if (!twitchUsername.isEmpty() && !ignoredUsernames.contains(twitchUsername))
    {
        menu->addSeparator();
        menu->addAction("&Open in new split", [twitchUsername, this] {
            this->openChannelIn.invoke(twitchUsername,
                                       FromTwitchLinkOpenChannelIn::Split);
        });
        menu->addAction("Open in new &tab", [twitchUsername, this] {
            this->openChannelIn.invoke(twitchUsername,
                                       FromTwitchLinkOpenChannelIn::Tab);
        });

        menu->addSeparator();
        menu->addAction("Open player in &browser", [twitchUsername, this] {
            this->openChannelIn.invoke(
                twitchUsername, FromTwitchLinkOpenChannelIn::BrowserPlayer);
        });
        menu->addAction("Open in &streamlink", [twitchUsername, this] {
            this->openChannelIn.invoke(twitchUsername,
                                       FromTwitchLinkOpenChannelIn::Streamlink);
        });

        if (!getSettings()->customURIScheme.getValue().isEmpty())
        {
            menu->addAction("Open in custom &player", [twitchUsername, this] {
                this->openChannelIn.invoke(
                    twitchUsername, FromTwitchLinkOpenChannelIn::CustomPlayer);
            });
        }
    }
}

void ChannelView::addUsernameContextMenuItems(
    QMenu *menu, const MessageLayoutElement *hoveredElement,
    const MessageLayoutPtr &layout)
{
    if (hoveredElement == nullptr || layout == nullptr ||
        hoveredElement->getLink().type != Link::UserInfo ||
        layout->getMessage()->platform == MessagePlatform::Kick)
    {
        return;
    }

    if (layout->getMessage()->platform == MessagePlatform::TikTok)
    {
        const auto message = layout->getMessagePtr();
        const auto target = hoveredElement->getLink().value;
        const bool isAuthor = target == message->loginName ||
                              target == u"id:"_s + message->userID;
        menu->addSeparator();
        menu->addAction(
            "View TikTok &profile", [this, message, target, isAuthor] {
                if (isAuthor)
                {
                    this->showTikTokUserPopup(message);
                }
                else if (target.startsWith(u"id:") &&
                         isTikTokUserID(QStringView(target).sliced(3)))
                {
                    this->showTikTokUserPopup(target.sliced(3), {},
                                              message->channelName);
                }
                else
                {
                    this->showTikTokUserPopup({}, target, message->channelName);
                }
            });
        const auto handle =
            normalizeTikTokHandle(hoveredElement->getLink().value);
        if (handle)
        {
            menu->addAction("Open TikTok profile in &browser", [handle] {
                QDesktopServices::openUrl(tikTokProfileUrl(*handle));
            });
            menu->addAction("Copy TikTok &handle", [handle] {
                crossPlatformCopy(u'@' + *handle);
            });
        }
        if (isAuthor)
        {
            menu->addAction("Copy TikTok user &ID", [message] {
                crossPlatformCopy(message->userID);
            });
        }
        return;
    }

    if (layout->getMessage()->platform == MessagePlatform::YouTube)
    {
        const auto message = layout->getMessagePtr();
        const auto author =
            youtubeAuthorForUserLink(message, hoveredElement->getLink());
        const auto handle = visibleYouTubeName(author.handle);
        const auto channelUrl = youtubeChannelUrl(author.channelId);

        menu->addSeparator();
        menu->addAction("View YouTube &profile", [this, message, author] {
            this->showYouTubeUserPopup(message, author);
        });
        if (!channelUrl.isEmpty())
        {
            menu->addAction("Open YouTube channel in &browser", [channelUrl] {
                openYouTubeExternalUrl(channelUrl);
            });
            if (!handle.isEmpty())
            {
                menu->addAction("Copy YouTube &handle", [handle] {
                    crossPlatformCopy(u'@' + handle);
                });
            }
            menu->addAction("Copy YouTube channel &ID", [author] {
                crossPlatformCopy(author.channelId);
            });
        }
        return;
    }

    const auto username = hoveredElement->getLink().value.trimmed();
    if (username.isEmpty() || username.startsWith(QStringLiteral("id:")))
    {
        return;
    }

    const auto encodedUsername =
        QString::fromLatin1(QUrl::toPercentEncoding(username));
    menu->addSeparator();
    menu->addAction("Open stream in &browser", [encodedUsername] {
        QDesktopServices::openUrl(QUrl(
            QStringLiteral("https://www.twitch.tv/%1").arg(encodedUsername)));
    });
    menu->addAction("Open &moderator view in browser", [encodedUsername] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.twitch.tv/moderator/%1")
                     .arg(encodedUsername)));
    });
}

void ChannelView::addCommandExecutionContextMenuItems(
    QMenu *menu, const MessageLayoutElement *hoveredElement,
    const MessageLayoutPtr &layout)
{
    /* Get commands to be displayed in context menu;
     * only those that had the showInMsgContextMenu check box marked in the Commands page */
    std::vector<Command> cmds;
    for (const auto &cmd : getApp()->getCommands()->items)
    {
        if (cmd.showInMsgContextMenu)
        {
            cmds.push_back(cmd);
        }
    }

    if (cmds.empty())
    {
        return;
    }

    menu->addSeparator();
    auto *executeAction = menu->addAction("&Execute command");
    auto *cmdMenu = new QMenu(menu);
    executeAction->setMenu(cmdMenu);

    QString elementCopyText;
    if (hoveredElement != nullptr)
    {
        hoveredElement->addCopyTextToString(elementCopyText);
        elementCopyText = elementCopyText.trimmed();
    }

    for (auto &cmd : cmds)
    {
        QString inputText = this->selection_.isEmpty()
                                ? layout->getMessage()->messageText
                                : this->getSelectedText();

        inputText.push_front(cmd.name + " ");

        /* Search popups and user message history's underlyingChannels aren't of type TwitchChannel, but
         * we would still like to execute commands from them. Use their source channel instead if applicable. */
        const auto channel = this->inferChannel(*layout->getMessage());
        cmdMenu->addAction(cmd.name, [this, channel, layout, cmd, inputText,
                                      elementCopyText] {
            auto *split = dynamic_cast<Split *>(this->parentWidget());
            QString userText;
            if (split)
            {
                userText = split->getInput().getInputText();
            }

            // Execute command through right-clicking a message -> Execute command
            QString value = getApp()->getCommands()->execCustomCommand(
                inputText.split(' '), cmd, true, channel, layout->getMessage(),
                {
                    {"input.text", userText},
                    {"element.copytext", elementCopyText},
                });

            value = getApp()->getCommands()->execCommand(value, channel, false);

            channel->sendMessage(value);
        });
    }
}

void ChannelView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    if (!this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex))
    {
        return;
    }

    this->isDoubleClick_ = true;
    this->lastDoubleClickPosition_ = event->globalPosition();
    this->clickTimer_.start();

    // message under cursor is collapsed
    if (layout->flags.has(MessageLayoutFlag::Collapsed))
    {
        return;
    }

    const MessageLayoutElement *hoverLayoutElement =
        layout->getElementAt(relativePos);

    if (hoverLayoutElement == nullptr)
    {
        // XXX: this is duplicate work
        auto idx = layout->getSelectionIndex(relativePos);
        SelectionItem item(messageIndex, idx);
        this->doubleClickSelection_ = {item, item};
        return;
    }

    auto [wordStart, wordEnd] =
        layout->getWordBounds(hoverLayoutElement, relativePos);

    this->doubleClickSelection_ = {SelectionItem(messageIndex, wordStart),
                                   SelectionItem(messageIndex, wordEnd)};
    this->setSelection(this->doubleClickSelection_);

    if (getSettings()->linksDoubleClickOnly)
    {
        const auto &link = hoverLayoutElement->getLink();
        this->handleLinkClick(event, link, layout.get());
    }
}

void ChannelView::hideEvent(QHideEvent * /*event*/)
{
    this->wallpaperPixmap_.reset();
    this->releaseVideoWallpaper();
    this->wallpaperCacheKey_.clear();
    if (this->hoverAnimateOnly_)
    {
        this->releaseHoveredAnimation();
        if (auto layout = this->hoveredMessageLayout_.lock())
        {
            layout->invalidateBuffer();
        }

        this->hoveredLayoutElement_ = nullptr;
        this->hoveredMessageLayout_.reset();
        this->hoveredElementRect_ = {};
    }
    this->animationRegion_ = {};
    this->selfTimedAnimationRegion_ = {};

    this->unpause(PauseReason::Mouse);

    if (this->isScrolling_)
    {
        this->disableScrolling();
    }
    this->releaseTooltip();
    this->clearMessageCaches();
    this->releaseHiddenMessageLayouts();
    this->layoutQueued_ = true;
}

void ChannelView::showYouTubeUserPopup(const MessagePtr &message,
                                       const YouTubeAuthor &author)
{
    if (message == nullptr || message->platform != MessagePlatform::YouTube ||
        author.channelId.isEmpty())
    {
        return;
    }
    if (!this->split_)
    {
        return;
    }

    auto contextChannel =
        this->inferChannel(*message, InferChannel::SourceChannelIfAvailable);
    if (this->context_ == Context::Search)
    {
        if (auto *search = dynamic_cast<SearchPopup *>(this->parentWidget()))
        {
            contextChannel = search->sourceChannelForMessage(message);
        }
    }
    if (!std::dynamic_pointer_cast<YouTubeChannel>(contextChannel))
    {
        return;
    }

    auto *popup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, this->split_);
    const auto openingChannel = this->hasSourceChannel()
                                    ? this->sourceChannel_
                                    : this->selectedChannel();
    popup->setYouTubeData(message, author, contextChannel, openingChannel);
    QPoint offset(popup->width() / 3, popup->height() / 5);
    popup->moveTo(QCursor::pos() - offset,
                  widgets::BoundsChecking::CursorPosition);
    popup->show();
}

void ChannelView::showTikTokUserPopup(const MessagePtr &message)
{
    if (!this->split_ || !message ||
        message->platform != MessagePlatform::TikTok)
    {
        return;
    }
    auto contextChannel =
        this->inferChannel(*message, InferChannel::SourceChannelIfAvailable);
    if (this->context_ == Context::Search)
    {
        if (auto *search = dynamic_cast<SearchPopup *>(this->parentWidget()))
        {
            contextChannel = search->sourceChannelForMessage(message);
        }
    }
    auto channel = std::dynamic_pointer_cast<TikTokChannel>(contextChannel);
    if (!channel || channel->getName().compare(message->channelName,
                                               Qt::CaseInsensitive) != 0)
    {
        channel =
            getApp()->getTikTokChatServer()->findByHandle(message->channelName);
    }
    auto author =
        channel ? channel->author(message->userID).value_or(TikTokAuthor{})
                : TikTokAuthor{};
    author.id = message->userID;
    if (author.displayName.isEmpty())
    {
        author.displayName = message->displayName;
    }

    for (const auto &element : message->elements)
    {
        if (element->getFlags().has(MessageElementFlag::Username) &&
            element->getLink().type == Link::UserInfo)
        {
            if (author.handle.isEmpty())
            {
                author.handle = normalizeTikTokHandle(element->getLink().value)
                                    .value_or(QString{});
            }
            if (const auto *name =
                    dynamic_cast<const TikTokUsernameElement *>(element.get());
                name && author.avatarUrl.isEmpty())
            {
                author.avatarUrl = name->avatarUrl();
            }
            break;
        }
    }
    auto *popup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, this->split_);
    const auto openingChannel = this->hasSourceChannel()
                                    ? this->sourceChannel_
                                    : this->selectedChannel();
    popup->setTikTokData(author, channel, openingChannel);
    popup->moveTo(
        QCursor::pos() - QPoint(popup->width() / 3, popup->height() / 5),
        widgets::BoundsChecking::CursorPosition);
    popup->show();
}

void ChannelView::showUserInfoPopup(const QString &userName,
                                    MessagePlatform platform,
                                    QString alternativePopoutChannel)
{
    if (!this->split_)
    {
        qCWarning(chatterinoCommon)
            << "Tried to show user info for" << userName
            << "but the channel view doesn't belong to a split.";
        return;
    }

    if (platform == MessagePlatform::TikTok)
    {
        this->showTikTokUserPopup({}, userName, alternativePopoutChannel);
        return;
    }
    if (platform == MessagePlatform::YouTube)
    {
        return;
    }

    auto *userPopup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, this->split_);

    auto openingChannel = this->hasSourceChannel() ? this->sourceChannel_
                                                   : this->selectedChannel();
    ChannelPtr contextChannel;
    if (openingChannel && platform == MessagePlatform::Kick)
    {
        contextChannel =
            getApp()->getKickChatServer()->findBySlug(alternativePopoutChannel);
        if (!contextChannel)
        {
            contextChannel = Channel::getEmpty();
        }
    }
    else
    {
        contextChannel =
            getApp()->getTwitch()->getChannelOrEmpty(alternativePopoutChannel);
    }
    userPopup->setData(userName, contextChannel, openingChannel);

    QPoint offset(userPopup->width() / 3, userPopup->height() / 5);
    userPopup->moveTo(QCursor::pos() - offset,
                      widgets::BoundsChecking::CursorPosition);
    userPopup->show();
}

void ChannelView::showTikTokUserPopup(const QString &userID,
                                      const QString &handle,
                                      const QString &channelName)
{
    if (!this->split_)
    {
        return;
    }
    auto channel = getApp()->getTikTokChatServer()->findByHandle(channelName);
    TikTokAuthor author;
    if (channel)
    {
        author = channel->author(userID.isEmpty() ? handle : userID)
                     .value_or(TikTokAuthor{});
    }
    if (author.id.isEmpty())
    {
        author.id = userID;
    }
    if (author.handle.isEmpty())
    {
        author.handle = normalizeTikTokHandle(handle).value_or(QString{});
    }
    if (author.displayName.isEmpty())
    {
        author.displayName = handle;
    }
    auto *popup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, this->split_);
    const auto openingChannel = this->hasSourceChannel()
                                    ? this->sourceChannel_
                                    : this->selectedChannel();
    popup->setTikTokData(author, channel, openingChannel);
    popup->moveTo(
        QCursor::pos() - QPoint(popup->width() / 3, popup->height() / 5),
        widgets::BoundsChecking::CursorPosition);
    popup->show();
}

bool ChannelView::mayContainMessage(const MessagePtr &message)
{
    switch (this->channel()->getType())
    {
        case Channel::Type::Direct:
        case Channel::Type::Twitch:
        case Channel::Type::TwitchWatching:
            // XXX: system messages may not have the channel set
            return message->flags.has(MessageFlag::System) ||
                   this->channel()->getName() == message->channelName;
        case Channel::Type::TwitchWhispers:
            return message->flags.has(MessageFlag::Whisper);
        case Channel::Type::TwitchMentions:
            return message->flags.has(MessageFlag::Highlighted);
        case Channel::Type::TwitchLive:
            return message->flags.has(MessageFlag::System);
        case Channel::Type::TwitchAutomod:
            return message->flags.has(MessageFlag::AutoMod);
        case Channel::Type::TwitchEnd:  // TODO: not used?
        case Channel::Type::None:       // Unspecific
        case Channel::Type::Misc:       // Unspecific
            return true;
        default:
            return true;  // unreachable
    }
}

void ChannelView::handleLinkClick(QMouseEvent *event, const Link &link,
                                  MessageLayout *layout)
{
    if (event->button() != Qt::LeftButton &&
        event->button() != Qt::MiddleButton)
    {
        return;
    }

    const auto selectAutoModCard = [this](const QString &key) {
        if (this->underlyingChannel_ != nullptr &&
            this->underlyingChannel_->getType() == Channel::Type::TwitchAutomod)
        {
            this->setAutoModReviewSelectedKey(key);
        }
    };

    switch (link.type)
    {
        case Link::UserWhisper:
        case Link::UserInfo: {
            if (layout->getMessage()->platform == MessagePlatform::TikTok)
            {
                const auto message = layout->getMessagePtr();
                const auto handle = normalizeTikTokHandle(link.value);
                if (event->button() == Qt::MiddleButton && handle)
                {
                    QDesktopServices::openUrl(tikTokProfileUrl(*handle));
                }
                else
                {
                    if (link.value == message->loginName ||
                        link.value == u"id:"_s + message->userID)
                    {
                        this->showTikTokUserPopup(message);
                    }
                    else if (handle)
                    {
                        this->showTikTokUserPopup({}, *handle,
                                                  message->channelName);
                    }
                    else if (link.value.startsWith(u"id:") &&
                             isTikTokUserID(QStringView(link.value).sliced(3)))
                    {
                        this->showTikTokUserPopup(link.value.sliced(3), {},
                                                  message->channelName);
                    }
                }
                break;
            }
            if (link.type == Link::UserInfo &&
                layout->getMessage()->platform == MessagePlatform::YouTube)
            {
                const auto message = layout->getMessagePtr();
                const auto author = youtubeAuthorForUserLink(message, link);
                if (event->button() == Qt::MiddleButton)
                {
                    openYouTubeExternalUrl(youtubeChannelUrl(author.channelId));
                }
                else
                {
                    this->showYouTubeUserPopup(message, author);
                }
                break;
            }

            auto user = link.value;
            this->showUserInfoPopup(user, layout->getMessage()->platform,
                                    layout->getMessage()->channelName);
        }
        break;

        case Link::Url: {
            const QUrl url(link.value);
            if (!url.isLocalFile() && getSettings()->openLinksIncognito &&
                supportsIncognitoLinks())
            {
                openLinkIncognito(link.value);
            }
            else
            {
                QDesktopServices::openUrl(url);
            }
        }
        break;

        case Link::UserAction: {
            QString value = link.value;

            const auto message = layout->getMessagePtr();
            ChannelPtr channel = this->inferChannel(
                *message, InferChannel::SourceChannelIfAvailable);
            if (this->context_ == Context::Search)
            {
                if (auto *search =
                        dynamic_cast<SearchPopup *>(this->parentWidget()))
                {
                    channel = search->sourceChannelForMessage(message);
                }
            }

            if (auto *youtube = dynamic_cast<YouTubeChannel *>(channel.get()))
            {
                auto author = YouTubeMessageBuilder::cachedAuthorForMessage(
                                  layout->getMessage()->id)
                                  .value_or(YouTubeAuthor{});
                author.channelId = layout->getMessage()->userID;
                if (!youtube->canModerateTarget(author))
                {
                    youtube->addSystemMessage(QStringLiteral(
                        "You no longer have permission to moderate this "
                        "YouTube user."));
                    return;
                }
            }

            if (value.startsWith("/pin") && (value == "/pin" || value.startsWith("/pin ")))
            {
                auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
                if (tc)
                {
                    QString id = layout->getMessage()->id;
                    if (id.isEmpty())
                    {
                        return;
                    }

                    QString durationStr = value.mid(4).trimmed();
                    auto duration = commands::normalizePinDuration(
                        getSettings()->defaultPinDuration);

                    if (durationStr.isEmpty())
                    {
                        tc->pinMessage(id, duration);
                        return;
                    }

                    const auto parsedDuration =
                        commands::parsePinDuration(durationStr);
                    if (parsedDuration.matched)
                    {
                        if (!parsedDuration.error.isEmpty())
                        {
                            tc->addSystemMessage(parsedDuration.error);
                            return;
                        }

                        tc->pinMessage(id, parsedDuration.durationSeconds);
                        return;
                    }
                }
            }

            // Execute command clicking a moderator button

            {
                const auto *msg = layout->getMessage();
                if (msg != nullptr && !msg->id.isEmpty())
                {
                    value.replace(QStringLiteral("{msg.id}"), msg->id);
                    value.replace(QStringLiteral("{msg-id}"), msg->id);
                }
            }

            value = getApp()->getCommands()->execCustomCommand(
                QStringList(), Command{"(modaction)", value}, true, channel,
                layout->getMessage());

            value = getApp()->getCommands()->execCommand(value, channel, false);

            channel->sendMessage(value);
        }
        break;

        case Link::AutoModAllow: {
            getApp()->getAccounts()->twitch.getCurrent()->autoModAllow(
                link.value, this->channel());
        }
        break;

        case Link::AutoModDeny: {
            getApp()->getAccounts()->twitch.getCurrent()->autoModDeny(
                link.value, this->channel());
        }
        break;

        case Link::AutoModReviewApprove: {
            selectAutoModCard(link.value);
            if (auto *review = getApp()->getAutoModReview())
            {
                review->approve(link.value);
            }
        }
        break;

        case Link::AutoModReviewDeny: {
            selectAutoModCard(link.value);
            if (auto *review = getApp()->getAutoModReview())
            {
                review->deny(link.value);
            }
        }
        break;

        case Link::AutoModReviewRetry: {
            selectAutoModCard(link.value);
            if (auto *review = getApp()->getAutoModReview())
            {
                review->retry(link.value);
            }
        }
        break;

        case Link::AutoModReviewTimeout: {
            const auto direct = parseAutoModTimeoutLink(link.value);
            const auto key = direct ? direct->key : link.value;
            selectAutoModCard(key);
            if (direct)
            {
                if (auto *review = getApp()->getAutoModReview())
                {
                    review->denyAndTimeout(key, direct->seconds,
                                           u"AutoMod review"_s);
                }
            }
            else
            {
                this->showAutoModTimeoutMenu(key,
                                             event->globalPosition().toPoint());
            }
        }
        break;

        case Link::AutoModReviewBan: {
            selectAutoModCard(link.value);
            this->confirmAutoModBan(link.value);
        }
        break;

        case Link::AcknowledgeChatWarning: {
            auto channel =
                std::dynamic_pointer_cast<TwitchChannel>(
                    getApp()->getTwitch()->getChannelOrEmptyByID(link.value));

            if (channel == nullptr)
            {
                auto fallback = std::dynamic_pointer_cast<TwitchChannel>(
                    this->underlyingChannel_);
                if (fallback != nullptr &&
                    (link.value.isEmpty() || fallback->roomId() == link.value))
                {
                    channel = std::move(fallback);
                }
            }

            if (channel != nullptr)
            {
                channel->acknowledgeChatWarning();
            }
        }
        break;

        case Link::OpenModerationReport: {
            openRememberedModerationReport(link.value, this);
        }
        break;

        case Link::OpenAccountsPage: {
            SettingsDialog::showDialog(SettingsDialogPreference::Accounts);
        }
        break;
        case Link::JumpToChannel: {
            // Get all currently open pages
            QList<SplitContainer *> openPages;

            auto &nb = getApp()->getWindows()->getMainWindow().getNotebook();
            for (int i = 0; i < nb.getPageCount(); ++i)
            {
                openPages.push_back(
                    static_cast<SplitContainer *>(nb.getPageAt(i)));
            }
            QStringView searchName = link.value;
            auto searchPlatform = MessagePlatform::AnyOrTwitch;
            if (link.value.startsWith(u":kick:"))
            {
                searchName = searchName.sliced(sizeof(":kick:") - 1);
                searchPlatform = MessagePlatform::Kick;
            }
            else if (link.value.startsWith(u":youtube:"))
            {
                searchName = searchName.sliced(sizeof(":youtube:") - 1);
                searchPlatform = MessagePlatform::YouTube;
            }
            else if (link.value.startsWith(u":tiktok:"))
            {
                searchName = searchName.sliced(sizeof(":tiktok:") - 1);
                searchPlatform = MessagePlatform::TikTok;
            }

            const auto caseSensitivity =
                searchPlatform == MessagePlatform::YouTube ? Qt::CaseSensitive
                                                          : Qt::CaseInsensitive;
            for (auto *page : openPages)
            {
                auto splits = page->getSplits();

                // Search for channel matching link in page/split container
                // TODO(zneix): Consider opening a channel if it's closed (?)
                Split *targetSplit = nullptr;
                std::optional<size_t> targetChild;
                for (auto *split : splits)
                {
                    const auto underlying = split->getChannel();
                    if (auto *multi =
                            dynamic_cast<MultiChannel *>(underlying.get()))
                    {
                        const auto children = multi->channels();
                        for (size_t index = 0; index < children.size(); ++index)
                        {
                            const auto &channel = children[index].channel;
                            if (channel->getName().compare(
                                    searchName, caseSensitivity) == 0 &&
                                channel->messagePlatform() == searchPlatform)
                            {
                                targetSplit = split;
                                targetChild = index;
                                break;
                            }
                        }
                    }
                    else if (underlying != nullptr &&
                             underlying->getName().compare(
                                 searchName, caseSensitivity) == 0 &&
                             underlying->messagePlatform() == searchPlatform)
                    {
                        targetSplit = split;
                    }
                    if (targetSplit != nullptr)
                    {
                        break;
                    }
                }

                if (targetSplit != nullptr)
                {
                    if (targetChild)
                    {
                        auto *multi = dynamic_cast<MultiChannel *>(
                            targetSplit->getChannel().get());
                        if (multi != nullptr)
                        {
                            if (multi->activeChannelIndex() != *targetChild)
                            {
                                multi->setActiveChannelIndex(*targetChild);
                                getApp()
                                    ->getWindows()
                                    ->forceLayoutChannelViews();
                            }
                        }
                    }
                    // Select SplitContainer and Split itself where mention message was sent
                    // TODO(zneix): Try exploring ways of scrolling to a certain message as well
                    nb.select(page);
                    page->setSelected(targetSplit);
                    break;
                }
            }
        }
        break;
        case Link::OpenChannel: {
            getApp()->getWindows()->openChannelOrMessageFromTray(link.value, {},
                                                                 true);
        }
        break;
        case Link::CopyToClipboard: {
            crossPlatformCopy(link.value);
        }
        break;
        case Link::Reconnect: {
            this->underlyingChannel_.get()->reconnect();
        }
        break;
        case Link::ReplyToMessage: {
            if (layout->getMessagePtr()->isReplyable() !=
                Message::ReplyStatus::NotReplyable)
            {
                this->setInputReply(layout->getMessagePtr());
            }
        }
        break;
        case Link::ViewThread: {
            this->showReplyThreadPopup(layout->getMessagePtr());
        }
        break;
        case Link::JumpToMessage: {
            if (this->context_ == Context::Search)
            {
                auto *search =
                    dynamic_cast<SearchPopup *>(this->parentWidget());
                if (search != nullptr)
                {
                    search->goToMessageId(link.value);
                }
                return;
            }

            this->scrollToMessageId(link.value);
        }
        break;

        default:;
    }
}

void ChannelView::showAutoModTimeoutMenu(const QString &key,
                                         const QPoint &globalPos)
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr ? review->find(key) : nullptr;
    if (item == nullptr || !item->actionsAllowed ||
        item->state != automod::ReviewState::Open)
    {
        return;
    }

    const QPointer<ChannelView> self(this);
    QPointer<QMenu> menu = new QMenu(this);
    const auto cleanup = qScopeGuard([menu] {
        delete menu;
    });
    const auto addTimeout = [&](QString label, int seconds) {
        menu->addAction(std::move(label), [review, key, seconds] {
            review->denyAndTimeout(key, seconds, u"AutoMod review"_s);
        });
    };
    addTimeout(u"30 seconds"_s, 30);
    addTimeout(u"1 minute"_s, 60);
    addTimeout(u"5 minutes"_s, 5 * 60);
    addTimeout(u"10 minutes"_s, 10 * 60);
    addTimeout(u"1 hour"_s, 60 * 60);
    addTimeout(u"1 day"_s, 24 * 60 * 60);
    menu->addSeparator();
    menu->addAction(u"Custom duration…"_s, [this, review, key] {
        bool ok = false;
        const auto minutes = QInputDialog::getInt(
            this, u"Deny and timeout"_s, u"Timeout length in minutes:"_s, 10, 1,
            14 * 24 * 60, 1, &ok);
        if (ok)
        {
            review->denyAndTimeout(key, minutes * 60, u"AutoMod review"_s);
        }
    });

    menu->exec(globalPos);
    if (self && self->underlyingChannel_ != nullptr &&
        self->underlyingChannel_->getType() == Channel::Type::TwitchAutomod)
    {
        self->setFocus(Qt::PopupFocusReason);
    }
}

bool ChannelView::confirmAutoModBan(QString key)
{
    auto *review = getApp()->getAutoModReview();
    const auto *item = review != nullptr ? review->find(key) : nullptr;
    if (item == nullptr || !item->actionsAllowed ||
        item->state != automod::ReviewState::Open)
    {
        return false;
    }

    const auto answer = QMessageBox::question(
        this, u"Deny and ban"_s,
        u"Deny this message and permanently ban %1 from #%2?"_s.arg(
            item->userLogin, item->broadcasterLogin),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes)
    {
        review->denyAndBan(key, u"AutoMod review"_s);
    }
    return true;
}

bool ChannelView::tryGetMessageAt(QPointF p,
                                  std::shared_ptr<MessageLayout> &_message,
                                  QPointF &relativePos, int &index)
{
    auto &messagesSnapshot = this->getMessagesSnapshot();

    const auto start = size_t(this->scrollBar_->getRelativeCurrentValue());

    if (start >= messagesSnapshot.size())
    {
        return false;
    }

    qreal y = -(messagesSnapshot[start]->getHeight() *
                (fmod(this->scrollBar_->getRelativeCurrentValue(), 1)));

    for (size_t i = start; i < messagesSnapshot.size(); ++i)
    {
        auto message = messagesSnapshot[i];

        if (p.y() < y + message->getHeight())
        {
            relativePos = QPointF(p.x(), p.y() - y);
            _message = message;
            index = i;
            return true;
        }

        y += message->getHeight();
    }

    return false;
}

int ChannelView::getLayoutWidth() const
{
    if (this->scrollBar_->isVisible())
    {
        return int(this->width() - SCROLLBAR_PADDING * this->scale());
    }

    return this->width();
}

void ChannelView::selectWholeMessage(MessageLayout *layout, int &messageIndex)
{
    SelectionItem msgStart(messageIndex,
                           layout->getFirstMessageCharacterIndex());
    SelectionItem msgEnd(messageIndex, layout->getLastCharacterIndex());
    this->setSelection(msgStart, msgEnd);
}

void ChannelView::enableScrolling(const QPointF &scrollStart)
{
    this->isScrolling_ = true;
    this->lastMiddlePressPosition_ = scrollStart;
    // The line below prevents a sudden jerk at the beginning
    this->currentMousePosition_ = scrollStart;

    this->scrollTimer_.start();

    if (!QGuiApplication::overrideCursor())
    {
        QGuiApplication::setOverrideCursor(this->cursors_.neutral);
    }
}

void ChannelView::disableScrolling()
{
    this->isScrolling_ = false;
    this->scrollTimer_.stop();
    QGuiApplication::restoreOverrideCursor();
}

void ChannelView::scrollUpdateRequested()
{
    const qreal dpi = this->devicePixelRatioF();
    const qreal delta = dpi * (this->currentMousePosition_.y() -
                               this->lastMiddlePressPosition_.y());
    const int cursorHeight = this->cursors_.neutral.pixmap().height();

    if (fabs(delta) <= cursorHeight * dpi)
    {
        /*
         * If within an area close to the initial position, don't do any
         * scrolling at all.
         */
        QGuiApplication::changeOverrideCursor(this->cursors_.neutral);
        return;
    }

    qreal offset;
    if (delta > 0)
    {
        QGuiApplication::changeOverrideCursor(this->cursors_.down);
        offset = delta - cursorHeight;
    }
    else
    {
        QGuiApplication::changeOverrideCursor(this->cursors_.up);
        offset = delta + cursorHeight;
    }

    // "Good" feeling multiplier found by trial-and-error
    const qreal multiplier(0.02);
    this->scrollBar_->offset(multiplier * offset);
}

bool ChannelView::setInputReply(const MessagePtr &message)
{
    assertInGuiThread();

    if (message == nullptr || this->split_ == nullptr)
    {
        return false;
    }

    auto chan = this->inferChannel(*message);
    if (chan->isYouTubeChannel())
    {
        QString handle;
        if (const auto author =
                YouTubeMessageBuilder::cachedAuthorForMessage(message->id))
        {
            handle = visibleYouTubeName(author->handle);
        }
        else
        {
            handle = visibleYouTubeName(message->loginName);
        }
        if (handle.isEmpty())
        {
            return false;
        }

        this->split_->setInputReply(nullptr, {});

        if (auto *multi =
                dynamic_cast<MultiChannel *>(this->underlyingChannel_.get()))
        {
            const auto children = multi->channels();
            for (size_t i = 0; i < children.size(); ++i)
            {
                if (children[i].channel == chan)
                {
                    if (multi->activeChannelIndex() != i)
                    {
                        multi->setActiveChannelIndex(i);
                        getApp()->getWindows()->forceLayoutChannelViews();
                    }
                    break;
                }
            }
        }

        this->split_->insertTextToInput(u'@' + handle + u' ');
        this->split_->getInput().focusEditor(Qt::MouseFocusReason);
        return true;
    }

    if (!message->replyThread)
    {
        // Message did not already have a thread attached, try to find or create one
        auto *tc = dynamic_cast<TwitchChannel *>(chan.get());
        auto *kc = dynamic_cast<KickChannel *>(chan.get());

        if (tc)
        {
            tc->getOrCreateThread(message);
        }
        else if (kc)
        {
            kc->getOrCreateThread(message->id);
        }
        else
        {
            qCWarning(chatterinoCommon) << "Failed to create new reply thread";
            // Unable to create new reply thread.
            // TODO(dnsge): Should probably notify user?
            return false;
        }
    }

    this->split_->setInputReply(message, chan);
    return true;
}

void ChannelView::showReplyThreadPopup(const MessagePtr &message)
{
    if (message == nullptr || message->replyThread == nullptr)
    {
        return;
    }

    if (!this->split_)
    {
        qCWarning(chatterinoCommon)
            << "Tried to show reply thread popup but the "
               "channel view doesn't belong to a split.";
        return;
    }

    auto *popup =
        new ReplyThreadPopup(getSettings()->autoCloseThreadPopup, this->split_);

    popup->setThread(message->replyThread, this->inferChannel(*message));

    popup->showAt(QCursor::pos());
    popup->giveFocus(Qt::MouseFocusReason);
}

ChannelView::Context ChannelView::getContext() const
{
    return this->context_;
}

bool ChannelView::canReplyToMessages() const
{
    if (this->context_ == ChannelView::Context::ReplyThread ||
        this->context_ == ChannelView::Context::Search)
    {
        return false;
    }

    assert(this->channel_ != nullptr);

    const auto supportsReplies = [](const ChannelPtr &channel) {
        return channel &&
               (channel->isTwitchOrKickChannel() ||
                channel->isYouTubeChannel()) &&
               channel->getType() != Channel::Type::TwitchWhispers &&
               channel->getType() != Channel::Type::TwitchLive;
    };
    if (const auto *multi =
            dynamic_cast<MultiChannel *>(this->underlyingChannel_.get()))
    {
        return std::ranges::any_of(multi->channels(), [&](const auto &child) {
            return supportsReplies(child.channel);
        });
    }
    return supportsReplies(this->underlyingChannel_);
}

void ChannelView::setLinkInfoTooltip(LinkInfo *info)
{
    assert(info);

    const auto thumbnailSize =
        info->thumbnailSize(getSettings()->thumbnailSize);

    ImagePtr thumbnail;
    if (info->hasThumbnail() && thumbnailSize > 0)
    {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideLinkThumbnails)
        {
            thumbnail = Image::fromResourcePixmap(getResources().streamerMode);
        }
        else
        {
            thumbnail = info->thumbnail();
        }
    }

    this->ensureTooltipWidget()->setOne({
        .image = thumbnail,
        .text = info->tooltip(),
        .customWidth = thumbnailSize,
        .customHeight = thumbnailSize,
    });

    if (info->isLoaded())
    {
        this->clearPendingLinkInfo();
        return;  // Either resolved or errored (can't change anymore)
    }

    // listen to changes

    if (this->pendingLinkInfo_.data() == info)
    {
        return;  // same info - already registered
    }

    this->clearPendingLinkInfo();
    QObject::connect(info, &LinkInfo::stateChanged, this,
                     &ChannelView::pendingLinkInfoStateChanged);
    this->pendingLinkInfo_ = info;
}

void ChannelView::pendingLinkInfoStateChanged()
{
    if (!this->pendingLinkInfo_)
    {
        return;
    }
    this->setLinkInfoTooltip(this->pendingLinkInfo_.data());
    this->ensureTooltipWidget()->applyLastBoundsCheck();
}

void ChannelView::updateID()
{
    if (!this->underlyingChannel_)
    {
        // cannot update
        return;
    }

    std::size_t seed = 0;
    auto first = qHash(this->underlyingChannel_->getName());
    auto second = qHash(this->getFilterIds());

    boost::hash_combine(seed, first);
    boost::hash_combine(seed, second);
    boost::hash_combine(seed, this->underlyingChannel_->getType());

    this->id_ = seed;
}

ChannelView::ChannelViewID ChannelView::getID() const
{
    return this->id_;
}

}  // namespace chatterino
