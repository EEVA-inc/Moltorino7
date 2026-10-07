// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/Connection.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/automod/AutoModReviewController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/eventsub/Controller.hpp"
#include "providers/twitch/eventsub/MessageBuilder.hpp"
#include "providers/twitch/eventsub/MessageHandlers.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"

#include <boost/json.hpp>
#include <QDateTime>
#include <QTimer>
#include <QVector>
#include <twitch-eventsub-ws/listener.hpp>
#include <twitch-eventsub-ws/session.hpp>

#include <algorithm>
#include <chrono>
#include <optional>

namespace {

using namespace chatterino;
using namespace chatterino::eventsub;

namespace channel_moderate = lib::payload::channel_moderate::v2;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoTwitchEventSub;

template <typename Action>
concept CanMakeModMessage = requires(
    EventSubMessageBuilder &builder, const channel_moderate::Event &event,
    const std::remove_cvref_t<Action> &action) {
    makeModerateMessage(builder, event, action);
};

template <typename Action>
concept CanHandleModMessage =
    requires(TwitchChannel *channel, const QDateTime &time,
             const channel_moderate::Event &event,
             const std::remove_cvref_t<Action> &action) {
        handleModerateMessage(channel, time, event, action);
    };

struct RecentRoleMod {
    QString key;
    QDateTime expiresAt;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
QVector<RecentRoleMod> recentRoleMods;

QString roleEventKey(const channel_moderate::Event &event,
                     const lib::String &targetLogin)
{
    const auto broadcaster = event.broadcasterUserLogin.qt().toLower();
    const auto moderator = event.moderatorUserLogin.qt().toLower();
    const auto target = targetLogin.qt().toLower();

    QString key;
    key.reserve(broadcaster.size() + moderator.size() + target.size() + 2);
    key.append(broadcaster);
    key.append(u'|');
    key.append(moderator);
    key.append(u'|');
    key.append(target);
    return key;
}

void pruneRecentRoleMods(const QDateTime &now)
{
    recentRoleMods.erase(
        std::remove_if(recentRoleMods.begin(), recentRoleMods.end(),
                       [&now](const auto &entry) {
                           return entry.expiresAt <= now;
                       }),
        recentRoleMods.end());
}

void rememberRecentRoleMod(const QString &key)
{
    const auto now = QDateTime::currentDateTimeUtc();
    pruneRecentRoleMods(now);

    if (recentRoleMods.size() >= 512)
    {
        recentRoleMods.removeFirst();
    }
    recentRoleMods.push_back({key, now.addMSecs(2500)});
}

bool hasRecentRoleMod(const QString &key)
{
    const auto now = QDateTime::currentDateTimeUtc();
    pruneRecentRoleMods(now);

    return std::any_of(recentRoleMods.cbegin(), recentRoleMods.cend(),
                       [&key](const auto &entry) {
                           return entry.key == key;
                       });
}

std::optional<QString> deletedMessageID(const MessagePtr &message)
{
    if (!message || !message->flags.has(MessageFlag::ModerationAction))
    {
        return std::nullopt;
    }

    for (const auto &element : message->elements)
    {
        const auto link = element->getLink();
        if (link.type == Link::JumpToMessage && !link.value.isEmpty())
        {
            return link.value;
        }
    }

    return std::nullopt;
}

void addOrReplaceDeleteAction(TwitchChannel *channel, MessagePtr message)
{
    const auto messageID = deletedMessageID(message);
    if (!messageID)
    {
        channel->addMessage(message, MessageContext::Original);
        return;
    }

    if (auto original = channel->findMessageByID(*messageID))
    {
        original->flags.set(MessageFlag::Disabled);
        original->flags.set(MessageFlag::InvalidReplyTarget);
    }

    const auto messages = channel->getMessageSnapshot();
    auto it = std::find_if(messages.rbegin(), messages.rend(),
                           [&](const auto &m) {
                               const auto existingID = deletedMessageID(m);
                               return existingID && *existingID == *messageID;
                           });
    if (it != messages.rend())
    {
        channel->replaceMessage(*it, message);
        return;
    }

    channel->addMessage(message, MessageContext::Original);
}

}  // namespace

namespace chatterino::eventsub {

void Connection::onSessionWelcome(
    const lib::messages::Metadata &metadata,
    const lib::payload::session_welcome::Payload &payload)
{
    (void)metadata;
    qCDebug(LOG) << "On session welcome:" << payload.id.c_str();

    this->sessionID = QString::fromStdString(payload.id);
}

void Connection::onNotification(const lib::messages::Metadata &metadata,
                                const boost::json::value &jv)
{
    (void)metadata;
    auto jsonString = boost::json::serialize(jv);
    qCDebug(LOG) << "on notification: " << jsonString.c_str();
}

void Connection::onRevocation(const lib::messages::Metadata &,
                              const boost::json::value &jv)
{
    const auto *subscription = jv.is_object()
                                   ? jv.as_object().if_contains("subscription")
                                   : nullptr;
    if (!subscription || !subscription->is_object())
    {
        return;
    }
    const auto &object = subscription->as_object();
    const auto *id = object.if_contains("id");
    const auto *status = object.if_contains("status");
    if (id && id->is_string() && status && status->is_string())
    {
        getApp()->getEventSub()->subscriptionRevoked(
            QString::fromStdString(std::string(id->as_string())),
            QString::fromStdString(std::string(status->as_string())));
    }
}

void Connection::onClose(std::unique_ptr<lib::Listener> self,
                         const std::optional<std::string> &reconnectURL)
{
    if (isAppAboutToQuit())
    {
        return;
    }

    auto *app = tryGetApp();
    if (!app)
    {
        return;
    }

    app->getEventSub()->reconnectConnection(std::move(self), reconnectURL,
                                            this->subscriptions);
}

void Connection::onChannelBan(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_ban::v1::Payload &payload)
{
    (void)metadata;
    qCDebug(LOG) << "On channel ban event for channel"
                 << payload.event.broadcasterUserLogin.c_str();
}

void Connection::onStreamOnline(
    const lib::messages::Metadata &metadata,
    const lib::payload::stream_online::v1::Payload &payload)
{
    (void)metadata;
    qCDebug(LOG) << "On stream online event for channel"
                 << payload.event.broadcasterUserLogin.c_str();
}

void Connection::onStreamOffline(
    const lib::messages::Metadata &metadata,
    const lib::payload::stream_offline::v1::Payload &payload)
{
    (void)metadata;
    qCDebug(LOG) << "On stream offline event for channel"
                 << payload.event.broadcasterUserLogin.c_str();
}

void Connection::onChannelChatNotification(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_chat_notification::v1::Payload &payload)
{
    (void)metadata;
    qCDebug(LOG) << "On channel chat notification for"
                 << payload.event.broadcasterUserLogin.c_str();
}

void Connection::onChannelUpdate(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_update::v1::Payload &payload)
{
    (void)metadata;
    qCDebug(LOG) << "On channel update for"
                 << payload.event.broadcasterUserLogin.c_str();
}

void Connection::onChannelChatMessage(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_chat_message::v1::Payload &payload)
{
    (void)metadata;

    qCDebug(LOG) << "Channel chat message event for"
                 << payload.event.broadcasterUserLogin.c_str();
}

void Connection::onChannelModerate(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_moderate::v2::Payload &payload)
{
    (void)metadata;

    auto channelPtr = getApp()->getTwitch()->getChannelOrEmpty(
        payload.event.broadcasterUserLogin.qt());
    if (channelPtr->isEmpty())
    {
        qCDebug(LOG)
            << "Channel moderate event for broadcaster we're not interested in"
            << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto *channel = dynamic_cast<TwitchChannel *>(channelPtr.get());
    if (channel == nullptr)
    {
        qCDebug(LOG)
            << "Channel moderate event for broadcaster is not a Twitch channel?"
            << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto now = chronoToQDateTime(metadata.messageTimestamp);

    std::visit(
        [&](auto &&action) {
            using Action = std::remove_cvref_t<decltype(action)>;
            if constexpr (std::is_same_v<Action, channel_moderate::Mod> ||
                          std::is_same_v<Action, channel_moderate::Unmod>)
            {


                if (!payload.event.isFromSharedChat())
                {
                    runInGuiThread([channelPtr, channel, now,
                                    login = action.userLogin.qt()] {
                        channel->setKnownModeratorStatus(
                            login, std::is_same_v<Action, channel_moderate::Mod>,
                            now);
                    });
                }
            }
            static_assert(CanMakeModMessage<Action> ||
                              CanHandleModMessage<Action> ||
                              std::is_same_v<Action, std::string>,
                          "All actions must be handled");

            if constexpr (std::is_same_v<Action, std::string>)
            {
                qCWarning(LOG) << "Unhandled moderation action:"
                               << QUtf8StringView(action);
            }

            if constexpr (CanMakeModMessage<Action>)
            {
                // FIXME: This message should still be added, but instead hidden during layout if the setting is enabled.
                if (getSettings()->hideDeletionActions)
                {
                    return;
                }
                EventSubMessageBuilder builder(channel, now);
                builder->loginName = payload.event.moderatorUserLogin.qt();
                if (payload.event.isFromSharedChat() &&
                    payload.event.sourceBroadcasterUserID)
                {
                    builder->sharedChatSourceId =
                        payload.event.sourceBroadcasterUserID->qt();
                }
                makeModerateMessage(builder, payload.event, action);
                auto msg = builder.release();
                if constexpr (std::is_same_v<Action, channel_moderate::Mod>)
                {
                    const auto key = roleEventKey(payload.event,
                                                  action.userLogin);
                    runInGuiThread([channelPtr, msg, key] {
                        auto *roleChannel =
                            dynamic_cast<TwitchChannel *>(channelPtr.get());
                        if (roleChannel == nullptr || roleChannel->isEmpty())
                        {
                            return;
                        }

                        rememberRecentRoleMod(key);
                        roleChannel->addMessage(msg, MessageContext::Original);
                    });
                }
                else if constexpr (std::is_same_v<Action,
                                                   channel_moderate::Unvip>)
                {
                    const auto key = roleEventKey(payload.event,
                                                  action.userLogin);
                    runInGuiThread([channelPtr, msg, key] {
                        QTimer::singleShot(1500, [channelPtr, msg, key] {
                            if (hasRecentRoleMod(key))
                            {
                                return;
                            }

                            auto *delayedChannel =
                                dynamic_cast<TwitchChannel *>(
                                    channelPtr.get());
                            if (delayedChannel == nullptr ||
                                delayedChannel->isEmpty())
                            {
                                return;
                            }

                            delayedChannel->addMessage(
                                msg, MessageContext::Original);
                        });
                    });
                }
                else
                {
                    runInGuiThread([channelPtr, msg] {
                        auto *moderateChannel = dynamic_cast<TwitchChannel *>(
                            channelPtr.get());
                        if (moderateChannel == nullptr ||
                            moderateChannel->isEmpty())
                        {
                            return;
                        }

                        if constexpr (std::is_base_of_v<
                                          channel_moderate::Delete, Action>)
                        {
                            addOrReplaceDeleteAction(moderateChannel, msg);
                        }
                        else
                        {
                            moderateChannel->addMessage(
                                msg, MessageContext::Original);
                        }
                    });
                }
            }

            if constexpr (CanHandleModMessage<Action>)
            {
                handleModerateMessage(channel, now, payload.event, action);
            }
        },
        payload.event.action);

    runInGuiThread([channel = std::move(channelPtr)] {});
}

void Connection::onAutomodMessageHold(
    const lib::messages::Metadata &metadata,
    const lib::payload::automod_message_hold::v2::Payload &payload)
{
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(
            payload.event.broadcasterUserLogin.qt()));
    if (!channel || channel->isEmpty())
    {
        qCDebug(LOG)
            << "Automod message hold for broadcaster we're not interested in"
            << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto data = makeAutoModReviewHoldData(
        QString::fromStdString(metadata.messageID),
        chronoToQDateTime(metadata.messageTimestamp), payload.event);
    runInGuiThread(
        [channel = std::move(channel), data = std::move(data)]() mutable {
            if (auto *review = getApp()->getAutoModReview())
            {
                review->ingestHold(std::move(data), channel);
            }
        });
}
void Connection::onAutomodMessageUpdate(
    const lib::messages::Metadata &metadata,
    const lib::payload::automod_message_update::v2::Payload &payload)
{
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(
            payload.event.broadcasterUserLogin.qt()));
    if (!channel || channel->isEmpty())
    {
        qCDebug(LOG)
            << "Automod message update for broadcaster we're not interested in"
            << payload.event.broadcasterUserLogin.qt();
        return;
    }
    auto data = makeAutoModReviewUpdateData(
        QString::fromStdString(metadata.messageID),
        chronoToQDateTime(metadata.messageTimestamp), payload.event);
    runInGuiThread(
        [channel = std::move(channel), data = std::move(data)]() mutable {
            if (auto *review = getApp()->getAutoModReview())
            {
                review->ingestUpdate(std::move(data), channel);
            }
        });
}

void Connection::onChannelSuspiciousUserMessage(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_suspicious_user_message::v1::Payload &payload)
{
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(
            payload.event.broadcasterUserLogin.qt()));
    if (!channel || channel->isEmpty())
    {
        qCDebug(LOG)
            << "Suspicious message for broadcaster we're not interested in"
            << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto time = chronoToQDateTime(metadata.messageTimestamp);
    const auto &event = payload.event;
    const auto status = event.lowTrustStatus;
    const auto messageID =
        event.message.messageID ? event.message.messageID->qt() : QString{};
    auto header = makeSuspiciousUserMessageHeader(channel.get(), time, event);
    auto body = status == lib::suspicious_users::Status::Restricted
                    ? makeSuspiciousUserMessageBody(channel.get(), time, event)
                    : MessagePtr{};
    runInGuiThread([channel = std::move(channel), status, messageID, header,
                    body] {
        if (status == lib::suspicious_users::Status::ActiveMonitoring)
        {


            if (!messageID.isEmpty())
            {
                channel->markMonitoredMessage(messageID, header->messageText);
            }
        }
        else if (body)
        {
            channel->addMessage(header, MessageContext::Original);
            channel->addMessage(body, MessageContext::Original);
        }
    });
}

void Connection::onChannelSuspiciousUserUpdate(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_suspicious_user_update::v1::Payload &payload)
{
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(
            payload.event.broadcasterUserLogin.qt()));
    if (!channel || channel->isEmpty())
    {
        qCDebug(LOG) << "Channel Suspicious User Update for broadcaster we're "
                        "not interested in"
                     << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto time = chronoToQDateTime(metadata.messageTimestamp);
    auto message = makeSuspiciousUserUpdate(channel.get(), time, payload.event);

    runInGuiThread([channel = std::move(channel), message] {
        channel->addMessage(message, MessageContext::Original);
    });
}

void Connection::onChannelChatUserMessageHold(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_chat_user_message_hold::v1::Payload &payload)
{
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(
            payload.event.broadcasterUserLogin.qt()));
    if (!channel || channel->isEmpty())
    {
        qCDebug(LOG) << "Channel Chat User Message Hold for broadcaster we're "
                        "not interested in"
                     << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto time = chronoToQDateTime(metadata.messageTimestamp);
    auto message =
        makeUserMessageHeldMessage(channel.get(), time, payload.event);

    runInGuiThread([channel = std::move(channel), message] {
        channel->addMessage(message, MessageContext::Original);
    });
}

void Connection::onChannelChatUserMessageUpdate(
    const lib::messages::Metadata &metadata,
    const lib::payload::channel_chat_user_message_update::v1::Payload &payload)
{
    auto channel = std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(
            payload.event.broadcasterUserLogin.qt()));
    if (!channel || channel->isEmpty())
    {
        qCDebug(LOG)
            << "Channel Chat User Message Update for broadcaster we're "
               "not interested in"
            << payload.event.broadcasterUserLogin.qt();
        return;
    }

    auto time = chronoToQDateTime(metadata.messageTimestamp);
    auto message =
        makeUserMessageUpdateMessage(channel.get(), time, payload.event);

    runInGuiThread([channel = std::move(channel), message] {
        channel->addMessage(message, MessageContext::Original);
    });
}

QString Connection::getSessionID() const
{
    return this->sessionID;
}

bool Connection::isSubscribedTo(const SubscriptionRequest &request) const
{
    return this->subscriptions.contains(request);
}

void Connection::markRequestSubscribed(const SubscriptionRequest &request)
{
    assert((this->twitchUserID.isEmpty() ||
            this->twitchUserID == request.ownerTwitchUserID) &&
           "A subscription was made when another user's subscriptions were "
           "still active");

    this->twitchUserID = request.ownerTwitchUserID;

    this->subscriptions.emplace(request);
}

void Connection::markRequestUnsubscribed(const SubscriptionRequest &request)
{
    this->subscriptions.erase(request);

    if (this->subscriptions.empty())
    {
        // TODO: Verify that it's fine for us to reuse a connection for another
        // user after all old subscriptions are gone
        this->twitchUserID.clear();
    }
}

bool Connection::canHandleSubscriptionFrom(
    const QString &otherTwitchUserID) const
{
    return this->twitchUserID.isEmpty() ||
           this->twitchUserID == otherTwitchUserID;
}

void Connection::debug()
{
    for (const auto &request : this->subscriptions)
    {
        qCInfo(LOG).noquote().nospace()
            << this->getSessionID() << " -> " << request;
    }
}

}  // namespace chatterino::eventsub
