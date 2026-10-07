#include "controllers/commands/builtin/kick/ModerationActions.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "util/Helpers.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QString>

#include <memory>
#include <utility>

namespace {

using namespace Qt::Literals::StringLiterals;
using namespace chatterino;

template <typename Fn>
void withUser(KickChannel *channel, const QString &userSpec,
              const QString &action, Fn fn, auto &&...args)
{
    auto onAction = [weakChan = channel->weakFromThis(), action,
                     userSpec](const auto &res) {
        auto chan = weakChan.lock();
        if (!chan || res)
        {
            return;
        }
        chan->addSystemMessage(u"Failed to " % action % ' ' % userSpec % u": " %
                               res.error());
    };

    if (userSpec.startsWith(u"id:"))
    {
        bool valid = false;
        auto userID = QStringView(userSpec).sliced(3).toULongLong(&valid);
        if (!valid || userID == 0)
        {
            channel->addSystemMessage(u"Invalid Kick user ID."_s);
            return;
        }
        (getKickApi()->*fn)(channel->userID(), userID,
                            std::forward<decltype(args)>(args)...,
                            std::move(onAction));
        return;
    }

    auto accountChanged = std::make_shared<bool>(false);
    auto connection = std::make_shared<pajlada::Signals::ScopedConnection>(
        getApp()->getAccounts()->kick.currentUserChanged.connect(
            [accountChanged] {
                *accountChanged = true;
            }));
    KickApi::privateChannelInfo(userSpec, [weakChan = channel->weakFromThis(),
                                           onAction = std::move(onAction),
                                           accountChanged, connection, userSpec,
                                           fn,
                                           ... args =
                                               std::forward<decltype(args)>(
                                                   args)](
                                              const auto &res) mutable {
        auto chan = weakChan.lock();
        if (!chan)
        {
            return;
        }
        if (*accountChanged)
        {
            chan->addSystemMessage(
                u"The Kick account changed while looking up the user. Try again."_s);
            return;
        }
        if (!res)
        {
            chan->addSystemMessage(u"Failed to find user " % userSpec % u": " %
                                   res.error());
            return;
        }
        if (res->user.userID == 0)
        {
            chan->addSystemMessage(u"Failed to find user " % userSpec);
            return;
        }
        (getKickApi()->*fn)(chan->userID(), res->user.userID,
                            std::forward<decltype(args)>(args)...,
                            std::move(onAction));
    });
}

}

namespace chatterino::commands {

QString doKickBan(const CommandContext &ctx)
{
    if (!ctx.kickChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in Kick channels"_s);
        return {};
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(u"Usage: /ban <username> [message...]"_s);
        return {};
    }
    const auto &username = ctx.words.at(1);
    auto reason = ctx.words.sliced(2).join(' ');
    withUser(ctx.kickChannel, username, u"ban"_s, &KickApi::banUser,
             std::nullopt, reason);
    return {};
}

QString doKickTimeout(const CommandContext &ctx)
{
    if (!ctx.kickChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in Kick channels"_s);
        return {};
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            u"Usage: /timeout <username> [duration] [message...]"_s);
        return {};
    }
    const auto &username = ctx.words.at(1);

    std::chrono::minutes duration(10);
    QString reason;
    if (ctx.words.size() >= 3)
    {
        auto seconds = parseDurationToSeconds(ctx.words.at(2), 60);
        if (seconds <= 0)
        {
            ctx.channel->addSystemMessage(u"Invalid duration."_s);
            return {};
        }
        if (seconds < 60)
        {
            ctx.channel->addSystemMessage(
                u"Timeouts shorter than one minute are not supported on Kick."_s);
            return {};
        }
        duration = std::chrono::round<std::chrono::minutes>(
            std::chrono::seconds(seconds));
        reason = ctx.words.sliced(3).join(' ');
    }

    withUser(ctx.kickChannel, username, u"timeout"_s, &KickApi::banUser,
             duration, reason);
    return {};
}

QString doKickUnban(const CommandContext &ctx)
{
    if (!ctx.kickChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in Kick channels"_s);
        return {};
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(u"Usage: /unban <username>"_s);
        return {};
    }
    const auto &username = ctx.words.at(1);
    withUser(ctx.kickChannel, username, u"unban"_s, &KickApi::unbanUser);
    return {};
}

QString doKickDelete(const CommandContext &ctx)
{
    if (!ctx.kickChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in Kick channels"_s);
        return {};
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(u"Usage: /delete <message-id>"_s);
        return {};
    }
    getKickApi()->deleteChatMessage(
        ctx.words.at(1),
        [weakChan = ctx.kickChannel->weakFromThis()](const auto &res) {
            auto chan = weakChan.lock();
            if (!chan || res)
            {
                return;
            }
            chan->addSystemMessage(u"Failed to delete message: " % res.error());
        });
    return {};
}

}
