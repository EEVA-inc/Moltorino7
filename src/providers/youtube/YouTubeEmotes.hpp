#pragma once

#include "messages/Emote.hpp"
#include "util/Expected.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace chatterino {

namespace youtube {

inline constexpr uint16_t CUSTOM_EMOJI_LOGICAL_SIZE = 24;
inline constexpr qreal CUSTOM_EMOJI_HORIZONTAL_PADDING = 3.0;

}

struct YouTubeLiveChatPage {
    QJsonObject innertubeContext;
    QString apiKey;
    QString clientName;
    QString clientVersion;
    QString visitorData;
    QString continuation;
    QString clickTrackingParams;
    std::shared_ptr<const EmoteMap> emotes = EMPTY_EMOTE_MAP;
};

class YouTubeEmotes
{
public:
    using PagePtr = std::shared_ptr<const YouTubeLiveChatPage>;
    using PageCallback = std::function<void(ExpectedStr<PagePtr>)>;
    using Callback =
        std::function<void(std::shared_ptr<const EmoteMap> emotes)>;

    static std::shared_ptr<const EmoteMap> globalEmotes();

    static EmotePtr makeCustomEmoji(const QString &shortcut,
                                    const QString &imageUrl,
                                    const QString &emojiID);

    static void loadPageForVideo(const QString &videoID,
                                 PageCallback callback,
                                 bool fresh = false);

    static void loadForVideo(const QString &videoID, Callback callback);

    static std::shared_ptr<const EmoteMap> parseLiveChatPage(
        const QByteArray &html);

    static ExpectedStr<PagePtr> parseLiveChatPageData(const QByteArray &html);
};

}
