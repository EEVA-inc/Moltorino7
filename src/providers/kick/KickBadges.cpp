#include "providers/kick/KickBadges.hpp"

#include "debug/AssertInGuiThread.hpp"
#include "util/QStringHash.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <QCache>
#include <algorithm>

#include <magic_enum/magic_enum.hpp>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

enum class BadgeID : uint8_t {
    bot,
    broadcaster,
    founder,
    moderator,
    og,
    sidekick,
    staff,
    sub_gifter,
    subscriber,
    trainwreckstv,
    verified,
    vip
};

struct BadgeNameData {
    QString friendlyName;
    QStringView pathSegment;
    MessageElementFlag flag{};
};
BadgeNameData nameDataFor(BadgeID id)
{
    switch (id)
    {
        case BadgeID::bot:
            return {
                .friendlyName = u"Bot"_s,
                .pathSegment = u"bot",
                .flag = MessageElementFlag::BadgeVanity,
            };
        case BadgeID::broadcaster:
            return {
                .friendlyName = u"Broadcaster"_s,
                .pathSegment = u"broadcaster",
                .flag = MessageElementFlag::BadgeChannelAuthority,
            };
        case BadgeID::founder:
            return {.friendlyName = u"Founder"_s,
                    .pathSegment = u"founder",
                    .flag = MessageElementFlag::BadgeSubscription};
        case BadgeID::moderator:
            return {
                .friendlyName = u"Moderator"_s,
                .pathSegment = u"moderator",
                .flag = MessageElementFlag::BadgeChannelAuthority,
            };
        case BadgeID::og:
            return {
                .friendlyName = u"OG"_s,
                .pathSegment = u"og",
                .flag = MessageElementFlag::BadgeVanity,
            };
        case BadgeID::sidekick:
            return {
                .friendlyName = u"Sidekick"_s,
                .pathSegment = u"sidekick",
                .flag = MessageElementFlag::BadgeVanity,
            };
        case BadgeID::staff:
            return {
                .friendlyName = u"Staff"_s,
                .pathSegment = u"staff",
                .flag = MessageElementFlag::BadgeGlobalAuthority,
            };
        case BadgeID::sub_gifter:
            return {
                .friendlyName = u"Sub Gifter"_s,
                .pathSegment = u"sub_gifter",
                .flag = MessageElementFlag::BadgeVanity,
            };
        case BadgeID::subscriber:
            return {
                .friendlyName = u"Subscriber"_s,
                .pathSegment = u"subscriber",
                .flag = MessageElementFlag::BadgeSubscription,
            };
        case BadgeID::trainwreckstv:
            return {
                .friendlyName = u"TrainwrecksTV"_s,
                .pathSegment = u"trainwreckstv",
                .flag = MessageElementFlag::BadgeVanity,
            };
        case BadgeID::verified:
            return {
                .friendlyName = u"Verified"_s,
                .pathSegment = u"verified",
                .flag = MessageElementFlag::BadgeVanity,
            };
        case BadgeID::vip:
            return {
                .friendlyName = u"VIP"_s,
                .pathSegment = u"vip",
                .flag = MessageElementFlag::BadgeChannelAuthority,
            };
    }
    return {};
}

using V2BadgeCache =
    boost::unordered_flat_map<QString, std::weak_ptr<const Emote>>;

using CacheData = std::pair<EmotePtr, MessageElementFlag>;

std::array<CacheData, magic_enum::enum_count<BadgeID>()> CACHE{};

}

namespace chatterino {

std::pair<EmotePtr, MessageElementFlag> KickBadges::lookup(
    std::string_view name)
{
    assertInGuiThread();

    auto id = magic_enum::enum_cast<BadgeID>(name);
    if (!id)
    {
        return {nullptr, {}};
    }

    auto idx = static_cast<uint8_t>(*id);
    if (idx >= CACHE.size())
    {
        assert(false);
        return {nullptr, {}};
    }

    auto &entry = CACHE[idx];
    if (!entry.first)
    {
        auto data = nameDataFor(*id);

        entry.first = std::make_shared<const Emote>(Emote{
            .name = {data.friendlyName},
            .images =
                ImageSet{
                    Image::fromUrl(
                        {u":/kick/badges/" % data.pathSegment % u"-18.webp"},
                        1.0, {18, 18}),
                    Image::fromUrl(
                        {u":/kick/badges/" % data.pathSegment % u"-36.webp"},
                        .5, {36, 36}),
                },
            .tooltip = Tooltip{data.friendlyName},
        });
        entry.second = data.flag;
    }

    return entry;
}

}

namespace chatterino {

std::pair<EmotePtr, MessageElementFlag> KickBadges::getV2Cached(
    BoostJsonObject badgeObj)
{
    assertInGuiThread();
    static V2BadgeCache cache;
    static size_t newEntriesSinceSweep = 0;

    auto imageUrl = badgeObj["image_url"].toQString();
    if (imageUrl.isEmpty())
    {
        return {};
    }
    QString name;
    auto origName = badgeObj["name"].toStringView();
    if (origName == "level")
    {
        auto level = QString::number(badgeObj["metadata"]["level"].toUint64());
        name = u"Level " % level;
    }
    else
    {
        name = QString::fromUtf8(origName.data(),
                                 static_cast<qsizetype>(origName.size()));
    }

    const auto key = QString::number(imageUrl.size()) + u':' + imageUrl + name;
    auto it = cache.find(key);
    if (it != cache.end())
    {
        if (auto emote = it->second.lock())
        {
            return {std::move(emote), MessageElementFlag::BadgeVanity};
        }
        cache.erase(it);
    }

    constexpr size_t CACHE_SWEEP_INTERVAL = 128;
    constexpr size_t CACHE_SWEEP_MIN_SIZE = 256;
    if (++newEntriesSinceSweep >= CACHE_SWEEP_INTERVAL)
    {
        newEntriesSinceSweep = 0;
        if (cache.size() >= CACHE_SWEEP_MIN_SIZE)
        {
            for (auto entry = cache.begin(); entry != cache.end();)
            {
                if (entry->second.expired())
                {
                    entry = cache.erase(entry);
                }
                else
                {
                    ++entry;
                }
            }
        }
    }

    auto emote = std::make_shared<const Emote>(Emote{
        .name = {name},
        .images =
            ImageSet{
                Image::fromAutoscaledUrl({imageUrl}, 18),
            },
        .tooltip = Tooltip{name},
    });
    if (cache.size() >= 1024)
    {
        cache.erase(cache.begin());
    }
    cache.emplace(key, std::weak_ptr<const Emote>{emote});
    return {emote, MessageElementFlag::BadgeVanity};
}

EmotePtr KickBadges::lookupSubGifter(unsigned amount)
{
    assertInGuiThread();
    static QCache<unsigned, EmotePtr> cache(256);
    if (const auto *cached = cache.object(amount))
    {
        return *cached;
    }
    static constexpr unsigned tiers[] = {
        1,   5,   10,  25,   50,   100,  150,  200,  250, 300,
        350, 400, 450, 500,  550,  600,  650,  700,  750, 800,
        850, 900, 950, 1000, 2000, 3000, 4000, 5000,
    };
    auto tier = std::ranges::upper_bound(tiers, amount);
    if (tier == std::begin(tiers))
    {
        return {};
    }
    const auto number = QString::number(*--tier);
    const auto name = QStringLiteral("Gifted %1 %2")
                          .arg(amount)
                          .arg(amount == 1 ? "sub" : "subs");
    auto badge = std::make_shared<const Emote>(Emote{
        .name = EmoteName{name},
        .images =
            ImageSet{
                Image::fromUrl({u":/kick/badges/gift-" % number % u"-18.webp"},
                               1., {18, 18}),
                Image::fromUrl({u":/kick/badges/gift-" % number % u"-36.webp"},
                               .5, {36, 36}),
            },
        .tooltip = Tooltip{name},
    });
    cache.insert(amount, new EmotePtr(badge));
    return badge;
}

}
