#pragma once

#include "common/Channel.hpp"
#include "common/ChannelChatters.hpp"
#include "common/websockets/WebSocketPool.hpp"
#include "providers/tiktok/TikTokProtocol.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QSet>

namespace chatterino {

class TikTokChatServer;
class ChannelAvatarSource;

class TikTokChannel : public Channel, public ChannelChatters
{
public:
    TikTokChannel(QString handle, TikTokChatServer &server);
    ~TikTokChannel() override;

    std::weak_ptr<TikTokChannel> weakFromThis();
    const QString &getDisplayName() const override;
    bool isLive() const override;
    bool isWritable() const override;
    bool canSendMessage() const override;
    void sendMessage(const QString &message) override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;
    QUrl browserUrl() const;
    const TikTokRoom &room() const;
    std::shared_ptr<ChannelAvatarSource> channelAvatar() const;
    std::optional<TikTokAuthor> author(QStringView id) const;

    void start();
    void stop();
    void applyRoom(TikTokRoom room);
    void resolutionFailed(const QString &error);
    void socketOpened(quint64 generation);
    void socketClosed(quint64 generation);
    void receive(quint64 generation, tiktok::Batch batch);
    void protocolFailed(quint64 generation);

    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal streamDataChanged;
    pajlada::Signals::NoArgSignal userStateChanged;

private:
    friend class TikTokChatServer;
    void openSocket();
    void closeSocket();
    void retry(const QString &error);
    void notice(const QString &text);
    TikTokAuthor rememberAuthor(TikTokAuthor author);
    void rememberEvent(const QString &key);

    TikTokChatServer &server_;
    QObject lifetime_;
    QTimer probeTimer_;
    QTimer heartbeatTimer_;
    QElapsedTimer lastTraffic_;
    WebSocketHandle socket_;
    TikTokRoom room_;
    std::shared_ptr<ChannelAvatarSource> channelAvatar_;
    QString lastNotice_;
    QString endedRoomID_;
    QHash<QString, TikTokAuthor> authors_;
    QQueue<QString> authorOrder_;
    QSet<QString> seen_;
    QQueue<QString> seenOrder_;
    QSet<QString> deletedMessages_;
    QQueue<QString> deletedMessageOrder_;
    quint64 generation_ = 0;
    quint64 resolutionGeneration_ = 0;
    int failures_ = 0;
    bool resolvePending_ = false;
    bool stopped_ = false;
    bool connected_ = false;
};

}
