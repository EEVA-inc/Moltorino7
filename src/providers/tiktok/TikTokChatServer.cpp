#include "providers/tiktok/TikTokChatServer.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/tiktok/TikTokApi.hpp"
#include "providers/tiktok/TikTokChannel.hpp"
#include "providers/tiktok/TikTokProtocol.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

namespace {
class Listener : public WebSocketListener
{
public:
    Listener(std::weak_ptr<TikTokChannel> channel, quint64 generation)
        : channel_(std::move(channel))
        , generation_(generation)
    {
    }

    void onOpen() override
    {
        this->post([](auto &channel, auto generation) {
            channel.socketOpened(generation);
        });
    }

    void onTextMessage(QByteArray) override
    {
    }

    void onBinaryMessage(QByteArray data) override
    {
        if (this->pending_->fetch_add(1) >= 8)
        {
            this->pending_->fetch_sub(1);
            if (!this->overflowed_.exchange(true))
            {
                this->post([](auto &channel, auto generation) {
                    channel.protocolFailed(generation);
                });
            }
            return;
        }
        auto batch = tiktok::decodeFrame(data);
        this->post([batch = std::move(batch), pending = this->pending_](
                       auto &channel, auto generation) mutable {
            pending->fetch_sub(1);
            if (batch)
            {
                channel.receive(generation, std::move(*batch));
            }
            else
            {
                channel.protocolFailed(generation);
            }
        });
    }

    void onClose(std::unique_ptr<WebSocketListener> self) override
    {
        this->post([](auto &channel, auto generation) {
            channel.socketClosed(generation);
        });
    }

private:
    void post(auto action)
    {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [weak = this->channel_, generation = this->generation_,
             action = std::move(action)]() mutable {
                if (auto channel = weak.lock())
                {
                    action(*channel, generation);
                }
            },
            Qt::QueuedConnection);
    }
    std::weak_ptr<TikTokChannel> channel_;
    quint64 generation_;
    std::shared_ptr<std::atomic<int>> pending_ =
        std::make_shared<std::atomic<int>>(0);
    std::atomic<bool> overflowed_{false};
};
}

TikTokChatServer::TikTokChatServer()
{
    this->dispatch_.setSingleShot(true);
    this->dispatch_.setInterval(500);
    QObject::connect(&this->dispatch_, &QTimer::timeout, this, [this] {
        this->pump();
    });
    this->cleanup_.setInterval(60000);
    QObject::connect(&this->cleanup_, &QTimer::timeout, this, [this] {
        for (auto it = this->channels_.begin(); it != this->channels_.end();)
        {
            it = it.value().expired() ? this->channels_.erase(it)
                                      : std::next(it);
        }
    });
}

TikTokChatServer::~TikTokChatServer()
{
    this->initialized_ = false;
    this->api_.reset();
    for (const auto &weak : this->channels_)
    {
        if (auto channel = weak.lock())
        {
            channel->stop();
        }
    }
}

void TikTokChatServer::initialize()
{
    this->initialized_ = true;
    this->cleanup_.start();
    auto refreshAccounts = [this] {
        for (const auto &weak : this->channels_)
        {
            if (auto channel = weak.lock())
            {
                channel->userStateChanged.invoke();
            }
        }
    };
    auto &accounts = getApp()->getAccounts()->tiktok;
    this->accountSignals_.managedConnect(accounts.currentChanged,
                                         [this, refreshAccounts] {
                                             ++this->accountGeneration_;
                                             refreshAccounts();
                                         });
    this->accountSignals_.managedConnect(accounts.credentialsChanged,
                                         refreshAccounts);
    if (!this->queue_.isEmpty())
    {
        this->dispatch_.start();
    }
}

ChannelPtr TikTokChatServer::getOrCreate(const QString &source)
{
    const auto handle = normalizeTikTokHandle(source);
    if (!handle)
    {
        return Channel::getEmpty();
    }
    if (auto existing = this->channels_.value(*handle).lock())
    {
        return existing;
    }
    auto channel = std::make_shared<TikTokChannel>(*handle, *this);
    this->channels_.insert(*handle, channel);
    channel->start();
    return channel;
}

std::shared_ptr<TikTokChannel> TikTokChatServer::findByHandle(
    QStringView source) const
{
    const auto handle = normalizeTikTokHandle(source);
    return handle ? this->channels_.value(*handle).lock() : nullptr;
}

void TikTokChatServer::resolve(const std::shared_ptr<TikTokChannel> &channel)
{
    if (channel->resolvePending_ || channel->stopped_)
    {
        return;
    }
    channel->resolvePending_ = true;
    this->queue_.enqueue({channel, channel->resolutionGeneration_});
    if (this->initialized_ && !this->dispatch_.isActive())
    {
        this->dispatch_.start();
    }
}

QByteArray TikTokChatServer::guestCookie() const
{
    for (const auto &cookie :
         this->guestCookies_.cookiesForUrl(QUrl(u"https://www.tiktok.com/"_s)))
    {
        if (cookie.name() == "ttwid")
        {
            return cookie.toRawForm(QNetworkCookie::NameAndValueOnly);
        }
    }
    return {};
}

void TikTokChatServer::request(const QUrl &url, const QByteArray &payload,
                               bool bootstrap,
                               std::function<void(HttpResult)> callback)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", tiktok::userAgent());
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Referer", "https://www.tiktok.com/");
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(20000);
    const auto cookie = this->guestCookie();
    if (!cookie.isEmpty())
    {
        request.setRawHeader("Cookie", cookie);
    }
    if (!payload.isNull())
    {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          u"application/x-www-form-urlencoded"_s);
        request.setRawHeader("Origin", "https://www.tiktok.com");
    }
    auto *reply = payload.isNull() ? this->network_.get(request)
                                   : this->network_.post(request, payload);
    reply->setReadBufferSize(256 * 1024);
    auto body = std::make_shared<QByteArray>();
    auto oversized = std::make_shared<bool>(false);
    QObject::connect(reply, &QNetworkReply::readyRead, reply,
                     [reply, body, oversized] {
                         body->append(reply->readAll());
                         if (body->size() > 2 * 1024 * 1024)
                         {
                             *oversized = true;
                             reply->abort();
                         }
                     });
    QObject::connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, body, oversized, bootstrap,
         callback = std::move(callback)]() mutable {
            HttpResult result;
            result.status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            result.retryAfter =
                std::clamp(reply->rawHeader("Retry-After").toInt(), 0, 3600);
            body->append(reply->readAll());
            result.success = !*oversized && body->size() <= 2 * 1024 * 1024 &&
                             reply->error() == QNetworkReply::NoError &&
                             result.status == 200;
            if (bootstrap && result.success)
            {
                for (const auto &[name, value] : reply->rawHeaderPairs())
                {
                    if (name.compare("Set-Cookie", Qt::CaseInsensitive) != 0)
                    {
                        continue;
                    }
                    for (const auto &cookie :
                         QNetworkCookie::parseCookies(value))
                    {
                        if (cookie.name() == "ttwid" &&
                            !cookie.value().isEmpty() &&
                            cookie.value().size() <= 4096 &&
                            cookie.isSecure() &&
                            !cookie.value().contains('\r') &&
                            !cookie.value().contains('\n'))
                        {
                            this->guestCookies_.setCookiesFromUrl({cookie},
                                                                  reply->url());
                        }
                    }
                }
            }
            result.data = std::move(*body);
            reply->deleteLater();
            callback(std::move(result));
        });
}

void TikTokChatServer::bootstrap()
{
    this->bootstrapping_ = true;
    ++this->activeRequests_;
    const QJsonObject options{
        {u"aid"_s, 1988},      {u"service"_s, u"www.tiktok.com"_s},
        {u"union"_s, false},   {u"unionHost"_s, u""_s},
        {u"needFid"_s, false}, {u"fid"_s, u""_s}};
    auto check = options;
    check.insert(u"migrate_priority"_s, 0);
    this->request(
        QUrl(u"https://www.tiktok.com/ttwid/check/"_s),
        QJsonDocument(check).toJson(QJsonDocument::Compact), true,
        [this, options](HttpResult result) {
            const auto root = QJsonDocument::fromJson(result.data).object();
            const auto status = root.value("status_code");
            if (!result.success || !status.isDouble())
            {
                result.success = false;
                this->finishBootstrap(std::move(result));
                return;
            }
            if (status.toInt(-1) <= 1001)
            {
                result.success = status.toInt(-1) == 0;
                this->finishBootstrap(std::move(result));
                return;
            }

            auto registration = options;
            if (root.contains("migrate_info"))
            {
                registration.insert(u"migrate_info"_s,
                                    root.value("migrate_info"));
            }
            this->request(
                QUrl(u"https://www.tiktok.com/ttwid/register/"_s),
                QJsonDocument(registration).toJson(QJsonDocument::Compact),
                true, [this](HttpResult registered) {
                    const auto data =
                        QJsonDocument::fromJson(registered.data).object();
                    registered.success =
                        registered.success &&
                        data.value("status_code").toInt(-1) == 0;
                    this->finishBootstrap(std::move(registered));
                });
        });
}

void TikTokChatServer::finishBootstrap(HttpResult result)
{
    --this->activeRequests_;
    if (!this->queue_.isEmpty())
    {
        this->dispatch_.start();
    }
    this->bootstrapping_ = false;
    if (result.success && !this->guestCookie().isEmpty())
    {
        this->bootstrapFailures_ = 0;
        this->guestRetryAt_ = {};
        this->guestError_.clear();
        for (const auto &weak : this->channels_)
        {
            if (auto channel = weak.lock(); channel && !channel->stopped_ &&
                                            channel->isLive() &&
                                            !channel->socket_.isValid())
            {
                channel->openSocket();
            }
        }
        return;
    }
    for (const auto &cookie :
         this->guestCookies_.cookiesForUrl(QUrl(u"https://www.tiktok.com/"_s)))
    {
        this->guestCookies_.deleteCookie(cookie);
    }
    if (++this->bootstrapFailures_ < 2 && result.status != 403 &&
        result.status != 429)
    {
        this->bootstrapping_ = true;
        QTimer::singleShot(5000, this, [this] {
            this->bootstrap();
        });
        return;
    }
    this->bootstrapFailures_ = 0;
    this->guestRetryAt_ = QDateTime::currentDateTimeUtc().addSecs(
        std::max(300, result.retryAfter));
    this->guestError_ =
        result.status == 403 || result.status == 429
            ? u"TikTok is temporarily limiting connections. Retrying later."_s
        : result.status == 200
            ? u"TikTok could not start an anonymous session. Retrying later."_s
            : u"Could not start TikTok chat. Check your connection. Retrying later."_s;
    for (const auto &weak : this->channels_)
    {
        if (auto channel = weak.lock();
            channel && !channel->stopped_ && channel->isLive())
        {
            channel->notice(this->guestError_);
        }
    }
}

void TikTokChatServer::pump()
{
    if (!this->initialized_ || this->queue_.isEmpty() ||
        this->activeRequests_ >= 2)
    {
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (this->blockedUntil_ > now)
    {
        while (!this->queue_.isEmpty())
        {
            const auto request = this->queue_.dequeue();
            if (auto channel = request.channel.lock();
                channel && request.generation == channel->resolutionGeneration_)
            {
                channel->resolvePending_ = false;
                channel->resolutionFailed(
                    u"TikTok is temporarily limiting connections. Retrying later."_s);
            }
        }
        return;
    }
    ResolveRequest request;
    std::shared_ptr<TikTokChannel> channel;
    while (!this->queue_.isEmpty())
    {
        request = this->queue_.dequeue();
        channel = request.channel.lock();
        if (channel && request.generation == channel->resolutionGeneration_ &&
            !channel->stopped_)
        {
            break;
        }
        channel.reset();
    }
    if (!channel)
    {
        return;
    }
    QUrl url(u"https://www.tiktok.com/api-live/user/room"_s);
    QUrlQuery query;
    for (const auto &[name, value] :
         QList<QPair<QString, QString>>{{u"aid"_s, u"1988"_s},
                                        {u"app_name"_s, u"tiktok_web"_s},
                                        {u"device_platform"_s, u"web_pc"_s},
                                        {u"app_language"_s, u"en"_s},
                                        {u"browser_language"_s, u"en-US"_s},
                                        {u"region"_s, u"DE"_s},
                                        {u"user_is_login"_s, u"false"_s},
                                        {u"uniqueId"_s, channel->getName()},
                                        {u"sourceType"_s, u"54"_s},
                                        {u"staleTime"_s, u"600000"_s}})
    {
        query.addQueryItem(name, value);
    }
    url.setQuery(query);
    ++this->activeRequests_;
    this->request(url, {}, false, [this, request](HttpResult result) {
        --this->activeRequests_;
        this->dispatch_.start();
        if (result.status == 403 || result.status == 429)
        {
            this->blockedUntil_ = QDateTime::currentDateTimeUtc().addSecs(
                std::max(300, result.retryAfter));
        }
        auto channel = request.channel.lock();
        if (!channel || channel->stopped_ ||
            request.generation != channel->resolutionGeneration_)
        {
            return;
        }
        channel->resolvePending_ = false;
        const auto root = QJsonDocument::fromJson(result.data).object();
        if (!result.success || !root.contains("statusCode") ||
            root.value("statusCode").toInt(-1) != 0)
        {
            channel->resolutionFailed(
                u"Could not check this TikTok channel. Retrying…"_s);
            return;
        }
        const auto data = root.value("data").toObject();
        const auto user = data.value("user").toObject();
        const auto live = data.value("liveRoom").toObject();
        const auto handle =
            normalizeTikTokHandle(user.value("uniqueId").toString());
        if (!handle || *handle != channel->getName())
        {
            channel->resolutionFailed(
                u"TikTok did not return this channel. Check its handle."_s);
            return;
        }
        TikTokRoom room;
        room.id = user.value("roomId").toString();
        bool validID = false;
        const auto numericID = room.id.toULongLong(&validID);
        room.live =
            validID && numericID != 0 && live.value("status").toInt() == 2;
        if (!room.live)
        {
            room.id.clear();
        }
        room.title = live.value("title").toString().left(2048);
        const auto avatar = user.value("avatarMedium").toString();
        room.avatarUrl = isTikTokImageUrl(QUrl(avatar)) ? avatar : QString{};
        channel->applyRoom(std::move(room));
    });
    if (!this->queue_.isEmpty())
    {
        this->dispatch_.start();
    }
}

WebSocketHandle TikTokChatServer::connectRoom(
    const std::shared_ptr<TikTokChannel> &channel, quint64 generation)
{
    if (!this->initialized_)
    {
        return {};
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (this->bootstrapping_)
    {
        return {};
    }
    const auto cookie = this->guestCookie();
    if (cookie.isEmpty())
    {
        if (this->guestRetryAt_ <= now)
        {
            this->bootstrap();
        }
        else if (this->guestRetryAt_ > now)
        {
            channel->notice(this->guestError_);
        }
        return {};
    }
    if (!this->sockets_)
    {
        this->sockets_ = std::make_unique<WebSocketPool>(u"tiktok"_s);
    }
    WebSocketOptions options;
    options.url = tiktok::socketUrl(channel->getCurrentStreamID());
    options.maxMessageBytes = tiktok::MAX_FRAME_BYTES;
    options.headers = {{"User-Agent", tiktok::userAgent().toStdString()},
                       {"Cookie", cookie.toStdString()},
                       {"Origin", "https://www.tiktok.com"},
                       {"Referer", "https://www.tiktok.com/"}};
    return this->sockets_->createSocket(
        std::move(options), std::make_unique<Listener>(channel, generation));
}

bool TikTokChatServer::canSend(const TikTokChannel &channel) const
{
    return channel.isLive() && getApp()->getAccounts()->tiktok.isLoggedIn();
}

void TikTokChatServer::send(const std::shared_ptr<TikTokChannel> &channel,
                            const QString &text)
{
    if (!channel)
    {
        return;
    }
    auto account = getApp()->getAccounts()->tiktok.current();
    if (!account || !account->hasCredentials())
    {
        channel->addSystemMessage(
            u"Connect a TikTok account in Settings > Accounts to send messages."_s);
        return;
    }
    if (!this->api_)
    {
        this->api_ = std::make_unique<TikTokApi>();
        this->accountSignals_.managedConnect(
            this->api_->sessionChanged, [](const auto &updated) {
                getApp()->getAccounts()->tiktok.sessionUpdated(updated);
            });
    }
    const auto roomID = channel->getCurrentStreamID();
    const auto generation = channel->generation_;
    const auto accountGeneration = this->accountGeneration_;
    this->api_->sendMessage(
        account, roomID, channel->getName(), text,
        [weak = channel->weakFromThis(), account, roomID, generation,
         text](TikTokSendResult result) {
            auto channel = weak.lock();
            if (!channel || channel->stopped_ ||
                channel->generation_ != generation ||
                channel->getCurrentStreamID() != roomID)
            {
                return;
            }
            if (result.status == TikTokSendResult::Status::LoginRequired)
            {
                getApp()->getAccounts()->tiktok.requireLogin(account);
            }
            if (!result.message.isEmpty())
            {
                channel->addSystemMessage(result.message + u" Message: "_s +
                                          text);
            }
            if (result.status != TikTokSendResult::Status::Accepted ||
                result.messageID.isEmpty())
            {
                return;
            }
            TikTokEvent event;
            event.id = result.messageID;
            event.roomID = roomID;
            event.time = QDateTime::currentDateTimeUtc();
            event.author.id = account->userID();
            event.author.handle = account->handle();
            event.author.displayName = account->displayName();
            event.author.avatarUrl = account->avatarUrl();
            event.text = text;

            channel->receive(generation, {{}, {std::move(event)}, false});
        },
        [this, weak = channel->weakFromThis(), account, roomID, generation,
         accountGeneration] {
            const auto channel = weak.lock();
            return channel && !channel->stopped_ &&
                   channel->generation_ == generation &&
                   this->accountGeneration_ == accountGeneration &&
                   channel->getCurrentStreamID() == roomID &&
                   getApp()->getAccounts()->tiktok.current() == account;
        });
}

}
