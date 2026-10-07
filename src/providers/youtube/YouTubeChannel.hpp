#pragma once

#include "common/Channel.hpp"
#include "common/ChannelChatters.hpp"
#include "messages/Emote.hpp"
#include "providers/youtube/YouTubeApi.hpp"

#include <pajlada/signals/signal.hpp>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace chatterino {

class YouTubeChatServer;
struct YouTubeAuthor;

class YouTubeChannel : public Channel, public ChannelChatters
{
public:
    YouTubeChannel(QString stableSource, YouTubeChatServer &server);
    ~YouTubeChannel() override;

    std::shared_ptr<YouTubeChannel> sharedFromThis();
    std::weak_ptr<YouTubeChannel> weakFromThis();

    const QString &getDisplayName() const override;
    const QString &getLocalizedName() const override;
    bool canSendMessage() const override;
    bool isWritable() const override;
    void sendMessage(const QString &message) override;
    bool isMod() const override;
    bool isBroadcaster() const override;
    bool hasModRights() const override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;
    QUrl browserUrl() const;

    const QString &channelID() const;
    const QString &videoID() const;
    const QString &liveChatID() const;
    const QString &streamTitle() const;
    std::shared_ptr<const EmoteMap> youtubeEmotes() const;
    EmotePtr youtubeEmote(QStringView shortcut) const;

    void beginResolving();
    void applyResolved(const YouTubeResolvedChannel &resolved);
    void applyResolutionError(const QString &error);
    void receiveMessages(const std::vector<YouTubeMessage> &messages,
                         bool historical);
    void setObservedSelfRole(const YouTubeAuthor &author);
    void setConnectionState(bool connected, const QString &detail = {});
    void markChatEnded();
    void refreshAccountState(bool recheckRole = false);

    void deleteMessage(const QString &messageID);
    void moderateUser(const QString &channelID,
                      std::optional<std::chrono::seconds> duration);
    bool canUnbanUser(const QString &channelID) const;
    void unbanUser(const QString &channelID);

    bool canUseModerationTools() const;

    bool canModerateTarget(const YouTubeAuthor &author) const;

    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal streamDataChanged;
    pajlada::Signals::NoArgSignal userStateChanged;
    pajlada::Signals::NoArgSignal moderationStateChanged;
    pajlada::Signals::NoArgSignal youtubeEmotesChanged;

private:
    struct PendingModerationEcho {
        QString key;
        QString targetChannelID;
        MessagePtr message;
        std::chrono::steady_clock::time_point receivedAt;
    };

    struct KnownAuthorRole {
        bool isOwner = false;
        bool isModerator = false;
    };

    using UnauthorizedRetry =
        std::function<bool(const YouTubeApiError &error)>;
    using AuthenticatedAction =
        std::function<void(const QString &accessToken,
                           UnauthorizedRetry retryUnauthorized)>;
    using AuthenticationFailure = std::function<void(const QString &error)>;

    void addLoginMessage();
    void withAccessToken(AuthenticatedAction action,
                         AuthenticationFailure failure = {});
    void loadCustomEmotes();
    void learnMessageEmotes(const std::vector<YouTubeMessage> &messages);
    void upgradePendingCustomEmotes();
    bool containsKnownCustomEmote(QStringView text) const;
    void receiveAcknowledgedMessage(YouTubeMessage message,
                                    QStringView localMessageID);
    void failPendingMessage(QStringView localMessageID,
                            const QString &error);
    void failAllPendingMessages(const QString &error);
    QString displayNameForUser(const QString &channelID) const;
    void disableMessagesForBan(const QString &channelID);
    void restoreMessagesForUser(const QString &channelID);
    void rejectModerationAccess(const YouTubeApiError &error);
    bool isCurrentModerationContext(std::uint64_t generation,
                                    const QString &liveChatID,
                                    const QString &accountID) const;

    YouTubeChatServer &server_;
    QString displayName_;
    QString channelID_;
    QString videoID_;
    QString liveChatID_;
    QString streamTitle_;
    YouTubeReadTransport readTransport_ = YouTubeReadTransport::Relay;
    std::shared_ptr<const EmoteMap> youtubeEmotes_ = EMPTY_EMOTE_MAP;
    bool live_ = false;
    bool liveStatusKnown_ = false;
    bool connected_ = false;
    bool everConnected_ = false;
    bool observedMod_ = false;
    bool observedOwner_ = false;
    bool selfRoleKnown_ = false;
    QString roleAccountChannelID_;
    QString roleTargetChannelID_;

    std::uint64_t moderationGeneration_ = 0;
    bool resolutionErrorShown_ = false;
    bool offlineNoticeShown_ = false;
    bool chatEndedMessageSeen_ = false;
    bool initialResolutionNoticeShown_ = false;
    bool customEmojiSendNoticeShown_ = false;
    bool youtubeEmotesLoading_ = false;
    std::uint64_t youtubeEmoteGeneration_ = 0;
    std::unordered_map<QString, YouTubeMessage> pendingEmojiUpgrades_;
    std::unordered_map<QString, QString> banIDsByChannel_;
    std::unordered_map<QString, std::chrono::steady_clock::time_point>
        banCreatedAtByChannel_;
    std::unordered_set<QString> pendingModerationChannelIDs_;
    std::unordered_map<QString, KnownAuthorRole> knownAuthorRoles_;
    std::unordered_map<QString, std::unordered_set<QString>>
        restorableBanMessageIDs_;
    std::deque<PendingModerationEcho> pendingModerationEchoes_;
    YouTubePendingSendTracker pendingSends_;
    std::chrono::steady_clock::time_point lastSend_{};
};

}
