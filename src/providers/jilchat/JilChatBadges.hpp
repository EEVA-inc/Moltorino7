#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <boost/unordered/unordered_flat_map.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <shared_mutex>
#include <vector>

namespace chatterino {

struct Emote;

class JilChatBadges final : public QObject
{
public:
    void initialize();
    std::vector<std::shared_ptr<const Emote>> getBadges(
        const UserId &userID) const;

    pajlada::Signals::NoArgSignal badgesUpdated;

private:
    bool applyPayload(const QByteArray &payload);
    void requestBadges();

    mutable std::shared_mutex mutex_;
    boost::unordered_flat_map<QString,
                              std::vector<std::shared_ptr<const Emote>>>
        users_;
    QByteArray lastPayload_;
    QTimer refreshTimer_;
    bool requestInFlight_ = false;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
