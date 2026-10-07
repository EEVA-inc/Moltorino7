#include "providers/moltorino/MoltorinoPresence.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QDateTime>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace chatterino {

Q_LOGGING_CATEGORY(chatterinoMoltorinoPresence,
                   "chatterino.moltorino.presence", QtWarningMsg)

namespace {

constexpr auto HEARTBEAT_INTERVAL_MS = 30 * 60 * 1000;
constexpr auto BADGE_SOCKET_RECONNECT_BASE_MS = 15000;
constexpr auto BADGE_SOCKET_RECONNECT_MAX_MS = 5 * 60 * 1000;

QUrl configuredUrl(const char *name, bool socket)
{
    const QUrl url(qEnvironmentVariable(name).trimmed());
    const auto scheme = url.scheme();
    const bool secure = scheme == (socket ? "wss" : "https");
    const bool local = (url.host() == "localhost" ||
                        url.host() == "127.0.0.1" || url.host() == "::1") &&
                       scheme == (socket ? "ws" : "http");
    if (!url.isValid() || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasFragment() || (!secure && !local))
    {
        return {};
    }
    return url;
}

QString platformKey()
{
#if defined(Q_OS_WIN)
    return "windows";
#elif defined(Q_OS_MACOS)
    return "macos";
#elif defined(Q_OS_LINUX)
    return "linux";
#else
    return QSysInfo::productType();
#endif
}

QString savedClientId()
{
    QSettings settings(QStringLiteral("Moltorino"), QStringLiteral("Moltorino7"));
    auto id = settings.value(QStringLiteral("presence/clientInstanceId"))
                  .toString()
                  .trimmed();

    if (id.isEmpty())
    {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("presence/clientInstanceId"), id);
    }

    return id;
}

bool activityHeartbeatsEnabled()
{
    const auto *settings = getSettings();
    return settings->transmitPresence && settings->sendActivityHeartbeats;
}

bool heartbeatAccountHidden()
{
    const auto *settings = getSettings();
    return !settings->sendActivityHeartbeats ||
           settings->hideAccountInHeartbeats;
}

QString heartbeatMode()
{
    const auto *settings = getSettings();
    if (!settings->sendActivityHeartbeats)
    {
        return QStringLiteral("disabled");
    }
    if (settings->hideAccountInHeartbeats)
    {
        return QStringLiteral("anonymous");
    }
    return QStringLiteral("normal");
}

}

class MoltorinoBadgeSocketListener : public WebSocketListener
{
public:
    MoltorinoBadgeSocketListener(MoltorinoPresence *presence, int generation)
        : presence_(presence)
        , generation_(generation)
    {
    }

    void onOpen() override
    {
        runInGuiThread([presence = this->presence_,
                        generation = this->generation_] {
            if (presence != nullptr)
            {
                presence->handleBadgeSocketOpen(generation);
            }
        });
    }

    void onTextMessage(QByteArray data) override
    {
        if (data.size() > 64 * 1024)
        {
            return;
        }
        runInGuiThread([presence = this->presence_,
                        generation = this->generation_,
                        data = std::move(data)]() mutable {
            if (presence != nullptr)
            {
                presence->handleBadgeSocketMessage(generation,
                                                   std::move(data));
            }
        });
    }

    void onBinaryMessage(QByteArray data) override
    {
        this->onTextMessage(std::move(data));
    }

    void onClose(std::unique_ptr<WebSocketListener>) override
    {
        runInGuiThread([presence = this->presence_,
                        generation = this->generation_] {
            if (presence != nullptr)
            {
                presence->handleBadgeSocketClosed(generation);
            }
        });
    }

private:
    QPointer<MoltorinoPresence> presence_;
    int generation_{};
};

MoltorinoPresence::MoltorinoPresence()
    : heartbeatUrl_(configuredUrl("MOLTORINO_HEARTBEAT_URL", false))
    , badgeSocketUrl_(configuredUrl("MOLTORINO_BADGE_SOCKET_URL", true))
    , clientInstanceId_(savedClientId())
{
    this->heartbeatTimer_.setInterval(HEARTBEAT_INTERVAL_MS);
    this->heartbeatTimer_.setTimerType(Qt::VeryCoarseTimer);

    QObject::connect(&this->heartbeatTimer_, &QTimer::timeout, this, [this] {
        this->sendHeartbeat();
    });

    this->badgeSocketReconnectTimer_.setSingleShot(true);
    this->badgeSocketReconnectTimer_.setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(&this->badgeSocketReconnectTimer_, &QTimer::timeout, this,
                     [this] {
                         this->connectBadgeSocket();
                     });
    this->badgeRefreshTimer_.setSingleShot(true);
    QObject::connect(&this->badgeRefreshTimer_, &QTimer::timeout, this, [this] {
        const auto generation =
            std::exchange(this->pendingBadgeGeneration_, QString{});
        const auto version = std::exchange(this->pendingBadgeVersion_, -1);
        if (auto *badges = getApp()->getMoltorinoSupporterBadges())
        {
            badges->refreshV2IfNewer(generation, version);
        }
    });
}

MoltorinoPresence::~MoltorinoPresence()
{
    this->stopHeartbeat();
}

MoltorinoPresence &MoltorinoPresence::instance()
{
    static MoltorinoPresence presence;
    return presence;
}

void MoltorinoPresence::init()
{
    if (this->initialized_)
    {
        return;
    }

    this->initialized_ = true;
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, [this] {
            this->sendHeartbeat(true);
        });
    getSettings()->transmitPresence.connect(
        [this](bool) {
            this->applyHeartbeatSettings(true);
        },
        this->signalHolder_, false);
    getSettings()->sendActivityHeartbeats.connect(
        [this](bool) {
            this->applyHeartbeatSettings(true);
        },
        this->signalHolder_, false);
    getSettings()->hideAccountInHeartbeats.connect(
        [this](bool) {
            this->applyHeartbeatSettings(true);
        },
        this->signalHolder_, false);
}

void MoltorinoPresence::startHeartbeat()
{
    if (isAppAboutToQuit())
    {
        return;
    }
    this->running_ = true;
    this->connectBadgeSocket();
    this->applyHeartbeatSettings(true);
}

void MoltorinoPresence::applyHeartbeatSettings(bool sendNow)
{
    if (!this->running_ || this->heartbeatUrl_.isEmpty() ||
        !activityHeartbeatsEnabled())
    {
        this->heartbeatTimer_.stop();
        this->heartbeatQueued_ = false;
        return;
    }

    if (!this->heartbeatTimer_.isActive())
    {
        this->heartbeatTimer_.start();
    }

    if (sendNow)
    {
        this->sendHeartbeat(true);
    }
}

void MoltorinoPresence::stopHeartbeat()
{
    this->running_ = false;
    this->heartbeatTimer_.stop();
    this->heartbeatQueued_ = false;
    this->badgeRefreshTimer_.stop();
    this->pendingBadgeGeneration_.clear();
    this->pendingBadgeVersion_ = -1;
    this->disconnectBadgeSocket();
}

void MoltorinoPresence::sendHeartbeat(bool force)
{
    if (!this->running_ || this->heartbeatUrl_.isEmpty() ||
        !activityHeartbeatsEnabled())
    {
        this->heartbeatTimer_.stop();
        this->heartbeatQueued_ = false;
        return;
    }

    if (this->heartbeatInFlight_)
    {
        this->heartbeatQueued_ = this->heartbeatQueued_ || force;
        return;
    }

    this->heartbeatInFlight_ = true;
    this->heartbeatQueued_ = false;
    NetworkRequest(this->heartbeatUrl_, NetworkRequestType::Post)
        .caller(this)
        .hideRequestBody()
        .maximumResponseSize(16 * 1024)
        .timeout(15000)
        .json(this->makePayload())
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoMoltorinoPresence)
                << "Heartbeat failed:" << result.formatError();
        })
        .finally([this] {
            this->heartbeatInFlight_ = false;
            if (this->heartbeatQueued_)
            {
                this->sendHeartbeat(true);
            }
        })
        .execute();
}

void MoltorinoPresence::connectBadgeSocket()
{
    if (!this->running_ || this->badgeSocketUrl_.isEmpty() ||
        this->badgeSocketConnecting_ || this->badgeSocketOpen_)
    {
        return;
    }

    if (!this->badgeSocketPool_)
    {
        this->badgeSocketPool_ =
            std::make_unique<WebSocketPool>(QStringLiteral("Moltorino badges"));
    }

    this->badgeSocketReconnectTimer_.stop();
    this->badgeSocketConnecting_ = true;
    this->badgeSocketOpen_ = false;

    const auto generation = ++this->badgeSocketGeneration_;
    this->badgeSocket_ = this->badgeSocketPool_->createSocket(
        WebSocketOptions{
            .url = this->badgeSocketUrl_,
            .headers = {},
        },
        std::make_unique<MoltorinoBadgeSocketListener>(this, generation));
}

void MoltorinoPresence::disconnectBadgeSocket()
{
    this->badgeSocketReconnectTimer_.stop();
    ++this->badgeSocketGeneration_;

    this->badgeSocket_.close();
    this->badgeSocket_ = WebSocketHandle();
    this->badgeSocketConnecting_ = false;
    this->badgeSocketOpen_ = false;
}

void MoltorinoPresence::handleBadgeSocketOpen(int generation)
{
    if (generation != this->badgeSocketGeneration_)
    {
        return;
    }

    this->badgeSocketConnecting_ = false;
    this->badgeSocketOpen_ = true;
    this->badgeSocketBackoffStep_ = 0;
    if (auto *badges = getApp()->getMoltorinoSupporterBadges())
    {
        badges->refreshPassive();
    }
}

void MoltorinoPresence::handleBadgeSocketClosed(int generation)
{
    if (generation != this->badgeSocketGeneration_)
    {
        return;
    }

    this->badgeSocketConnecting_ = false;
    this->badgeSocketOpen_ = false;

    if (!this->running_)
    {
        return;
    }

    const auto shift = std::min(this->badgeSocketBackoffStep_, 5);
    const auto baseDelay =
        std::min(BADGE_SOCKET_RECONNECT_MAX_MS,
                 BADGE_SOCKET_RECONNECT_BASE_MS * (1 << shift));
    const auto jitter = int(QRandomGenerator::global()->bounded(5000));

    this->badgeSocketBackoffStep_ =
        std::min(this->badgeSocketBackoffStep_ + 1, 8);
    this->badgeSocketReconnectTimer_.start(
        std::min(BADGE_SOCKET_RECONNECT_MAX_MS, baseDelay + jitter));
}

void MoltorinoPresence::handleBadgeSocketMessage(int generation,
                                                 QByteArray data)
{
    if (generation != this->badgeSocketGeneration_ || !this->running_)
    {
        return;
    }

    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
    {
        return;
    }

    const auto root = doc.object();
    const auto type =
        root.value(QStringLiteral("type")).toString().trimmed().toLower();
    if (type == QStringLiteral("badge_v2_changed"))
    {
        const auto badgeGeneration =
            root.value(QStringLiteral("generation")).toString().trimmed();
        if (badgeGeneration.isEmpty() || badgeGeneration.size() > 256)
        {
            return;
        }
        const auto version = root.value(QStringLiteral("version")).toInt(-1);
        if (badgeGeneration == this->pendingBadgeGeneration_)
        {
            this->pendingBadgeVersion_ =
                std::max(this->pendingBadgeVersion_, version);
        }
        else
        {
            this->pendingBadgeGeneration_ = badgeGeneration;
            this->pendingBadgeVersion_ = version;
        }
        if (!this->badgeRefreshTimer_.isActive())
        {
            this->badgeRefreshTimer_.start(
                int(QRandomGenerator::global()->bounded(750, 7001)));
        }
        return;
    }
    if (type != QStringLiteral("badge_update") &&
        type != QStringLiteral("badges_updated") &&
        type != QStringLiteral("moltorino_badges_updated") &&
        type != QStringLiteral("supporter_badges_changed"))
    {
        return;
    }

    auto version = root.value(QStringLiteral("version")).toInt(-1);
    if (version < 0)
    {
        version = root.value(QStringLiteral("data"))
                      .toObject()
                      .value(QStringLiteral("version"))
                      .toInt(-1);
    }

    if (auto *badges = getApp()->getMoltorinoSupporterBadges())
    {
        if (version >= 0)
        {
            badges->refreshIfNewer(version);
        }
        else
        {
            badges->refreshNow();
        }
    }
}

QJsonObject MoltorinoPresence::makePayload() const
{
    const auto now = QDateTime::currentDateTimeUtc();

    QJsonObject payload;
    payload.insert(QStringLiteral("clientInstanceId"), this->clientInstanceId_);
    payload.insert(QStringLiteral("platform"), platformKey());
    payload.insert(QStringLiteral("appVersion"), Version::instance().version());
    payload.insert(QStringLiteral("internalBuild"),
                   Version::instance().internalVersion());
    payload.insert(QStringLiteral("sentAt"), now.toString(Qt::ISODate));
    payload.insert(QStringLiteral("heartbeatMode"), heartbeatMode());
    payload.insert(QStringLiteral("status"),
                   QGuiApplication::applicationState() == Qt::ApplicationActive
                       ? QStringLiteral("active")
                       : QStringLiteral("background"));

    if (!heartbeatAccountHidden())
    {
        payload.insert(QStringLiteral("activeAccount"), this->activeAccount());
    }

    return payload;
}

QJsonObject MoltorinoPresence::activeAccount() const
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        return {};
    }

    QJsonObject root;
    root.insert(QStringLiteral("userId"), account->getUserId());
    root.insert(QStringLiteral("username"), account->getUserName());
    return root;
}

MoltorinoPresence *getMoltorinoPresence()
{
    return &MoltorinoPresence::instance();
}

}
