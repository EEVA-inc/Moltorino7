#pragma once

#include "providers/youtube/YouTubeLiveChatSession.hpp"
#include "providers/youtube/YouTubeTypes.hpp"

#include <QObject>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QNetworkReply;

namespace chatterino {

class YouTubeInnertubeSession : public QObject
{
public:
    using State = YouTubeLiveChatSession::State;
    using MessagesCallback = YouTubeLiveChatSession::MessagesCallback;
    using StateCallback = YouTubeLiveChatSession::StateCallback;

    YouTubeInnertubeSession(QString videoID, QString liveChatID,
                            MessagesCallback messages, StateCallback state,
                            std::shared_ptr<YouTubeMessageDeduper> deduper);
    ~YouTubeInnertubeSession() override;

    Q_DISABLE_COPY_MOVE(YouTubeInnertubeSession)

    void start();
    void stop();

private:
    struct Runtime;

    void bootstrap(bool fresh);
    void retryBootstrap(const QString &detail);
    void requestContinuation();
    void readAvailable(QNetworkReply *reply);
    void requestFinished(QNetworkReply *reply);
    void schedulePoll(std::chrono::milliseconds delay);
    void retry(const QString &detail, bool rebootstrap = false,
               std::optional<std::chrono::milliseconds> delay = {});
    void finish(State state, const QString &detail = {});
    void cancelActiveCall();
    void dispatchMessages(std::vector<YouTubeMessage> messages,
                          bool historical);
    void deliverPendingMessages();
    [[nodiscard]] std::size_t pendingLiveMessageCount() const;
    [[nodiscard]] std::chrono::milliseconds remainingDeliveryWindow() const;
    void dispatchState(State state, QString detail);

    QString videoID_;
    QString liveChatID_;
    MessagesCallback messagesCallback_;
    StateCallback stateCallback_;
    std::unique_ptr<Runtime> runtime_;
    bool started_ = false;
    bool stopping_ = false;
};

}
