#pragma once

#include "common/Channel.hpp"
#include "common/websockets/WebSocketPool.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QQueue>
#include <QTimer>

#include <functional>

namespace chatterino {

class TikTokChannel;
class TikTokApi;

class TikTokChatServer : public QObject
{
public:
    TikTokChatServer();
    ~TikTokChatServer() override;
    void initialize();
    ChannelPtr getOrCreate(const QString &source);
    std::shared_ptr<TikTokChannel> findByHandle(QStringView source) const;
    void resolve(const std::shared_ptr<TikTokChannel> &channel);
    WebSocketHandle connectRoom(const std::shared_ptr<TikTokChannel> &channel,
                                quint64 generation);
    bool canSend(const TikTokChannel &channel) const;
    void send(const std::shared_ptr<TikTokChannel> &channel,
              const QString &text);

private:
    struct ResolveRequest {
        std::weak_ptr<TikTokChannel> channel;
        quint64 generation;
    };
    struct HttpResult {
        QByteArray data;
        int status = 0;
        int retryAfter = 0;
        bool success = false;
    };
    void pump();
    void bootstrap();
    void finishBootstrap(HttpResult result);
    QByteArray guestCookie() const;
    void request(const QUrl &url, const QByteArray &payload, bool bootstrap,
                 std::function<void(HttpResult)> callback);

    QNetworkAccessManager network_;
    QNetworkCookieJar guestCookies_;
    std::unique_ptr<WebSocketPool> sockets_;
    std::unique_ptr<TikTokApi> api_;
    pajlada::Signals::SignalHolder accountSignals_;
    QHash<QString, std::weak_ptr<TikTokChannel>> channels_;
    QQueue<ResolveRequest> queue_;
    QTimer dispatch_;
    QTimer cleanup_;
    QDateTime blockedUntil_;
    QDateTime guestRetryAt_;
    QString guestError_;
    quint64 accountGeneration_ = 0;
    int activeRequests_ = 0;
    int bootstrapFailures_ = 0;
    bool bootstrapping_ = false;
    bool initialized_ = false;
};

}
