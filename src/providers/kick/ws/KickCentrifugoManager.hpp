#pragma once

#include "providers/kick/ws/KickWebSocketManager.hpp"

#include <memory>

namespace chatterino {

class KickCentrifugoManagerPrivate;

class KickCentrifugoManager : public KickWebSocketManager
{
public:
    KickCentrifugoManager(QString url, QString clientID);
    ~KickCentrifugoManager() override;

    void joinChannel(const QString &channelName) override;
    void partChannel(const QString &channelName) override;

private:
    std::unique_ptr<KickCentrifugoManagerPrivate> private_;
};

}
