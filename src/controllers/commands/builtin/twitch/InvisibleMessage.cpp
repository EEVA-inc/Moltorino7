#include "controllers/commands/builtin/twitch/InvisibleMessage.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"

#include <memory>

namespace chatterino::commands {

QString sendInvisibleMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return {};
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /invis command only works in Twitch channels.");
        return {};
    }

    const auto message = ctx.rawText.trimmed().section(u' ', 1).trimmed();
    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /invis <message>");
        return {};
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&authError);
    if (!auth.hasToken())
    {
        ctx.channel->addSystemMessage(authError.isEmpty()
                                          ? MoltorinoAuth::authRequiredMessage(
                                                "sending invisible messages")
                                          : authError);
        return {};
    }

    const auto channel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);
    if (!channel)
    {
        ctx.channel->addSystemMessage(
            "The /invis command only works in Twitch channels.");
        return {};
    }

    getApp()->getTwitch()->sendInvisibleMessage(channel, message, auth.token);
    return {};
}

}
