#include "controllers/commands/builtin/youtube/ModerationActions.hpp"

#include "controllers/commands/CommandContext.hpp"
#include "messages/Message.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeTypes.hpp"
#include "util/Helpers.hpp"

#include <QSet>
#include <QStringBuilder>

#include <chrono>
#include <optional>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

enum class TargetResolution {
    Missing,
    Ambiguous,
    Unique,
};

struct ResolvedTarget {
    TargetResolution resolution = TargetResolution::Missing;
    QString channelID;
    QString displayName;
};

ResolvedTarget resolveTarget(YouTubeChannel *channel, QString userSpec)
{
    userSpec = userSpec.trimmed();
    if (userSpec.startsWith(u"id:"_s, Qt::CaseInsensitive))
    {
        const auto channelID = userSpec.sliced(3).trimmed();
        bool containsWhitespace = false;
        for (const auto character : channelID)
        {
            if (character.isSpace())
            {
                containsWhitespace = true;
                break;
            }
        }
        if (channelID.isEmpty() || containsWhitespace)
        {
            return {};
        }
        return {
            .resolution = TargetResolution::Unique,
            .channelID = channelID,
            .displayName = channelID,
        };
    }

    const auto wanted = visibleYouTubeName(std::move(userSpec));
    if (wanted.isEmpty())
    {
        return {};
    }

    QSet<QString> matchingIDs;
    QString matchedName;
    const auto messages = channel->getMessageSnapshot();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        const auto &message = *it;
        if (message->userID.isEmpty())
        {
            continue;
        }

        const bool matches =
            message->displayName.compare(wanted, Qt::CaseInsensitive) == 0 ||
            message->loginName.compare(wanted, Qt::CaseInsensitive) == 0;
        if (!matches)
        {
            continue;
        }

        matchingIDs.insert(message->userID);
        if (matchedName.isEmpty())
        {
            matchedName = message->displayName;
        }
        if (matchingIDs.size() > 1)
        {
            return {
                .resolution = TargetResolution::Ambiguous,
                .displayName = wanted,
            };
        }
    }

    if (matchingIDs.isEmpty())
    {
        return {
            .resolution = TargetResolution::Missing,
            .displayName = wanted,
        };
    }

    return {
        .resolution = TargetResolution::Unique,
        .channelID = *matchingIDs.constBegin(),
        .displayName = matchedName,
    };
}

void reportTargetError(YouTubeChannel *channel, const ResolvedTarget &target)
{
    if (target.resolution == TargetResolution::Ambiguous)
    {
        channel->addSystemMessage(
            u"More than one YouTube user in this chat is named \""_s %
            target.displayName %
            u"\". Use the message menu or id:<channel-id> to choose the "
            u"right user."_s);
        return;
    }

    channel->addSystemMessage(
        u"Could not find a recent YouTube message from \""_s %
        target.displayName % u"\". Use the message menu or id:<channel-id>."_s);
}

}

namespace chatterino::commands {

QString doYouTubeBan(const CommandContext &ctx)
{
    auto *channel = ctx.youtubeChannel;
    if (channel == nullptr)
    {
        return {};
    }
    if (ctx.words.size() < 2)
    {
        channel->addSystemMessage(u"Usage: /ban <name|id:channel-id>"_s);
        return {};
    }

    auto target = resolveTarget(channel, ctx.words.sliced(1).join(' '));
    if (target.resolution == TargetResolution::Missing && ctx.words.size() > 2)
    {
        for (qsizetype end = 2; end < ctx.words.size(); ++end)
        {
            auto candidate = resolveTarget(
                channel, ctx.words.sliced(1, end - 1).join(' '));
            if (candidate.resolution == TargetResolution::Missing)
            {
                continue;
            }
            if (candidate.resolution == TargetResolution::Ambiguous ||
                (target.resolution == TargetResolution::Unique &&
                 target.channelID != candidate.channelID))
            {
                channel->addSystemMessage(
                    u"That command is ambiguous. "
                    "Use the message menu or specify a channel ID with id:."_s);
                return {};
            }
            target = std::move(candidate);
        }
    }
    if (target.resolution == TargetResolution::Unique)
    {
        channel->moderateUser(target.channelID, std::nullopt);
    }
    else
    {
        reportTargetError(channel, target);
    }
    return {};
}

QString doYouTubeTimeout(const CommandContext &ctx)
{
    auto *channel = ctx.youtubeChannel;
    if (channel == nullptr)
    {
        return {};
    }
    if (ctx.words.size() < 2)
    {
        channel->addSystemMessage(
            u"Usage: /timeout <name|id:channel-id> [duration]"_s);
        return {};
    }

    auto target = resolveTarget(channel, ctx.words.sliced(1).join(' '));
    if (target.resolution == TargetResolution::Ambiguous)
    {
        reportTargetError(channel, target);
        return {};
    }
    std::chrono::seconds duration{600};
    if (target.resolution != TargetResolution::Unique && ctx.words.size() > 2)
    {
        bool foundDuration = false;
        bool foundKnownTargetWithInvalidDuration = false;
        ResolvedTarget durationTarget;
        for (qsizetype split = ctx.words.size() - 1; split >= 2; --split)
        {
            auto candidate = resolveTarget(
                channel, ctx.words.sliced(1, split - 1).join(' '));
            const auto seconds = parseDurationToSeconds(ctx.words.at(split));
            if (seconds <= 0)
            {
                foundKnownTargetWithInvalidDuration |=
                    candidate.resolution == TargetResolution::Unique;
                continue;
            }

            foundDuration = true;
            durationTarget = std::move(candidate);
            if (durationTarget.resolution == TargetResolution::Ambiguous ||
                (durationTarget.resolution == TargetResolution::Unique &&
                 target.resolution == TargetResolution::Unique &&
                 (target.channelID != durationTarget.channelID ||
                  duration.count() != seconds)))
            {
                channel->addSystemMessage(
                    u"That command is ambiguous. "
                    "Use the message menu or specify a channel ID with id:."_s);
                return {};
            }
            if (durationTarget.resolution == TargetResolution::Unique)
            {
                duration = std::chrono::seconds{seconds};
                target = std::move(durationTarget);
            }
        }

        if (target.resolution == TargetResolution::Missing && !foundDuration &&
            foundKnownTargetWithInvalidDuration)
        {
            channel->addSystemMessage(
                u"Invalid YouTube timeout duration. Try 30s, 10m, or 1h."_s);
            return {};
        }
        if (target.resolution != TargetResolution::Unique && foundDuration)
        {
            target = std::move(durationTarget);
        }
    }

    if (target.resolution != TargetResolution::Unique)
    {
        reportTargetError(channel, target);
        return {};
    }

    channel->moderateUser(target.channelID, duration);
    return {};
}

QString doYouTubeUnban(const CommandContext &ctx)
{
    auto *channel = ctx.youtubeChannel;
    if (channel == nullptr)
    {
        return {};
    }
    if (ctx.words.size() < 2)
    {
        channel->addSystemMessage(u"Usage: /unban <name|id:channel-id>"_s);
        return {};
    }

    const auto target =
        resolveTarget(channel, ctx.words.sliced(1).join(' '));
    if (target.resolution != TargetResolution::Unique)
    {
        reportTargetError(channel, target);
        return {};
    }
    channel->unbanUser(target.channelID);
    return {};
}

QString doYouTubeDelete(const CommandContext &ctx)
{
    auto *channel = ctx.youtubeChannel;
    if (channel == nullptr)
    {
        return {};
    }
    if (ctx.words.size() < 2 || ctx.words.at(1).trimmed().isEmpty())
    {
        channel->addSystemMessage(u"Usage: /delete <message-id>"_s);
        return {};
    }

    channel->deleteMessage(ctx.words.at(1).trimmed());
    return {};
}

}
