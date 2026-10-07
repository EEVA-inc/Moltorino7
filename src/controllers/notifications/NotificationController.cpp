// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/notifications/NotificationController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/notifications/NotificationModel.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"

#include <QUrl>

#include <ranges>

namespace ranges = std::ranges;

namespace chatterino {

NotificationController::NotificationController()
{
    for (const QString &channelName : this->youtubeSetting_.getValue())
    {
        this->channelMap[Platform::YouTube].append(
            YouTubeApi::normalizeSource(channelName));
    }
    std::ignore =
        this->channelMap[Platform::YouTube].delayedItemsChanged.connect([this] {
            this->youtubeSetting_.setValue(
                this->channelMap[Platform::YouTube].raw());
        });
    for (const QString &channelName : this->twitchSetting_.getValue())
    {
        this->channelMap[Platform::Twitch].append(channelName);
    }

    std::ignore =
        this->channelMap[Platform::Twitch].delayedItemsChanged.connect([this] {
            this->twitchSetting_.setValue(
                this->channelMap[Platform::Twitch].raw());
        });

    QObject::connect(&this->liveStatusTimer_, &QTimer::timeout, [this] {
        this->fetchFakeChannels();
    });
    this->liveStatusTimer_.start(60 * 1000);
}

void NotificationController::initialize()
{
    this->fetchFakeChannels();
}

void NotificationController::updateChannelNotification(
    const QString &channelName, Platform p, const QString &resolvedChannelId)
{
    if (this->isChannelNotified(channelName, p, resolvedChannelId))
    {
        this->removeChannelNotification(channelName, p, resolvedChannelId);
    }
    else
    {
        this->addChannelNotification(channelName, p, resolvedChannelId);
    }
}

bool NotificationController::isChannelNotified(
    const QString &channelName, Platform p,
    const QString &resolvedChannelId) const
{
    const auto normalized = p == Platform::YouTube
                                ? YouTubeApi::normalizeSource(channelName)
                                : channelName;
    const auto canonical = resolvedChannelId.isEmpty()
                               ? QString{}
                               : QStringLiteral("channel:") + resolvedChannelId;
    return ranges::any_of(this->channelMap.at(p).raw(), [&](const auto &name) {
        return p == Platform::YouTube
                   ? name == normalized ||
                         (!canonical.isEmpty() && name == canonical)
                   : name.compare(channelName, Qt::CaseInsensitive) == 0;
    });
}

void NotificationController::addChannelNotification(
    const QString &channelName, Platform p, const QString &resolvedChannelId)
{
    if (this->isChannelNotified(channelName, p, resolvedChannelId))
    {
        return;
    }

    auto name = channelName;
    if (p == Platform::YouTube)
    {
        name = resolvedChannelId.isEmpty()
                   ? YouTubeApi::normalizeSource(channelName)
                   : QStringLiteral("channel:") + resolvedChannelId;
    }
    this->channelMap[p].append(name);
}

void NotificationController::removeChannelNotification(
    const QString &channelName, Platform p, const QString &resolvedChannelId)
{
    const auto normalized = p == Platform::YouTube
                                ? YouTubeApi::normalizeSource(channelName)
                                : channelName;
    const auto canonical = resolvedChannelId.isEmpty()
                               ? QString{}
                               : QStringLiteral("channel:") + resolvedChannelId;
    for (std::vector<int>::size_type i = 0;
         i != this->channelMap[p].raw().size(); i++)
    {
        const auto &name = this->channelMap[p].raw()[i];
        if (p == Platform::YouTube
                ? name == normalized ||
                      (!canonical.isEmpty() && name == canonical)
                : name.compare(channelName, Qt::CaseInsensitive) == 0)
        {
            this->channelMap[p].removeAt(static_cast<int>(i));
            i--;
        }
    }
}

void NotificationController::playSound() const
{
    QUrl highlightSoundUrl =
        getSettings()->notificationCustomSound
            ? QUrl::fromLocalFile(
                  getSettings()->notificationPathSound.getValue())
            : QUrl("qrc:/sounds/ping2.wav");

    getApp()->getSound()->play(highlightSoundUrl);
}

NotificationModel *NotificationController::createModel(QObject *parent,
                                                       Platform p)
{
    auto *model = new NotificationModel(parent, p);
    model->initialize(&this->channelMap[p]);
    return model;
}

void NotificationController::notifyChannelLive(
    const NotificationPayload &payload) const
{
    bool showNotification =
        !payload.isDuplicateBroadcast &&
        !(getSettings()->suppressInitialLiveNotification &&
          payload.isInitialUpdate) &&
        !(getApp()->getStreamerMode()->isEnabled() &&
          getSettings()->streamerModeSuppressLiveNotifications);
    bool playedSound = false;

    if (showNotification &&
        this->isChannelNotified(payload.channelName, payload.platform,
                                payload.resolvedChannelId))
    {
        if (Toasts::isEnabled())
        {
            getApp()->getToasts()->sendChannelNotification(
                payload.channelName, payload.title, payload.url,
                payload.displayName);
        }
        if (getSettings()->notificationPlaySound)
        {
            this->playSound();
            playedSound = true;
        }
        if (getSettings()->notificationFlashTaskbar)
        {
            getApp()->getWindows()->sendAlert();
        }
    }

    auto liveChannel = getApp()->getTwitch()->getLiveChannel();
    if (payload.platform == Platform::YouTube)
    {
        MessageBuilder builder;
        builder.emplace<TimestampElement>();
        builder->messageText =
            getSettings()->showTitleInLiveMessage
                ? QString("%1 is live: %2")
                      .arg(payload.displayName, payload.title)
                : QString("%1 is live!").arg(payload.displayName);
        builder->searchText = builder->messageText;
        builder
            .emplace<TextElement>(builder->messageText,
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::Url, payload.url.toString()});
        builder->id = payload.channelId;
        builder->channelName = payload.channelName;
        builder->platform = MessagePlatform::YouTube;
        builder->flags.set(MessageFlag::DoNotLog);
        liveChannel->addMessage(builder.release(), MessageContext::Original);
    }
    else
    {
        liveChannel->addMessage(
            MessageBuilder::makeLiveMessage(payload.displayName,
                                            payload.channelId, payload.title),
            MessageContext::Original);
    }

    if (showNotification && !playedSound &&
        getSettings()->notificationOnAnyChannel)
    {
        this->playSound();
    }
}

void NotificationController::notifyTwitchChannelOffline(const QString &id) const
{

    auto snapshot =
        getApp()->getTwitch()->getLiveChannel()->getMessageSnapshot(200);
    for (const auto &s : snapshot | std::views::reverse)
    {
        if (s->id == id)
        {
            s->flags.set(MessageFlag::Disabled);
            break;
        }
    }
}

void NotificationController::fetchFakeChannels()
{
    qCDebug(chatterinoNotification) << "fetching fake channels";

    QStringList channels;
    for (size_t i = 0; i < this->channelMap[Platform::Twitch].raw().size(); i++)
    {
        const auto &name = this->channelMap[Platform::Twitch].raw()[i];
        auto chan = getApp()->getTwitch()->getChannelOrEmpty(name);
        if (chan->isEmpty())
        {
            channels.push_back(name);
        }
        else
        {
            this->fakeChannels_.erase(name);
        }
    }

    for (const auto &batch : splitListIntoBatches(channels))
    {
        getHelix()->fetchStreams(
            {}, batch,
            [batch, this](const auto &streams) {
                std::map<QString, std::optional<HelixStream>,
                         QCompareCaseInsensitive>
                    liveStreams;
                for (const auto &stream : streams)
                {
                    liveStreams.emplace(stream.userLogin, stream);
                }

                for (const auto &name : batch)
                {
                    auto it = liveStreams.find(name);
                    if (it == liveStreams.end())
                    {
                        this->updateFakeChannel(name, std::nullopt);
                    }
                    else
                    {
                        this->updateFakeChannel(name, it->second);
                    }
                }
            },
            [batch]() {

                qCWarning(chatterinoNotification)
                    << "Failed to fetch live status for " << batch;
            },
            []() {

            });
    }
}
void NotificationController::updateFakeChannel(
    const QString &channelName, const std::optional<HelixStream> &stream)
{
    bool live = stream.has_value();
    qCDebug(chatterinoNotification).nospace().noquote()
        << "[FakeTwitchChannel " << channelName
        << "] New live status: " << stream.has_value();

    auto channelIt = this->fakeChannels_.find(channelName);
    bool isInitialUpdate = false;
    if (channelIt == this->fakeChannels_.end())
    {
        channelIt = this->fakeChannels_
                        .emplace(channelName,
                                 FakeChannel{
                                     .id = {},
                                     .isLive = live,
                                 })
                        .first;
        isInitialUpdate = true;
    }
    if (channelIt->second.isLive == live && !isInitialUpdate)
    {
        return;
    }

    if (live && channelIt->second.id.isNull())
    {
        channelIt->second.id = stream->userId;
    }

    channelIt->second.isLive = live;

    if (!live)
    {

        this->notifyTwitchChannelOffline(channelIt->second.id);
        return;
    }

    this->notifyChannelLive({
        .channelId = stream->userId,
        .channelName = channelName,
        .displayName = stream->userName,
        .title = stream->title,
        .isInitialUpdate = isInitialUpdate,
    });
}

}
