#include "providers/kick/ws/KickPusherManager.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/kick/ws/KickWebSocketCommon.hpp"
#include "providers/liveupdates/BasicPubSubClient.hpp"
#include "providers/liveupdates/BasicPubSubManager.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json.hpp>
#include <QPointer>

#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

constexpr std::chrono::seconds MAX_HEARTBEAT_INTERVAL{20};

class KickPusherClient : public BasicPubSubClient<QString, KickPusherClient>,
                         public std::enable_shared_from_this<KickPusherClient>
{
public:
    KickPusherClient(QPointer<KickChatServer> chatServer)
        : BasicPubSubClient(100)
        , lastHeartbeat_(std::chrono::steady_clock::now())
        , heartbeatInterval_(MAX_HEARTBEAT_INTERVAL)
        , chatServer_(std::move(chatServer))
    {
        this->heartbeatTimer_.setInterval(this->heartbeatInterval_);
        QObject::connect(&this->heartbeatTimer_, &QTimer::timeout,
                         &this->heartbeatTimer_, [this] {
                             this->checkHeartbeat();
                         });
    }

    void onOpen()
    {
        BasicPubSubClient::onOpen();
        this->lastHeartbeat_ = std::chrono::steady_clock::now();
        this->heartbeatTimer_.start();
    }

    void onMessage(const QByteArray &msg);
    void onClose(const std::unordered_set<QString> &subscriptions)
    {
        this->closed_ = true;
        this->heartbeatTimer_.stop();
        kick::ws::disconnected(this->chatServer_, subscriptions);
    }

    void checkHeartbeat();

    QByteArray encodeSubscription(const Subscription &subscription);
    QByteArray encodeUnsubscription(const Subscription &subscription);

private:
    void onMessageUi(const QByteArray &msg);

    std::chrono::steady_clock::time_point lastHeartbeat_;
    std::chrono::milliseconds heartbeatInterval_;
    QPointer<KickChatServer> chatServer_;
    QTimer heartbeatTimer_;
    bool closed_ = false;
};

void KickPusherClient::onMessage(const QByteArray &msg)
{
    runInGuiThread([weak = this->weak_from_this(), msg] {
        auto self = weak.lock();
        if (self && !self->closed_)
        {
            self->onMessageUi(msg);
        }
    });
}

void KickPusherClient::onMessageUi(const QByteArray &msg)
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
    auto rootObj = rootRef.toObject();
    auto event = rootObj["event"].toStringView();

    auto dataStr = rootObj["data"].toStringView();
    BoostJsonValue data = rootObj["data"];
    boost::json::value dataJv;
    if (!dataStr.empty() && dataStr != "{}")
    {
        dataJv = boost::json::parse(dataStr, ec);
        if (ec)
        {
            return;
        }
        data = BoostJsonValue(dataJv);
    }

    if (event == "pusher:ping")
    {
        this->lastHeartbeat_ = std::chrono::steady_clock::now();
        this->sendText(R"({"event":"pusher:pong","data":{}})"_ba);
    }
    else if (event == "pusher:pong")
    {
        this->lastHeartbeat_ = std::chrono::steady_clock::now();
    }
    else if (event == "pusher_internal:subscription_succeeded")
    {
        auto channel = rootObj["channel"].toStdString();

        if (channel.starts_with("chatrooms.") && channel.ends_with(".v2"))
        {
            auto ids = kick::ws::parseIDs(channel);
            if (this->chatServer_ && ids.roomID > 0 &&
                this->isSubscribed(QString::fromStdString(channel)))
            {
                this->chatServer_->onJoin(ids.roomID);
            }
        }
    }
    else if (event == "pusher:subscription_error")
    {
        qCWarning(chatterinoKick) << "Failed to subscribe" << msg;
    }
    else if (event == "pusher:connection_established")
    {
        const auto timeout = data["activity_timeout"].toInt64();
        if (timeout > 2 && timeout < MAX_HEARTBEAT_INTERVAL.count())
        {
            this->heartbeatInterval_ = std::chrono::seconds(timeout - 1);
            this->heartbeatTimer_.setInterval(this->heartbeatInterval_);
        }
    }
    else
    {
        const auto channel = rootObj["channel"].toQString();
        if (this->isSubscribed(channel))
        {
            kick::ws::dispatch(this->chatServer_,
                               rootObj["channel"].toStringView(), event, data);
        }
    }
}

void KickPusherClient::checkHeartbeat()
{
    if (!this->isOpen() || this->closed_)
    {
        return;
    }

    if ((std::chrono::steady_clock::now() - this->lastHeartbeat_) >
        this->heartbeatInterval_ * 1.5)
    {
        qCDebug(chatterinoKick) << "Heartbeat timed out";
        this->close();
        return;
    }

    this->sendText(R"({"event":"pusher:ping","data":0})"_ba);
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
QByteArray KickPusherClient::encodeSubscription(const Subscription &sub)
{
    return QByteArray::fromStdString(boost::json::serialize(boost::json::object{
        {"event", "pusher:subscribe"},
        {"data",
         boost::json::object{
             {"auth", ""},
             {"channel", sub.toStdString()},
         }},
    }));
}

QByteArray KickPusherClient::encodeUnsubscription(const Subscription &sub)
{
    return QByteArray::fromStdString(boost::json::serialize(boost::json::object{
        {"event", "pusher:unsubscribe"},
        {"data",
         boost::json::object{
             {"channel", sub.toStdString()},
         }},
    }));
}
// NOLINTEND(readability-convert-member-functions-to-static)

}

namespace chatterino {

class KickPusherManagerPrivate
    : public BasicPubSubManager<KickPusherManagerPrivate, KickPusherClient>
{
public:
    explicit KickPusherManagerPrivate(QString url);
    ~KickPusherManagerPrivate() override;

    Q_DISABLE_COPY_MOVE(KickPusherManagerPrivate);

    std::shared_ptr<KickPusherClient> makeClient();

private:
    friend KickPusherManager;
};

KickPusherManagerPrivate::KickPusherManagerPrivate(QString url)
    : BasicPubSubManager(std::move(url), "kick")
{
}

KickPusherManagerPrivate::~KickPusherManagerPrivate()
{
    this->stop();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::shared_ptr<KickPusherClient> KickPusherManagerPrivate::makeClient()
{
    return std::make_shared<KickPusherClient>(getApp()->getKickChatServer());
}

KickPusherManager::KickPusherManager(QString url)
    : private_(new KickPusherManagerPrivate(std::move(url)))
{
}
KickPusherManager::~KickPusherManager() = default;

void KickPusherManager::joinChannel(const QString &name)
{
    this->private_->subscribe(name);
}

void KickPusherManager::partChannel(const QString &name)
{
    this->private_->unsubscribe(name);
}

}
