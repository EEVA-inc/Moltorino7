#include "providers/tiktok/TikTokEmotes.hpp"

#include "messages/Image.hpp"
#include "providers/tiktok/TikTokTypes.hpp"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

std::shared_ptr<const EmoteMap> tikTokBuiltinEmotes()
{
    static const auto emotes = [] {
        auto result = std::make_shared<EmoteMap>();
        for (const auto *name : {u"wow",
                                 u"laugh",
                                 u"thanks",
                                 u"laughcry",
                                 u"thumb",
                                 u"hi",
                                 u"heart",
                                 u"congrat",
                                 u"rockyserious",
                                 u"rockyloveit",
                                 u"rockyproud",
                                 u"rockycool",
                                 u"rosiedislike",
                                 u"rosieawkward",
                                 u"rosiekisskiss",
                                 u"rosiecute",
                                 u"jolliekissingface",
                                 u"jolliewow",
                                 u"jolliespeechless",
                                 u"jolliesatisfied",
                                 u"sagethink",
                                 u"sagefulfilled",
                                 u"sageclever",
                                 u"sagemoney"})
        {
            const auto shortcut = u'[' + QString::fromUtf16(name) + u']';
            auto emote = std::make_shared<const Emote>(Emote{
                .name = EmoteName{shortcut},
                .images = ImageSet{Image::fromUrl(
                    Url{u":/tiktok/"_s + QString::fromUtf16(name) + u".png"_s},
                    0.5, QSize{56, 56})},
                .tooltip = Tooltip{shortcut + u" (TikTok)"_s},
            });
            result->emplace(EmoteName{shortcut}, std::move(emote));
        }
        return result;
    }();
    return emotes;
}

EmotePtr tikTokEmote(QStringView name, QStringView url, int height)
{
    if (!isTikTokImageUrl(QUrl(url.toString())))
    {
        return {};
    }
    static QMutex mutex;
    static QHash<QString, EmotePtr> cache;
    static QQueue<QString> order;
    const auto key = url.toString() + u'\n' + QString::number(height) + u'\n' +
                     name.toString();
    QMutexLocker guard(&mutex);
    if (const auto found = cache.constFind(key); found != cache.cend())
    {
        return *found;
    }
    auto result = std::make_shared<const Emote>(Emote{
        .name = EmoteName{name.toString()},
        .images =
            ImageSet{Image::fromAutoscaledUrl(Url{url.toString()}, height)},
        .tooltip = Tooltip{name.toString().toHtmlEscaped()},
    });
    cache.insert(key, result);
    order.enqueue(key);
    while (order.size() > 512)
    {
        cache.remove(order.dequeue());
    }
    return result;
}
}
