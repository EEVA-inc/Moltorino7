#pragma once

#include "messages/Message.hpp"

#include <QJsonObject>
#include <QVariantMap>

#include <functional>

namespace chatterino {
class Channel;
struct YouTubeMessage;

namespace recording {

MessagePtrMut makeSavedMessage(const QString &path, qint64 messages,
                               double durationSeconds, qint64 missingImages);

bool isSupportedSource(const Channel &channel);
QJsonObject describeSource(const Channel &channel);
QJsonObject normalizeMessage(const Channel &channel, const Message &message,
                             QString originalText = {},
                             QJsonObject metadata = {});
QJsonObject normalizeYouTube(const Channel &channel, const Message &message,
                             const YouTubeMessage &source);
QJsonObject normalizeTwitch(const Channel &channel, const Message &message,
                            const QString &originalText,
                            const QVariantMap &tags);
QString stableKey(const QJsonObject &object);
void publicEvent(const Channel &channel, QJsonObject event);

class LiveMessageScope
{
public:
    LiveMessageScope(const Message *message,
                     std::function<QJsonObject()> factory);
    ~LiveMessageScope();
    LiveMessageScope(const LiveMessageScope &) = delete;
    LiveMessageScope &operator=(const LiveMessageScope &) = delete;

    static bool enabled();
    static void setEnabled(bool enabled);
    static const QJsonObject *current(const Message *message);

private:
    const Message *message_;
    std::function<QJsonObject()> factory_;
    QJsonObject record_;
    LiveMessageScope *previous_;
};

template <typename Factory, typename Deliver>
void deliverLive(const MessagePtr &message, Factory &&factory,
                 Deliver &&deliver)
{
    if (!LiveMessageScope::enabled())
    {
        deliver();
        return;
    }
    LiveMessageScope scope(message.get(), std::forward<Factory>(factory));
    deliver();
}

}
}
