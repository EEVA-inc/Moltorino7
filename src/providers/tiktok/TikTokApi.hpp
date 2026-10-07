#pragma once

#include "providers/tiktok/TikTokAccount.hpp"

#include <pajlada/signals/signal.hpp>
#include <QHash>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>

namespace chatterino {

struct TikTokApiRequest {
    QUrl url;
    QByteArray method;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct TikTokApiResponse {
    int status = 0;
    bool transportSucceeded = false;
    QByteArray body;
    QMap<QByteArray, QByteArray> headers;
    QList<QNetworkCookie> cookies;
    bool dispatched = true;
};

struct TikTokSendResult {
    enum class Status { Accepted, Rejected, LoginRequired, Limited, Unknown };
    Status status = Status::Rejected;
    QString messageID;
    QString message;
};

class TikTokApi : public QObject
{
public:
    using ResponseCallback = std::function<void(TikTokApiResponse)>;
    using Transport = std::function<void(TikTokApiRequest, ResponseCallback)>;
    using VerifyCallback = std::function<void(ExpectedStr<TikTokAccountData>)>;
    using SendCallback = std::function<void(TikTokSendResult)>;
    explicit TikTokApi(QObject *parent = nullptr, Transport transport = {});
    void verifyAccount(TikTokAccountData data, VerifyCallback callback);
    void sendMessage(const std::shared_ptr<TikTokAccount> &account,
                     const QString &roomID, const QString &handle,
                     const QString &content, SendCallback callback,
                     std::function<bool()> isCurrent = {});
    pajlada::Signals::Signal<std::shared_ptr<TikTokAccount>> sessionChanged;

private:
    using TokenResult = Expected<void, TikTokSendResult>;
    using TokenCallback = std::function<void(TokenResult)>;
    void csrf(const std::shared_ptr<TikTokAccount> &account, const QUrl &url,
              TokenCallback callback);
    void request(const std::shared_ptr<TikTokAccount> &account,
                 TikTokApiRequest request, ResponseCallback callback);
    void networkRequest(TikTokApiRequest request, ResponseCallback callback);
    QNetworkAccessManager network_;
    Transport transport_;
    QHash<QString, int> pending_;
    QHash<QString, QDateTime> limitedUntil_;
};

QUrl tikTokChatEndpoint(const TikTokSession &session);
QString tikTokSerializeQuery(const QMap<QString, QString> &parameters);
QMap<QString, QString> tikTokChatParameters(const TikTokSession &session,
                                            const QString &roomID,
                                            const QString &content);
QByteArray tikTokChatBody(const QString &roomID, const QString &content);
TikTokSendResult classifyTikTokSendResponse(const TikTokApiResponse &response);

}
