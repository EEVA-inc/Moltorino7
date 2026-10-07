// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "controllers/highlights/HighlightWordList.hpp"
#include "controllers/ignores/HiddenUserController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/tiktok/TikTokAccount.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"

#include <QSet>
#include <QTimer>

namespace {

using namespace chatterino;

constexpr std::size_t MAX_EXACT_HIGHLIGHT_MARKERS_PER_MESSAGE = 256;

auto highlightPhraseCheck(
    const HighlightPhrase &highlight, bool hasVisibleExactAppearance,
    std::optional<MessagePlatform> onlyPlatform = std::nullopt)
    -> HighlightCheck
{
    return HighlightCheck{
        [highlight, hasVisibleExactAppearance, onlyPlatform](
            const auto &args, const auto &twitchBadges, const auto &senderName,
            const auto &originalMessage, const auto &flags, const auto self,
            const auto platform, const auto &normalizedChannelName,
            const auto collectExactMatches) -> std::optional<HighlightResult> {
            (void)args;
            (void)twitchBadges;
            (void)senderName;
            (void)flags;

            if (self || (onlyPlatform && *onlyPlatform != platform) ||
                !highlight.getChannelScope().appliesToNormalized(
                    platform, normalizedChannelName))
            {

                return std::nullopt;
            }

            if (!highlight.isMatch(originalMessage))
            {
                return std::nullopt;
            }

            std::optional<QUrl> highlightSoundUrl;
            if (highlight.hasCustomSound())
            {
                highlightSoundUrl = highlight.getSoundUrl();
            }

            auto result = HighlightResult{
                highlight.hasAlert(),       highlight.hasSound(),
                highlightSoundUrl,          highlight.getColor(),
                highlight.showInMentions(),
            };
            if (!collectExactMatches || !hasVisibleExactAppearance)
            {
                return result;
            }
            for (const auto &match : highlight.findMatches(originalMessage))
            {
                result.matches.emplace_back(HighlightMatch{
                    .start = match.capturedStart(),
                    .length = match.capturedLength(),
                    .color = *highlight.getMatchColor(),
                    .ruleName = highlight.getPattern(),
                    .pattern = highlight.getPattern(),
                    .source = HighlightMatchSource::Phrase,
                    .style = highlight.getMatchStyle(),
                    .paintID = highlight.getMatchPaintID(),
                });
            }
            return result;
        }};
}

void rebuildSubscriptionHighlights(Settings &settings,
                                   std::vector<HighlightCheck> &checks)
{
    if (settings.enableSubHighlight)
    {
        auto highlightSound = settings.enableSubHighlightSound.getValue();
        auto highlightAlert = settings.enableSubHighlightTaskbar.getValue();
        auto highlightSoundUrlValue = settings.subHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags, const auto self, const auto, const auto &,
                const auto) -> std::optional<HighlightResult> {
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!args.isSubscriptionMessage)
                {
                    return std::nullopt;
                }

                auto highlightColor =
                    ColorProvider::instance().color(ColorType::Subscription);

                return HighlightResult{
                    highlightAlert,
                    highlightSound,
                    highlightSoundUrl,
                    highlightColor,
                    false,
                };
            }});
    }
}

void rebuildWhisperHighlights(Settings &settings,
                              std::vector<HighlightCheck> &checks)
{
    if (settings.enableWhisperHighlight)
    {
        auto highlightSound = settings.enableWhisperHighlightSound.getValue();
        auto highlightAlert = settings.enableWhisperHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.whisperHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags, const auto self, const auto, const auto &,
                const auto) -> std::optional<HighlightResult> {
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!args.isReceivedWhisper)
                {
                    return std::nullopt;
                }

                return HighlightResult{
                    highlightAlert,
                    highlightSound,
                    highlightSoundUrl,
                    ColorProvider::instance().color(ColorType::Whisper),
                    false,
                };
            }});
    }
}

void rebuildReplyThreadHighlight(Settings &settings,
                                 std::vector<HighlightCheck> &checks)
{
    if (settings.enableThreadHighlight)
    {
        auto highlightSound = settings.enableThreadHighlightSound.getValue();
        auto highlightAlert = settings.enableThreadHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.threadHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }
        auto highlightInMentions =
            settings.showThreadHighlightInMentions.getValue();
        checks.emplace_back(HighlightCheck{
            [=](const auto &, const auto &, const auto &, const auto &,
                const auto &flags, const auto self, const auto, const auto &,
                const auto) -> std::optional<HighlightResult> {
                if (flags.has(MessageFlag::SubscribedThread) && !self)
                {
                    return HighlightResult{
                        highlightAlert,
                        highlightSound,
                        highlightSoundUrl,
                        ColorProvider::instance().color(
                            ColorType::ThreadMessageHighlight),
                        highlightInMentions,
                    };
                }

                return std::nullopt;
            }});
    }
}

void rebuildMessageHighlights(Settings &settings,
                              std::vector<HighlightCheck> &checks,
                              QObject *lifetimeContext)
{
    QSet<QString> explicitPaintIDs;
    const bool paintsEnabled = settings.displaySevenTVPaints;
    const auto hasVisibleExactAppearance = [=](HighlightMatchStyle style,
                                               const QString &paintID) {
        return style != HighlightMatchStyle::None ||
               (paintsEnabled && SeventvPaints::isValidPaintID(paintID));
    };
    auto selfMatchColor = QColor(settings.selfHighlightMatchColor.getValue());
    if (!selfMatchColor.isValid())
    {
        selfMatchColor = defaultNewHighlightMatchColor();
    }
    const auto selfMatchStyle = highlightMatchStyleFromName(
        settings.selfHighlightMatchStyle.getValue());
    const auto selfMatchPaintID = settings.selfHighlightMatchPaintID.getValue();
    const auto addSelfHighlight = [&](const QString &name,
                                      MessagePlatform platform) {
        if (name.isEmpty())
        {
            return;
        }
        HighlightPhrase highlight(
            name, settings.showSelfHighlightInMentions,
            settings.enableSelfHighlightTaskbar,
            settings.enableSelfHighlightSound, false, false,
            settings.selfHighlightSoundUrl.getValue(),
            ColorProvider::instance().color(ColorType::SelfHighlight),
            std::make_shared<QColor>(selfMatchColor), selfMatchStyle,
            selfMatchPaintID);
        checks.emplace_back(highlightPhraseCheck(
            highlight,
            hasVisibleExactAppearance(highlight.getMatchStyle(),
                                      highlight.getMatchPaintID()),
            platform));
        if (paintsEnabled && SeventvPaints::isValidPaintID(selfMatchPaintID))
        {
            explicitPaintIDs.insert(
                SeventvPaints::normalizePaintID(selfMatchPaintID));
        }
    };
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    QString currentUsername = currentUser->getUserName();

    if (settings.enableSelfHighlight && !currentUsername.isEmpty() &&
        !currentUser->isAnon())
    {
        addSelfHighlight(currentUsername, MessagePlatform::AnyOrTwitch);
    }

    auto kickUser = getApp()->getAccounts()->kick.current();
    auto kickUsername = kickUser->username();
    if (settings.enableSelfHighlight && !kickUsername.isEmpty() &&
        !kickUser->isAnonymous())
    {
        addSelfHighlight(kickUsername, MessagePlatform::Kick);
    }

    const auto youtubeUser = getApp()->getAccounts()->youtube.current();
    if (settings.enableSelfHighlight && !youtubeUser->isAnonymous())
    {
        const auto handle = youtubeUser->handle();
        const auto displayName = youtubeUser->displayName();
        addSelfHighlight(handle, MessagePlatform::YouTube);
        if (displayName.compare(handle, Qt::CaseInsensitive) != 0)
        {
            addSelfHighlight(displayName, MessagePlatform::YouTube);
        }
    }

    const auto tiktokUser = getApp()->getAccounts()->tiktok.current();
    if (settings.enableSelfHighlight && !tiktokUser->isAnonymous())
    {
        addSelfHighlight(tiktokUser->handle(), MessagePlatform::TikTok);
        if (tiktokUser->displayName().compare(tiktokUser->handle(),
                                              Qt::CaseInsensitive) != 0)
        {
            addSelfHighlight(tiktokUser->displayName(),
                             MessagePlatform::TikTok);
        }
    }

    auto messageHighlights = settings.highlightedMessages.readOnly();
    for (const auto &highlight : *messageHighlights)
    {
        if (settings.displaySevenTVPaints &&
            SeventvPaints::isValidPaintID(highlight.getMatchPaintID()))
        {
            explicitPaintIDs.insert(
                SeventvPaints::normalizePaintID(highlight.getMatchPaintID()));
        }
        checks.emplace_back(highlightPhraseCheck(
            highlight, hasVisibleExactAppearance(highlight.getMatchStyle(),
                                                 highlight.getMatchPaintID())));
    }

    auto wordLists = settings.highlightWordLists.readOnly();
    for (const auto &wordList : *wordLists)
    {
        if (settings.displaySevenTVPaints && wordList.enabled() &&
            SeventvPaints::isValidPaintID(wordList.matchPaintID()))
        {
            explicitPaintIDs.insert(
                SeventvPaints::normalizePaintID(wordList.matchPaintID()));
        }
        const bool hasExactAppearance = hasVisibleExactAppearance(
            wordList.matchStyle(), wordList.matchPaintID());
        checks.emplace_back(HighlightCheck{
            [wordList, hasExactAppearance](
                const auto &, const auto &, const auto &,
                const auto &originalMessage, const auto &, const auto self,
                const auto platform, const auto &normalizedChannelName,
                const auto collectExactMatches)
                -> std::optional<HighlightResult> {
                if (self)
                {
                    return std::nullopt;
                }
                auto match = wordList.matchForNormalizedChannel(
                    originalMessage, normalizedChannelName, platform,
                    collectExactMatches && hasExactAppearance
                        ? HighlightWordListMatchMode::WithRanges
                        : HighlightWordListMatchMode::MatchOnly);
                if (!match.matched)
                {
                    return std::nullopt;
                }

                std::optional<QUrl> soundUrl;
                if (wordList.hasCustomSound())
                {
                    soundUrl = wordList.soundUrl();
                }
                auto result = HighlightResult{
                    wordList.hasAlert(), wordList.hasSound(), soundUrl,
                    wordList.color(), wordList.showInMentions()};
                result.matches = std::move(match.matches);
                return result;
            }});
    }

    if (!explicitPaintIDs.isEmpty())
    {
        QTimer::singleShot(3000, lifetimeContext,
                           [paintIDs = std::move(explicitPaintIDs)] {
                               auto *app = tryGetApp();
                               if (app == nullptr)
                               {
                                   return;
                               }
                               auto *paints = app->getSeventvPaints();
                               for (const auto &paintID : paintIDs)
                               {
                                   paints->loadPaintByID(paintID);
                               }
                           });
    }

    if (settings.enableAutomodHighlight)
    {
        const auto highlightSound =
            settings.enableAutomodHighlightSound.getValue();
        const auto highlightAlert =
            settings.enableAutomodHighlightTaskbar.getValue();
        const auto highlightSoundUrlValue =
            settings.automodHighlightSoundUrl.getValue();
        auto highlightColor =
            ColorProvider::instance().color(ColorType::AutomodHighlight);

        checks.emplace_back(HighlightCheck{
            [=](const auto &, const auto &, const auto &, const auto &,
                const auto &flags, const auto, const auto, const auto &,
                const auto) -> std::optional<HighlightResult> {
                if (!flags.has(MessageFlag::AutoModOffendingMessage))
                {
                    return std::nullopt;
                }

                std::optional<QUrl> highlightSoundUrl;
                if (!highlightSoundUrlValue.isEmpty())
                {
                    highlightSoundUrl = highlightSoundUrlValue;
                }

                return HighlightResult{
                    highlightAlert,
                    highlightSound,
                    highlightSoundUrl,
                    highlightColor,
                    false,
                };
            }});
    }
}

void rebuildUserHighlights(Settings &settings,
                           std::vector<HighlightCheck> &checks)
{
    auto userHighlights = settings.highlightedUsers.readOnly();

    if (settings.enableSelfMessageHighlight)
    {
        bool showInMentions = settings.showSelfMessageHighlightInMentions;

        checks.emplace_back(HighlightCheck{
            [showInMentions](const auto &args, const auto &twitchBadges,
                             const auto &senderName,
                             const auto &originalMessage, const auto &flags,
                             const auto self, const auto, const auto &,
                             const auto) -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)senderName;
                (void)flags;
                (void)originalMessage;

                if (!self)
                {
                    return std::nullopt;
                }

                auto highlightColor = ColorProvider::instance().color(
                    ColorType::SelfMessageHighlight);

                return HighlightResult{false, false, (QUrl) nullptr,
                                       highlightColor, showInMentions};
            }});
    }

    for (const auto &highlight : *userHighlights)
    {
        checks.emplace_back(HighlightCheck{
            [highlight](const auto &args, const auto &twitchBadges,
                        const auto &senderName, const auto &originalMessage,
                        const auto &flags, const auto self, const auto platform,
                        const auto &normalizedChannelName,
                        const auto) -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!highlight.getChannelScope().appliesToNormalized(
                        platform, normalizedChannelName) ||
                    !highlight.isMatch(senderName))
                {
                    return std::nullopt;
                }

                std::optional<QUrl> highlightSoundUrl;
                if (highlight.hasCustomSound())
                {
                    highlightSoundUrl = highlight.getSoundUrl();
                }

                return HighlightResult{
                    highlight.hasAlert(),
                    highlight.hasSound(),
                    highlightSoundUrl,
                    highlight.getColor(),
                    highlight.showInMentions(),
                };
            }});
    }
}

void rebuildBadgeHighlights(Settings &settings,
                            std::vector<HighlightCheck> &checks)
{
    auto badgeHighlights = settings.highlightedBadges.readOnly();

    for (const auto &highlight : *badgeHighlights)
    {
        checks.emplace_back(HighlightCheck{
            [highlight](const auto &args, const auto &twitchBadges,
                        const auto &senderName, const auto &originalMessage,
                        const auto &flags, const auto self, const auto,
                        const auto &, const auto)
                -> std::optional<HighlightResult> {
                (void)args;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                for (const TwitchBadge &badge : twitchBadges)
                {
                    if (highlight.isMatch(badge))
                    {
                        std::optional<QUrl> highlightSoundUrl;
                        if (highlight.hasCustomSound())
                        {
                            highlightSoundUrl = highlight.getSoundUrl();
                        }

                        return HighlightResult{
                            highlight.hasAlert(),
                            highlight.hasSound(),
                            highlightSoundUrl,
                            highlight.getColor(),
                            highlight.showInMentions(),
                        };
                    }
                }

                return std::nullopt;
            }});
    }
}

}

namespace chatterino {

HighlightController::HighlightController(Settings &settings,
                                         AccountController *accounts)
{
    assert(accounts != nullptr);

    this->rebuildListener_.addSetting(settings.enableSelfHighlight);
    this->rebuildListener_.addSetting(settings.enableSelfHighlightSound);
    this->rebuildListener_.addSetting(settings.enableSelfHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.selfHighlightSoundUrl);
    this->rebuildListener_.addSetting(settings.showSelfHighlightInMentions);
    this->rebuildListener_.addSetting(settings.selfHighlightMatchColor);
    this->rebuildListener_.addSetting(settings.selfHighlightMatchStyle);
    this->rebuildListener_.addSetting(settings.selfHighlightMatchPaintID);

    this->rebuildListener_.addSetting(settings.enableWhisperHighlight);
    this->rebuildListener_.addSetting(settings.enableWhisperHighlightSound);
    this->rebuildListener_.addSetting(settings.enableWhisperHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.whisperHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableSubHighlight);
    this->rebuildListener_.addSetting(settings.enableSubHighlightSound);
    this->rebuildListener_.addSetting(settings.enableSubHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.enableSelfMessageHighlight);
    this->rebuildListener_.addSetting(
        settings.showSelfMessageHighlightInMentions);

    this->rebuildListener_.addSetting(settings.subHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableThreadHighlight);
    this->rebuildListener_.addSetting(settings.enableThreadHighlightSound);
    this->rebuildListener_.addSetting(settings.enableThreadHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.threadHighlightSoundUrl);
    this->rebuildListener_.addSetting(settings.showThreadHighlightInMentions);

    this->rebuildListener_.addSetting(settings.enableAutomodHighlight);
    this->rebuildListener_.addSetting(settings.showAutomodInMentions);
    this->rebuildListener_.addSetting(settings.enableAutomodHighlightSound);
    this->rebuildListener_.addSetting(settings.enableAutomodHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.automodHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.displaySevenTVPaints);

    this->rebuildListener_.setCB([this, &settings] {
        qCDebug(chatterinoHighlights)
            << "Rebuild checks because a setting changed";
        this->rebuildChecks(settings);
    });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedBadges.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight badges changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedUsers.delayedItemsChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight users changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedMessages.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight messages changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightWordLists.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight word lists changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->twitch.currentUserChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because user swapped accounts";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->twitch.currentUserNameChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because user name changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->kick.currentUserChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because Kick user changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->youtube.currentChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because YouTube user changed";
            this->rebuildChecks(settings);
        });
    this->signalHolder_.managedConnect(accounts->youtube.userListUpdated,
                                       [this, &settings] {
                                           this->rebuildChecks(settings);
                                       });

    this->signalHolder_.managedConnect(accounts->tiktok.currentChanged,
                                       [this, &settings] {
                                           this->rebuildChecks(settings);
                                       });
    this->signalHolder_.managedConnect(accounts->tiktok.userListUpdated,
                                       [this, &settings] {
                                           this->rebuildChecks(settings);
                                       });

    this->rebuildChecks(settings);
}

void HighlightController::rebuildChecks(Settings &settings)
{

    auto checks = this->checks_.access();
    checks->clear();

    rebuildSubscriptionHighlights(settings, *checks);

    rebuildWhisperHighlights(settings, *checks);

    rebuildMessageHighlights(settings, *checks, &this->lifetimeGuard_);

    rebuildUserHighlights(settings, *checks);

    rebuildReplyThreadHighlight(settings, *checks);

    rebuildBadgeHighlights(settings, *checks);
}

std::pair<bool, HighlightResult> HighlightController::check(
    const MessageParseArgs &args, const std::vector<TwitchBadge> &twitchBadges,
    const QString &senderName, const QString &originalMessage,
    const MessageFlags &messageFlags, MessagePlatform platform,
    const QString &senderID, const QString &channelName,
    bool forceMatchRanges) const
{
    bool highlighted = false;
    auto result = HighlightResult::emptyResult();

    if (auto *hiddenUsers = getApp()->getHiddenUsers();
        hiddenUsers && hiddenUsers->shouldSuppressHighlights(
                           platform, senderID, senderName, originalMessage))
    {
        return {false, result};
    }

    const auto checks = this->checks_.accessConst();

    bool self = false;
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch: {
            auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
            self = senderName == currentUser->getUserName();
        }
        break;
        case MessagePlatform::Kick: {
            auto kickUser = getApp()->getAccounts()->kick.current();
            self =
                !kickUser->isAnonymous() && senderName == kickUser->username();
        }
        break;
        case MessagePlatform::YouTube: {
            const auto youtubeUser = getApp()->getAccounts()->youtube.current();
            self = !youtubeUser->isAnonymous() && !senderID.isEmpty() &&
                   senderID == youtubeUser->channelID();
        }
        break;
        case MessagePlatform::TikTok: {
            const auto tiktokUser = getApp()->getAccounts()->tiktok.current();
            self = !tiktokUser->isAnonymous() && !senderID.isEmpty() &&
                   senderID == tiktokUser->userID();
        }
        break;
    }

    const auto normalizedChannelName =
        normalizeHighlightChannelName(channelName);
    const bool collectExactMatches =
        forceMatchRanges || getSettings()->highlightMatchedFragments;
    for (const auto &check : *checks)
    {
        if (auto checkResult = check.cb(
                args, twitchBadges, senderName, originalMessage, messageFlags,
                self, platform, normalizedChannelName, collectExactMatches);
            checkResult)
        {
            highlighted = true;

            if (checkResult->alert)
            {
                if (!result.alert)
                {
                    result.alert = checkResult->alert;
                }
            }

            if (checkResult->playSound)
            {
                if (!result.playSound)
                {
                    result.playSound = checkResult->playSound;
                }
            }

            if (checkResult->customSoundUrl)
            {
                if (!result.customSoundUrl)
                {
                    result.customSoundUrl = checkResult->customSoundUrl;
                }
            }

            if (checkResult->color)
            {
                if (!result.color)
                {
                    result.color = checkResult->color;
                }
            }

            if (checkResult->showInMentions)
            {
                if (!result.showInMentions)
                {
                    result.showInMentions = checkResult->showInMentions;
                }
            }

            const auto remaining =
                MAX_EXACT_HIGHLIGHT_MARKERS_PER_MESSAGE -
                std::min(result.matches.size(),
                         MAX_EXACT_HIGHLIGHT_MARKERS_PER_MESSAGE);
            const auto count = std::min(remaining, checkResult->matches.size());
            result.matches.insert(result.matches.end(),
                                  checkResult->matches.begin(),
                                  checkResult->matches.begin() + count);
        }
    }

    return {highlighted, result};
}

}
