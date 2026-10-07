#pragma once

#include "util/Expected.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QString>
#include <QUrl>

namespace chatterino {

struct TikTokSession {
    QString userAgent;
    QList<QNetworkCookie> cookies;
    QJsonObject context;
    QJsonObject ticketGuardStorage;
    QString msToken;
    int msTokenStatus = 0;

    QByteArray csrfToken;
    QString csrfOrigin;
    QDateTime csrfExpires;

    QByteArray cookieHeader(const QUrl &url) const;
    QByteArray cookieValue(const QByteArray &name, const QUrl &url) const;
    bool hasCredentials() const;
    void updateCookies(const QList<QNetworkCookie> &values, const QUrl &url);
    bool updateMsToken(const QByteArray &token, int requestStatus);
};

bool isTikTokAccountUrl(const QUrl &url);
ExpectedStr<QString> encodeTikTokSession(const TikTokSession &session);
ExpectedStr<TikTokSession> decodeTikTokSession(const QString &encoded);

}
