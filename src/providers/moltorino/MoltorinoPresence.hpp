#pragma once

#include "common/websockets/WebSocketPool.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace chatterino {

class MoltorinoBadgeSocketListener;

class MoltorinoPresence : public QObject
{
public:
    MoltorinoPresence();
    ~MoltorinoPresence() override;
    static MoltorinoPresence &instance();

    void init();
    void startHeartbeat();
    void stopHeartbeat();

private:
    void applyHeartbeatSettings(bool sendNow = false);
    void sendHeartbeat(bool force = false);
    void connectBadgeSocket();
    void disconnectBadgeSocket();
    void handleBadgeSocketOpen(int generation);
    void handleBadgeSocketClosed(int generation);
    void handleBadgeSocketMessage(int generation, QByteArray data);

    QJsonObject makePayload() const;
    QJsonObject activeAccount() const;

    QTimer heartbeatTimer_;
    QTimer badgeSocketReconnectTimer_;
    QTimer badgeRefreshTimer_;
    pajlada::Signals::SignalHolder signalHolder_;

    std::unique_ptr<WebSocketPool> badgeSocketPool_;
    WebSocketHandle badgeSocket_;
    QUrl heartbeatUrl_;
    QUrl badgeSocketUrl_;
    QString clientInstanceId_;
    QString pendingBadgeGeneration_;
    int pendingBadgeVersion_ = -1;

    bool initialized_{};
    bool running_{};
    bool heartbeatInFlight_{};
    bool heartbeatQueued_{};
    bool badgeSocketConnecting_{};
    bool badgeSocketOpen_{};
    int badgeSocketGeneration_{};
    int badgeSocketBackoffStep_{};

    friend class MoltorinoBadgeSocketListener;
};

MoltorinoPresence *getMoltorinoPresence();

}
