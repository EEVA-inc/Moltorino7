#include "providers/homies/HomiesBadges.hpp"

#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringBuilder>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <optional>

namespace chatterino {

namespace {

using namespace chatterino::literals;

using BadgeMap = boost::unordered_flat_map<std::uint64_t, int>;
using BadgeStorage = std::vector<HomiesBadgeDefinition>;
constexpr auto HOMIES_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36";
constexpr auto HOMIES_BADGE_FRAME_LIFETIME = std::chrono::minutes{4};
constexpr auto HOMIES_PRIMARY_IMAGE_PREFIX =
    u"https://cdn.chatterinohomies.com/badges/";
constexpr auto HOMIES_PRIMARY_IMAGE_SUFFIX = u"/18.webp";

QString scaledBadgeImageUrl(QString url, int size)
{
    constexpr auto SIZE_MARKER = u"/18.";
    const auto marker = url.lastIndexOf(SIZE_MARKER);
    if (marker < 0)
    {
        return {};
    }

    url.replace(marker + 1, 2, QString::number(size));
    return url;
}

QJsonArray extractBadgesArray(const NetworkResult &result, const QString &sourceName)
{
    const auto object = result.parseJson();
    if (!object.isEmpty() && object.contains("badges") &&
        object.value("badges").isArray())
    {
        return object.value("badges").toArray();
    }

    const auto array = result.parseJsonArray();
    if (!array.isEmpty())
    {
        return array;
    }

    qCWarning(chatterinoApp) << "[Homies] Unexpected payload shape from"
                             << sourceName << "- expected { badges: [...] }"
                             << "or a root array.";
    return {};
}

std::optional<HomiesBadgeDefinition> makeBadgeDefinition(
    const QString &badgeName, const QJsonObject &badgeJson)
{
    const auto image1 = badgeJson.value("image1").toString().trimmed();
    if (image1.isEmpty())
    {
        return std::nullopt;
    }

    const auto tooltip = badgeJson.value("tooltip").toString().trimmed();
    auto resolvedName = badgeName.trimmed();
    if (resolvedName.isEmpty())
    {
        resolvedName = tooltip.isEmpty() ? u"badge"_s : tooltip;
    }

    const auto image2 = badgeJson.value("image2").toString().trimmed();
    const auto image3 = badgeJson.value("image3").toString().trimmed();
    const bool deriveScaledImages =
        homies::detail::canDeriveScaledImageUrls(image1, image2, image3);

    return HomiesBadgeDefinition{
        .name = resolvedName.toUtf8(),
        .tooltip = tooltip.toUtf8(),
        .image1 = image1.toUtf8(),
        .image2 = deriveScaledImages ? QByteArray{} : image2.toUtf8(),
        .image3 = deriveScaledImages ? QByteArray{} : image3.toUtf8(),
        .deriveScaledImages = deriveScaledImages,
    };
}

EmotePtr createBadgeEmote(const QString &sourceTag, const QString &badgeName,
                          const QString &tooltip,
                          std::array<QString, 3> imageUrls)
{
    if (imageUrls[0].isEmpty())
    {
        return nullptr;
    }

    auto makeImage = [](const QString &url, qreal scale, QSize expectedSize) {
        if (url.isEmpty())
        {
            return Image::getEmpty();
        }

        auto image = Image::fromUrl(Url{url}, scale, expectedSize);
        image->setFrameCacheLifetime(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                HOMIES_BADGE_FRAME_LIFETIME));
        return image;
    };

    auto emote = Emote{
        .name = EmoteName{sourceTag % u":" % badgeName},
        .images = ImageSet{
            makeImage(imageUrls[0], 1, {18, 18}),
            makeImage(imageUrls[1], 0.5, {36, 36}),
            makeImage(imageUrls[2], 0.25, {72, 72}),
        },
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{imageUrls[0]},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

EmotePtr createBadgeEmote(const QString &sourceTag,
                          const HomiesBadgeDefinition &badge)
{
    return createBadgeEmote(sourceTag, QString::fromUtf8(badge.name),
                            QString::fromUtf8(badge.tooltip),
                            homies::detail::resolveImageUrls(badge));
}

void storeBadgeCentricBadges(const QJsonArray &badges, BadgeMap &badgeMap,
                             BadgeStorage &emotes)
{
    qsizetype userCountEstimate = 0;
    for (const auto &badgeValue : badges)
    {
        userCountEstimate += badgeValue.toObject().value("users").toArray().size();
    }

    badgeMap.clear();
    emotes.clear();
    badgeMap.reserve(static_cast<size_t>(std::max<qsizetype>(badges.size(), userCountEstimate)));
    emotes.reserve(badges.size());

    for (const auto &badgeValue : badges)
    {
        const auto badgeJson = badgeValue.toObject();
        auto badge = makeBadgeDefinition(
            badgeJson.value("tooltip").toString(), badgeJson);
        if (!badge)
        {
            continue;
        }

        const int index = static_cast<int>(emotes.size());
        emotes.push_back(std::move(*badge));

        for (const auto &userValue : badgeJson.value("users").toArray())
        {
            const auto userId = userValue.toString().trimmed();
            if (const auto numericUserId =
                    homies::detail::parseTwitchUserId(QStringView{userId}))
            {
                badgeMap[*numericUserId] = index;
            }
        }
    }
}

}

namespace homies::detail {

bool canDeriveScaledImageUrls(QStringView image1, QStringView image2,
                              QStringView image3)
{
    const auto derived2 = scaledBadgeImageUrl(image1.toString(), 36);
    return !derived2.isEmpty() && image2 == derived2 &&
           image3 == scaledBadgeImageUrl(image1.toString(), 72);
}

std::array<QString, 3> resolveImageUrls(
    const HomiesBadgeDefinition &definition)
{
    auto image1 = QString::fromUtf8(definition.image1);
    if (definition.deriveScaledImages)
    {
        return {image1, scaledBadgeImageUrl(image1, 36),
                scaledBadgeImageUrl(image1, 72)};
    }

    return {std::move(image1), QString::fromUtf8(definition.image2),
            QString::fromUtf8(definition.image3)};
}

std::optional<std::uint64_t> parseTwitchUserId(QStringView userId)
{
    if (userId.isEmpty() || (userId.size() > 1 && userId.front() == u'0'))
    {
        return std::nullopt;
    }
    for (const auto character : userId)
    {
        if (character < u'0' || character > u'9')
        {
            return std::nullopt;
        }
    }

    bool ok = false;
    const auto parsed = userId.toULongLong(&ok);
    if (!ok)
    {
        return std::nullopt;
    }
    return parsed;
}

std::optional<QUuid> parseStandardBadgeImageId(QStringView image1,
                                                QStringView image2,
                                                QStringView image3)
{
    const QStringView prefix{HOMIES_PRIMARY_IMAGE_PREFIX};
    const QStringView suffix{HOMIES_PRIMARY_IMAGE_SUFFIX};
    if (!canDeriveScaledImageUrls(image1, image2, image3) ||
        !image1.startsWith(prefix) || !image1.endsWith(suffix))
    {
        return std::nullopt;
    }

    const auto idText = image1.sliced(
        prefix.size(), image1.size() - prefix.size() - suffix.size());
    const auto imageId = QUuid::fromString(idText);
    if (imageId.toString(QUuid::WithoutBraces) != idText)
    {
        return std::nullopt;
    }

    return imageId;
}

std::array<QString, 3> makeStandardBadgeImageUrls(const QUuid &imageId)
{
    const auto base = QStringView{HOMIES_PRIMARY_IMAGE_PREFIX}.toString() %
                      imageId.toString(QUuid::WithoutBraces);
    return {base % QStringView{HOMIES_PRIMARY_IMAGE_SUFFIX},
            base % u"/36.webp", base % u"/72.webp"};
}

}

HomiesBadges::HomiesBadges()
    : lifetimeState_(std::make_shared<LifetimeState>(this))
{
    QTimer::singleShot(3500, this, [this] {
        this->startLoading();
    });
}

HomiesBadges::~HomiesBadges()
{
    std::scoped_lock lock(this->lifetimeState_->mutex);
    this->lifetimeState_->owner = nullptr;
}

void HomiesBadges::startLoading()
{
    if (this->loadStarted_.exchange(true))
    {
        return;
    }

    this->loadHomiesBadges();
}

void HomiesBadges::storeUserCentricBadges(
    const QJsonArray &badges, std::vector<PrimaryBadgeEntry> &entries,
    QByteArray &tooltips, BadgeStorage &fallbackBadges)
{
    entries.clear();
    tooltips.clear();
    fallbackBadges.clear();
    entries.reserve(static_cast<size_t>(badges.size()));

    for (const auto &badgeValue : badges)
    {
        const auto badgeJson = badgeValue.toObject();

        const auto rawUserId =
            badgeJson.value("userId").toString().trimmed();
        const auto numericUserId =
            homies::detail::parseTwitchUserId(QStringView{rawUserId});
        if (!numericUserId)
        {
            continue;
        }

        auto badge = makeBadgeDefinition(rawUserId, badgeJson);
        if (!badge)
        {
            continue;
        }

        PrimaryBadgeEntry entry{.userId = *numericUserId};
        const auto imageUrls = homies::detail::resolveImageUrls(*badge);
        const auto imageId = homies::detail::parseStandardBadgeImageId(
            imageUrls[0], imageUrls[1], imageUrls[2]);
        const auto tooltipSize =
            static_cast<std::uint64_t>(badge->tooltip.size());
        const auto tooltipEnd =
            static_cast<std::uint64_t>(tooltips.size()) + tooltipSize;
        const bool tooltipFits =
            tooltipSize <= std::numeric_limits<std::uint16_t>::max() &&
            tooltipEnd <= std::numeric_limits<std::uint32_t>::max();

        if (imageId && tooltipFits)
        {
            entry.imageId = *imageId;
            entry.tooltipOffset = static_cast<std::uint32_t>(tooltips.size());
            entry.tooltipSize =
                static_cast<std::uint16_t>(badge->tooltip.size());
            tooltips.append(badge->tooltip);
        }
        else
        {
            if (fallbackBadges.size() >=
                std::numeric_limits<std::uint32_t>::max())
            {
                continue;
            }
            entry.fallbackBadge =
                static_cast<std::uint32_t>(fallbackBadges.size());
            fallbackBadges.push_back(std::move(*badge));
        }

        entries.push_back(std::move(entry));
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto &left, const auto &right) {
                         return left.userId < right.userId;
                     });

    size_t writeIndex = 0;
    bool hadDuplicates = false;
    for (size_t readIndex = 0; readIndex < entries.size();)
    {
        auto groupEnd = readIndex + 1;
        while (groupEnd < entries.size() &&
               entries[groupEnd].userId == entries[readIndex].userId)
        {
            ++groupEnd;
        }
        const auto retainedIndex = groupEnd - 1;
        hadDuplicates |= retainedIndex != readIndex;
        if (writeIndex != retainedIndex)
        {
            entries[writeIndex] = std::move(entries[retainedIndex]);
        }
        ++writeIndex;
        readIndex = groupEnd;
    }
    entries.resize(writeIndex);

    if (!hadDuplicates)
    {
        return;
    }

    QByteArray retainedTooltips;
    retainedTooltips.reserve(tooltips.size());
    BadgeStorage retainedFallbackBadges;
    retainedFallbackBadges.reserve(fallbackBadges.size());
    for (auto &entry : entries)
    {
        if (entry.fallbackBadge != NO_FALLBACK_BADGE)
        {
            const auto oldIndex = static_cast<size_t>(entry.fallbackBadge);
            if (oldIndex >= fallbackBadges.size())
            {
                entry.fallbackBadge = NO_FALLBACK_BADGE;
                continue;
            }
            entry.fallbackBadge =
                static_cast<std::uint32_t>(retainedFallbackBadges.size());
            retainedFallbackBadges.push_back(
                std::move(fallbackBadges[oldIndex]));
            continue;
        }

        const auto oldOffset = static_cast<qsizetype>(entry.tooltipOffset);
        const auto tooltipSize = static_cast<qsizetype>(entry.tooltipSize);
        if (oldOffset > tooltips.size() ||
            tooltipSize > tooltips.size() - oldOffset)
        {
            entry.tooltipOffset = 0;
            entry.tooltipSize = 0;
            continue;
        }
        entry.tooltipOffset =
            static_cast<std::uint32_t>(retainedTooltips.size());
        retainedTooltips.append(tooltips.constData() + oldOffset, tooltipSize);
    }
    retainedTooltips.squeeze();
    tooltips = std::move(retainedTooltips);
    fallbackBadges = std::move(retainedFallbackBadges);
}

bool HomiesBadges::hasPrimaryBadge(std::uint64_t userId) const
{
    const auto it = std::ranges::lower_bound(
        this->primaryBadges_, userId, {}, &PrimaryBadgeEntry::userId);
    return it != this->primaryBadges_.end() && it->userId == userId;
}

EmotePtr HomiesBadges::lookupPrimaryBadge(std::uint64_t userId,
                                          bool materialize) const
{
    const auto entry = std::ranges::lower_bound(
        this->primaryBadges_, userId, {}, &PrimaryBadgeEntry::userId);
    if (entry == this->primaryBadges_.end() || entry->userId != userId)
    {
        return nullptr;
    }

    if (const auto cached = this->primaryCachedEmotes_.find(userId);
        cached != this->primaryCachedEmotes_.end())
    {
        if (auto emote = cached->second.lock())
        {
            return emote;
        }
    }
    if (!materialize)
    {
        return nullptr;
    }

    EmotePtr emote;
    if (entry->fallbackBadge != NO_FALLBACK_BADGE)
    {
        const auto fallbackIndex =
            static_cast<size_t>(entry->fallbackBadge);
        if (fallbackIndex < this->primaryFallbackBadges_.size())
        {
            emote = createBadgeEmote(
                u"homies"_s, this->primaryFallbackBadges_[fallbackIndex]);
        }
    }
    else
    {
        const auto offset = static_cast<qsizetype>(entry->tooltipOffset);
        const auto size = static_cast<qsizetype>(entry->tooltipSize);
        if (offset <= this->primaryTooltips_.size() &&
            size <= this->primaryTooltips_.size() - offset)
        {
            emote = createBadgeEmote(
                u"homies"_s, QString::number(entry->userId),
                QString::fromUtf8(this->primaryTooltips_.constData() + offset,
                                  size),
                homies::detail::makeStandardBadgeImageUrls(entry->imageId));
        }
    }

    if (emote)
    {
        this->primaryCachedEmotes_.insert_or_assign(
            userId, std::weak_ptr<const Emote>{emote});
    }
    return emote;
}

void HomiesBadges::loadHomiesBadges()
{
    const auto lifetimeState = this->lifetimeState_;
    NetworkRequest("https://chatterinohomies.com/api/badges/list")
        .header("Accept", "application/json")
        .header("User-Agent", HOMIES_USER_AGENT)
        .timeout(5000)
        .maximumResponseSize(8 * 1024 * 1024)
        .concurrent()
        .onSuccess([lifetimeState](const NetworkResult &result) {
            const auto badges = extractBadgesArray(
                result, u"chatterinohomies.com/api/badges/list"_s);

            std::vector<PrimaryBadgeEntry> entries;
            QByteArray tooltips;
            BadgeStorage fallbackBadges;
            storeUserCentricBadges(badges, entries, tooltips,
                                   fallbackBadges);
            const auto loadedCount = entries.size();

            std::scoped_lock lifetimeLock(lifetimeState->mutex);
            auto *owner = lifetimeState->owner;
            if (owner == nullptr)
            {
                return;
            }

            {
                std::unique_lock storageLock(owner->mutex_);
                owner->primaryBadges_ = std::move(entries);
                owner->primaryTooltips_ = std::move(tooltips);
                owner->primaryFallbackBadges_ = std::move(fallbackBadges);
                owner->primaryCachedEmotes_.clear();
            }

            if (loadedCount > 0)
            {
                owner->hasLoadedBadges_.store(true);
            }

            qCDebug(chatterinoApp) << "[Homies] Loaded"
                                   << loadedCount
                                   << "badges from chatterinohomies.com";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Homies] Failed to load chatterinohomies.com badges:"
                << result.formatError();
        })
        .execute();

    NetworkRequest("https://itzalex.github.io/badges")
        .header("Accept", "application/json")
        .header("User-Agent", HOMIES_USER_AGENT)
        .timeout(5000)
        .maximumResponseSize(8 * 1024 * 1024)
        .concurrent()
        .onSuccess([lifetimeState](const NetworkResult &result) {
            const auto badges =
                extractBadgesArray(result, u"itzalex.github.io/badges"_s);

            BadgeMap badgeMap;
            BadgeStorage emotes;
            storeBadgeCentricBadges(badges, badgeMap, emotes);
            const auto loadedCount = emotes.size();

            std::scoped_lock lifetimeLock(lifetimeState->mutex);
            auto *owner = lifetimeState->owner;
            if (owner == nullptr)
            {
                return;
            }

            {
                std::unique_lock storageLock(owner->mutex_);
                owner->badgeMap2_ = std::move(badgeMap);
                owner->badges2_ = std::move(emotes);
            }

            if (loadedCount > 0)
            {
                owner->hasLoadedBadges_.store(true);
            }

            qCDebug(chatterinoApp) << "[Homies] Loaded"
                                   << loadedCount
                                   << "badges from itzalex.github.io/badges";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Homies] Failed to load itzalex.github.io/badges:"
                << result.formatError();
        })
        .execute();

    NetworkRequest("https://itzalex.github.io/badges2")
        .header("Accept", "application/json")
        .header("User-Agent", HOMIES_USER_AGENT)
        .timeout(5000)
        .maximumResponseSize(8 * 1024 * 1024)
        .concurrent()
        .onSuccess([lifetimeState](const NetworkResult &result) {
            const auto badges =
                extractBadgesArray(result, u"itzalex.github.io/badges2"_s);

            BadgeMap badgeMap;
            BadgeStorage emotes;
            storeBadgeCentricBadges(badges, badgeMap, emotes);
            const auto loadedCount = emotes.size();

            std::scoped_lock lifetimeLock(lifetimeState->mutex);
            auto *owner = lifetimeState->owner;
            if (owner == nullptr)
            {
                return;
            }

            {
                std::unique_lock storageLock(owner->mutex_);
                owner->badgeMap3_ = std::move(badgeMap);
                owner->badges3_ = std::move(emotes);
            }

            if (loadedCount > 0)
            {
                owner->hasLoadedBadges_.store(true);
            }

            qCDebug(chatterinoApp) << "[Homies] Loaded"
                                   << loadedCount
                                   << "badges from itzalex.github.io/badges2";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Homies] Failed to load itzalex.github.io/badges2:"
                << result.formatError();
        })
        .execute();
}

std::array<EmotePtr, 3> HomiesBadges::getBadges(const QString &userId) const
{
    if (!this->hasLoadedBadges_.load())
    {
        return {};
    }
    const auto numericUserId =
        homies::detail::parseTwitchUserId(QStringView{userId});
    if (!numericUserId)
    {
        return {};
    }

    {
        std::shared_lock lock(this->mutex_);
        std::array<EmotePtr, 3> cached{
            this->lookupPrimaryBadge(*numericUserId, false),
            this->lookupBadge(this->badgeMap2_, this->badges2_,
                              *numericUserId, false),
            this->lookupBadge(this->badgeMap3_, this->badges3_,
                              *numericUserId, false),
        };

        const auto hasUnmaterializedBadge =
            (!cached[0] && this->hasPrimaryBadge(*numericUserId)) ||
            (!cached[1] && this->badgeMap2_.contains(*numericUserId)) ||
            (!cached[2] && this->badgeMap3_.contains(*numericUserId));
        if (!hasUnmaterializedBadge)
        {
            return cached;
        }
    }

    std::unique_lock lock(this->mutex_);
    return {
        this->lookupPrimaryBadge(*numericUserId, true),
        this->lookupBadge(this->badgeMap2_, this->badges2_, *numericUserId,
                          true),
        this->lookupBadge(this->badgeMap3_, this->badges3_, *numericUserId,
                          true),
    };
}

EmotePtr HomiesBadges::lookupBadge(const BadgeMap &badgeMap,
                                   const BadgeStorage &badges,
                                   std::uint64_t userId,
                                   bool materialize) const
{
    const auto it = badgeMap.find(userId);
    if (it != badgeMap.end() && it->second >= 0 &&
        it->second < static_cast<int>(badges.size()))
    {
        const auto &badge = badges.at(it->second);
        if (auto cached = badge.cachedEmote.lock())
        {
            return cached;
        }

        if (!materialize)
        {
            return nullptr;
        }

        auto emote = createBadgeEmote(u"homies"_s, badge);
        badge.cachedEmote = emote;
        return emote;
    }

    return nullptr;
}

}
