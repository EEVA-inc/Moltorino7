#include "providers/kick/KickEmotes.hpp"

#include "messages/Image.hpp"

#include <QCache>

namespace {

using namespace chatterino;

QCache<QString, EmotePtr> CACHE(4096);

}

namespace chatterino {

EmotePtr KickEmotes::emoteForID(QStringView id, QStringView name)
{
    auto idStr = id.toString();
    if (const auto *cached = CACHE.object(idStr))
    {
        return *cached;
    }

    auto nameStr = name.toString();
    QString tooltip = nameStr.toHtmlEscaped() % u"<br>Kick Emote";
    auto emote = std::make_shared<const Emote>(Emote{
        .name = {std::move(nameStr)},
        .images = ImageSet(Image::fromAutoscaledUrl(
            {u"https://files.kick.com/emotes/" % id % u"/fullsize"}, 28)),
        .tooltip = {std::move(tooltip)},
        .id = {idStr},
    });
    CACHE.insert(idStr, new EmotePtr(emote));
    return emote;
}

}
