#include "providers/kick/KickLiveController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"

#include <QPointer>

#include <chrono>

namespace {

using namespace std::chrono_literals;

constexpr auto IMMEDIATE_DELAY = 1s;
constexpr auto REFRESH_INTERVAL = 30s;

constexpr size_t CHUNK_SIZE = 50;

}

namespace chatterino {

KickLiveController::KickLiveController(KickChatServer &chatServer)
    : chatServer(chatServer)
{
    this->immediateTimer.setInterval(IMMEDIATE_DELAY);
    this->immediateTimer.setSingleShot(true);
    QObject::connect(&this->immediateTimer, &QTimer::timeout, this,
                     &KickLiveController::refreshImmediate);

    this->refreshTimer.setInterval(REFRESH_INTERVAL);
    this->refreshTimer.setSingleShot(false);
    this->refreshTimer.start();
    QObject::connect(&this->refreshTimer, &QTimer::timeout, this,
                     &KickLiveController::refreshAll);
}

KickLiveController::~KickLiveController() = default;

void KickLiveController::queueNewChannel(uint64_t userID)
{
    this->immediateChannels.emplace_back(userID);
    if (!this->immediateTimer.isActive())
    {
        this->immediateTimer.start();
    }
}

void KickLiveController::refreshImmediate()
{
    if (this->immediateChannels.empty())
    {
        return;
    }
    this->refreshList(this->immediateChannels);
    this->immediateChannels.clear();
}

void KickLiveController::refreshAll()
{
    std::vector<uint64_t> userIDs;
    for (const auto &[channelID, weak] : this->chatServer.channelMap())
    {
        auto chan = weak.lock();
        if (chan && chan->userID() != 0)
        {
            userIDs.emplace_back(chan->userID());
        }
    }
    this->refreshList(userIDs);
}

void KickLiveController::refreshList(const std::span<uint64_t> userIDs)
{
    if (!getApp()->getAccounts()->kick.isLoggedIn())
    {
        for (const auto id : userIDs)
        {
            const auto channel = this->chatServer.findByUserID(id);
            if (!channel || this->publicRequests.contains(id))
            {
                continue;
            }
            this->publicRequests.insert(id);
            KickApi::privateChannelInfo(
                channel->getName(),
                [self = QPointer(this), weak = channel->weakFromThis(),
                 id](const auto &result) {
                    if (!self)
                    {
                        return;
                    }
                    self->publicRequests.remove(id);
                    const auto channel = weak.lock();
                    if (channel && result && result->user.userID == id &&
                        result->isLive &&
                        !getApp()->getAccounts()->kick.isLoggedIn())
                    {
                        channel->updateLiveStatus(*result->isLive,
                                                  result->streamTitle);
                    }
                });
        }
        return;
    }

    std::span<uint64_t> remaining(userIDs);
    while (!remaining.empty())
    {
        auto chunk =
            remaining.subspan(0, std::min(CHUNK_SIZE, remaining.size()));
        remaining = remaining.subspan(chunk.size());

        getKickApi()->getChannels(
            chunk, [self = QPointer(this)](
                       const ExpectedStr<std::vector<KickChannelInfo>> &res) {
                if (!self)
                {
                    return;
                }
                if (!res)
                {
                    qCDebug(chatterinoKick)
                        << "Failed to refresh channels" << res.error();
                    return;
                }

                for (const auto &info : *res)
                {
                    auto chan = self->chatServer.findByUserID(info.userID);
                    if (!chan)
                    {
                        continue;
                    }
                    chan->updateStreamData(info);
                }
            });
    }
}

}
