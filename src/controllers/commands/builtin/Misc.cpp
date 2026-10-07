// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/Misc.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/ignores/HiddenUserController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/IvrApi.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/ModerationActionLogs.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchNameHistory.hpp"
#include "providers/translation/Translator.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/FormatTime.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/MultiChannel.hpp"
#include "util/StreamLink.hpp"
#include "util/Twitch.hpp"
#include "widgets/dialogs/ChatAutomationDialog.hpp"
#include "widgets/dialogs/CrossBanDialog.hpp"
#include "widgets/dialogs/ModerationReportDialog.hpp"
#include "widgets/dialogs/UnbanRequestsDialog.hpp"
#include "widgets/dialogs/VanityDialog.hpp"
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
#    include "widgets/dialogs/RewardRequestQueueDialog.hpp"
#endif
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCommandLineParser>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonObject>
#include <QPoint>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace chatterino::commands {

namespace {

QString followAction(bool unfollow)
{
    return unfollow ? "unfollow" : "follow";
}

QString followActionNoun(bool unfollow)
{
    return unfollow ? "unfollowing users" : "following users";
}

QString normalizedHiddenCommandTarget(QString target)
{
    target = target.trimmed();
    while (target.startsWith(u'@'))
    {
        target.remove(0, 1);
    }
    return target;
}

std::optional<HiddenUserPlatform> hiddenUserPlatformForChannel(
    const ChannelPtr &channel)
{
    if (!channel)
    {
        return std::nullopt;
    }

    const Channel *effectiveChannel = channel.get();
    if (const auto *multi =
            dynamic_cast<const MultiChannel *>(effectiveChannel))
    {
        const auto *active = multi->activeChannel();
        if (active == nullptr)
        {
            return std::nullopt;
        }
        effectiveChannel = active->channel.get();
    }

    if (effectiveChannel->isYouTubeChannel())
    {
        return HiddenUserPlatform::YouTube;
    }
    if (effectiveChannel->isKickChannel())
    {
        return HiddenUserPlatform::Kick;
    }
    if (effectiveChannel->isTikTokChannel())
    {
        return HiddenUserPlatform::TikTok;
    }
    if (effectiveChannel->isTwitchChannel())
    {
        return HiddenUserPlatform::Twitch;
    }
    return std::nullopt;
}

bool splitContainsChannel(const Split *split, const ChannelPtr &channel)
{
    if (split == nullptr || channel == nullptr)
    {
        return false;
    }

    const auto displayed = split->getChannel();
    if (displayed == channel)
    {
        return true;
    }
    const auto *multi = dynamic_cast<const MultiChannel *>(displayed.get());
    return multi != nullptr &&
           std::ranges::any_of(multi->channels(),
                               [&channel](const auto &child) {
                                   return child.channel == channel;
                               });
}

bool hiddenCommandTargetsCurrentAccount(HiddenUserPlatform platform,
                                        const QString &target)
{
    const auto matches = [&target](const QString &candidate) {
        return !candidate.trimmed().isEmpty() &&
               normalizedHiddenCommandTarget(candidate).compare(
                   target, Qt::CaseInsensitive) == 0;
    };

    switch (platform)
    {
        case HiddenUserPlatform::Twitch: {
            const auto account = getApp()->getAccounts()->twitch.getCurrent();
            return account && !account->isAnon() &&
                   matches(account->getUserName());
        }
        case HiddenUserPlatform::Kick: {
            const auto account = getApp()->getAccounts()->kick.current();
            return account && !account->isAnonymous() &&
                   matches(account->username());
        }
        case HiddenUserPlatform::YouTube: {
            const auto account = getApp()->getAccounts()->youtube.current();
            return account && !account->isAnonymous() &&
                   (matches(account->handle()) ||
                    matches(account->displayName()) ||
                    matches(account->channelID()));
        }
        case HiddenUserPlatform::TikTok: {
            const auto account = getApp()->getAccounts()->tiktok.current();
            return account && !account->isAnonymous() &&
                   (matches(account->handle()) || matches(account->userID()));
        }
    }
    return false;
}

QString runHiddenUserCommand(const CommandContext &ctx, bool hidden)
{
    if (!ctx.channel)
    {
        return {};
    }

    const auto command = ctx.words.value(0);
    const auto target = normalizedHiddenCommandTarget(ctx.words.value(1));
    if (target.isEmpty() || ctx.words.size() != 2)
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: %1 <username>").arg(command));
        return {};
    }

    const auto platform = hiddenUserPlatformForChannel(ctx.channel);
    if (!platform)
    {
        ctx.channel->addSystemMessage(
            QStringLiteral(
                "Use %1 in a Twitch, Kick, YouTube, or TikTok channel.")
                .arg(command));
        return {};
    }

    if (hidden && hiddenCommandTargetsCurrentAccount(*platform, target))
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("You cannot hide your active %1 account.")
                .arg(hiddenUserPlatformName(*platform)));
        return {};
    }

    auto *controller = getApp()->getHiddenUsers();
    if (controller == nullptr)
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Unable to update hidden users."));
        return {};
    }

    const bool changed =
        controller->setHidden(*platform, {}, target, {}, hidden);
    const auto name = u'@' + target;
    const auto platformName = hiddenUserPlatformName(*platform);
    if (hidden)
    {
        ctx.channel->addSystemMessage(
            changed ? QStringLiteral("Hidden %1 on %2.").arg(name, platformName)
                    : QStringLiteral("%1 is already hidden on %2.")
                          .arg(name, platformName));
    }
    else
    {
        ctx.channel->addSystemMessage(
            changed
                ? QStringLiteral("Unhidden %1 on %2.").arg(name, platformName)
                : QStringLiteral("%1 is not hidden on %2.")
                      .arg(name, platformName));
    }
    return {};
}

bool selectedTwitchUserMatches(const QString &userId, const QString &login)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (!current || current->isAnon())
    {
        return false;
    }

    const auto currentUserId = current->getUserId().trimmed();
    const auto normalizedUserId = userId.trimmed();
    if (!currentUserId.isEmpty() && !normalizedUserId.isEmpty())
    {
        return normalizedUserId == currentUserId;
    }

    const auto currentLogin = current->getUserName().trimmed().toLower();
    const auto normalizedLogin = login.trimmed().toLower();
    if (currentLogin.isEmpty() || normalizedLogin.isEmpty())
    {
        return false;
    }

    return normalizedLogin == currentLogin;
}

QString normalizeFollowError(bool unfollow, const QString &error)
{
    auto normalized =
        MoltorinoAuth::normalizeAuthError(followActionNoun(unfollow), error);
    if (normalized.contains("failed integrity check", Qt::CaseInsensitive))
    {
        normalized =
            "This saved login cannot use follow commands. Re-login with "
            "Device Login in Settings -> Moltorino -> Authentication, then "
            "try again.";
    }
    return normalized;
}

QString formatNameHistoryRow(const TwitchNameHistoryEntry &entry)
{
    return QStringLiteral("%1: %2 - %3")
        .arg(entry.login, entry.leftText, entry.rightText);
}

struct ModLogRange {
    int days = 7;
    QString text = QStringLiteral("last 7 days");
};

enum class ModLogRangeParseState {
    NotRange,
    Valid,
    Invalid,
};

struct ModLogRangeParseResult {
    ModLogRangeParseState state = ModLogRangeParseState::NotRange;
    ModLogRange range;
};

QString modLogRangeText(int amount, QChar unit)
{
    switch (unit.toLower().unicode())
    {
        case 'd':
            if (amount == 1)
            {
                return QStringLiteral("last 24 hours");
            }
            return QStringLiteral("last %1 days").arg(amount);
        case 'w':
            return QStringLiteral("last %1 %2")
                .arg(amount)
                .arg(amount == 1 ? QStringLiteral("week")
                                 : QStringLiteral("weeks"));
        case 'm':
            return QStringLiteral("last %1 %2")
                .arg(amount)
                .arg(amount == 1 ? QStringLiteral("month")
                                 : QStringLiteral("months"));
    }

    return QString();
}

ModLogRangeParseResult parseModLogRange(QString value)
{
    value = value.trimmed().toLower();
    if (value.isEmpty())
    {
        return {};
    }

    QChar unit = 'd';
    auto numberPart = value;
    if (value.back().isLetter())
    {
        unit = value.back();
        numberPart.chop(1);
    }

    bool ok = false;
    const auto amount = numberPart.toInt(&ok);
    if (!ok)
    {
        return {};
    }
    if (amount <= 0)
    {
        return {ModLogRangeParseState::Invalid, {}};
    }

    qint64 days = amount;
    if (unit == 'w')
    {
        days = qint64(amount) * 7;
    }
    else if (unit == 'm')
    {
        days = qint64(amount) * 30;
    }
    else if (unit != 'd')
    {
        return {ModLogRangeParseState::Invalid, {}};
    }
    if (days > 3650)
    {
        return {ModLogRangeParseState::Invalid, {}};
    }

    return {
        ModLogRangeParseState::Valid,
        {
            int(days),
            modLogRangeText(amount, unit),
        },
    };
}

void addModLogsReadyMessage(const ChannelPtr &channel,
                            const QString &channelLogin,
                            const QString &rangeText, const QString &reportId)
{
    if (channel == nullptr)
    {
        return;
    }

    MessageBuilder builder;
    const auto prefix =
        QStringLiteral("Moderation report for #%1, %2 is ready. ")
            .arg(channelLogin, rangeText);
    const auto linkText = QStringLiteral("Open report");
    const auto fullText = prefix + linkText;

    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->messageText = fullText;
    builder->searchText = fullText;
    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(prefix, MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(QStringList{linkText}, MessageElementFlag::Text,
                              MessageColor::Link, FontStyle::ChatMediumBold)
        ->setLink({Link::OpenModerationReport, reportId});
    channel->addMessage(builder.release(), MessageContext::Original);
}

void addNameHistorySystemMessage(const ChannelPtr &channel,
                                 const TwitchNameHistory &history)
{
    if (history.entries.empty())
    {
        channel->addSystemMessage("No name history found.");
        return;
    }

    MessageBuilder builder;
    QString searchText;

    for (auto it = history.entries.cbegin(); it != history.entries.cend(); ++it)
    {
        const auto row = formatNameHistoryRow(*it);
        if (!searchText.isEmpty())
        {
            searchText += '\n';
            builder.emplace<LinebreakElement>(MessageElementFlag::Text);
        }
        searchText += row;

        builder.emplace<TextElement>(it->login + ':', MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatMediumBold);
        builder.emplace<TextElement>(" " + it->leftText + " - " + it->rightText,
                                     MessageElementFlag::Text,
                                     MessageColor::System);
    }

    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->messageText = searchText;
    builder->searchText = searchText;

    channel->addMessage(builder.release(), MessageContext::Original);
}

void publishModLogsReport(const ChannelPtr &channel, const QString &channelId,
                          const QString &channelLogin, const ModLogRange &range,
                          const QDateTime &cutoffUtc,
                          const ModerationActionLogScanSnapshot &snapshot,
                          std::vector<HelixModerator> currentModerators,
                          bool currentRosterAvailable,
                          ModerationActionLogScanner *scanner)
{
    ModerationReportContext context{
        .channelId = channelId,
        .channelLogin = channelLogin,
        .rangeText = range.text,
        .cutoffUtc = cutoffUtc,
        .generatedAtUtc = QDateTime::currentDateTimeUtc(),
        .snapshot = snapshot,
        .currentModerators = std::move(currentModerators),
        .outputChannel = channel,
        .currentRosterAvailable = currentRosterAvailable,
    };
    const auto reportId = rememberModerationReport(std::move(context));
    addModLogsReadyMessage(channel, channelLogin, range.text, reportId);
    openRememberedModerationReport(reportId,
                                   &getApp()->getWindows()->getMainWindow());
    scanner->deleteLater();
}

QString commandWordsAfter(const CommandContext &ctx, int wordCount)
{
    return ctx.words.mid(wordCount).join(QLatin1Char(' ')).trimmed();
}

QString supportedTranslationLanguageText()
{
    return QStringLiteral(
        "examples: en, es, pt, fr, de, ja, ko, zh-cn, zh-tw, ar");
}

void addTranslationSystemMessage(const ChannelPtr &channel,
                                 const TranslationResult &result,
                                 const QString &targetLanguage)
{
    if (channel == nullptr)
    {
        return;
    }

    const auto targetName = translationLanguageName(targetLanguage);
    const auto detectedLanguage =
        normalizedLanguageCode(result.detectedLanguage);
    const auto detectedName = translationLanguageName(detectedLanguage);

    QString prefix = QStringLiteral("Translation");
    if (!detectedName.isEmpty() && detectedLanguage != targetLanguage)
    {
        prefix += QStringLiteral(" (%1 -> %2)").arg(detectedName, targetName);
    }
    else if (!targetName.isEmpty())
    {
        prefix += QStringLiteral(" (%1)").arg(targetName);
    }

    channel->addSystemMessage(
        QStringLiteral("%1: %2").arg(prefix, result.translatedText.trimmed()));
}

QString runTranslatePreviewCommand(const CommandContext &ctx,
                                   const QString &targetLanguage,
                                   const QString &message,
                                   const QString &usage)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage(usage);
        return "";
    }

    requestTextTranslation(
        message, targetLanguage, nullptr,
        [channel = ctx.channel, targetLanguage](
            const TranslationResult &result) {
            addTranslationSystemMessage(channel, result, targetLanguage);
        },
        [channel = ctx.channel](const QString &) {
            channel->addSystemMessage(QStringLiteral(
                "Translation failed. Try switching providers in Settings."));
        });

    return "";
}

QString runTranslateSendCommand(const CommandContext &ctx,
                                const QString &targetLanguage,
                                const QString &message)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /tl <language> <message>");
        return "";
    }

    requestTextTranslation(
        message, targetLanguage, nullptr,
        [channel = ctx.channel](const TranslationResult &result) {
            auto translatedText = result.translatedText.trimmed();
            translatedText.replace('\n', ' ');
            if (translatedText.isEmpty())
            {
                channel->addSystemMessage(QStringLiteral(
                    "Translation failed, so nothing was sent. Try switching "
                    "providers in Settings."));
                return;
            }
            if (translatedText.size() > TWITCH_MESSAGE_LIMIT)
            {
                channel->addSystemMessage(QStringLiteral(
                    "The translated message is too long for Twitch."));
                return;
            }

            channel->sendMessage(translatedText);
        },
        [channel = ctx.channel](const QString &) {
            channel->addSystemMessage(QStringLiteral(
                "Translation failed, so nothing was sent. Try switching "
                "providers in Settings."));
        });

    return "";
}

void runNameHistoryLookup(const ChannelPtr &channel, const QString &userId,
                          const QString &targetName,
                          const QString &expectedLogin, bool announceFetch)
{
    if (channel == nullptr)
    {
        return;
    }

    if (const auto cached =
            getCachedTwitchNameHistory(userId, expectedLogin))
    {
        addNameHistorySystemMessage(channel, *cached);
        return;
    }

    if (announceFetch)
    {
        channel->addSystemMessage("Fetching name history...");
    }

    fetchTwitchNameHistoryByUserId(
        userId, expectedLogin,
        [channel](TwitchNameHistory history) {
            addNameHistorySystemMessage(channel, history);
        },
        [channel, targetName](const QString &error) {
            channel->addSystemMessage(
                QString("Failed to fetch name history for %1: %2")
                    .arg(targetName, error));
        });
}

void runFollowMutation(const ChannelPtr &channel,
                       const MoltorinoAuthToken &auth, const QString &targetId,
                       const QString &targetLogin, const QString &targetName,
                       bool unfollow)
{
    const auto requestUserId = auth.userId;
    const auto requestLogin = auth.login;
    auto successCallback = [channel, requestUserId, requestLogin, targetId,
                            targetLogin, targetName, unfollow] {
        detail::rememberFollowingStatus(requestUserId, targetId, !unfollow);
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
            twitchChannel != nullptr &&
            selectedTwitchUserMatches(requestUserId, requestLogin) &&
            (twitchChannel->roomId() == targetId ||
             twitchChannel->getName().compare(targetLogin,
                                               Qt::CaseInsensitive) == 0))
        {
            std::optional<QDateTime> followedAt;
            if (!unfollow)
            {
                followedAt = QDateTime::currentDateTimeUtc();
            }
            twitchChannel->setFollowingStatus(!unfollow, followedAt);
        }

        channel->addSystemMessage(
            unfollow ? QString("You unfollowed %1.").arg(targetName)
                     : QString("You followed %1.").arg(targetName));
    };
    auto failureCallback = [channel, targetName, unfollow](const QString &error) {
        channel->addSystemMessage(
            QString("Failed to %1 %2: %3")
                .arg(followAction(unfollow), targetName,
                     normalizeFollowError(unfollow, error)));
    };

    if (unfollow)
    {
        TwitchGql::unfollowUser(targetId, auth.token, std::move(successCallback),
                                std::move(failureCallback));
    }
    else
    {
        TwitchGql::followUser(targetId, auth.token, std::move(successCallback),
                              std::move(failureCallback));
    }
}

QString runFollowCommand(const CommandContext &ctx, bool unfollow)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    auto target = ctx.words.value(1).trimmed();
    if (target.isEmpty())
    {
        if (ctx.twitchChannel == nullptr)
        {
            ctx.channel->addSystemMessage(
                QString("Usage: /%1 <username>").arg(followAction(unfollow)));
            return "";
        }
        target = ctx.twitchChannel->getName();
    }

    auto [targetLogin, targetId] = parseUserNameOrID(target);
    targetLogin = targetLogin.trimmed().toLower();
    targetId = targetId.trimmed();

    if (targetId.isEmpty() &&
        (targetLogin.isEmpty() ||
         !twitchUserLoginRegexp().match(targetLogin).hasMatch()))
    {
        ctx.channel->addSystemMessage(
            QString("Usage: /%1 [username]").arg(followAction(unfollow)));
        return "";
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&authError);
    if (!auth.hasToken())
    {
        ctx.channel->addSystemMessage(
            authError.isEmpty()
                ? MoltorinoAuth::authRequiredMessage(followActionNoun(unfollow))
                : authError);
        return "";
    }

    if (!targetId.isEmpty())
    {
        runFollowMutation(ctx.channel, auth, targetId,
                          QString(),
                          QString("id:%1").arg(targetId), unfollow);
        return "";
    }

    TwitchGql::getUserByLogin(
        targetLogin, auth.token,
        [channel = ctx.channel, auth, targetLogin, unfollow](
            std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QString("Could not find Twitch user %1.").arg(targetLogin));
                return;
            }

            const auto targetName =
                user->displayName.isEmpty() ? user->login : user->displayName;
            runFollowMutation(channel, auth, user->id, user->login, targetName,
                              unfollow);
        },
        [channel = ctx.channel, targetLogin, unfollow](const QString &error) {
            channel->addSystemMessage(
                QString("Failed to look up %1: %2")
                    .arg(targetLogin, normalizeFollowError(unfollow, error)));
        });

    return "";
}

}  // namespace

QString follow(const CommandContext &ctx)
{
    return runFollowCommand(ctx, false);
}

QString unfollow(const CommandContext &ctx)
{
    return runFollowCommand(ctx, true);
}

QString hideUser(const CommandContext &ctx)
{
    return runHiddenUserCommand(ctx, true);
}

QString unhideUser(const CommandContext &ctx)
{
    return runHiddenUserCommand(ctx, false);
}

QString nameHistory(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    auto target = ctx.words.value(1).trimmed();
    if (target.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /namehistory <username>");
        return "";
    }

    auto [targetLogin, targetId] = parseUserNameOrID(target);
    targetLogin = targetLogin.trimmed().toLower();
    targetId = targetId.trimmed();

    if (!targetId.isEmpty())
    {
        runNameHistoryLookup(ctx.channel, targetId,
                             QString("id:%1").arg(targetId), QString(), true);
        return "";
    }

    if (targetLogin.isEmpty() ||
        !twitchUserLoginRegexp().match(targetLogin).hasMatch())
    {
        ctx.channel->addSystemMessage("Usage: /namehistory <username>");
        return "";
    }

    if (const auto cached = getCachedTwitchNameHistory(QString(), targetLogin))
    {
        addNameHistorySystemMessage(ctx.channel, *cached);
        return "";
    }

    ctx.channel->addSystemMessage("Fetching name history...");

    TwitchGql::getUserByLogin(
        targetLogin, QString(),
        [channel = ctx.channel, targetLogin](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QString("Could not find Twitch user %1.").arg(targetLogin));
                return;
            }

            const auto targetName =
                user->displayName.isEmpty() ? user->login : user->displayName;
            runNameHistoryLookup(channel, user->id, targetName, user->login,
                                 false);
        },
        [channel = ctx.channel, targetLogin](const QString &error) {
            channel->addSystemMessage(
                QString("Failed to look up %1: %2").arg(targetLogin, error));
        });

    return "";
}

QString logs(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /logs <user> [channel]");
        return "";
    }

    QString userName = ctx.words[1];
    stripUserName(userName);
    userName = userName.trimmed();

    QString channelName;
    if (ctx.words.size() > 2)
    {
        channelName = ctx.words[2];
        stripChannelName(channelName);
        channelName = channelName.trimmed();
    }
    else if (ctx.twitchChannel != nullptr)
    {
        channelName = ctx.twitchChannel->getName();
    }

    if (userName.isEmpty() || channelName.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /logs <user> [channel]");
        return "";
    }

    QUrl url(QStringLiteral("https://tv.supa.sh/logs"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("c"), channelName);
    query.addQueryItem(QStringLiteral("u"), userName);
    url.setQuery(query);

    const auto link = url.toString();
    ctx.channel->addSystemMessage(
        QStringLiteral("Logs from %1 in %2: %3")
            .arg(userName, channelName, link));

    return "";
}

QString modLogs(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto usage = QStringLiteral("Usage: /modlogs [range] [channel]");
    auto normalizedCommandArgument = [](QString value) {
        value = value.trimmed();
        stripUserName(value);
        stripChannelName(value);
        return value;
    };

    const auto args = ctx.words.mid(1);
    ModLogRange range;
    QString channelLogin;
    QString channelId;

    if (args.isEmpty())
    {
        const auto defaultChannel = ctx.twitchChannel != nullptr
                                        ? ctx.twitchChannel->getName()
                                        : QString();
        const auto request = requestModerationReport(
            &getApp()->getWindows()->getMainWindow(), defaultChannel);
        if (!request)
        {
            return "";
        }
        range.days = request->days;
        range.text = request->rangeText;
        channelLogin = request->channelLogin;
        if (ctx.twitchChannel != nullptr &&
            channelLogin.compare(ctx.twitchChannel->getName(),
                                 Qt::CaseInsensitive) == 0)
        {
            channelId = ctx.twitchChannel->roomId();
        }
    }
    else
    {
        int argIndex = 0;
        const auto first = normalizedCommandArgument(args.value(argIndex));
        const auto parsedRange = parseModLogRange(first);
        if (parsedRange.state == ModLogRangeParseState::Valid)
        {
            range = parsedRange.range;
            argIndex++;
        }
        else if (parsedRange.state == ModLogRangeParseState::Invalid)
        {
            ctx.channel->addSystemMessage(usage);
            return "";
        }
        else
        {
            channelLogin = first;
            argIndex++;
        }

        if (argIndex < args.size())
        {
            if (!channelLogin.isEmpty())
            {
                ctx.channel->addSystemMessage(usage);
                return "";
            }
            channelLogin = normalizedCommandArgument(args.value(argIndex++));
        }
        if (argIndex < args.size())
        {
            ctx.channel->addSystemMessage(usage);
            return "";
        }
    }

    if (channelLogin.isEmpty() && ctx.twitchChannel != nullptr)
    {
        channelLogin = ctx.twitchChannel->getName();
        channelId = ctx.twitchChannel->roomId();
    }

    if (channelLogin.isEmpty())
    {
        ctx.channel->addSystemMessage(usage);
        return "";
    }

    const auto startScan = [channel = ctx.channel, channelLogin,
                            range](const QString &resolvedChannelId) {
        QString authError;
        const auto auth = MoltorinoAuth::resolveModerationToken(
            resolvedChannelId, channelLogin, &authError);
        if (!auth.hasToken())
        {
            channel->addSystemMessage(authError);
            return;
        }

        const auto beginScanner = [channel, channelLogin, range,
                                   resolvedChannelId](
                                      ModerationActionLogScanRequest request) {
            channel->addSystemMessage(
                QStringLiteral("Building moderation report for #%1...")
                    .arg(channelLogin));
            const auto cutoffUtc = request.cutoffUtc;
            auto *scanner = new ModerationActionLogScanner(std::move(request));
            scanner->onDone =
                [channel, channelLogin, range, resolvedChannelId, cutoffUtc,
                 scanner](const ModerationActionLogScanSnapshot &snapshot) {
                    auto *ivr = getIvr();
                    if (ivr == nullptr)
                    {
                        publishModLogsReport(channel, resolvedChannelId,
                                             channelLogin, range, cutoffUtc,
                                             snapshot, {}, false, scanner);
                        return;
                    }

                    ivr->getModVip(
                        channelLogin,
                        [channel, channelLogin, range, resolvedChannelId,
                         cutoffUtc, scanner,
                         snapshot](std::vector<HelixModerator> moderators,
                                   std::vector<HelixVip>) mutable {
                            publishModLogsReport(
                                channel, resolvedChannelId, channelLogin, range,
                                cutoffUtc, snapshot, std::move(moderators),
                                true, scanner);
                        },
                        [channel, channelLogin, range, resolvedChannelId,
                         cutoffUtc, scanner, snapshot] {
                            publishModLogsReport(channel, resolvedChannelId,
                                                 channelLogin, range, cutoffUtc,
                                                 snapshot, {}, false, scanner);
                        });
                };
            scanner->onError = [channel, scanner](const QString &error) {
                const auto normalized = MoltorinoAuth::normalizeAuthError(
                    "building a moderation report", error);
                channel->addSystemMessage(
                    QStringLiteral("Could not build the moderation report: "
                                   "%1")
                        .arg(normalized));
                scanner->deleteLater();
            };
            scanner->start();
        };

        ModerationActionLogScanRequest request;
        request.channelId = resolvedChannelId;
        request.channelLogin = channelLogin;
        request.oauthToken = auth.token;
        request.cutoffUtc =
            QDateTime::currentDateTimeUtc().addDays(-range.days);
        beginScanner(std::move(request));
    };

    if (!channelId.isEmpty())
    {
        startScan(channelId);
        return "";
    }

    QString readAuthError;
    const auto readAuth = MoltorinoAuth::resolveReadToken(&readAuthError);
    if (!readAuth.hasToken())
    {
        ctx.channel->addSystemMessage(readAuthError);
        return "";
    }

    TwitchGql::getUserByLogin(
        channelLogin, readAuth.token,
        [channel = ctx.channel, channelLogin,
         startScan](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QStringLiteral("Could not find Twitch channel %1.")
                        .arg(channelLogin));
                return;
            }
            startScan(user->id);
        },
        [channel = ctx.channel, channelLogin](const QString &error) {
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "looking up a moderation report channel", error);
            channel->addSystemMessage(QStringLiteral("Failed to look up %1: %2")
                                          .arg(channelLogin, normalized));
        });

    return "";
}

QString translate(const CommandContext &ctx)
{
    const auto targetLanguage = normalizedTranslationTargetLanguage(
        getSettings()->messageTranslationTargetLanguage.getValue());
    return runTranslatePreviewCommand(
        ctx, targetLanguage, commandWordsAfter(ctx, 1),
        QStringLiteral("Usage: /translate <message>"));
}

QString translateTo(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto targetLanguage = translationLanguageCodeFromInput(
        ctx.words.value(1));
    if (targetLanguage.isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: /translateto <language> <message> (%1)")
                .arg(supportedTranslationLanguageText()));
        return "";
    }

    return runTranslatePreviewCommand(
        ctx, targetLanguage, commandWordsAfter(ctx, 2),
        QStringLiteral("Usage: /translateto <language> <message>"));
}

QString sayTranslate(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto targetLanguage = translationLanguageCodeFromInput(
        ctx.words.value(1));
    if (targetLanguage.isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: /tl <language> <message> (%1)")
                .arg(supportedTranslationLanguageText()));
        return "";
    }

    return runTranslateSendCommand(ctx, targetLanguage,
                                   commandWordsAfter(ctx, 2));
}

QString uptime(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /uptime command only works in Twitch Channels.");
        return "";
    }

    const auto &streamStatus = ctx.twitchChannel->accessStreamStatus();

    QString messageText =
        streamStatus->live ? streamStatus->uptime : "Channel is not live.";

    ctx.channel->addSystemMessage(messageText);

    return "";
}

QString user(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /user <user> [channel]");
        return "";
    }

    QString userName = ctx.words[1];
    stripUserName(userName);

    QString channelName = ctx.channel->getName();
    if (ctx.words.size() > 2)
    {
        channelName = ctx.words[2];
        stripChannelName(channelName);
    }

    if (userName.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /user <user> [channel]");
        return "";
    }

    QDesktopServices::openUrl(
        QUrl(QString("https://www.twitch.tv/popout/%1/viewercard/%2")
                 .arg(channelName, userName)));
    return "";
}

QString vanity(const CommandContext &ctx)
{
    if (ctx.channel == nullptr || ctx.twitchChannel == nullptr ||
        ctx.channel->getType() != Channel::Type::Twitch)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "Open a Twitch channel before using /vanity.");
        }
        return {};
    }

    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon())
    {
        ctx.channel->addSystemMessage(
            "Log in to Twitch before opening the vanity editor.");
        return {};
    }

    VanityDialog::showDialog(ctx.twitchChannel->sharedFromThis(),
                             &getApp()->getWindows()->getMainWindow());
    return {};
}

QString selfbot(const CommandContext &ctx)
{
    QString initialChannel;
    if (ctx.twitchChannel != nullptr)
    {
        initialChannel = ctx.twitchChannel->getName();
    }
    else if (const auto *multi =
                 dynamic_cast<const MultiChannel *>(ctx.channel.get()))
    {
        if (const auto *active = multi->activeChannel();
            active != nullptr &&
            active->platform == MultiChannel::Platform::Twitch)
        {
            initialChannel = active->channel->getName();
        }
        else
        {
            for (const auto &child : multi->channels())
            {
                if (child.platform == MultiChannel::Platform::Twitch)
                {
                    initialChannel = child.channel->getName();
                    break;
                }
            }
        }
    }

    ChatAutomationDialog::showDialog(std::move(initialChannel),
                                     &getApp()->getWindows()->getMainWindow());
    return {};
}

QString rewardRequests(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage("Usage: /rewardrequests [channel]");
            return "";
        }
    }

    stripChannelName(target);
    target = target.trimmed().toLower();
    if (target.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /rewardrequests [channel]");
        return "";
    }
#if !MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    QDesktopServices::openUrl(QUrl(
        QString("https://www.twitch.tv/popout/%1/reward-queue").arg(target)));
    return "";
#else
    QString channelId;
    if (ctx.twitchChannel != nullptr &&
        target.compare(ctx.twitchChannel->getName(), Qt::CaseInsensitive) == 0)
    {
        channelId = ctx.twitchChannel->roomId();
    }

    const auto openQueue = [channel = ctx.channel,
                            target](const QString &resolvedChannelId) {
        QString error;
        const auto auth = MoltorinoAuth::resolveModerationToken(
            resolvedChannelId, target, &error);
        if (!auth.hasToken())
        {
            channel->addSystemMessage(
                error.isEmpty()
                    ? MoltorinoAuth::authRequiredMessage("reward requests")
                    : error);
            return;
        }
        RewardRequestQueueDialog::showDialog(
            resolvedChannelId, target, channel,
            &getApp()->getWindows()->getMainWindow());
    };

    if (!channelId.isEmpty())
    {
        openQueue(channelId);
        return "";
    }

    QString readError;
    const auto readAuth = MoltorinoAuth::resolveReadToken(&readError);
    if (!readAuth.hasToken())
    {
        ctx.channel->addSystemMessage(readError);
        return "";
    }
    TwitchGql::getUserByLogin(
        target, readAuth.token,
        [channel = ctx.channel, target,
         openQueue](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QStringLiteral("Could not find Twitch channel %1.")
                        .arg(target));
                return;
            }
            openQueue(user->id);
        },
        [channel = ctx.channel, target](const QString &error) {
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "opening reward requests", error);
            channel->addSystemMessage(
                QStringLiteral("Could not open reward requests for #%1: %2")
                    .arg(target, normalized));
        });

    return "";
#endif
}

QString unbanRequests(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));
    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            ctx.twitchChannel != nullptr && !ctx.twitchChannel->isEmpty())
        {
            target = ctx.twitchChannel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage("Usage: /unbanrequests [channel]");
            return "";
        }
    }

    stripChannelName(target);
    target = target.trimmed().toLower();
    if (target.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /unbanrequests [channel]");
        return "";
    }
    QString channelId;
    if (ctx.twitchChannel != nullptr &&
        target.compare(ctx.twitchChannel->getName(), Qt::CaseInsensitive) == 0)
    {
        channelId = ctx.twitchChannel->roomId();
    }

    const auto openQueue = [channel = ctx.channel,
                            target](const QString &resolvedChannelId) {
        QString error;
        const auto auth = MoltorinoAuth::resolveModerationToken(
            resolvedChannelId, target, &error);
        if (!auth.hasToken())
        {
            channel->addSystemMessage(
                error.isEmpty()
                    ? MoltorinoAuth::authRequiredMessage("unban requests")
                    : error);
            return;
        }
        UnbanRequestsDialog::showDialog(
            resolvedChannelId, target, channel,
            &getApp()->getWindows()->getMainWindow());
    };

    if (!channelId.isEmpty())
    {
        openQueue(channelId);
        return "";
    }

    QString readError;
    const auto readAuth = MoltorinoAuth::resolveReadToken(&readError);
    if (!readAuth.hasToken())
    {
        ctx.channel->addSystemMessage(readError);
        return "";
    }
    TwitchGql::getUserByLogin(
        target, readAuth.token,
        [channel = ctx.channel, target,
         openQueue](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QStringLiteral("Could not find Twitch channel %1.")
                        .arg(target));
                return;
            }
            openQueue(user->id);
        },
        [channel = ctx.channel, target](const QString &error) {
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "opening unban requests", error);
            channel->addSystemMessage(
                QStringLiteral("Could not open unban requests for #%1: %2")
                    .arg(target, normalized));
        });

    return "";
}

static QString crossChannelAction(const CommandContext &ctx,
                                  CrossChannelAction action)
{
    const bool ban = action == CrossChannelAction::Ban;
    const auto command =
        ban ? QStringLiteral("/crossban") : QStringLiteral("/crossunban");
    const auto feature =
        ban ? QStringLiteral("Cross ban") : QStringLiteral("Cross unban");
    if (ctx.channel == nullptr)
    {
        return "";
    }
    if (ctx.twitchChannel == nullptr || ctx.twitchChannel->isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("The %1 command only works in Twitch channels.")
                .arg(command));
        return "";
    }
    if (ctx.words.size() != 2)
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: %1 <username>").arg(command));
        return "";
    }

    QString target = ctx.words.at(1);
    stripUserName(target);
    target = target.trimmed().toLower();
    static const QRegularExpression validLogin(
        QStringLiteral("^[a-z0-9_]{1,25}$"));
    if (!validLogin.match(target).hasMatch())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Invalid Twitch username: %1").arg(target));
        return "";
    }

    const auto channelId = ctx.twitchChannel->roomId();
    const auto channelLogin = ctx.twitchChannel->getName();
    if (channelId.isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("%1 is still waiting for this channel's Twitch "
                           "ID. Try again in a moment.")
                .arg(feature));
        return "";
    }

    QString authError;
    auto auth = MoltorinoAuth::resolveModerationToken(channelId, channelLogin,
                                                      &authError);
    const bool canModerateCurrentChannel = auth.hasToken() && !auth.legacy;
    if (!auth.hasToken() &&
        getSettings()->showCrossActionsInUnmoderatedChannels)
    {
        auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    }
    if (!auth.hasToken() || auth.legacy)
    {
        ctx.channel->addSystemMessage(
            !authError.isEmpty()
                ? authError
                : QStringLiteral("%1 needs a saved Twitch account in "
                                 "Moltorino Authentication.")
                      .arg(feature));
        return "";
    }
    if (ban && target.compare(auth.login, Qt::CaseInsensitive) == 0)
    {
        ctx.channel->addSystemMessage("You cannot cross ban your own account.");
        return "";
    }

    const auto savedAccounts = MoltorinoAuth::accounts();
    const auto accountIt = std::ranges::find_if(
        savedAccounts, [&auth](const MoltorinoAuthAccount &account) {
            return account.enabled && account.valid &&
                   account.userId == auth.userId &&
                   account.token.trimmed() == auth.token.trimmed();
        });
    if (accountIt == savedAccounts.end())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral(
                "%1 could not find the saved account for this action. "
                "Refresh accounts in Moltorino Authentication.")
                .arg(feature));
        return "";
    }

    QVector<MoltorinoAuthChannel> channels = accountIt->moderatedChannels;
    const auto addChannel = [&channels](MoltorinoAuthChannel channel) {
        if (channel.id.trimmed().isEmpty() || channel.login.trimmed().isEmpty())
        {
            return;
        }
        if (std::ranges::none_of(channels, [&channel](const auto &existing) {
                return existing.id == channel.id;
            }))
        {
            channels.push_back(std::move(channel));
        }
    };
    addChannel({accountIt->userId, accountIt->login, accountIt->displayName});
    if (canModerateCurrentChannel)
    {
        addChannel({channelId, channelLogin, ctx.channel->getLocalizedName()});
    }

    TwitchGql::getUserByLogin(
        target, auth.token,
        [channel = ctx.channel, target, action, auth, channelId,
         channels = std::move(channels)](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QStringLiteral("Could not find Twitch user %1.")
                        .arg(target));
                return;
            }
            CrossBanDialog::showDialog(
                action, user->id, user->login, user->displayName, auth.login,
                auth.token, channels, channelId, channel,
                &getApp()->getWindows()->getMainWindow());
        },
        [channel = ctx.channel, target, feature](const QString &error) {
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                QStringLiteral("opening %1").arg(feature.toLower()), error);
            channel->addSystemMessage(
                QStringLiteral("Could not open %1 for %2: %3")
                    .arg(feature.toLower(), target, normalized));
        });

    return "";
}

QString crossBan(const CommandContext &ctx)
{
    return crossChannelAction(ctx, CrossChannelAction::Ban);
}

QString crossUnban(const CommandContext &ctx)
{
    return crossChannelAction(ctx, CrossChannelAction::Unban);
}

QString lowtrust(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "Usage: /lowtrust [channel]. You can also use the command "
                "without arguments in any Twitch channel to open its "
                "suspicious user activity feed. Only the broadcaster and "
                "moderators have permission to view this feed.");
            return "";
        }
    }

    stripChannelName(target);
    QDesktopServices::openUrl(QUrl(
        QString("https://www.twitch.tv/popout/moderator/%1/low-trust-users")
            .arg(target)));

    return "";
}

QString clip(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (const auto type = ctx.channel->getType();
        type != Channel::Type::Twitch && type != Channel::Type::TwitchWatching)
    {
        ctx.channel->addSystemMessage(
            "The /clip command only works in Twitch Channels.");
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /clip command only works in Twitch Channels.");
        return "";
    }

    QString title = "";
    std::optional<int> duration = std::nullopt;
    if (!ctx.words.empty())
    {
        QCommandLineParser parser;
        parser.setSingleDashWordOptionMode(
            QCommandLineParser::ParseAsLongOptions);
        parser.setOptionsAfterPositionalArgumentsMode(
            QCommandLineParser::ParseAsPositionalArguments);
        parser.addPositionalArgument("title", "The title of the clip");

        QCommandLineOption durationOption(
            {"d", "duration"}, "The duration of the clip", "duration");
        parser.addOptions({
            durationOption,
        });
        parser.parse(ctx.words);

        title = parser.positionalArguments().join(' ');

        if (parser.isSet(durationOption))
        {
            bool ok = false;
            duration = parser.value(durationOption).toInt(&ok);

            if (!ok)
            {
                ctx.channel->addSystemMessage(
                    "Could not parse clip duration to an integer.");
                return "";
            }
        }
    }

    ctx.twitchChannel->createClip(title, duration);

    return "";
}

QString marker(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /marker command only works in Twitch channels.");
        return "";
    }

    // Avoid Helix calls without Client ID and/or OAuth Token
    if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You need to be logged in to create stream markers!");
        return "";
    }

    // Exact same message as in webchat
    if (!ctx.twitchChannel->isLive())
    {
        ctx.channel->addSystemMessage(
            "You can only add stream markers during live streams. Try "
            "again when the channel is live streaming.");
        return "";
    }

    auto arguments = ctx.words;
    arguments.removeFirst();

    getHelix()->createStreamMarker(
        // Limit for description is 140 characters, webchat just crops description
        // if it's >140 characters, so we're doing the same thing
        ctx.twitchChannel->roomId(), arguments.join(" ").left(140),
        [channel{ctx.channel},
         arguments](const HelixStreamMarker &streamMarker) {
            channel->addSystemMessage(
                QString("Successfully added a stream marker at %1%2")
                    .arg(formatTime(streamMarker.positionSeconds))
                    .arg(streamMarker.description.isEmpty()
                             ? ""
                             : QString(": \"%1\"")
                                   .arg(streamMarker.description)));
        },
        [channel{ctx.channel}](auto error) {
            QString errorMessage("Failed to create stream marker - ");

            switch (error)
            {
                case HelixStreamMarkerError::UserNotAuthorized: {
                    errorMessage +=
                        "you don't have permission to perform that action.";
                }
                break;

                case HelixStreamMarkerError::UserNotAuthenticated: {
                    errorMessage += "you need to re-authenticate.";
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixStreamMarkerError::Unknown:
                default: {
                    errorMessage += "an unknown error occurred.";
                }
                break;
            }

            channel->addSystemMessage(errorMessage);
        });

    return "";
}

QString streamlink(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else if (ctx.kickChannel)
        {
            target = ctx.kickChannel->slug();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "/streamlink [channel or URL]. Open a Twitch channel or "
                "supported stream URL in Streamlink. Without an argument, "
                "the current Twitch or Kick channel is used.");
            return "";
        }
    }

    stripChannelName(target);
    if (ctx.kickChannel)
    {
        openStreamlinkForChannel(target, u"kick.com/");
    }
    else
    {
        openStreamlinkForChannel(target);
    }

    return "";
}

QString popout(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "Usage: /popout <channel>. You can also use the command "
                "without arguments in any Twitch channel to open its "
                "popout chat.");
            return "";
        }
    }

    stripChannelName(target);
    QDesktopServices::openUrl(QUrl(
        QString("https://www.twitch.tv/popout/%1/chat?popout=").arg(target)));

    return "";
}

QString popup(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    static const auto *usageMessage =
        "Usage: /popup [channel]. Open specified Twitch channel in "
        "a new window. If no channel argument is specified, open "
        "the currently selected split instead.";

    QString target(ctx.words.value(1));
    stripChannelName(target);

    // Popup the current split
    if (target.isEmpty())
    {
        auto *currentPage =
            dynamic_cast<SplitContainer *>(getApp()
                                               ->getWindows()
                                               ->getMainWindow()
                                               .getNotebook()
                                               .getSelectedPage());
        if (currentPage != nullptr)
        {
            auto *currentSplit = currentPage->getSelectedSplit();
            if (currentSplit != nullptr)
            {
                currentSplit->popup();

                return "";
            }
        }

        ctx.channel->addSystemMessage(usageMessage);
        return "";
    }

    // Open channel passed as argument in a popup
    auto targetChannel = getApp()->getTwitch()->getOrAddChannel(target);
    getApp()->getWindows()->openInPopup(targetChannel);

    return "";
}

QString clearmessages(const CommandContext &ctx)
{
    (void)ctx;

    auto *currentPage = getApp()
                            ->getWindows()
                            ->getLastSelectedWindow()
                            ->getNotebook()
                            .getSelectedPage();

    if (auto *split = currentPage ? currentPage->getSelectedSplit() : nullptr)
    {
        split->getChannelView().clearMessages();
    }

    return "";
}

QString openURL(const CommandContext &ctx)
{
    /**
     * The /openurl command
     * Takes a positional argument as the URL to open
     *
     * Accepts the option --private or --no-private (or --incognito or --no-incognito).
     * These options will force the URL to be opened in private or non-private mode, regardless of the
     * default incognito mode setting.
     *
     * Examples:
     *  - /openurl https://twitch.tv/forsen
     *    with the setting "Open links in incognito/private mode" enabled
     *    Opens https://twitch.tv/forsen in private mode
     *  - /openurl https://twitch.tv/forsen
     *    with the setting "Open links in incognito/private mode" disabled
     *    Opens https://twitch.tv/forsen in normal mode
     *  - /openurl https://twitch.tv/forsen --private
     *    with the setting "Open links in incognito/private mode" disabled
     *    Opens https://twitch.tv/forsen in private mode
     *  - /openurl https://twitch.tv/forsen --no-private
     *    with the setting "Open links in incognito/private mode" enabled
     *    Opens https://twitch.tv/forsen in normal mode
     */
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QCommandLineParser parser;
    parser.setOptionsAfterPositionalArgumentsMode(
        QCommandLineParser::ParseAsPositionalArguments);
    parser.addPositionalArgument("URL", "The URL to open");
    QCommandLineOption privateModeOption(
        {
            "private",
            "incognito",
        },
        "Force private mode. Cannot be used together with --no-private");
    QCommandLineOption noPrivateModeOption(
        {
            "no-private",
            "no-incognito",
        },
        "Force non-private mode. Cannot be used together with --private");
    parser.addOptions({
        privateModeOption,
        noPrivateModeOption,
    });
    parser.parse(ctx.words);

    const auto &positionalArguments = parser.positionalArguments();
    if (positionalArguments.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "Usage: /openurl <URL> [--incognito/--no-incognito]");
        return "";
    }
    auto urlString = parser.positionalArguments().join(' ');

    QUrl url = QUrl::fromUserInput(urlString);
    if (!url.isValid())
    {
        ctx.channel->addSystemMessage("Invalid URL specified.");
        return "";
    }

    auto preferPrivateMode = getSettings()->openLinksIncognito.getValue();
    auto forcePrivateMode = parser.isSet(privateModeOption);
    auto forceNonPrivateMode = parser.isSet(noPrivateModeOption);

    if (forcePrivateMode && forceNonPrivateMode)
    {
        ctx.channel->addSystemMessage(
            "Error: /openurl may only be called with --incognito or "
            "--no-incognito, not both at the same time.");
        return "";
    }

    bool usePrivateMode = false;

    if (forceNonPrivateMode)
    {
        usePrivateMode = false;
    }
    else if (supportsIncognitoLinks() &&
             (forcePrivateMode || preferPrivateMode))
    {
        usePrivateMode = true;
    }

    bool res = false;
    if (usePrivateMode)
    {
        res = openLinkIncognito(url.toString(QUrl::FullyEncoded));
    }
    else
    {
        res = QDesktopServices::openUrl(url);
    }

    if (!res)
    {
        ctx.channel->addSystemMessage("Could not open URL.");
    }

    return "";
}

QString sendRawMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.channel->isTwitchChannel())
    {
        getApp()->getTwitch()->sendRawMessage(ctx.words.mid(1).join(" "));
    }
    else
    {
        // other code down the road handles this for IRC
        return ctx.words.join(" ");
    }
    return "";
}

QString injectFakeMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (!ctx.channel->isTwitchChannel())
    {
        ctx.channel->addSystemMessage(
            "The /fakemsg command only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            "Usage: /fakemsg (raw irc text) - injects raw irc text as "
            "if it was a message received from TMI");
        return "";
    }

    auto ircText = ctx.words.mid(1).join(" ");
    getApp()->getTwitch()->addFakeMessage(ircText);

    return "";
}

QString injectStreamUpdateNoStream(const CommandContext &ctx)
{
    /**
     * /debug-update-to-no-stream makes the current channel mimic going offline
     */
    if (ctx.channel == nullptr)
    {
        return "";
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /debug-update-to-no-stream command only "
            "works in Twitch channels");
        return "";
    }

    ctx.twitchChannel->updateStreamStatus(std::nullopt, false);
    return "";
}

QString copyToClipboard(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /copy <text> - copies provided "
                                      "text to clipboard.");
        return "";
    }

    crossPlatformCopy(ctx.words.mid(1).join(" "));
    return "";
}

QString unstableSetUserClientSideColor(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /unstable-set-user-color command only "
            "works in Twitch channels.");
        return "";
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            QString("Usage: %1 <TwitchUserID> [color]").arg(ctx.words.at(0)));
        return "";
    }

    auto userID = ctx.words.at(1);

    auto color = ctx.words.value(2);

    getApp()->getUserData()->setUserColor(userID, color);

    return "";
}

QString openUsercard(const CommandContext &ctx)
{
    auto channel = ctx.channel;

    if (channel == nullptr)
    {
        return "";
    }

    QString userName;
    if (ctx.words.size() < 2)
    {
        if (ctx.kickChannel != nullptr)
        {
            const auto currentUser = getApp()->getAccounts()->kick.current();
            if (!currentUser || currentUser->isAnonymous())
            {
                channel->addSystemMessage(
                    "Log in to a Kick account to open your own usercard, or "
                    "use /usercard <username>.");
                return "";
            }
            userName = currentUser->username();
        }
        else
        {
            const auto currentUser =
                getApp()->getAccounts()->twitch.getCurrent();
            if (!currentUser || currentUser->isAnon())
            {
                channel->addSystemMessage(
                    "Log in to a Twitch account to open your own usercard, "
                    "or use /usercard <username>.");
                return "";
            }
            userName = currentUser->getUserName();
        }
    }
    else
    {
        userName = ctx.words[1];
    }
    stripUserName(userName);

    if (ctx.words.size() > 2)
    {
        QString channelName = ctx.words[2];
        stripChannelName(channelName);

        ChannelPtr channelTemp =
            ctx.kickChannel != nullptr
                ? getApp()->getKickChatServer()->findBySlug(channelName)
                : getApp()->getTwitch()->getChannelOrEmpty(channelName);

        if (channelTemp == nullptr || channelTemp->isEmpty())
        {
            channel->addSystemMessage(
                "A usercard can only be displayed for a channel that is "
                "currently opened in Chatterino.");
            return "";
        }

        channel = channelTemp;
    }

    // try to link to current split if possible
    Split *currentSplit = nullptr;
    auto *currentPage = dynamic_cast<SplitContainer *>(getApp()
                                                           ->getWindows()
                                                           ->getMainWindow()
                                                           .getNotebook()
                                                           .getSelectedPage());
    if (currentPage != nullptr)
    {
        currentSplit = currentPage->getSelectedSplit();
    }

    auto differentChannel =
        currentSplit != nullptr && !splitContainsChannel(currentSplit, channel);
    if (differentChannel || currentSplit == nullptr)
    {
        currentSplit = nullptr;
        // not possible to use current split, try searching for one
        const auto &notebook =
            getApp()->getWindows()->getMainWindow().getNotebook();
        auto count = notebook.getPageCount();
        for (int i = 0; i < count; i++)
        {
            auto *page = notebook.getPageAt(i);
            auto *container = dynamic_cast<SplitContainer *>(page);
            assert(container != nullptr);
            for (auto *split : container->getSplits())
            {
                if (splitContainsChannel(split, channel))
                {
                    currentSplit = split;
                    break;
                }
            }
        }

        // This would have crashed either way.
        assert(currentSplit != nullptr &&
               "something went HORRIBLY wrong with the /usercard "
               "command. It couldn't find a split for a channel which "
               "should be open.");
    }

    auto *userPopup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, currentSplit);
    userPopup->setData(userName, channel);

    QPoint center = QCursor::pos();
    if (currentSplit != nullptr && currentSplit->window() != nullptr)
    {
        center = currentSplit->window()->geometry().center();
    }

    userPopup->show();
    const auto size = userPopup->size();
    userPopup->showAndMoveTo(
        center - QPoint(size.width() / 2, size.height() / 2),
        widgets::BoundsChecking::DesiredPosition);
    userPopup->raise();
    userPopup->activateWindow();
    return "";
}

}  // namespace chatterino::commands
