#pragma once

#include "providers/twitch/TwitchNameHistory.hpp"

#include <QHash>

#include <deque>
#include <memory>

namespace chatterino::detail {

using TwitchNameHistoryPtr = std::shared_ptr<TwitchNameHistory>;

class TwitchNameHistoryMemoryCache
{
public:
    explicit TwitchNameHistoryMemoryCache(qsizetype maxHistories);

    void insert(const TwitchNameHistory &history);
    TwitchNameHistoryPtr findByUserId(const QString &userId) const;
    TwitchNameHistoryPtr findByLogin(const QString &login) const;

    const std::deque<TwitchNameHistoryPtr> &histories() const;
    qsizetype size() const;

private:
    void rebuildIndexes();

    qsizetype maxHistories_;
    QHash<QString, TwitchNameHistoryPtr> userIds_;
    QHash<QString, TwitchNameHistoryPtr> currentLogins_;
    QHash<QString, TwitchNameHistoryPtr> historicalLogins_;
    std::deque<TwitchNameHistoryPtr> order_;
};

}
