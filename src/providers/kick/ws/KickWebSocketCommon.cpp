#include "providers/kick/ws/KickWebSocketCommon.hpp"

#include "common/QLogging.hpp"
#include "controllers/recording/ChatRecordingMessage.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json.hpp>
#include <QJsonArray>
#include <QUrl>

#include <algorithm>
#include <charconv>

namespace chatterino::kick::ws {

namespace {

bool validPusherCredential(QStringView value)
{
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](QChar c) {
               return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
                      (c >= u'0' && c <= u'9') || c == u'_' || c == u'-';
           });
}

}

bool stripPrefix(std::string_view &str, std::string_view prefix)
{
    if (str.starts_with(prefix))
    {
        str = str.substr(prefix.size());
        return true;
    }
    return false;
}

bool stripSuffix(std::string_view &str, std::string_view suffix)
{
    if (str.ends_with(suffix))
    {
        str = str.substr(0, str.size() - suffix.size());
        return true;
    }
    return false;
}

IDs parseIDs(std::string_view channel)
{
    bool isChannel = false;
    if (stripPrefix(channel, "chatrooms.") || stripPrefix(channel, "chatroom_"))
    {
        stripSuffix(channel, ".v2");
    }
    else if (stripPrefix(channel, "channel_") ||
             stripPrefix(channel, "channel.") ||
             stripPrefix(channel, "predictions-channel-"))
    {
        isChannel = true;
    }
    else
    {
        return {};
    }

    uint64_t v = 0;
    const auto [end, error] =
        std::from_chars(channel.data(), channel.data() + channel.size(), v);
    if (error != std::errc{} || end != channel.data() + channel.size())
    {
        return {};
    }

    if (isChannel)
    {
        return IDs{.channelID = v};
    }
    return IDs{.roomID = v};
}

QString pusherUrl(QStringView appKey, QStringView cluster)
{
    if (!validPusherCredential(appKey) || !validPusherCredential(cluster))
    {
        appKey = u"32cbd69e4b950bf97679";
        cluster = u"us2";
    }
    return u"wss://ws-" % cluster % u".pusher.com/app/" % appKey %
           u"?protocol=7&client=js&version=8.4.0&flash=false";
}

std::optional<Connection> parseConnection(const QJsonObject &response)
{
    for (const auto value : response["data"]["connections"].toArray())
    {
        const auto connection = value.toObject();
        const auto credentials = connection["credentials"].toObject();
        if (connection["provider"].toString() == u"pusher")
        {
            const auto key = credentials["app_key"].toString();
            const auto cluster = credentials["cluster"].toString();
            if (validPusherCredential(key) && validPusherCredential(cluster))
            {
                return Connection{false, pusherUrl(key, cluster)};
            }
        }
        else if (connection["provider"].toString() == u"centrifugo")
        {
            const QUrl url(credentials["url"].toString());
            if (url.isValid() && url.scheme() == u"wss" &&
                !url.host().isEmpty() && url.userInfo().isEmpty() &&
                !url.hasFragment())
            {
                return Connection{true, url.toString()};
            }
        }
    }
    return std::nullopt;
}

void dispatch(KickChatServer *server, std::string_view channel,
              std::string_view event, BoostJsonValue data)
{
    const auto ids = parseIDs(channel);
    if (!server || (!ids.roomID && !ids.channelID) || event.empty())
    {
        return;
    }
    boost::json::value parsed;
    if (data.isString())
    {
        boost::system::error_code error;
        parsed = boost::json::parse(data.toStringView(), error);
        if (error)
        {
            return;
        }
        data = BoostJsonValue(parsed);
    }
    if (!data.isObject())
    {
        return;
    }
    stripPrefix(event, "App\\Events\\");
    if (!server->onAppEvent(ids.roomID, ids.channelID, event, data.toObject()))
    {
        qCDebug(chatterinoKick) << "Unknown event" << event;
    }
}

void disconnected(KickChatServer *server,
                  const std::unordered_set<QString> &subscriptions)
{
    if (!server || !recording::LiveMessageScope::enabled())
    {
        return;
    }
    for (const auto &subscription : subscriptions)
    {
        if (subscription.startsWith(u"chatrooms.") &&
            subscription.endsWith(u".v2"))
        {
            const auto ids = parseIDs(subscription.toStdString());
            if (const auto channel = server->findByRoomID(ids.roomID))
            {
                recording::publicEvent(
                    *channel,
                    {{"kind", "disconnected"}, {"text", "Chat disconnected"}});
            }
        }
    }
}

}
