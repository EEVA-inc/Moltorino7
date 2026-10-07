#pragma once

#include "messages/ImageSet.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <QByteArray>
#include <array>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUuid>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

struct HomiesBadgeDefinition {
    QByteArray name;
    QByteArray tooltip;
    QByteArray image1;
    QByteArray image2;
    QByteArray image3;

    bool deriveScaledImages = false;
    mutable std::weak_ptr<const Emote> cachedEmote;
};

namespace homies::detail {

bool canDeriveScaledImageUrls(QStringView image1, QStringView image2,
                              QStringView image3);
std::array<QString, 3> resolveImageUrls(
    const HomiesBadgeDefinition &definition);
std::optional<std::uint64_t> parseTwitchUserId(QStringView userId);
std::optional<QUuid> parseStandardBadgeImageId(QStringView image1,
                                                QStringView image2,
                                                QStringView image3);
std::array<QString, 3> makeStandardBadgeImageUrls(const QUuid &imageId);

}

class HomiesBadges final : public QObject
{
public:
    HomiesBadges();
    ~HomiesBadges() override;

    void startLoading();
    void loadHomiesBadges();

    std::array<EmotePtr, 3> getBadges(const QString &userId) const;

private:
    using BadgeMap = boost::unordered_flat_map<std::uint64_t, int>;
    using BadgeStorage = std::vector<HomiesBadgeDefinition>;

    struct PrimaryBadgeEntry {
        std::uint64_t userId = 0;
        QUuid imageId;
        std::uint32_t tooltipOffset = 0;
        std::uint32_t fallbackBadge =
            std::numeric_limits<std::uint32_t>::max();
        std::uint16_t tooltipSize = 0;
    };

    static constexpr auto NO_FALLBACK_BADGE =
        std::numeric_limits<std::uint32_t>::max();

    struct LifetimeState {
        explicit LifetimeState(HomiesBadges *owner)
            : owner(owner)
        {
        }

        std::mutex mutex;
        HomiesBadges *owner;
    };

    static void storeUserCentricBadges(
        const QJsonArray &badges, std::vector<PrimaryBadgeEntry> &entries,
        QByteArray &tooltips, BadgeStorage &fallbackBadges);

    bool hasPrimaryBadge(std::uint64_t userId) const;
    EmotePtr lookupPrimaryBadge(std::uint64_t userId,
                                bool materialize) const;

    EmotePtr lookupBadge(const BadgeMap &badgeMap,
                         const BadgeStorage &badges,
                         std::uint64_t userId, bool materialize) const;

    std::atomic_bool loadStarted_{false};
    std::atomic_bool hasLoadedBadges_{false};
    std::shared_ptr<LifetimeState> lifetimeState_;
    mutable std::shared_mutex mutex_;
    std::vector<PrimaryBadgeEntry> primaryBadges_;
    QByteArray primaryTooltips_;
    BadgeStorage primaryFallbackBadges_;
    mutable boost::unordered_flat_map<std::uint64_t,
                                      std::weak_ptr<const Emote>>
        primaryCachedEmotes_;

    BadgeMap badgeMap2_;
    BadgeStorage badges2_;

    BadgeMap badgeMap3_;
    BadgeStorage badges3_;
};

}
