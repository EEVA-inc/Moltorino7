#pragma once

#include "messages/MessageBuilder.hpp"
#include "providers/youtube/YouTubeTypes.hpp"

#include <QDateTime>

#include <optional>
#include <utility>

namespace chatterino {

class YouTubeChannel;

class YouTubeMessageBuilder : public MessageBuilder
{
public:
    static std::pair<MessagePtrMut, HighlightAlert> makeMessage(
        YouTubeChannel *channel, const YouTubeMessage &message);

    static std::optional<YouTubeAuthor> cachedAuthorForMessage(
        const QString &messageID);

    static std::optional<YouTubeAuthor> cachedAuthorForChannel(
        const QString &sourceChannel, const QString &channelID);

private:
    YouTubeMessageBuilder(YouTubeChannel *channel, const QDateTime &time,
                          bool isSystemMessage);

    void appendChannelName();
    void appendRoleBadges(const YouTubeAuthor &author);
    void appendUsername(const YouTubeAuthor &author);
    void appendUserText(const QString &text);
    void appendUserRuns(const std::vector<YouTubeMessageRun> &runs);
    void appendLinkedUser(const QString &displayName, const QString &channelId,
                          QString &plainText);
    void setCommonFields(const YouTubeMessage &source);

    static std::pair<MessagePtrMut, HighlightAlert> makeChatMessage(
        YouTubeChannel *channel, const YouTubeMessage &source);
    static MessagePtrMut makeSystemEvent(YouTubeChannel *channel,
                                         const YouTubeMessage &source);
    static void rememberAuthor(const QString &sourceChannel,
                               const QString &messageID,
                               const YouTubeAuthor &author);

    YouTubeChannel *channel_ = nullptr;
};

}
