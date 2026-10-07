// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <functional>
#include <optional>
#include <QDateTime>
#include <QHash>
#include <QJsonValue>
#include <QStringList>
#include <QString>
#include <QVector>

namespace chatterino {

struct PinnedMessage;
class TwitchAccount;

struct CustomAuthValidationResult {
    QString normalizedToken;
    QString userId;
    QString login;
    QString displayName;
};

struct GqlModeratedChannel {
    QString id;
    QString login;
    QString displayName;
};

struct GqlChannelSelfData {
    bool isLeadModerator = false;
};

struct GqlBroadcastCategory {
    QString id;
    QString name;
    QString displayName;
};

struct GqlContentClassificationLabel {
    QString id;
    QString name;
    QString description;
    QString lockedUntil;
    bool isEnabled = false;
    bool isLocked = false;
    bool isSelectable = false;
};

struct GqlBroadcastSettings {
    QString userId;
    QString title;
    QString language;
    GqlBroadcastCategory category;
    QStringList tags;
    bool isRerun = false;
    QString audience;
    bool canEditAudience = false;
    QVector<GqlContentClassificationLabel> contentLabels;
    QStringList audienceOptions;
    QStringList allowedContentLabelIds;
};

enum class GqlStartAdTrigger {
    ChatCommand,
    QuickAction,
};

struct GqlStartAdResult {
    QString adSessionId;
    QString errorCode;
    int lengthSeconds = 0;
    int retryAfterSeconds = 0;
};

struct GqlBlockedTerm {
    QString id;
    QString phrase;
    QString expiresAt;
    bool isModEditable = false;
    int hitCount = 0;
};

struct GqlAddBlockedTermResult {
    GqlBlockedTerm term;
    bool wasRemovedFromPermittedList = false;
};

struct GqlUser {
    QString id;
    QString login;
    QString displayName;
};

struct GqlVanityBadge {
    QString id;
    QString setId;
    QString version;
    QString title;
    QString image1;
    QString image2;
    QString image4;
};

struct GqlVanityState {
    QString currentUserId;
    QString currentUserLogin;
    QString currentUserDisplayName;
    QString channelId;
    QVector<GqlVanityBadge> globalBadges;
    std::optional<GqlVanityBadge> selectedGlobalBadge;
    QVector<GqlVanityBadge> channelBadges;
    std::optional<GqlVanityBadge> selectedChannelBadge;
};

struct TwitchGqlAuth {
    QString oauthToken;
    QString clientId;

    [[nodiscard]] bool isValid() const
    {
        return !this->oauthToken.trimmed().isEmpty();
    }

    bool operator==(const TwitchGqlAuth &) const = default;
};

namespace twitchgql::detail {

QHash<QString, bool> parseChatRoomBanStatuses(
    const QJsonValue &response, const QVector<QString> &channelIds);

}

enum class TwitchGqlAuthTransport {
    Browser,
    Tv,
};

namespace twitchgql::detail {

TwitchGqlAuthTransport authTransport(const TwitchGqlAuth &auth);
QString effectiveClientId(const TwitchGqlAuth &auth);
QString tvClientId();

}

struct GqlModLogMessage {
    QString id;
    QString text;
    QString sentAt;
};

struct GqlUsercardMessage {
    QString id;
    QString senderId;
    QString senderLogin;
    QString senderDisplayName;
    QString senderColor;
    QString senderBadges;
    QString text;
    QString sentAt;
    QString cursor;
    QString deletedBy;
    bool isDeleted = false;
};

struct GqlUsercardMessagePage {
    QVector<GqlUsercardMessage> messages;
    QString nextCursor;
    bool hasNextPage = false;
};

enum class GqlModerationActionKind {
    Ban,
    Unban,
    Timeout,
    Untimeout,
    Delete,
    Message,
    Other,
};

struct GqlModerationActionLogEntry {
    QString id;
    QString cursor;
    QString category;
    QString icon;
    QString text;
    QDateTime createdAt;
    GqlModerationActionKind kind = GqlModerationActionKind::Other;

    QString moderatorId;
    QString moderatorLogin;
    QString moderatorDisplayName;
    QString targetId;
    QString targetLogin;
    QString targetDisplayName;
};

struct GqlModerationActionLogPage {
    QVector<GqlModerationActionLogEntry> actions;
    QString nextCursor;
    bool hasNextPage = false;
};

struct GqlModeratorQueueUser {
    QString id;
    QString login;
    QString displayName;
    QString profileImageUrl;
    QString createdAt;
    QString chatColor;
};

struct GqlUnbanRequest {
    QString id;
    QString cursor;
    QString createdAt;
    QString status;
    GqlModeratorQueueUser requester;
    QString requesterMessage;
    QString resolvedAt;
    QString resolverMessage;
    GqlModeratorQueueUser resolvedBy;
};

struct GqlUnbanRequestPage {
    QVector<GqlUnbanRequest> requests;
    QString nextCursor;
    int totalCount = 0;
    int cooldownMinutes = 0;
    bool hasNextPage = false;
    bool isEnabled = true;
};

struct GqlUnbanRequestUserContext {
    GqlModeratorQueueUser user;
    QString bannedAt;
    QString bannedByLogin;
    int banCount = 0;
    int timeoutCount = 0;
    bool currentlyBanned = false;
};

struct GqlModeratorComment {
    QString id;
    QString cursor;
    QString timestamp;
    QString text;
    QString channelLogin;
    QString authorLogin;
    QString authorDisplayName;
    QString authorColor;
    bool shareable = false;
    bool shared = false;
};

struct GqlModeratorCommentPage {
    QVector<GqlModeratorComment> comments;
    QString nextCursor;
    bool hasNextPage = false;
};

struct RaidChannelIDs {
    QString sourceId;
    QString targetId;
    QString targetLogin;
    QString targetDisplayName;
};

struct PredictionTemplate {
    QString title;
    QStringList outcomes;
    int durationSeconds = 120;
};

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
struct GqlRewardRequestSummary {
    QString id;
    QString title;
    QString prompt;
    QString backgroundColor;
    QString imageUrl;
    int cost = 0;
    int pendingCount = 0;
    bool countAtMaximum = false;
    bool isEnabled = false;
    bool isPaused = false;
};

struct GqlRewardRequestOverview {
    QString channelId;
    QVector<GqlRewardRequestSummary> rewards;
    int totalPendingCount = 0;
    bool countAtMaximum = false;
    bool isAvailable = false;
    bool isEnabled = false;
};

struct GqlRewardRequest {
    QString id;
    QString cursor;
    QString rewardId;
    QString rewardTitle;
    GqlModeratorQueueUser user;
    QString input;
    QString timestamp;
};

struct GqlRewardRequestPage {
    QVector<GqlRewardRequest> requests;
    QString nextCursor;
    bool hasNextPage = false;
};

enum class GqlRewardRequestResolution {
    Complete,
    RejectAndRefund,
};

struct GqlChannelPointReward {
    QString id;
    QString title;
    QString prompt;
    QString rewardType;
    QString pricingType;
    QString backgroundColor;
    QString imageUrl;
    int cost = 0;
    bool isAutomatic = false;
    bool isEnabled = false;
    bool isInStock = false;
    bool isUserInputRequired = false;
};

struct GqlChannelPointRewards {
    QString channelId;
    QString channelDisplayName;
    qint64 balance = -1;
    QVector<GqlChannelPointReward> rewards;
};

struct GqlChannelPointEmoteModification {
    QString modifierId;
    QString emoteId;
    QString emoteToken;
};

struct GqlChannelPointEmote {
    QString id;
    QString token;
    QString type;
    QString ownerLogin;
    QString ownerDisplayName;
    QVector<GqlChannelPointEmoteModification> modifications;
};

struct GqlChannelPointEmoteModifier {
    QString id;
    QString title;
};

struct GqlChannelPointRedeemResult {
    qint64 balance = -1;
    QString emoteId;
    QString emoteToken;
};
#endif

class TwitchGql
{
public:
    static void sendChatMessageWithNonce(
        const QString &channelId, const QString &message, const QString &nonce,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getVanityState(
        const QString &channelLogin, const QString &expectedChannelId,
        const TwitchGqlAuth &auth,
        std::function<void(GqlVanityState)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void selectGlobalBadge(
        const QString &setId, const QString &version,
        const TwitchGqlAuth &auth,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void deselectGlobalBadge(
        const TwitchGqlAuth &auth, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void selectChannelBadge(
        const QString &channelId, const QString &setId, const QString &version,
        const TwitchGqlAuth &auth, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void deselectChannelBadge(
        const QString &channelId, const TwitchGqlAuth &auth,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void banUserFromChatRoom(
        const QString &channelId, const QString &targetLogin,
        const QString &reason, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unbanUserFromChatRoom(
        const QString &channelId, const QString &targetLogin,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChatRoomBanStatuses(
        const QString &targetUserId, const QVector<QString> &channelIds,
        const QString &oauthToken,
        std::function<void(QHash<QString, bool>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getUnbanRequests(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &cursor, bool newestFirst, const QString &oauthToken,
        std::function<void(GqlUnbanRequestPage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getUnbanRequestUserContext(
        const QString &channelId, const QString &userId,
        const QString &oauthToken,
        std::function<void(GqlUnbanRequestUserContext)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModeratorComments(
        const QString &channelId, const QString &userId, const QString &cursor,
        const QString &oauthToken,
        std::function<void(GqlModeratorCommentPage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getSharedModeratorComments(
        const QString &channelId, const QString &userId, const QString &cursor,
        const QString &oauthToken,
        std::function<void(GqlModeratorCommentPage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModeratorCommentSharingSetting(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(bool)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void createModeratorComment(
        const QString &channelId, const QString &userId, const QString &text,
        bool shareable, const QString &oauthToken,
        std::function<void(GqlModeratorComment)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void deleteModeratorComment(
        const QString &commentId, const QString &channelId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void resolveUnbanRequest(
        const QString &requestId, bool approve, const QString &moderatorNote,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelEditorStatus(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(bool)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getBroadcastSettings(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(GqlBroadcastSettings)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getBroadcastManagementState(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(GqlBroadcastSettings)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void updateBroadcastSettings(
        const GqlBroadcastSettings &settings, const QString &oauthToken,
        std::function<void(GqlBroadcastSettings)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void setFreeformTags(
        const QString &channelId, const QStringList &tags,
        const QString &oauthToken,
        std::function<void(QStringList)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void setContentClassificationLabels(
        const QString &channelId,
        const QVector<GqlContentClassificationLabel> &labels,
        const QString &oauthToken,
        std::function<void(QVector<GqlContentClassificationLabel>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);
    static void setChannelRerunStatus(
        const QString &channelId, bool shouldBeRerun,
        const QString &oauthToken, std::function<void(bool)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void startAd(
        const QString &channelId, int lengthSeconds,
        GqlStartAdTrigger trigger, const QString &oauthToken,
        std::function<void(GqlStartAdResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void pinMessage(const QString &channelId, const QString &messageId,
                           int durationSeconds, const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback);
    static void unpinMessage(const QString &pinId,
                             const QString &oauthToken,
                             std::function<void()> successCallback,
                             std::function<void(const QString &)> failureCallback);
    static void updatePinnedMessage(const QString &pinId,
                                    std::optional<int> durationSeconds,
                                    const QString &oauthToken,
                                    std::function<void()> successCallback,
                                    std::function<void(const QString &)> failureCallback);
    static void getCurrentPin(const QString &channelId,
                              std::shared_ptr<TwitchAccount> account,
                              std::function<void(std::optional<TwitchChannel::PinnedMessage>)>
                                  successCallback,
                              std::function<void(const QString &)> failureCallback);
    static void getUserByLogin(
        const QString &login, const QString &oauthToken,
        std::function<void(std::optional<GqlUser>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void followUser(
        const QString &targetId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unfollowUser(
        const QString &targetId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getLatestModLogMessageBySender(
        const QString &channelId, const QString &senderId,
        const QString &oauthToken,
        std::function<void(std::optional<GqlModLogMessage>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getUsercardMessagesBySender(
        const QString &channelId, const QString &senderId,
        const QString &cursor, const QString &oauthToken,
        std::function<void(GqlUsercardMessagePage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModerationActionLogs(
        const QString &channelId, const QString &cursor,
        const QString &oauthToken,
        std::function<void(GqlModerationActionLogPage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getActivePrediction(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(std::optional<TwitchChannel::PredictionEvent>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getActivePoll(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(std::optional<TwitchChannel::PollEvent>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);
    static void makePrediction(const QString &eventID, const QString &outcomeID,
                               int points, const QString &oauthToken,
                               std::function<void()> successCallback,
                               std::function<void(const QString &)> failureCallback);
    static void createPredictionEvent(
        const QString &channelId, const QString &title,
        const QStringList &outcomes, int predictionWindowSeconds,
        const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getPredictionTemplates(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(QVector<PredictionTemplate>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void lockPrediction(
        const QString &eventId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void cancelPrediction(
        const QString &eventId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void resolvePrediction(
        const QString &eventId, const QString &outcomeId,
        const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void createPollEvent(
        const QString &channelId, const QString &title,
        const QStringList &choices, int durationSeconds,
        std::optional<int> pointsPerVote, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void terminatePoll(const QString &pollId,
                              const QString &currentUserId,
                              const QString &oauthToken,
                              std::function<void()> successCallback,
                              std::function<void(const QString &)> failureCallback);
    static void archivePoll(const QString &pollId, const QString &oauthToken,
                            std::function<void()> successCallback,
                            std::function<void(const QString &)> failureCallback);
    static void addChannelBlockedTerm(
        const QString &channelId, const QString &phrase,
        const QString &oauthToken,
        std::function<void(GqlAddBlockedTermResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelBlockedTerms(
        const QString &channelId, const QString &oauthToken,
        std::function<void(QVector<GqlBlockedTerm>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelSelfData(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(GqlChannelSelfData)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void deleteChannelBlockedTerm(
        const QString &channelId, const QString &termId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void grantVIP(const QString &channelId, const QString &targetLogin,
                         const QString &oauthToken,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback);
    static void revokeVIP(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback);
    static void modUser(const QString &channelId, const QString &targetLogin,
                        const QString &oauthToken,
                        std::function<void()> successCallback,
                        std::function<void(const QString &)> failureCallback);
    static void unmodUser(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback);
    static void assignLeadModerator(
        const QString &channelId, const QString &targetUserId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unassignLeadModerator(
        const QString &channelId, const QString &targetUserId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void addEditorUser(
        const QString &channelId, const QString &targetLogin,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void removeEditorUser(
        const QString &channelId, const QString &targetLogin,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getRaidChannelIDs(
        const QString &sourceLogin, const QString &targetLogin,
        const QString &oauthToken,
        std::function<void(RaidChannelIDs)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void createRaid(const QString &sourceId, const QString &targetId,
                           const QString &oauthToken,
                           std::function<void(const QString &)> successCallback,
                           std::function<void(const QString &)> failureCallback);
    static void sendRaidNow(const QString &sourceId, const QString &oauthToken,
                            std::function<void()> successCallback,
                            std::function<void(const QString &)> failureCallback);
    static void cancelRaidGql(const QString &sourceId, const QString &oauthToken,
                              std::function<void()> successCallback,
                              std::function<void(const QString &)> failureCallback);
    static void voteInPoll(const QString &pollId, const QString &choiceId,
                           const QString &userId, int extraVotes,
                           std::optional<int> pointsPerVote,
                           const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback);
    static void getChannelPoints(const QString &channelLogin,
                                 const QString &oauthToken,
                                 std::function<void(qint64)> successCallback,
                                 std::function<void(const QString &)> failureCallback);
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    static void getRewardRequestOverview(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(GqlRewardRequestOverview)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getRewardRequests(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &rewardId, const QString &cursor,
        const QString &oauthToken,
        std::function<void(GqlRewardRequestPage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void updateRewardRequests(
        const QString &channelId, const QStringList &redemptionIds,
        GqlRewardRequestResolution resolution, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void updateAllRewardRequests(
        const QString &channelId, const QString &rewardId,
        GqlRewardRequestResolution resolution, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void sendGigantifiedChatEmote(
        const QString &channelId, const QString &emoteId,
        const QString &message, int bitsCost, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getAvailableGigantifyEmotes(
        const QString &channelId, const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelPointRewards(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRewards)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void redeemCustomReward(
        const QString &channelId, const GqlChannelPointReward &reward,
        const QString &textInput, const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void sendHighlightedChatMessage(
        const QString &channelId, int cost, const QString &message,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void sendSubOnlyBypassMessage(
        const QString &channelId, int cost, const QString &message,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unlockRandomSubscriberEmote(
        const QString &channelId, int cost, const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unlockChosenSubscriberEmote(
        const QString &channelId, const QString &emoteId, int cost,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unlockModifiedSubscriberEmote(
        const QString &channelId, const QString &modifiedEmoteId, int cost,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getAvailableChannelPointEmotes(
        const QString &channelId, const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModifiableChannelPointEmotes(
        const QString &channelLogin, const QString &expectedChannelId,
        const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelPointEmoteModifiers(
        const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmoteModifier>)> successCallback,
        std::function<void(const QString &)> failureCallback);
#endif
    static void getChatWarningStatus(
        const QString &channelId, const QString &targetUserId,
        const QString &oauthToken,
        std::function<void(std::optional<TwitchChannel::ChatWarning>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);
    static void acknowledgeChatWarning(
        const QString &channelId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void validateCustomAuthToken(
        const QString &oauthToken,
        std::function<void(CustomAuthValidationResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModeratedChannels(
        const QString &oauthToken,
        std::function<void(QVector<GqlModeratedChannel>)> successCallback,
        std::function<void(const QString &)> failureCallback);
};

}  // namespace chatterino
