// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/CommandSource.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/builtin/twitch/Chatters.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/Helpers.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/potat/PotatCommands.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <QHash>
#include <QProcess>
#include <QSet>

#include <algorithm>

namespace chatterino::completion {

namespace {

QString commandUsage(const QString &command)
{
    static const QHash<QString, QString> usages{
        {"/announce", "<message>"},
        {"/announceblue", "<message>"},
        {"/announcegreen", "<message>"},
        {"/announceorange", "<message>"},
        {"/announcepurple", "<message>"},
        {"/ban", "<username> [reason]"},
        {"/banid", "<user-id> [reason]"},
        {"/block", "<username>"},
        {"/blockterm", "<term>"},
        {"/bot", "[message]"},
        {"/cancelpoll", ""},
        {"/cancelprediction", ""},
        {"/chatters", ""},
        {"/clear", ""},
        {"/clearmessages", ""},
        {"/clip", ""},
        {"/color", "<color>"},
        {"/commercial", "<length>"},
        {"/completeprediction", "<outcome>"},
        {"/copy", "<text>"},
        {"/crossban", "<username>"},
        {"/crossunban", "<username>"},
        {"/debug-args", ""},
        {"/debug-env", ""},
        {"/debug-eventsub", ""},
        {"/debug-force-image-gc", ""},
        {"/debug-force-image-unload", ""},
        {"/debug-force-layout-channel-views", ""},
        {"/debug-increment-image-generation", ""},
        {"/debug-invalidate-buffers", ""},
        {"/debug-kick-raw-event", ""},
        {"/debug-test", ""},
        {"/debug-update-to-no-stream", ""},
        {"/delete", "<message-id>"},
        {"/disconnect", ""},
        {"/editor", "<username>"},
        {"/emoteonly", ""},
        {"/emoteonlyoff", ""},
        {"/endpoll", ""},
        {"/fakemsg", "<message>"},
        {"/follow", "[username]"},
        {"/followers", "[duration]"},
        {"/followersoff", ""},
        {"/founders", ""},
        {"/gigantify", "<Twitch emote>"},
        {"/help", ""},
        {"/hide", "<username>"},
        {"/hideuser", "<username>"},
        {"/host", "<username>"},
        {"/ignore", "<username>"},
        {"/invis", "<message>"},
        {"/leadmod", "<username>"},
        {"/lockprediction", ""},
        {"/logs", "<username> [channel]"},
        {"/lowtrust", "<username>"},
        {"/marker", "[description]"},
        {"/me", "<message>"},
        {"/mod", "<username>"},
        {"/modlogs", "[range] [channel]"},
        {"/mods", ""},
        {"/monitor", "<username>"},
        {"/namehistory", "<username>"},
        {"/nuke", "<text> <timeout duration|ban|delete> <range>"},
        {"/openurl", "<url> [--incognito|--no-incognito]"},
        {"/pin", "<messageid/message/username>"},
        {"/poll", ""},
        {"/popout", "[channel]"},
        {"/popup", "[channel]"},
        {"/prediction", ""},
        {"/pyramid", "<height> <message>"},
        {"/raid", "<username>"},
        {"/raidcancel", ""},
        {"/raidsend", ""},
        {"/raw", "<message>"},
        {"/reply", "<username> <message>"},
        {"/rewardrequests", "[channel]"},
        {"/restrict", "<username>"},
        {"/setgame", "<game>"},
        {"/settitle", "<stream title>"},
        {"/shield", ""},
        {"/shieldoff", ""},
        {"/shoutout", "<username>"},
        {"/slow", "[duration]"},
        {"/slowoff", ""},
        {"/spam", "<count> <message>"},
        {"/streamlink", "[channel]"},
        {"/subscribers", ""},
        {"/subscribersoff", ""},
        {"/saytranslate", "<language> <message>"},
        {"/test-chatters", ""},
        {"/timeout", "<username> [duration] [reason]"},
        {"/tl", "<language> <message>"},
        {"/translate", "<message>"},
        {"/translateto", "<language> <message>"},
        {"/unban", "<username>"},
        {"/unbanrequests", "[channel]"},
        {"/unblock", "<username>"},
        {"/unblockterm", "<term>"},
        {"/uneditor", "<username>"},
        {"/unfollow", "[username]"},
        {"/unhide", "<username>"},
        {"/unhideuser", "<username>"},
        {"/unhost", ""},
        {"/unignore", "<username>"},
        {"/unleadmod", "<username>"},
        {"/unmod", "<username>"},
        {"/unmonitor", "<username>"},
        {"/unpin", ""},
        {"/unraid", ""},
        {"/unrestrict", "<username>"},
        {"/untimeout", "<username>"},
        {"/unvip", "<username>"},
        {"/uniquechat", ""},
        {"/uniquechatoff", ""},
        {"/unstable-set-user-color", "<username> <color>"},
        {"/vanity", ""},
        {"/user", "<username> [channel]"},
        {"/usercard", "[username] [channel]"},
        {"/vip", "<username>"},
        {"/vips", ""},
        {"/warn", "<username> <reason>"},
        {"/chatwarnings", ""},
        {".w", "<username> <message>"},
        {"/w", "<username> <message>"},
        {"/whisper", "<username> <message>"},
    };

    return usages.value(command.toLower());
}

void addCommand(const QString &command, std::vector<CommandItem> &out,
                bool useBuiltInUsage)
{
    const auto normalized =
        command.startsWith('/') || command.startsWith('.')
            ? command
            : QStringLiteral("/") + command;
    const auto usage = useBuiltInUsage ? commandUsage(normalized) : QString{};
    const auto hintMode =
        normalized.compare(QStringLiteral("/nuke"), Qt::CaseInsensitive) == 0
            ? CommandHintMode::Nuke
            : CommandHintMode::Dynamic;

    if (command.startsWith('/') || command.startsWith('.'))
    {
        out.push_back({
            .name = command.mid(1),
            .prefix = command.at(0),
            .usage = usage,
            .hintMode = hintMode,
            .showArgumentHint = !usage.isEmpty(),
            .builtIn = useBuiltInUsage,
        });
    }
    else
    {
        out.push_back({
            .name = command,
            .prefix = "",
            .usage = usage,
            .hintMode = hintMode,
            .showArgumentHint = !usage.isEmpty(),
            .builtIn = useBuiltInUsage,
        });
    }
}

const QSet<QString> &gqlModeratorQueueCommands()
{
    static const QSet<QString> commands{
        "/crossban",
        "/crossunban",
        "/rewardrequests",
        "/unbanrequests",
    };
    return commands;
}

bool hasGqlModeratorQueueAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName(), &ignored);
    return auth.hasToken() && !auth.legacy;
}

const QSet<QString> &youtubeModerationCommands()
{
    static const QSet<QString> commands{
        "/ban", "/delete", "/timeout", "/unban", "/untimeout",
    };
    return commands;
}

bool hasSelectedGqlAuth()
{
    QString ignored;
    return MoltorinoAuth::resolveSelectedUserToken(&ignored).hasToken();
}

const QSet<QString> &currentAccountModCommands()
{
    static const QSet<QString> commands{
        "/announce",
        "/announceblue",
        "/announcegreen",
        "/announceorange",
        "/announcepurple",
        "/ban",
        "/banid",
        "/blockterm",
        "/bot",
        "/cancelpoll",
        "/cancelprediction",
        "/clear",
        "/clearmessages",
        "/commercial",
        "/completeprediction",
        "/delete",
        "/emoteonly",
        "/emoteonlyoff",
        "/endpoll",
        "/followers",
        "/followersoff",
        "/host",
        "/marker",
        "/lockprediction",
        "/lowtrust",
        "/mod",
        "/monitor",
        "/nuke",
        "/r9kbeta",
        "/r9kbetaoff",
        "/raid",
        "/rewardrequests",
        "/restrict",
        "/setgame",
        "/settitle",
        "/shield",
        "/shieldoff",
        "/shoutout",
        "/slow",
        "/slowoff",
        "/subscribers",
        "/subscribersoff",
        "/timeout",
        "/unban",
        "/unbanrequests",
        "/unhost",
        "/unblockterm",
        "/unmod",
        "/unmonitor",
        "/unpin",
        "/unraid",
        "/unrestrict",
        "/untimeout",
        "/unvip",
        "/vip",
        "/warn",
        "/chatwarnings",
    };
    return commands;
}

const QSet<QString> &moltorinoModerationCommands()
{
    static const QSet<QString> commands{
        "/blockterm",
        "/cancelpoll",
        "/cancelprediction",
        "/completeprediction",
        "/endpoll",
        "/lockprediction",
        "/modlogs",
        "/pin",
        "/unblockterm",
        "/unpin",
    };
    return commands;
}

const QSet<QString> &currentAccountBroadcasterCommands()
{
    static const QSet<QString> commands{
        "/raid",
        "/raidcancel",
        "/raidsend",
        "/unraid",
    };
    return commands;
}

const QSet<QString> &roleManagementCommands()
{
    static const QSet<QString> commands{
        "/editor",
        "/leadmod",
        "/uneditor",
        "/unleadmod",
    };
    return commands;
}

bool hasMoltorinoModerationAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    return MoltorinoAuth::resolveModerationToken(
               twitchChannel->roomId(), twitchChannel->getName(), &ignored)
        .hasToken();
}

bool hasMoltorinoBroadcasterAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    return MoltorinoAuth::resolveBroadcasterToken(
               twitchChannel->roomId(), twitchChannel->getName(), &ignored)
        .hasToken();
}

bool hasMoltorinoRoleManagementAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    return MoltorinoAuth::resolveSavedBroadcasterToken(
               twitchChannel->roomId(), twitchChannel->getName(), &ignored)
        .hasToken();
}

bool hasBotBadgeAuth()
{
    const auto &settings = *getSettings();
    return !settings.botBadgeAppAccessToken.getValue().trimmed().isEmpty() &&
           !settings.botBadgeClientID.getValue().trimmed().isEmpty() &&
           !settings.botBadgeUserID.getValue().trimmed().isEmpty();
}

QString normalizedCommand(const CommandItem &item)
{
    const auto prefix = item.prefix.isEmpty() ? QStringLiteral("/")
                                              : item.prefix;
    return (prefix + item.name).toLower();
}

bool isInternalCommand(const QString &command)
{
    return command.startsWith(QStringLiteral("/debug-")) ||
           command.startsWith(QStringLiteral("/c2-")) ||
           command.startsWith(QStringLiteral("/unstable-")) ||
           command == QStringLiteral("/fakemsg") ||
           command == QStringLiteral("/test-chatters");
}

bool moltorinoFeatureHandlesCommand(const QString &command)
{
    const auto &settings = *getSettings();
    if (command == "/cancelpoll" || command == "/endpoll")
    {
        return settings.enablePolls;
    }
    if (command == "/cancelprediction" || command == "/completeprediction" ||
        command == "/lockprediction")
    {
        return settings.enablePredictions;
    }
    if (command == "/modlogs" || command == "/pin" || command == "/unpin")
    {
        return true;
    }
    if (command == "/blockterm" || command == "/unblockterm")
    {
        return true;
    }
    if (currentAccountBroadcasterCommands().contains(command))
    {
        return true;
    }
    return false;
}

bool needsMoltorinoModerationAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        if (!item.builtIn)
        {
            return false;
        }
        const auto command = normalizedCommand(item);
        return moltorinoFeatureHandlesCommand(command) &&
               (moltorinoModerationCommands().contains(command) ||
                currentAccountBroadcasterCommands().contains(command));
    });
}

bool needsMoltorinoBroadcasterAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        if (!item.builtIn)
        {
            return false;
        }
        const auto command = normalizedCommand(item);
        return moltorinoFeatureHandlesCommand(command) &&
               currentAccountBroadcasterCommands().contains(command);
    });
}

bool needsMoltorinoRoleManagementAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        return item.builtIn &&
               roleManagementCommands().contains(normalizedCommand(item));
    });
}

bool needsGqlModeratorQueueAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        return item.builtIn &&
               gqlModeratorQueueCommands().contains(normalizedCommand(item));
    });
}

bool shouldHideCommand(const CommandItem &item, bool hideUnavailable,
                       bool hasCurrentAccountModRights,
                       bool hasCurrentAccountBroadcasterRights,
                       bool isYouTubeChannel, bool hasMoltorinoModerationAccess,
                       bool hasGqlModeratorQueueAccess,
                       bool hasMoltorinoBroadcasterAccess,
                       bool hasMoltorinoRoleManagementAccess,
                       bool hasBotBadgeAuth, bool hasSelectedGqlAuth,
                       const Channel *channel)
{
    if (!item.builtIn)
    {
        return false;
    }
    const auto command = normalizedCommand(item);
    if (isInternalCommand(command))
    {
        return true;
    }

    if (command == "/chatters" && channel != nullptr)
    {
        return !commands::isChattersCommandAvailable(channel);
    }

    if (roleManagementCommands().contains(command))
    {
        return !hasMoltorinoRoleManagementAccess;
    }

    if (command == "/bot" && !hasBotBadgeAuth)
    {
        return true;
    }

    if (command == "/chatwarnings" && !hasCurrentAccountModRights)
    {
        return true;
    }

    if (command == "/invis")
    {
        if (isYouTubeChannel)
        {
            return true;
        }
        return !hasSelectedGqlAuth;
    }

    if (isYouTubeChannel)
    {
        if (youtubeModerationCommands().contains(command))
        {
            return hideUnavailable && !hasCurrentAccountModRights;
        }
        if (currentAccountModCommands().contains(command) ||
            currentAccountBroadcasterCommands().contains(command) ||
            moltorinoModerationCommands().contains(command))
        {
            return true;
        }
    }

    if (gqlModeratorQueueCommands().contains(command))
    {
        return !hasGqlModeratorQueueAccess;
    }

    if (!hideUnavailable)
    {
        return false;
    }

    if (command == "/modlogs")
    {
        return false;
    }

    if (currentAccountBroadcasterCommands().contains(command))
    {
        return !hasCurrentAccountBroadcasterRights &&
               !(moltorinoFeatureHandlesCommand(command) &&
                 (hasMoltorinoBroadcasterAccess ||
                  hasCurrentAccountModRights ||
                  hasMoltorinoModerationAccess));
    }

    if (hasCurrentAccountModRights)
    {
        return false;
    }

    if (moltorinoModerationCommands().contains(command) &&
        moltorinoFeatureHandlesCommand(command) &&
        hasMoltorinoModerationAccess)
    {
        return false;
    }

    return currentAccountModCommands().contains(command) ||
           moltorinoModerationCommands().contains(command);
}

}  // namespace

QString remainingCommandUsage(const CommandItem &command,
                              const QString &arguments, bool *appendDirectly)
{
    if (appendDirectly)
    {
        *appendDirectly = false;
    }
    if (!command.showArgumentHint || command.usage.isEmpty())
    {
        return {};
    }
    if (command.argumentHint)
    {
        return remainingCommandUsage(*command.argumentHint, arguments,
                                     appendDirectly);
    }
    if (command.hintMode == CommandHintMode::Static)
    {
        return command.usage;
    }

    const auto usageArguments = splitCommandUsageFields(command.usage);
    if (usageArguments.isEmpty())
    {
        return {};
    }

    const auto typed = QProcess::splitCommand(arguments.trimmed());
    if (command.hintMode == CommandHintMode::Nuke)
    {
        if (typed.isEmpty())
        {
            return usageArguments.join(QChar(' '));
        }

        static const QSet<QString> namedActions{
            QStringLiteral("ban"),
            QStringLiteral("delete"),
        };
        const auto isAction = [&](const QString &value) {
            return namedActions.contains(value.toLower()) ||
                   parseDurationToSeconds(value) > 0;
        };
        if (typed.size() < 2)
        {
            return usageArguments.mid(1).join(QChar(' '));
        }
        const auto last = typed.size() - 1;
        if (typed.size() >= 3 && isAction(typed.at(last - 1)))
        {
            return parseDurationToSeconds(typed.at(last)) > 0
                       ? QString{}
                       : usageArguments.at(2);
        }
        if (!isAction(typed.at(last)))
        {
            return usageArguments.mid(1).join(QChar(' '));
        }
        return usageArguments.size() >= 3 ? usageArguments.at(2) : QString{};
    }

    return usageArguments.mid(std::min(typed.size(), usageArguments.size()))
        .join(QChar(' '));
}

CommandSource::CommandSource(std::unique_ptr<CommandStrategy> strategy,
                             ActionCallback callback, const Channel *channel)
    : strategy_(std::move(strategy))
    , callback_(std::move(callback))
    , channel_(channel)
{
    this->initializeItems();
}

void CommandSource::update(const QString &query)
{
    this->output_.clear();
    if (query.startsWith(QChar('#')))
    {
        if (!getSettings()->includePotatCommands ||
            this->strategy_ == nullptr || this->channel_ == nullptr ||
            this->channel_->getType() != Channel::Type::Twitch)
        {
            return;
        }
        auto *potat = getApp()->getPotatCommands();
        if (potat == nullptr)
        {
            return;
        }
        potat->ensureLoaded();
        std::vector<CommandItem> items;
        items.reserve(potat->commands().size());
        for (const auto &command : potat->commands())
        {
            if (command.alias && !getSettings()->showPotatCommandAliases)
            {
                continue;
            }
            items.push_back({
                .name = command.name,
                .prefix = QStringLiteral("#"),
                .usage = command.usage,
                .hintMode = command.dynamicUsage ? CommandHintMode::Dynamic
                                                 : CommandHintMode::Static,
                .showArgumentHint = (!command.alias || command.argumentHint) &&
                                    !command.usage.isEmpty(),
                .argumentHint = command.argumentHint,
            });
        }
        this->strategy_->apply(items, this->output_, query);
        return;
    }
    if (this->strategy_)
    {
        this->strategy_->apply(this->items_, this->output_, query);
        const bool isYouTubeChannel =
            dynamic_cast<const YouTubeChannel *>(this->channel_) != nullptr;
        if (isYouTubeChannel)
        {
            for (auto &item : this->output_)
            {
                if (!item.builtIn)
                {
                    continue;
                }
                const auto command = normalizedCommand(item);
                if (command == "/ban")
                {
                    item.usage = "<name|id:channel-id>";
                }
                else if (command == "/timeout")
                {
                    item.usage = "<name|id:channel-id> [duration]";
                }
                else if (command == "/unban" || command == "/untimeout")
                {
                    item.usage = "<name|id:channel-id>";
                }
                else if (command == "/delete")
                {
                    item.usage = "<message-id>";
                }
            }
        }
        const bool hideUnavailable =
            getSettings()->hideUnavailableModCommands;
        const bool hasCurrentAccountModRights =
            this->channel_ != nullptr && this->channel_->hasModRights();
        const bool hasCurrentAccountBroadcasterRights =
            this->channel_ != nullptr && this->channel_->isBroadcaster();
        const bool needsQueueAccess =
            needsGqlModeratorQueueAccess(this->output_);
        const bool moltorinoAccess =
            hideUnavailable && !hasCurrentAccountModRights &&
            needsMoltorinoModerationAccess(this->output_) &&
            hasMoltorinoModerationAccess(this->channel_);
        const bool gqlModeratorQueueAccess =
            needsQueueAccess && hasGqlModeratorQueueAccess(this->channel_);
        const bool moltorinoBroadcasterAccess =
            hideUnavailable && !hasCurrentAccountBroadcasterRights &&
            needsMoltorinoBroadcasterAccess(this->output_) &&
            hasMoltorinoBroadcasterAccess(this->channel_);
        const bool moltorinoRoleManagementAccess =
            needsMoltorinoRoleManagementAccess(this->output_) &&
            hasMoltorinoRoleManagementAccess(this->channel_);
        const bool botBadgeAuth = hasBotBadgeAuth();
        const bool selectedGqlAuth =
            std::any_of(this->output_.begin(), this->output_.end(),
                        [](const auto &item) {
                            return item.builtIn &&
                                   normalizedCommand(item) == "/invis";
                        }) &&
            hasSelectedGqlAuth();
        this->output_.erase(
            std::remove_if(
                this->output_.begin(), this->output_.end(),
                [&](const auto &item) {
                    return shouldHideCommand(
                        item, hideUnavailable, hasCurrentAccountModRights,
                        hasCurrentAccountBroadcasterRights, isYouTubeChannel,
                        moltorinoAccess, gqlModeratorQueueAccess,
                        moltorinoBroadcasterAccess,
                        moltorinoRoleManagementAccess, botBadgeAuth,
                        selectedGqlAuth, this->channel_);
                }),
            this->output_.end());
    }
}

void CommandSource::addToListModel(GenericListModel &model,
                                   size_t maxCount) const
{
    addVecToListModel(this->output_, model, maxCount,
                      [this](const CommandItem &command) {
                          return std::make_unique<InputCompletionItem>(
                              nullptr, command.name, this->callback_);
                      });
}

void CommandSource::addToStringList(QStringList &list, size_t maxCount,
                                    bool /* isFirstWord */) const
{
    addVecToStringList(this->output_, list, maxCount,
                       [](const CommandItem &command) {
                           return command.prefix + command.name + " ";
                       });
}

qsizetype CommandSource::tabCompletionCount() const
{
    return static_cast<qsizetype>(this->output_.size());
}

const EmoteItem *CommandSource::emoteAtTabCompletionIndex(qsizetype) const
{
    return nullptr;
}

void CommandSource::initializeItems()
{
    std::vector<CommandItem> commands;

#ifdef CHATTERINO_HAVE_PLUGINS
    for (const auto &command : getApp()->getCommands()->pluginCommands())
    {
        addCommand(command, commands, false);
    }
#endif

    // Custom Chatterino commands
    for (const auto &command : getApp()->getCommands()->items)
    {
        addCommand(command.name, commands, false);
    }

    // Default Chatterino commands
    auto x = getApp()->getCommands()->getDefaultChatterinoCommandList();
    for (const auto &command : x)
    {
        addCommand(command, commands, true);
    }

    // Default Twitch commands
    for (const auto &command : TWITCH_DEFAULT_COMMANDS)
    {
        addCommand(command, commands, true);
    }

    this->items_ = std::move(commands);
}

const std::vector<CommandItem> &CommandSource::output() const
{
    return this->output_;
}

}  // namespace chatterino::completion
