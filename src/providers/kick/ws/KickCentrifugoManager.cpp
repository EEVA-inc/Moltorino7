#include "providers/kick/ws/KickCentrifugoManager.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/kick/ws/KickWebSocketCommon.hpp"
#include "providers/liveupdates/BasicPubSubClient.hpp"
#include "providers/liveupdates/BasicPubSubManager.hpp"
#include "util/BoostJsonWrap.hpp"
#include "util/Variant.hpp"

#include <boost/json.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <QPointer>

#include <functional>
#include <limits>
#include <unordered_map>
#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

constexpr std::chrono::seconds MAX_HEARTBEAT_INTERVAL{20};

class KickCentrifugoClient
    : public BasicPubSubClient<QString, KickCentrifugoClient>,
      public std::enable_shared_from_this<KickCentrifugoClient>
{
public:
    KickCentrifugoClient(QString clientID, QPointer<KickChatServer> chatServer,
                         std::function<void()> onReady);

    void onOpen();

    void onMessage(const QByteArray &msg);

    bool isOpen() const
    {
        return BasicPubSubClient::isOpen() && !this->retryBackoff_;
    }

    void checkHeartbeat();

    void subscribeImpl(const Subscription &topic);
    void unsubscribeImpl(const Subscription &topic);

    void onClose(const std::unordered_set<QString> &subscriptions)
    {
        this->closed_ = true;
        this->refreshTimer.stop();
        kick::ws::disconnected(this->chatServer_, subscriptions);
    }

private:
    void fail()
    {
        this->retryBackoff_ = true;
        this->refreshTimer.stop();
        this->closed_ = true;
        this->close();
    }

    void onMessageUi(QByteArrayView msg);
    void onResponse(BoostJsonObject root);

    void doRefresh();
    void sendRefreshOrConnect();
    void notifyReady();

    std::string encodeSubImpl(std::string channel);
    std::string encodeUnsubImpl(std::string_view channel);

    std::chrono::steady_clock::time_point lastHeartbeat_;
    std::chrono::milliseconds heartbeatInterval_;

    QTimer refreshTimer;
    bool isRefreshing = false;
    bool isConnected = false;
    bool closed_ = false;
    bool retryBackoff_ = false;
    bool joinedChat_ = false;
    bool pong_ = true;
    std::chrono::steady_clock::time_point responseDeadline_;
    uint32_t nextId = 1;

    QString clientID;
    std::string token;

    QPointer<KickChatServer> chatServer_;
    std::function<void()> onReady_;

    struct SubCompletion {
        std::string channel;
    };
    struct Refresh {
    };
    struct Connect {
    };
    using CompletionData = std::variant<SubCompletion, Refresh, Connect>;

    boost::unordered_flat_set<QString> subscriptionBacklog;
    std::unordered_map<uint32_t, CompletionData> pendingCompletions;
};

KickCentrifugoClient::KickCentrifugoClient(QString clientID,
                                           QPointer<KickChatServer> chatServer,
                                           std::function<void()> onReady)
    : BasicPubSubClient(100)
    , lastHeartbeat_(std::chrono::steady_clock::now())
    , heartbeatInterval_(MAX_HEARTBEAT_INTERVAL)
    , clientID(std::move(clientID))
    , chatServer_(std::move(chatServer))
    , onReady_(std::move(onReady))
{
    this->refreshTimer.setSingleShot(true);
    // NOLINTNEXTLINE(clazy-connect-3arg-lambda)
    QObject::connect(&this->refreshTimer, &QTimer::timeout, [this] {
        this->doRefresh();
    });
}

void KickCentrifugoClient::onOpen()
{
    this->lastHeartbeat_ = std::chrono::steady_clock::now();
    this->responseDeadline_ = this->lastHeartbeat_ + std::chrono::seconds(30);
    this->doRefresh();
}

void KickCentrifugoClient::doRefresh()
{
    if (this->isRefreshing || this->closed_)
    {
        return;
    }
    qCDebug(chatterinoKick) << "[Centrifugo] Refreshing...";
    this->isRefreshing = true;
    NetworkRequest(u"https://web.kick.com/api/v1/realtime/auth/connection"_s,
                   NetworkRequestType::Post)
        .json(QJsonObject{{"client_id"_L1, this->clientID}})
        .timeout(10'000)
        .maximumResponseSize(64 * 1024)
        .hideRequestBody()
        .onSuccess([weak = this->weak_from_this()](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self || self->closed_)
            {
                return;
            }
            self->token = res.parseJson()
                              .value("data"_L1)["token"_L1]
                              .toString()
                              .toStdString();
            if (self->token.empty())
            {
                self->fail();
                return;
            }
            self->sendRefreshOrConnect();
        })
        .onError([weak = this->weak_from_this()](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self || self->closed_)
            {
                return;
            }
            self->isRefreshing = false;
            qCWarning(chatterinoKick)
                << "Failed to get centrifugo token:" << res.formatError();
            self->fail();
        })
        .execute();
}

void KickCentrifugoClient::sendRefreshOrConnect()
{
    assert(this->isRefreshing);
    assertInGuiThread();
    this->isRefreshing = false;
    this->responseDeadline_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);

    QByteArray data;
    if (this->isConnected)
    {
        auto id = this->nextId++;
        qCDebug(chatterinoKick) << "[Centrifugo] Refresh id:" << id;
        this->pendingCompletions.emplace(id, Refresh{});
        data.append(boost::json::serialize(boost::json::object{
            {
                "refresh",
                boost::json::object{{"token", this->token}},
            },
            {"id", id},
        }));
    }
    else
    {
        auto id = this->nextId++;
        qCDebug(chatterinoKick) << "[Centrifugo] Connect id:" << id;
        this->pendingCompletions.emplace(id, Connect{});
        data.append(boost::json::serialize(boost::json::object{
            {
                "connect",
                boost::json::object{
                    {"token", this->token},
                    {"name", "js"},
                },
            },
            {"id", id},
        }));
    }

    this->sendText(data);
}

void KickCentrifugoClient::subscribeImpl(const Subscription &topic)
{
    assertInGuiThread();
    if (this->isConnected && !this->closed_)
    {
        this->sendText(QByteArray::fromStdString(
            this->encodeSubImpl(topic.toStdString())));
    }
    else
    {
        this->subscriptionBacklog.emplace(topic);
    }
}

void KickCentrifugoClient::unsubscribeImpl(const Subscription &topic)
{
    assertInGuiThread();
    const auto name = topic.toStdString();
    std::erase_if(this->pendingCompletions, [&](const auto &entry) {
        const auto *sub = std::get_if<SubCompletion>(&entry.second);
        return sub && sub->channel == name;
    });
    this->notifyReady();
    if (this->isConnected && !this->closed_)
    {
        this->sendText(QByteArray::fromStdString(
            this->encodeUnsubImpl(topic.toStdString())));
    }
    else
    {
        this->subscriptionBacklog.erase(topic);
    }
}

void KickCentrifugoClient::notifyReady()
{
    if (this->closed_ || !this->joinedChat_ || !this->onReady_)
    {
        return;
    }
    for (const auto &[id, completion] : this->pendingCompletions)
    {
        const auto *sub = std::get_if<SubCompletion>(&completion);
        if (sub && sub->channel.starts_with("chatrooms.") &&
            sub->channel.ends_with(".v2"))
        {
            return;
        }
    }

    auto ready = std::exchange(this->onReady_, {});
    ready();
}

std::string KickCentrifugoClient::encodeSubImpl(std::string channel)
{
    auto id = this->nextId++;
    qCDebug(chatterinoKick) << "[Centrifugo] Sub to" << channel << "id:" << id;
    auto res = boost::json::serialize(boost::json::object{
        {
            "subscribe",
            boost::json::object{
                {"channel", channel},

                {"flag", 0},
            },
        },
        {"id", id},
    });
    if (this->pendingCompletions.empty())
    {
        this->responseDeadline_ =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    this->pendingCompletions.emplace(id, SubCompletion{std::move(channel)});

    return res;
}

std::string KickCentrifugoClient::encodeUnsubImpl(std::string_view channel)
{
    auto id = this->nextId++;
    qCDebug(chatterinoKick)
        << "[Centrifugo] Unsub from " << channel << "id:" << id;

    return boost::json::serialize(boost::json::object{
        {
            "unsubscribe",
            boost::json::object{{"channel", channel}},
        },
        {"id", id},
    });
}

void KickCentrifugoClient::onMessage(const QByteArray &msg)
{
    runInGuiThread([weak = this->weak_from_this(), msg] {
        auto self = weak.lock();
        if (self && !self->closed_)
        {
            QByteArrayView ba = msg;
            while (!ba.empty() && !self->closed_)
            {
                const auto end = ba.indexOf('\n');
                self->onMessageUi(end < 0 ? ba : ba.first(end));
                ba = end < 0 ? QByteArrayView{} : ba.sliced(end + 1);
            }
        }
    });
}

void KickCentrifugoClient::onMessageUi(QByteArrayView msg)
{
    boost::system::error_code ec;
    auto rootJv =
        boost::json::parse(std::string_view(msg.data(), msg.size()), ec);
    if (ec)
    {
        qCWarning(chatterinoKick) << "Failed to parse message:" << ec.message();
        return;
    }
    BoostJsonValue rootRef(rootJv);
    if (!rootRef.isObject())
    {
        return;
    }
    this->lastHeartbeat_ = std::chrono::steady_clock::now();
    auto rootObj = rootRef.toObject();
    if (rootObj.empty())
    {
        if (this->pong_)
        {
            this->sendText("{}"_ba);
        }
        return;
    }

    auto pushEvent = rootObj["push"].toObject();
    if (pushEvent.empty())
    {
        this->onResponse(rootObj);
        return;
    }

    const auto channel = pushEvent["channel"].toQString();
    if (pushEvent.contains("disconnect") ||
        (pushEvent.contains("unsubscribe") && this->isSubscribed(channel)))
    {
        this->fail();
        return;
    }
    if (!this->isConnected || !this->isSubscribed(channel))
    {
        return;
    }
    const auto publication = pushEvent["pub"]["data"].toObject();
    kick::ws::dispatch(this->chatServer_, pushEvent["channel"].toStringView(),
                       publication["event"].toStringView(),
                       publication["data"]);
}

void KickCentrifugoClient::onResponse(BoostJsonObject root)
{
    const auto rawID = root["id"].toUint64();
    if (!rawID || rawID > std::numeric_limits<uint32_t>::max())
    {
        return;
    }
    auto id = static_cast<uint32_t>(rawID);
    auto it = this->pendingCompletions.find(id);
    if (it == this->pendingCompletions.end())
    {
        return;
    }
    auto completion = std::move(it->second);
    this->pendingCompletions.erase(it);

    const auto error = root["error"];
    if (error.isObject())
    {
        qCWarning(chatterinoKick)
            << "[Centrifugo] id" << id
            << "errored: " << error["message"].toStringView();

        const auto *sub = std::get_if<SubCompletion>(&completion);
        if (!sub || (sub->channel.starts_with("chatrooms.") &&
                     sub->channel.ends_with(".v2")))
        {
            this->fail();
        }
        return;
    }

    std::visit(
        variant::Overloaded{
            [&](const SubCompletion &sub) {
                if (sub.channel.starts_with("chatrooms.") &&
                    sub.channel.ends_with(".v2"))
                {
                    auto ids = kick::ws::parseIDs(sub.channel);
                    if (ids.roomID > 0 &&
                        this->isSubscribed(QString::fromStdString(sub.channel)))
                    {
                        this->joinedChat_ = true;
                        this->notifyReady();
                        if (this->chatServer_)
                        {
                            this->chatServer_->onJoin(ids.roomID);
                        }
                    }
                }
            },
            [&](Connect) {
                const auto o = root["connect"].toObject();
                if (!root["connect"].isObject())
                {
                    this->fail();
                    return;
                }
                BasicPubSubClient::onOpen();
                this->isConnected = true;
                const auto ping =
                    std::clamp<int64_t>(o["ping"].toInt64(), 0, 300);
                this->heartbeatInterval_ = std::chrono::seconds(ping);
                this->pong_ = o["pong"].toBool();
                if (o["expires"].toBool())
                {
                    const auto ttl = std::clamp<int64_t>(o["ttl"].toInt64(1800),
                                                         11, 24 * 60 * 60) -
                                     10;
                    this->refreshTimer.start(std::chrono::seconds(ttl));
                }
                const auto subs = std::exchange(this->subscriptionBacklog, {});
                for (const auto &sub : subs)
                {
                    this->subscribeImpl(sub);
                }
            },
            [&](Refresh) {
                const auto o = root["refresh"].toObject();
                if (!root["refresh"].isObject())
                {
                    this->fail();
                    return;
                }
                if (o["expires"].toBool())
                {
                    const auto ttl = std::clamp<int64_t>(o["ttl"].toInt64(1800),
                                                         11, 24 * 60 * 60) -
                                     10;
                    this->refreshTimer.start(std::chrono::seconds(ttl));
                }
            },
        },
        completion);
}

void KickCentrifugoClient::checkHeartbeat()
{
    if (this->closed_ ||
        this->responseDeadline_ == std::chrono::steady_clock::time_point{})
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if ((!this->pendingCompletions.empty() && now > this->responseDeadline_) ||
        (!this->isConnected && now > this->responseDeadline_) ||
        (this->isConnected && this->heartbeatInterval_.count() > 0 &&
         now - this->lastHeartbeat_ >
             this->heartbeatInterval_ + std::chrono::seconds(10)))
    {
        qCDebug(chatterinoKick) << "Centrifugo response timed out";
        this->fail();
    }
}

}

namespace chatterino {

class KickCentrifugoManagerPrivate
    : public BasicPubSubManager<KickCentrifugoManagerPrivate,
                                KickCentrifugoClient>
{
public:
    KickCentrifugoManagerPrivate(QString url, QString clientID);
    ~KickCentrifugoManagerPrivate() override;

    Q_DISABLE_COPY_MOVE(KickCentrifugoManagerPrivate);

    std::shared_ptr<KickCentrifugoClient> makeClient();
    void checkHeartbeats();

    QTimer heartbeatTimer;
    QString clientID;

private:
    friend KickCentrifugoManager;
};

KickCentrifugoManagerPrivate::KickCentrifugoManagerPrivate(QString url,
                                                           QString clientID)
    : BasicPubSubManager(std::move(url), "kick")
    , clientID(std::move(clientID))
{
    QObject::connect(&this->heartbeatTimer, &QTimer::timeout, this,
                     &KickCentrifugoManagerPrivate::checkHeartbeats);
    this->heartbeatTimer.setInterval(std::chrono::seconds(5));
    this->heartbeatTimer.setSingleShot(false);
    this->heartbeatTimer.start();
}

KickCentrifugoManagerPrivate::~KickCentrifugoManagerPrivate()
{
    this->stop();
}

std::shared_ptr<KickCentrifugoClient> KickCentrifugoManagerPrivate::makeClient()
{
    return std::make_shared<KickCentrifugoClient>(
        this->clientID, getApp()->getKickChatServer(), [self = QPointer(this)] {
            if (self)
            {
                self->resetConnectBackoff();
            }
        });
}

void KickCentrifugoManagerPrivate::checkHeartbeats()
{
    for (const auto &[id, client] : this->clients())
    {
        client->checkHeartbeat();
    }
}

KickCentrifugoManager::KickCentrifugoManager(QString url, QString clientID)
    : private_(
          new KickCentrifugoManagerPrivate(std::move(url), std::move(clientID)))
{
}
KickCentrifugoManager::~KickCentrifugoManager() = default;

void KickCentrifugoManager::joinChannel(const QString &name)
{
    this->private_->subscribe(name);
}

void KickCentrifugoManager::partChannel(const QString &name)
{
    this->private_->unsubscribe(name);
}

}
