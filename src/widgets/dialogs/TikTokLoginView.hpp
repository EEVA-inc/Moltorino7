#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QString>

#include <functional>
#include <memory>

namespace chatterino {

struct TikTokLoginExport {
    QString userAgent;
    QList<QNetworkCookie> cookies;
    QJsonObject context;
    QJsonObject ticketGuardStorage;
    QString gnarly;
    QString dynosaur;
    QString msToken;
};

class TikTokLoginBrowser;

class TikTokLoginView : public QDialog
{
public:
    using Callback = std::function<void(TikTokLoginExport)>;
    using BrowserFactory =
        std::function<std::unique_ptr<TikTokLoginBrowser>(QObject *)>;
    explicit TikTokLoginView(QWidget *parent, Callback callback);
    TikTokLoginView(QWidget *parent, Callback callback,
                    BrowserFactory browserFactory);
    ~TikTokLoginView() override;
    static bool available();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void done(int result) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
