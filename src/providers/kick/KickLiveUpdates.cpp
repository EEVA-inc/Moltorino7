#include "providers/kick/KickLiveUpdates.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "providers/kick/ws/KickCentrifugoManager.hpp"
#include "providers/kick/ws/KickPusherManager.hpp"
#include "providers/kick/ws/KickWebSocketCommon.hpp"

#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include <map>

using namespace Qt::Literals;

namespace chatterino {

class KickLiveUpdatesPrivate : public QObject
{
public:
    KickLiveUpdatesPrivate()
    {
        this->retry.setSingleShot(true);
        this->retry.setInterval(std::chrono::seconds(30));
        QObject::connect(&this->retry, &QTimer::timeout, this, [this] {
            this->fetchConnection();
        });
    }

    void join(uint64_t roomID, uint64_t channelID)
    {
        assertInGuiThread();
        if (!roomID || !channelID)
        {
            return;
        }
        auto &references = this->rooms[{roomID, channelID}];
        ++references;
        if (references != 1)
        {
            return;
        }
        if (this->manager)
        {
            this->applyRoom(roomID, channelID, true);
        }
        else
        {
            this->fetchConnection();
        }
    }

    void leave(uint64_t roomID, uint64_t channelID)
    {
        assertInGuiThread();
        const auto it = this->rooms.find({roomID, channelID});
        if (it == this->rooms.end())
        {
            return;
        }
        --it->second;
        if (it->second != 0)
        {
            return;
        }
        this->rooms.erase(it);
        if (this->manager)
        {
            this->applyRoom(roomID, channelID, false);
        }
        if (this->rooms.empty())
        {
            ++this->generation;
            this->requestInProgress = false;
            this->retry.stop();
            this->manager.reset();
        }
    }

private:
    void fetchConnection()
    {
        if (this->requestInProgress || this->rooms.empty())
        {
            return;
        }
        this->requestInProgress = true;
        const auto requestGeneration = ++this->generation;
        const auto channelID = this->rooms.begin()->first.second;
        NetworkRequest(
            QString(u"https://web.kick.com/api/v1/realtime/channels/" %
                    QString::number(channelID) % u"/chat/connection"),
            NetworkRequestType::Post)
            .json(QJsonObject{
                {"client",
                 QJsonObject{{"id", this->clientID}, {"type", "web"}}},
                {"capabilities",
                 QJsonObject{
                     {"accepted_providers",
                      QJsonArray{QJsonObject{{"provider", "pusher"}},
                                 QJsonObject{{"provider", "centrifugo"}}}}}},
            })
            .timeout(10'000)
            .maximumResponseSize(64 * 1024)
            .onSuccess([self = QPointer(this),
                        requestGeneration](const NetworkResult &res) {
                if (self && self->generation == requestGeneration)
                {
                    self->useConnection(
                        kick::ws::parseConnection(res.parseJson()));
                }
            })
            .onError([self = QPointer(this),
                      requestGeneration](const NetworkResult &) {
                if (self && self->generation == requestGeneration)
                {
                    self->useConnection(std::nullopt);
                }
            })
            .execute();
    }

    void useConnection(std::optional<kick::ws::Connection> connection)
    {
        this->requestInProgress = false;
        if (this->rooms.empty())
        {
            return;
        }
        if (!connection)
        {
            this->retry.start();
            if (this->manager)
            {
                return;
            }
            qCWarning(chatterinoKick)
                << "Kick connection discovery failed; using Pusher";
            connection = kick::ws::Connection{false, kick::ws::pusherUrl()};
        }
        else
        {
            this->retry.stop();
        }
        if (this->manager &&
            connection->centrifugo == this->connection.centrifugo &&
            connection->url == this->connection.url)
        {
            return;
        }
        this->manager.reset();
        this->connection = *connection;
        if (connection->centrifugo)
        {
            this->manager = std::make_unique<KickCentrifugoManager>(
                connection->url, this->clientID);
        }
        else
        {
            this->manager =
                std::make_unique<KickPusherManager>(connection->url);
        }
        for (const auto &[room, references] : this->rooms)
        {
            this->applyRoom(room.first, room.second, true);
        }
    }

    void applyRoom(uint64_t roomID, uint64_t channelID, bool join)
    {
        const QString topics[] = {
            u"chatroom_" % QString::number(roomID),
            u"chatrooms." % QString::number(roomID),
            u"chatrooms." % QString::number(roomID) % u".v2",
            u"channel." % QString::number(channelID),
            u"channel_" % QString::number(channelID),
            u"predictions-channel-" % QString::number(channelID),
        };
        for (const auto &topic : topics)
        {
            if (join)
            {
                this->manager->joinChannel(topic);
            }
            else
            {
                this->manager->partChannel(topic);
            }
        }
    }

    std::unique_ptr<KickWebSocketManager> manager;
    std::map<std::pair<uint64_t, uint64_t>, size_t> rooms;
    const QString clientID = QUuid::createUuid().toString(QUuid::WithoutBraces);
    kick::ws::Connection connection;
    QTimer retry;
    uint64_t generation = 0;
    bool requestInProgress = false;
};

KickLiveUpdates::KickLiveUpdates()
    : private_(std::make_unique<KickLiveUpdatesPrivate>())
{
}
KickLiveUpdates::~KickLiveUpdates() = default;

void KickLiveUpdates::joinRoom(uint64_t roomID, uint64_t channelID)
{
    this->private_->join(roomID, channelID);
}

void KickLiveUpdates::leaveRoom(uint64_t roomID, uint64_t channelID)
{
    this->private_->leave(roomID, channelID);
}

}
