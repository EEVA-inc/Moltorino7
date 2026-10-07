#include "TikTokLoginView.hpp"

#include "TikTokLoginBrowser.hpp"
#include "TikTokLoginQrCode.hpp"
#include "providers/tiktok/TikTokRequestSigning.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QIcon>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QXmlStreamReader>

#include <algorithm>
#include <utility>

static void initializeTikTokLoginResources()
{
    Q_INIT_RESOURCE(tiktok_login);
}

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

namespace {

class LoginQrLabel final : public QLabel
{
public:
    explicit LoginQrLabel(QWidget *parent)
        : QLabel(parent)
        , progress_(new QProgressBar(this))
    {
        this->setAlignment(Qt::AlignCenter);
        this->setTextFormat(Qt::PlainText);
        this->progress_->setRange(0, 0);
        this->progress_->setTextVisible(false);
        this->progress_->setFocusPolicy(Qt::NoFocus);
    }

    void setBusy(bool busy)
    {
        this->progress_->setVisible(busy);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        const auto height = std::max(8, this->fontMetrics().height() / 2);
        this->progress_->setGeometry(this->width() / 4,
                                     this->height() * 3 / 4,
                                     this->width() / 2, height);
    }

    void paintEvent(QPaintEvent *event) override
    {
        if (!this->pixmap().isNull())
        {
            QPainter painter(this);
            painter.fillRect(this->rect(), Qt::white);
            painter.end();
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.fillRect(this->rect(), this->palette().brush(QPalette::Base));
        painter.setPen(this->palette().color(QPalette::Mid));
        painter.drawRect(this->rect().adjusted(0, 0, -1, -1));
        const auto side = this->fontMetrics().height() * 3;
        const QRect icon((this->width() - side) / 2,
                         this->height() / 2 - side - 10, side, side);
        QIcon(u":/tiktok-login/logo.png"_s).paint(&painter, icon);
        auto font = this->font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(this->palette().color(QPalette::Text));
        painter.drawText(QRect(8, this->height() / 2 + 4, this->width() - 16,
                               this->fontMetrics().height() * 2),
                          Qt::AlignHCenter | Qt::AlignTop, this->text());
    }

private:
    QProgressBar *progress_;
};

const auto SNAPSHOT_SCRIPT = QString::fromUtf8(R"JS((() => {
    if (location.origin !== 'https://www.tiktok.com') return {};
    const result = {};
    const root = document.querySelector('[data-e2e="qr-code"]');
    if (root) {
        const mask = root.querySelector('[class*="CodeMask"], [class*="PopupCodeMask"]');
        const text = (mask?.textContent || '').toLowerCase();
        result.qrState = mask ? (text.includes('scanned') ? 'scanned' : 'covered') : 'waiting';
        if (!mask) {
            const canvas = [...root.querySelectorAll('canvas')].find(e => e.width >= 100 && e.height >= 100);
            const svg = [...root.querySelectorAll('svg')].find(e => {
                const box = e.viewBox?.baseVal;
                return box && box.width >= 21 && box.height >= 21 && e.getBoundingClientRect().width >= 100;
            });
            try {
                if (canvas) {
                    const png = canvas.toDataURL('image/png');
                    if (png.length <= 262144) result.qr = {format: 'png', data: png};
                } else if (svg) {
                    const clone = svg.cloneNode(true);
                    clone.setAttribute('xmlns', 'http://www.w3.org/2000/svg');
                    const data = new XMLSerializer().serializeToString(clone);
                    if (data.length <= 262144) result.qr = {format: 'svg', data};
                }
            } catch {}
            if (result.qr) result.qrState = 'ready';
        }
    }
    const read = id => { try { return JSON.parse(document.getElementById(id)?.textContent || '{}'); } catch { return {}; } };
    const data = window['__$UNIVERSAL_DATA$__'] || read('__UNIVERSAL_DATA_FOR_REHYDRATION__');
    const app = data.__DEFAULT_SCOPE__?.['webapp.app-context'];
    if (!app?.user) return result;
    const context = {};
    for (const key of ['wid', 'region', 'os', 'language', 'odinId', 'webIdCreatedTime']) {
        if (app[key] !== undefined) context[key] = app[key];
    }
    context.user = {};
    for (const key of ['uid', 'uniqueId', 'nickname', 'region', 'avatarLarger', 'avatarMedium', 'avatarThumb']) {
        if (app.user[key] !== undefined) context.user[key] = app.user[key];
    }
    context.webcastHost = read('__REGION__DATA__INJECTED__').__DATA__?.region_data_domains?.WEBAPP_WEBCAST_API || 'https://webcast.tiktok.com';
    context.browserLanguage = navigator.language;
    context.browserPlatform = navigator.platform;
    context.browserName = navigator.appCodeName;
    context.browserVersion = navigator.appVersion;
    context.screenWidth = screen.width;
    context.screenHeight = screen.height;
    context.timezone = Intl.DateTimeFormat().resolvedOptions().timeZone;
    const storage = {};
    for (const key of ['security-sdk/s_sdk_crypt_sdk', 'security-sdk/s_sdk_cert_key', 'security-sdk/s_sdk_sign_data_key/tt_fetch']) {
        const value = localStorage.getItem(key);
        if (value !== null) storage[key] = value;
    }
    if (!storage['security-sdk/s_sdk_crypt_sdk']) return result;
    result.userAgent = navigator.userAgent;
    result.context = context;
    result.storage = storage;
    const requests = performance.getEntriesByType('resource');
    for (let i = requests.length - 1; i >= 0; --i) {
        try {
            const url = new URL(requests[i].name);
            if (url.protocol !== 'https:' || url.username || url.password || (url.port && url.port !== '443') ||
                !['www.tiktok.com', 'webcast.tiktok.com', 'webcast.us.tiktok.com'].includes(url.hostname) ||
                !url.pathname.startsWith('/webcast/')) continue;
            const gnarly = url.searchParams.get('X-Gnarly');
            const dynosaur = url.searchParams.get('X-Dynosaur');
            if (gnarly && dynosaur && gnarly.length <= 8192 && dynosaur.length <= 8192) {
                result.gnarly = gnarly;
                result.dynosaur = dynosaur;
                result.msToken = url.searchParams.get('msToken') || '';
                break;
            }
        } catch {}
    }
    return result;
})())JS");

QImage qrImage(const QJsonObject &qr)
{
    const auto data = qr.value("data").toString();
    if (data.isEmpty() || data.size() > 262144)
        return {};
    if (qr.value("format").toString() == u"png")
    {
        if (!data.startsWith(u"data:image/png;base64,"))
            return {};
        auto bytes =
            QByteArray::fromBase64(data.sliced(22).toLatin1(),
                                   QByteArray::AbortOnBase64DecodingErrors);
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "png");
        const auto size = reader.size();
        if (size.width() < 100 || size.width() > 2048 ||
            size.height() != size.width())
            return {};
        return reader.read();
    }
    if (qr.value("format").toString() != u"svg")
        return {};

    QXmlStreamReader xml(data);
    while (!xml.atEnd())
    {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD ||
            token == QXmlStreamReader::EntityReference)
            return {};
        if (token != QXmlStreamReader::StartElement)
            continue;
        const auto name = xml.name();
        if (name != u"svg" && name != u"path" && name != u"rect" &&
            name != u"g" && name != u"circle" && name != u"title" &&
            name != u"desc")
            return {};
        for (const auto &attribute : xml.attributes())
        {
            const auto value = attribute.value();
            if (attribute.name().startsWith(u"on", Qt::CaseInsensitive) ||
                attribute.name() == u"href" ||
                value.contains(u"url(", Qt::CaseInsensitive))
                return {};
        }
    }
    if (xml.hasError())
        return {};
    QSvgRenderer renderer(data.toUtf8());
    if (!renderer.isValid())
        return {};
    const auto box = renderer.viewBoxF();
    if (box.width() < 21 || box.width() > 2048 || box.height() != box.width())
        return {};
    QImage image(768, 768, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}
}

struct TikTokLoginView::Impl {
    TikTokLoginView *view = nullptr;
    Callback callback;
    BrowserFactory browserFactory;
    std::unique_ptr<TikTokLoginBrowser> browser;
    QStackedWidget *pages = nullptr;
    QWidget *qrPage = nullptr;
    QWidget *surface = nullptr;
    LoginQrLabel *qr = nullptr;
    QLabel *status = nullptr;
    QPushButton *refresh = nullptr;
    QPushButton *other = nullptr;
    QTimer poll;
    QElapsedTimer sessionAge;
    QElapsedTimer contextWait;
    QElapsedTimer codeAge;
    QElapsedTimer requestAge;
    QString lastQr;
    QString loginError;
    bool ready = false;
    bool checking = false;
    bool connecting = false;
    bool closed = false;
    bool fullPage = false;
    bool codeShown = false;
    quint64 generation = 0;
    quint64 browserGeneration = 0;

    void clearCode(const QString &text, bool busy = false)
    {
        this->lastQr.clear();
        this->codeAge.invalidate();
        this->qr->clear();
        this->qr->setText(text);
        this->qr->setBusy(busy);
    }

    void fail(const QString &message)
    {
        this->poll.stop();
        this->ready = false;
        this->checking = false;
        this->connecting = false;
        ++this->generation;
        ++this->browserGeneration;
        this->showFullPage(false);
        this->clearCode(u"Login unavailable"_s);
        this->status->setText(message);
        this->refresh->setEnabled(TikTokLoginBrowser::available());
        this->other->setEnabled(false);
        if (this->browser)
            this->browser->close();
    }

    void showFullPage(bool visible)
    {
        this->fullPage = visible;
        if (this->browser)
            this->browser->setVisible(visible);
        const bool embedded = this->browser && this->browser->embedded();
        this->pages->setCurrentWidget(visible && embedded ? this->surface
                                                          : this->qrPage);
        this->other->setText(visible ? u"Back to QR code"_s
                                     : u"Other login methods"_s);
        this->view->resize(visible && embedded ? QSize(570, 760)
                                               : this->view->minimumSizeHint());
        if (this->browser)
            this->browser->resize();
    }

    void start()
    {
        if (this->closed || this->connecting)
            return;
        ++this->generation;
        const auto browserGeneration = ++this->browserGeneration;
        this->poll.stop();
        this->ready = false;
        this->checking = false;
        this->codeShown = false;
        this->loginError.clear();

        if (this->browser)
            this->browser->close();
        this->browser = this->browserFactory(this->view);
        if (!this->browser)
        {
            this->fail(u"This build does not include TikTok login support."_s);
            return;
        }
        this->sessionAge.start();
        this->clearCode(u"Starting TikTok..."_s, true);
        this->showFullPage(false);
        this->status->setText(u"Opening TikTok login..."_s);
        this->refresh->setEnabled(false);
        this->other->setEnabled(false);
        this->poll.start();
        QPointer<TikTokLoginView> weak(this->view);
        this->browser->start(
            this->surface,
            {
                .ready =
                    [weak, browserGeneration] {
                        if (!weak || weak->impl_->closed ||
                            weak->impl_->browserGeneration !=
                                browserGeneration)
                            return;
                        auto &self = *weak->impl_;
                        self.ready = true;
                        self.clearCode(u"Loading QR code..."_s, true);
                        self.status->setText(
                            u"Getting a fresh code from TikTok."_s);
                        self.refresh->setEnabled(true);
                        self.other->setEnabled(true);
                        self.browser->navigate(
                            QUrl(u"https://www.tiktok.com/login/qrcode"_s));
                    },
                .error =
                    [weak, browserGeneration](const QString &message) {
                        if (!weak || weak->impl_->closed ||
                            weak->impl_->browserGeneration !=
                                browserGeneration)
                            return;
                        QTimer::singleShot(
                            0, weak.data(), [weak, browserGeneration, message] {
                                if (weak && !weak->impl_->closed &&
                                    weak->impl_->browserGeneration ==
                                        browserGeneration)
                                    weak->impl_->fail(message);
                            });
                    },
            });
    }

    void refreshCode()
    {
        this->start();
    }

    void updateCode(const QJsonObject &snapshot)
    {
        if (this->connecting || this->fullPage)
            return;
        const auto state = snapshot.value("qrState").toString();
        if (state == u"scanned")
        {
            this->clearCode(u"Scanned"_s, true);
            this->status->setText(
                u"Confirm the login on your phone."_s);
            return;
        }

        if (state == u"covered" && this->codeShown)
        {
            this->clearCode(u"Get a new QR code"_s);
            this->status->setText(
                u"This code is no longer available. Get a new one to continue."_s);
            return;
        }
        const auto object = snapshot.value("qr").toObject();
        const auto data = object.value("data").toString();
        if (state == u"ready" && !data.isEmpty() && data != this->lastQr)
        {
            const auto image = qrImage(object);
            if (!image.isNull())
            {
                this->lastQr = data;
                this->codeShown = true;
                this->codeAge.start();
                const auto ratio = this->qr->devicePixelRatioF();
                auto branded = renderTikTokLoginQr(
                    image,
                    QIcon(u":/tiktok-login/logo.png"_s).pixmap(128).toImage(),
                    qRound(this->qr->width() * ratio));
                branded.setDevicePixelRatio(ratio);
                this->qr->setBusy(false);
                this->qr->setPixmap(QPixmap::fromImage(branded));
                this->status->setText(
                    u"Scan this code with your phone."_s);
            }
        }

        if (this->codeAge.isValid() && this->codeAge.elapsed() > 150000)
        {
            this->qr->clear();
            this->qr->setText(u"Get a new QR code"_s);
            this->status->setText(u"Refresh the code to continue."_s);
        }
        else if (this->lastQr.isEmpty() && this->sessionAge.elapsed() > 30000)
        {
            this->status->setText(
                u"If the code does not appear, open TikTok's other login methods."_s);
        }
    }

    void check()
    {
        if (this->closed)
            return;
        const bool contextTimedOut =
            this->connecting && this->contextWait.elapsed() > 90000;
        if (contextTimedOut || this->sessionAge.elapsed() > 600000 ||
            (!this->ready && this->sessionAge.elapsed() > 45000) ||
            (this->checking && this->requestAge.elapsed() > 20000))
        {
            this->fail(
                contextTimedOut && !this->loginError.isEmpty()
                    ? this->loginError
                    : u"TikTok did not finish connecting. Get a new code and try again."_s);
            return;
        }
        if (!this->ready || this->checking)
            return;
        this->checking = true;
        this->requestAge.start();
        const auto generation = this->generation;
        QPointer<TikTokLoginView> weak(this->view);
        this->browser->evaluate(SNAPSHOT_SCRIPT, [weak, generation](
                                                     QJsonObject snapshot) {
            if (!weak || weak->impl_->closed ||
                generation != weak->impl_->generation)
                return;
            auto &self = *weak->impl_;
            self.updateCode(snapshot);
            self.browser->cookies([weak, generation,
                                   snapshot = std::move(snapshot)](
                                      QList<QNetworkCookie> cookies) mutable {
                if (!weak || weak->impl_->closed ||
                    generation != weak->impl_->generation)
                    return;
                auto &self = *weak->impl_;
                self.checking = false;
                bool loggedIn = false;
                for (const auto &cookie : cookies)
                {
                    if (cookie.name() == "sessionid" &&
                        !cookie.value().isEmpty() &&
                        (cookie.domain() == u"tiktok.com" ||
                         cookie.domain().endsWith(u".tiktok.com")) &&
                        (cookie.isSessionCookie() ||
                         cookie.expirationDate() >
                             QDateTime::currentDateTimeUtc()))
                        loggedIn = true;
                }
                if (!loggedIn)
                    return;
                if (!self.connecting)
                {
                    self.connecting = true;
                    self.contextWait.start();
                    self.showFullPage(false);
                    self.clearCode(u"Connecting..."_s, true);
                    self.status->setText(
                        u"Connecting your TikTok account..."_s);
                    self.refresh->setEnabled(false);
                    self.other->setEnabled(false);
                    self.browser->navigate(
                        QUrl(u"https://www.tiktok.com/live"_s));
                    return;
                }
                const auto context = snapshot.value("context").toObject();
                const auto user = context.value("user").toObject();
                const auto storage = snapshot.value("storage").toObject();
                const auto gnarly = snapshot.value("gnarly").toString();
                const auto dynosaur = snapshot.value("dynosaur").toString();
                if (user.value("uid").toString().isEmpty() ||
                    user.value("uniqueId").toString().isEmpty() ||
                    storage.value("security-sdk/s_sdk_crypt_sdk")
                        .toString()
                        .isEmpty() ||
                    gnarly.isEmpty() || dynosaur.isEmpty())
                    return;
                TikTokLoginExport data{snapshot.value("userAgent").toString(),
                                       std::move(cookies),
                                       context,
                                       storage,
                                       gnarly,
                                       dynosaur,
                                       snapshot.value("msToken").toString()};

                TikTokSession session{data.userAgent, data.cookies,
                                      data.context,   data.ticketGuardStorage,
                                      data.msToken,   9};
                session.context.insert(u"signingGnarly"_s, data.gnarly);
                session.context.insert(u"signingDynosaur"_s, data.dynosaur);
                const auto stored = encodeTikTokSession(session);
                const auto signing = validateTikTokSigningSession(session);
                if (!stored || !signing)
                {
                    self.loginError =
                        !stored ? stored.error() : signing.error();
                    return;
                }


                self.poll.stop();
                QTimer::singleShot(
                    0, weak.data(),
                    [weak, generation, data = std::move(data)]() mutable {
                        if (!weak || weak->impl_->closed ||
                            weak->impl_->generation != generation)
                            return;
                        auto callback = std::move(weak->impl_->callback);
                        weak->accept();
                        if (callback)
                            callback(std::move(data));
                    });
            });
        });
    }

    void close()
    {
        if (std::exchange(this->closed, true))
            return;
        ++this->generation;
        ++this->browserGeneration;
        this->poll.stop();
        this->callback = {};
        this->lastQr.clear();
        this->qr->clear();
        this->qr->setBusy(false);
        if (this->browser)
            this->browser->close();
    }
};

TikTokLoginView::TikTokLoginView(QWidget *parent, Callback callback)
    : TikTokLoginView(parent, std::move(callback),
                      &TikTokLoginBrowser::create)
{
}

TikTokLoginView::TikTokLoginView(QWidget *parent, Callback callback,
                                 BrowserFactory browserFactory)
    : QDialog(parent)
    , impl_(std::make_unique<Impl>())
{
    initializeTikTokLoginResources();
    this->setWindowTitle(u"Connect TikTok account"_s);
    this->setAttribute(Qt::WA_WindowPropagation);
    if (parent)
    {
        this->setPalette(parent->palette());
        this->setFont(parent->font());
    }
    this->impl_->view = this;
    this->impl_->callback = std::move(callback);
    this->impl_->browserFactory = std::move(browserFactory);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    this->impl_->pages = new QStackedWidget(this);
    this->impl_->qrPage = new QWidget(this);
    auto *qrLayout = new QVBoxLayout(this->impl_->qrPage);
    qrLayout->setContentsMargins(0, 0, 0, 0);
    qrLayout->setSpacing(14);

    auto *heading = new QLabel(u"Scan to log in"_s, this);
    auto font = heading->font();
    font.setBold(true);
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() + 2);
    else
        font.setPixelSize(font.pixelSize() + 2);
    heading->setFont(font);
    heading->setAlignment(Qt::AlignCenter);
    qrLayout->addWidget(heading);

    this->impl_->qr = new LoginQrLabel(this);
    this->impl_->qr->setObjectName(u"tiktokLoginQr"_s);
    this->impl_->qr->setAccessibleName(u"TikTok login QR code"_s);
    const auto codeSize = std::max(264, this->fontMetrics().height() * 17);
    this->impl_->qrPage->setMinimumWidth(codeSize + 80);
    this->impl_->qr->setFixedSize(codeSize, codeSize);
    this->impl_->qr->setText(u"Starting TikTok..."_s);
    this->impl_->qr->setBusy(true);
    qrLayout->addWidget(this->impl_->qr, 0, Qt::AlignHCenter);
    this->impl_->qrPage->setSizePolicy(QSizePolicy::Preferred,
                                      QSizePolicy::Maximum);
    this->impl_->pages->addWidget(this->impl_->qrPage);

    this->impl_->surface = new QWidget(this);
    this->impl_->surface->resize(570, 660);
    this->impl_->surface->setAttribute(Qt::WA_NativeWindow);
    this->impl_->surface->installEventFilter(this);
    this->impl_->pages->addWidget(this->impl_->surface);
    layout->addWidget(this->impl_->pages);

    this->impl_->status =
        new QLabel(u"Opening TikTok login..."_s, this);
    this->impl_->status->setObjectName(u"tiktokLoginStatus"_s);
    this->impl_->status->setWordWrap(true);
    this->impl_->status->setFixedWidth(codeSize);
    this->impl_->status->setMinimumHeight(this->fontMetrics().height() * 2);
    this->impl_->status->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    this->impl_->status->setTextFormat(Qt::PlainText);
    layout->addWidget(this->impl_->status, 0, Qt::AlignHCenter);

    auto *actions = new QHBoxLayout;
    this->impl_->refresh = new QPushButton(u"New QR code"_s, this);
    this->impl_->other = new QPushButton(u"Other login methods"_s, this);
    this->impl_->refresh->setObjectName(u"tiktokLoginRefresh"_s);
    this->impl_->other->setObjectName(u"tiktokLoginOther"_s);
    const auto buttonHeight = this->fontMetrics().height() + 14;
    this->impl_->refresh->setMinimumHeight(buttonHeight);
    this->impl_->other->setMinimumHeight(buttonHeight);
    this->impl_->refresh->setAutoDefault(false);
    this->impl_->other->setAutoDefault(false);
    actions->setSpacing(8);
    actions->addWidget(this->impl_->refresh, 1);
    actions->addWidget(this->impl_->other, 1);
    layout->addLayout(actions);

    QObject::connect(this->impl_->refresh, &QPushButton::clicked, this, [this] {
        this->impl_->refreshCode();
    });
    QObject::connect(this->impl_->other, &QPushButton::clicked, this, [this] {
        auto &self = *this->impl_;
        if (!self.browser || !self.ready)
            return;
        if (self.fullPage)
            self.refreshCode();
        else
        {
            ++self.generation;
            self.checking = false;
            self.showFullPage(true);
            self.status->setText(u"Complete the login in TikTok."_s);
            self.browser->navigate(QUrl(u"https://www.tiktok.com/login"_s));
        }
    });

    this->setSizeGripEnabled(false);
    this->adjustSize();

    this->impl_->poll.setInterval(1500);
    QObject::connect(&this->impl_->poll, &QTimer::timeout, this, [this] {
        this->impl_->check();
    });
    QTimer::singleShot(0, this, [this] {
        this->impl_->start();
    });
}

TikTokLoginView::~TikTokLoginView()
{
    this->impl_->close();
}
bool TikTokLoginView::available()
{
    return TikTokLoginBrowser::available();
}
bool TikTokLoginView::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->impl_->surface && event->type() == QEvent::Resize &&
        this->impl_->browser)
        this->impl_->browser->resize();
    return QDialog::eventFilter(object, event);
}
void TikTokLoginView::done(int result)
{
    this->impl_->close();
    QDialog::done(result);
}
}
