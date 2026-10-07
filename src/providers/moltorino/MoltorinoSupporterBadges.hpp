#pragma once

#include "messages/Emote.hpp"

#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chatterino {

struct MoltorinoSupporterBadge {
    QString categoryId;
    QString displayName;
    QString description;
    EmotePtr emote;
    bool listedInVanity = true;
};

struct MoltorinoVanityLayout {
    std::vector<QString> order;
    std::unordered_set<QString> hidden;
    QString moltorinoBadge;
    bool moltorinoBadgeSelectionExplicit = false;

    bool operator==(const MoltorinoVanityLayout &) const = default;
};

namespace vanity::detail {

QString badgeOrderSlot(const QString &key);
MoltorinoVanityLayout normalizeLayout(MoltorinoVanityLayout layout);
MoltorinoVanityLayout defaultLayoutPreservingBadge(
    const MoltorinoVanityLayout &layout);
bool isBadgeVisible(const MoltorinoVanityLayout &layout, const QString &key);
void setBadgeVisible(MoltorinoVanityLayout &layout, const QString &key,
                     bool visible);
void appendMissingBadgeKeys(MoltorinoVanityLayout &layout,
                            const std::vector<QString> &keys);
void replaceActiveBadgeOrder(MoltorinoVanityLayout &layout,
                             const std::vector<QString> &keys);
bool shouldSaveProfile(const MoltorinoVanityLayout &original,
                       const MoltorinoVanityLayout &current,
                       bool forceOnlineSync = false);
bool shouldClearLocalLayout(const MoltorinoVanityLayout &local,
                            const std::optional<MoltorinoVanityLayout> &remote);
bool resolveMoltorinoBadgeVanityListing(
    const QString &categoryId, std::optional<bool> serverListing);
bool shouldShowMoltorinoBadge(bool listedInVanity, bool owned);

}

class MoltorinoSupporterBadges final : public QObject
{
public:
    explicit MoltorinoSupporterBadges(QObject *parent = nullptr);

    pajlada::Signals::NoArgSignal badgesUpdated;

    void initialize();
    void refreshNow();
    void refreshPassive();
    void refreshIfNewer(int version);
    void refreshV2IfNewer(const QString &generation, int version);

    std::vector<MoltorinoSupporterBadge> getBadges(const QString &userId) const;
    std::vector<MoltorinoSupporterBadge> getAssignedBadges(
        const QString &userId) const;
    std::vector<MoltorinoSupporterBadge> getBadgeCatalog() const;
    std::vector<MoltorinoSupporterBadge> getBadgesByCategoryIds(
        const std::vector<QString> &categoryIds) const;
    std::optional<MoltorinoVanityLayout> getVanityLayout(
        const QString &userId) const;
    std::optional<MoltorinoVanityLayout> getLocalVanityLayout(
        const QString &userId) const;
    void setLocalVanityLayout(const QString &userId,
                              MoltorinoVanityLayout layout);
    bool decorationsEnabledForUser(const QString &userId, bool isSelf) const;

private:
    void refreshInternal(bool force, std::optional<int> minimumVersion,
                         QString expectedGeneration = {},
                         bool bypassCache = false);
    void requestLegacyFallback();
    void finishRequest();
    void loadCache();
    void loadLocalVanityLayouts();
    void saveLocalVanityLayouts() const;
    void saveCache(const QByteArray &payload) const;
    bool applyPayload(const QByteArray &payload, bool fromCache,
                      std::optional<int> minimumVersion = std::nullopt,
                      const QString &expectedGeneration = {},
                      bool *expectationMismatch = nullptr);

    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, std::vector<MoltorinoSupporterBadge>>
        userBadges_;
    std::unordered_map<QString, MoltorinoSupporterBadge> categoryBadges_;
    std::vector<QString> categoryOrder_;
    std::unordered_set<QString> decorationsDisabledUsers_;
    std::unordered_map<QString, MoltorinoVanityLayout> vanityLayouts_;
    std::unordered_map<QString, MoltorinoVanityLayout> localVanityLayouts_;

    int version_ = -1;
    QString generation_;
    bool usingV2_ = false;
    bool initialized_ = false;
    bool requestInFlight_ = false;
    bool legacyFallbackInFlight_ = false;
    bool pendingRefresh_ = false;
    bool pendingForce_ = false;
    bool pendingBypassCache_ = false;
    std::optional<int> pendingMinimumVersion_;
    QString pendingGeneration_;
    QDateTime lastFetchAttempt_;
};

}  // namespace chatterino
