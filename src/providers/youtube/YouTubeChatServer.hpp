#pragma once

#include "common/Channel.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "providers/youtube/YouTubeInnertubeSession.hpp"
#include "providers/youtube/YouTubeLiveChatSession.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chatterino {

class YouTubeChannel;

class YouTubeChatServer : public QObject
{
public:
    YouTubeChatServer();
    ~YouTubeChatServer() override;

    Q_DISABLE_COPY_MOVE(YouTubeChatServer)

    void initialize();
    ChannelPtr getOrCreate(const QString &source);
    std::shared_ptr<YouTubeChannel> findBySource(const QString &source) const;
    bool hasLiveNotificationSibling(const YouTubeChannel &channel) const;
    void resolve(const std::shared_ptr<YouTubeChannel> &channel,
                 bool immediate = false);
    void probe(const std::shared_ptr<YouTubeChannel> &channel,
               bool immediate = false);
    void restart(const std::shared_ptr<YouTubeChannel> &channel);
    void refreshAccountRoles();
    void setActiveChannels(
        const std::vector<std::shared_ptr<YouTubeChannel>> &channels,
        QObject *owner = nullptr);
    void clearActiveChannels(QObject *owner);

private:
    static constexpr std::size_t MAX_CONCURRENT_PROBES = 4;

    struct SessionEntry {
        explicit SessionEntry(std::size_t replayLimit)
            : replay(replayLimit)
        {
        }

        std::shared_ptr<YouTubeInnertubeSession> innertubeSession;
        std::shared_ptr<YouTubeLiveChatSession> relaySession;
        std::vector<std::weak_ptr<YouTubeChannel>> channels;
        YouTubeReplayBuffer replay;
        std::shared_ptr<YouTubeMessageDeduper> deduper =
            std::make_shared<YouTubeMessageDeduper>();
        QString videoID;
        QString readTicket;
        YouTubeReadTransport readTransport = YouTubeReadTransport::Relay;
        bool relayFallback = false;
        bool innertubeRecoveryScheduled = false;
    };

    struct ProbeState {
        std::chrono::steady_clock::time_point lastStarted{};
        std::chrono::steady_clock::time_point lastSafetyFallback{};
        std::uint8_t consecutiveFailures = 0;
    };

    void processResolveQueue();
    void processProbeQueue();
    void scheduleProbeDispatch();
    void attachResolved(const std::shared_ptr<YouTubeChannel> &channel,
                        const YouTubeResolvedChannel &resolved);
    void attachChannel(SessionEntry &entry,
                       const std::shared_ptr<YouTubeChannel> &channel);
    void startInnertubeSession(const QString &videoID,
                               const QString &liveChatID, SessionEntry &entry);
    void startRelaySession(const QString &liveChatID, const QString &readTicket,
                           SessionEntry &entry);
    void scheduleInnertubeRecovery(const QString &liveChatID,
                                   SessionEntry &entry,
                                   std::chrono::milliseconds delay);
    void fanOut(const QString &liveChatID, std::vector<YouTubeMessage> messages,
                bool historical);
    void sessionState(const QString &liveChatID,
                      YouTubeLiveChatSession::State state,
                      const QString &detail, bool fromRelay);
    void scheduleResolve(const std::shared_ptr<YouTubeChannel> &channel,
                         std::chrono::milliseconds delay);
    void prune(SessionEntry &entry);
    bool isRegistered(const std::shared_ptr<YouTubeChannel> &channel) const;

    std::unordered_map<QString, std::weak_ptr<YouTubeChannel>> channels_;
    std::unordered_map<QString, SessionEntry> sessions_;
    std::unordered_map<QString, std::uint64_t> resolveGenerations_;
    std::unordered_map<QString, std::uint8_t> resolveFailures_;
    std::unordered_set<QString> resolvesInFlight_;
    std::deque<std::weak_ptr<YouTubeChannel>> resolveQueue_;
    std::unordered_map<QString, ProbeState> probeStates_;
    std::unordered_set<QString> probesInFlight_;
    std::deque<std::weak_ptr<YouTubeChannel>> probeQueue_;
    std::unordered_map<QString, std::weak_ptr<YouTubeChannel>> activeChannels_;
    QPointer<QObject> activeOwner_;
    QMetaObject::Connection activeOwnerDestroyed_;
    QTimer resolveTimer_;
    QTimer probeDispatchTimer_;
    QTimer cleanupTimer_;
    QTimer offlineProbeTimer_;
    pajlada::Signals::SignalHolder signals_;
    bool initialized_ = false;
};

}
