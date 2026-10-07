#pragma once

#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

class QWidget;

namespace chatterino {

class TikTokLoginBrowser : public QObject
{
public:
    struct Callbacks {
        std::function<void()> ready;
        std::function<void(const QString &)> error;
    };
    using ScriptCallback = std::function<void(QJsonObject)>;
    using CookiesCallback = std::function<void(QList<QNetworkCookie>)>;

    using QObject::QObject;
    ~TikTokLoginBrowser() override = default;
    virtual void start(QWidget *surface, Callbacks callbacks) = 0;
    virtual void navigate(const QUrl &url) = 0;
    virtual void evaluate(const QString &script, ScriptCallback callback) = 0;
    virtual void cookies(CookiesCallback callback) = 0;
    virtual void setVisible(bool visible) = 0;
    virtual void resize() = 0;
    virtual void close() = 0;
    virtual bool embedded() const
    {
        return true;
    }

    static bool available();
    static std::unique_ptr<TikTokLoginBrowser> create(QObject *parent);
    static bool allowedNavigation(const QUrl &url)
    {
        return url.isValid() && url.scheme() == u"https" &&
               url.userInfo().isEmpty() && url.port(443) == 443 &&
               (url.host() == u"tiktok.com" ||
                url.host().endsWith(u".tiktok.com"));
    }
};

}
