#pragma once

#include "common/SecureCredentials.hpp"

#include <QHash>
#include <QObject>
#include <QSet>

#include <deque>
#include <vector>

namespace chatterino {

class AccountCredentials : public QObject
{
public:
    using ReadCallback = SecureCredentials::ReadCallback;
    using WriteCallback = SecureCredentials::WriteCallback;
    struct Backend {
        std::function<bool()> save;
        std::function<void(const QString &, ReadCallback)> readLegacy;
        std::function<void(const QString &, WriteCallback)> removeLegacy;
    };

    explicit AccountCredentials(Backend backend = {});
    void read(const QString &key, ReadCallback callback);
    void write(const QString &key, const QString &value,
               WriteCallback callback);
    void remove(const QString &key, WriteCallback callback);

private:
    void enqueue(const QString &key, std::function<void()> operation);
    void finish(const QString &key, std::function<void()> callback);
    void clearLegacy(const QString &key, WriteCallback callback = {});
    Backend backend_;
    QHash<QString, std::deque<std::function<void()>>> pending_;
    QSet<QString> legacyPending_;
    QHash<QString, std::vector<WriteCallback>> legacyCleanups_;
};

AccountCredentials &accountCredentials();

}
