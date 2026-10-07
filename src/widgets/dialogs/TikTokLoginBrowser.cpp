#include "TikTokLoginBrowser.hpp"

#ifdef MOLTORINO_TIKTOK_WEBKITGTK
#    include <QCoreApplication>
#    include <QDateTime>
#    include <QFileInfo>
#    include <QJsonArray>
#    include <QJsonDocument>
#    include <QMap>
#    include <QPointer>
#    include <QProcess>
#    include <QTimer>

#    include <utility>
#endif

namespace chatterino {

#ifdef MOLTORINO_TIKTOK_WEBKITGTK
namespace {
QString gtkHelperPath()
{
    const auto directory = QCoreApplication::applicationDirPath();
    const auto name = QStringLiteral("/moltorino-tiktok-browser");
    const auto buildPath = directory + name;
    return QFileInfo::exists(buildPath)
               ? buildPath
               : directory + QStringLiteral("/../libexec/moltorino") + name;
}

class GtkProcessBrowser final : public TikTokLoginBrowser
{
public:
    using TikTokLoginBrowser::TikTokLoginBrowser;
    ~GtkProcessBrowser() override
    {
        this->close();
    }

    void start(QWidget *, Callbacks callbacks) override
    {
        this->close();
        this->callbacks_ = std::move(callbacks);
        auto *process = new QProcess(this);
        this->process_ = process;
        process->setProgram(gtkHelperPath());

        process->setStandardErrorFile(QProcess::nullDevice());
        QObject::connect(process, &QProcess::readyReadStandardOutput, this,
                         [this] {
                             this->read();
                         });
        QObject::connect(process, &QProcess::errorOccurred, this,
                         [this](QProcess::ProcessError error) {
                             this->fail(error == QProcess::FailedToStart);
                         });
        QObject::connect(process, &QProcess::finished, this, [this] {
            this->fail();
        });
        QTimer::singleShot(45000, this, [this, process = QPointer(process)] {
            if (process && this->process_ == process && !this->ready_)
            {
                this->fail();
            }
        });
        process->start();
    }

    void navigate(const QUrl &url) override
    {
        if (allowedNavigation(url))
        {
            this->send({{"op", "navigate"},
                        {"url", url.toString(QUrl::FullyEncoded)}});
        }
    }

    void evaluate(const QString &script, ScriptCallback callback) override
    {
        if (!this->ready_)
        {
            callback({});
            return;
        }
        const auto id = ++this->nextRequest_;
        this->scripts_.insert(id, std::move(callback));
        this->send({{"op", "evaluate"},
                    {"id", QString::number(id)},
                    {"script", script}});
    }

    void cookies(CookiesCallback callback) override
    {
        if (!this->ready_)
        {
            callback({});
            return;
        }
        const auto id = ++this->nextRequest_;
        this->cookies_.insert(id, std::move(callback));
        this->send({{"op", "cookies"}, {"id", QString::number(id)}});
    }

    void setVisible(bool visible) override
    {
        this->send({{"op", "visible"}, {"value", visible ? "yes" : "no"}});
    }

    void resize() override
    {
    }

    bool embedded() const override
    {
        return false;
    }

    void close() override
    {
        this->callbacks_ = {};
        this->ready_ = false;
        this->scripts_.clear();
        this->cookies_.clear();
        this->incoming_.clear();
        auto *process = std::exchange(this->process_, nullptr);
        if (!process)
        {
            return;
        }
        QObject::disconnect(process, nullptr, this, nullptr);
        process->setParent(QCoreApplication::instance());
        if (process->state() == QProcess::NotRunning)
        {
            process->deleteLater();
            return;
        }
        QObject::connect(process, &QProcess::finished, process,
                         &QObject::deleteLater);
        QObject::connect(process, &QProcess::errorOccurred, process, [process] {
            if (process->state() == QProcess::NotRunning)
            {
                process->deleteLater();
            }
        });
        process->write("{\"op\":\"close\"}\n");
        process->closeWriteChannel();

        QTimer::singleShot(1500, process, [process] {
            if (process->state() != QProcess::NotRunning)
            {
                process->kill();
            }
            else
            {
                process->deleteLater();
            }
        });
    }

private:
    static constexpr qsizetype MAX_FRAME = 2 * 1024 * 1024;

    void fail(bool missingHelper = false)
    {
        const auto callback = this->callbacks_.error;
        this->close();
        if (callback)
        {
            callback(
                missingHelper
                    ? QStringLiteral("TikTok login could not load its browser. "
                                     "Reinstall Moltorino and try again.")
                    : QStringLiteral("TikTok login stopped responding. Close "
                                     "this window and try again."));
        }
    }

    void send(QJsonObject object)
    {
        if (!this->process_ || !this->ready_)
        {
            return;
        }
        auto frame = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (frame.size() > MAX_FRAME ||
            this->process_->bytesToWrite() > MAX_FRAME)
        {
            this->fail();
            return;
        }
        frame += '\n';
        this->process_->write(frame);
    }

    void read()
    {
        QPointer<GtkProcessBrowser> weak(this);
        const QPointer<QProcess> process = this->process_;
        this->incoming_ += process->readAllStandardOutput();
        if (this->incoming_.size() > 2 * MAX_FRAME)
        {
            this->fail();
            return;
        }
        qsizetype newline;
        while ((newline = this->incoming_.indexOf('\n')) >= 0)
        {
            if (newline > MAX_FRAME)
            {
                this->fail();
                return;
            }
            const auto object =
                QJsonDocument::fromJson(this->incoming_.first(newline))
                    .object();
            this->incoming_.remove(0, newline + 1);
            const auto type = object.value("type").toString();
            const auto id = object.value("id").toString().toULongLong();
            if (type == u"ready" && !this->ready_)
            {
                this->ready_ = true;
                const auto ready = this->callbacks_.ready;
                if (ready)
                {
                    ready();
                }
            }
            else if (type == u"script" && id)
            {
                auto callback = this->scripts_.take(id);
                if (callback)
                {
                    callback(object.value("value").toObject());
                }
            }
            else if (type == u"cookies" && id)
            {
                auto callback = this->cookies_.take(id);
                if (callback)
                {
                    QList<QNetworkCookie> exported;
                    const auto cookies = object.value("value").toArray();
                    if (cookies.size() <= 512)
                    {
                        for (const auto &value : cookies)
                        {
                            const auto cookie = value.toObject();
                            const auto domain =
                                cookie.value("domain").toString();
                            if (domain != u"tiktok.com" &&
                                !domain.endsWith(u".tiktok.com"))
                            {
                                continue;
                            }
                            QNetworkCookie saved(
                                cookie.value("name").toString().toUtf8(),
                                cookie.value("value").toString().toUtf8());
                            saved.setDomain(domain);
                            saved.setPath(cookie.value("path").toString());
                            saved.setSecure(cookie.value("secure").toBool());
                            saved.setHttpOnly(
                                cookie.value("httpOnly").toBool());
                            const auto expires =
                                cookie.value("expires").toInteger();
                            if (expires)
                            {
                                saved.setExpirationDate(
                                    QDateTime::fromSecsSinceEpoch(expires)
                                        .toUTC());
                            }
                            exported.push_back(std::move(saved));
                        }
                    }
                    callback(std::move(exported));
                }
            }
            else
            {
                this->fail();
                return;
            }
            if (!weak || !process || this->process_ != process)
            {
                return;
            }
        }
        if (this->incoming_.size() > MAX_FRAME)
        {
            this->fail();
        }
    }

    QProcess *process_ = nullptr;
    bool ready_ = false;
    Callbacks callbacks_;
    quint64 nextRequest_ = 0;
    QByteArray incoming_;
    QMap<quint64, ScriptCallback> scripts_;
    QMap<quint64, CookiesCallback> cookies_;
};
}
#endif

#if defined(MOLTORINO_TIKTOK_WEBVIEW2) || defined(MOLTORINO_TIKTOK_WEBKIT)
std::unique_ptr<TikTokLoginBrowser> createPlatformTikTokLoginBrowser(
    QObject *parent);
#endif

bool TikTokLoginBrowser::available()
{
#if defined(MOLTORINO_TIKTOK_WEBKITGTK)
    return QFileInfo(gtkHelperPath()).isExecutable();
#elif defined(MOLTORINO_TIKTOK_WEBVIEW2) || defined(MOLTORINO_TIKTOK_WEBKIT)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<TikTokLoginBrowser> TikTokLoginBrowser::create(QObject *parent)
{
#if defined(MOLTORINO_TIKTOK_WEBKITGTK)
    return std::make_unique<GtkProcessBrowser>(parent);
#elif defined(MOLTORINO_TIKTOK_WEBVIEW2) || defined(MOLTORINO_TIKTOK_WEBKIT)
    return createPlatformTikTokLoginBrowser(parent);
#else
    return {};
#endif
}

}
