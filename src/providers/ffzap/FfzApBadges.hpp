#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <pajlada/signals/signal.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <shared_mutex>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class FfzApBadges final : public QObject
{
public:
    struct Badge {
        EmotePtr emote;
        QColor color;
    };

    void initialize();
    std::optional<Badge> getBadge(const UserId &userID) const;

    pajlada::Signals::NoArgSignal badgesUpdated;

private:
    struct BadgeEntry {
        QString tooltip;
        EmotePtr emote;
        QColor color;
    };

    void loadCache();
    void requestBadges(bool useEtag = true);
    bool applyPayload(const QByteArray &payload);
    bool saveCache(const QByteArray &payload) const;
    void saveMetadata() const;
    void scheduleRetry();

    mutable std::shared_mutex mutex_;
    mutable boost::unordered_flat_map<QString, BadgeEntry> badges_;
    QString etag_;
    bool requestInFlight_ = false;
    bool retryWithoutEtag_ = false;
    bool loaded_ = false;
    int requestAttempts_ = 0;
};

}
