#pragma once

#include "providers/youtube/YouTubeTypes.hpp"

#include <QObject>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QByteArray;
class QJsonObject;
class QNetworkReply;

namespace chatterino {

class YouTubeLiveChatSession : public QObject
{
public:
    enum class State {
        Connected,
        Reconnecting,
        RefreshRequired,
        Ended,
        Failed,
    };
    using MessagesCallback =
        std::function<void(std::vector<YouTubeMessage>, bool historical)>;
    using StateCallback = std::function<void(State, const QString &)>;

    YouTubeLiveChatSession(QString liveChatID, QString readTicket,
                           MessagesCallback messages, StateCallback state,
                           std::shared_ptr<YouTubeMessageDeduper> deduper);
    ~YouTubeLiveChatSession() override;

    Q_DISABLE_COPY_MOVE(YouTubeLiveChatSession)

    void start();
    void stop();

private:
    struct Runtime;

    void openStream();
    void readAvailable(QNetworkReply *reply);
    void streamFinished(QNetworkReply *reply);
    bool processLine(const QByteArray &line);
    void processFrame(const QJsonObject &frame);
    void scheduleReconnect(
        const QString &detail,
        std::optional<std::chrono::milliseconds> requestedDelay = {});
    void finish(State state, const QString &detail = {});
    void dispatchMessages(std::vector<YouTubeMessage> messages,
                          bool historical);
    void dispatchState(State state, QString detail);
    void cancelActiveCall();

    QString liveChatID_;
    QString readTicket_;
    MessagesCallback messagesCallback_;
    StateCallback stateCallback_;
    std::unique_ptr<Runtime> runtime_;
    bool started_ = false;
    bool stopping_ = false;
};

}
