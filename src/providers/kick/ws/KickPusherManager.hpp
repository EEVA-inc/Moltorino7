#pragma once

#include "providers/kick/ws/KickWebSocketManager.hpp"

#include <memory>

namespace chatterino {

class KickPusherManagerPrivate;

class KickPusherManager : public KickWebSocketManager
{
public:
    explicit KickPusherManager(QString url);
    ~KickPusherManager() override;

    void joinChannel(const QString &channelName) override;
    void partChannel(const QString &channelName) override;

private:
    std::unique_ptr<KickPusherManagerPrivate> private_;
};

}
