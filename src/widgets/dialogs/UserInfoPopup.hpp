// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Paths.hpp"
#include "providers/twitch/TwitchNameHistory.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QColor>
#include <QDateTime>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

class QCheckBox;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QMenu;
class QMovie;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
struct Message;
struct Command;
using MessagePtr = std::shared_ptr<const Message>;
class Label;
class MarkdownLabel;
class EditUserNotesDialog;
class ChannelView;
class Split;
struct HelixUser;
struct IvrUserProfile;
struct YouTubeAuthor;
struct TikTokAuthor;
enum class MessagePlatform : uint8_t;
class LabelButton;
class PixmapButton;
class DrawnButton;
class LiveIndicator;
class ModeratorCommentsView;
class UserLogsView;
class UserRolesView;

class UserInfoPopup final : public DraggablePopup
{
    Q_OBJECT

public:
    /**
     * @param closeAutomatically Decides whether the popup should close when it loses focus
     * @param split Will be used as the popup's parent. Must not be null
     */
    UserInfoPopup(bool closeAutomatically, Split *split);

    void setData(const QString &name, const ChannelPtr &channel);
    void setData(const QString &name, const ChannelPtr &contextChannel,
                 const ChannelPtr &openingChannel);
    void setYouTubeData(const MessagePtr &message,
                        const YouTubeAuthor &author,
                        const ChannelPtr &contextChannel,
                        const ChannelPtr &openingChannel);
    void setTikTokData(const TikTokAuthor &author,
                       const ChannelPtr &contextChannel,
                       const ChannelPtr &openingChannel);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void windowDeactivationEvent() override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void addShortcuts() override;

    void registerMnemonicButton(LabelButton *button, int key,
                                std::function<void()> action);

    void installEvents();
    void resetTargetState();
    void updateIdentityLayout();
    void clearSevenTVAvatar();
    void refreshAvatarVisibility();
    void refreshCustomActions();
    void runCustomAction(const Command &command);
    BaseWidget *customActions_ = nullptr;
    std::function<void()> actionForCurrentTarget(std::function<void()> action);
    void updateUserData();
    void updateLatestMessages();
    void updateModeratorCommentsAvailability();
    void updateUserLogsContext();
    void updateUserRolesContext();
    bool usercardActionAvailable() const;
    void openPlatformUsercard();
    bool moderatorCommentsActionAvailable() const;
    bool userLogsActionAvailable() const;
    bool userRolesActionAvailable() const;
    void toggleModeratorComments();
    void toggleUserLogs();
    void toggleUserRoles();
    void openSevenTVUser();
    void refreshUsercardActionPlacements();
    bool crossActionAvailable() const;
    void runCrossAction(const QString &command);
    void appendPlacedUsercardActions(QMenu *menu);
    enum class ActivityPage {
        Messages,
        Comments,
        Logs,
        Roles,
    };
    void showActivityPage(ActivityPage page);
    void updateUsercardMessagesVisibility();
    void resetUsercardMessageLoader();
    void updateLoadMoreMessagesButton();
    bool canLoadMoreUsercardMessages() const;
    void maybeStartUsercardMessageAutoLoad();
    void requestMoreUsercardMessages(bool enableLazyLoadOnSuccess);
    void maybeLoadMoreUsercardMessagesFromScroll();
    void fetchMoreUsercardMessages(int emptyPageSkipsLeft,
                                   bool enableLazyLoadOnSuccess);
    void updateNotes();
    void refreshSevenTVUserButtonVisibility();
    void resetNameHistory();
    bool applyCachedNameHistory();
    void updateNameHistoryButton();
    void showNameHistoryMenu();
    void openNameHistoryMenu(const QString &statusText = {});
    void requestNameHistory();
    void updateUsercardStatusIcons();
    void resetUsercardInfoRows();
    void loadTwitchFollowage(bool onlyIfChanged = false);
    void setFollowage(const QDateTime &followedAt);
    void updateUsercardColor();
    void applyIvrUserProfile(const IvrUserProfile &profile);
    void updateIdentityStripVisibility();
    void refreshIdentityBadges();
    void refreshIdentityPaint();
    void refreshFollowButton();
    void updateFollowButtonAppearance();
    void runFollowAction();
    void openUserChannelInNewTab();
    std::unique_ptr<QMenu> createUserActionsMenu();
    void setTargetBlocked(bool blocked);
    void setTargetLocallyHidden(bool hidden);
    void setTargetIgnoringHighlights(bool ignored);
    void refreshLocalUserActions();
    QString highlightIdentity() const;
    QString notesUserKey() const;
    MessagePlatform targetMessagePlatform() const;
    void openUserNotes();
    bool isTargetCurrentUser() const;

    void loadAvatar(const QString &userID, const QString &pictureURL,
                    bool isKick, bool allowSevenTVLookup = true);

    void loadSevenTVAvatar(const QString &userID, bool isKick,
                           bool allowAvatarDownload = true);
    void setSevenTVAvatar(const QString &filename, const QByteArray &format);

    void saveCacheAvatar(const QByteArray &avatar,
                         const QString &filename) const;

    void updateAvatarUrl();

    void updateKickUserData();
    void onKickProfilePictureClick(Qt::MouseButton button);
    QString showProfilePictureContextMenu();
    bool canShowRoleManagementMenu() const;
    void showRoleManagementMenu(QWidget *anchor = nullptr);
    void runRoleManagementCommand(const QString &command,
                                  const QString &actionText);

    QStringView platformName() const;

    void appendCommonProfileActions(QMenu *menu);
    void refreshTargetModerationStatus();
    bool updateTargetModerationStatusFromMessage(const MessagePtr &message);
    bool shouldShowModerationActions() const;

    enum class UsercardModerationAction { Ban, Unban, Timeout };

    struct UsercardModerationRequest {
        UsercardModerationAction action{};
        int durationSeconds = -1;
        QString reason;
        bool promptForReason = false;
    };

    void executeUsercardModerationAction(
        const UsercardModerationRequest &request);
    void showUsercardModerationReasonPopup(
        const UsercardModerationRequest &request);

    bool isMod_{};
    QDateTime targetModerationTime_;
    bool isBroadcaster_{};
    bool targetBlocked_{};
    bool targetLocallyHidden_{};
    bool targetIgnoringHighlights_{};
    bool targetIgnoreMatchedByRegex_{};
    bool canChangeTargetBlock_{};
    bool canChangeTargetHighlightIgnore_{};
    bool canEditTargetNotes_{};
    bool followStatusKnown_{};
    bool following_{};
    bool followStatusRequestInFlight_{};
    bool followMutationInFlight_{};
    QString targetBlockStateAccountName_;
    QColor identityUserColor_;
    QColor messageUserColor_;
    QColor apiUserColor_;
    bool apiUserColorLookupFinished_ = false;

    Split *split_;

    QString userName_;
    QString userId_;
    QString platformHandle_;
    QString platformRoles_;
    MessagePtr identityMessageFallback_;
    uint64_t userDataRequestGeneration_ = 0;
    uint64_t followageRequestGeneration_ = 0;
    QStringList followageRequestKey_;
    bool twitchUserLookupFinished_ = false;
    QString avatarUrl_;
    QString helixAvatarUrl_;
    QString seventvAvatarUrl_;
    QString seventvUserID_;
    int seventvUserRequestGeneration_ = 0;
    bool seventvUserLookupInFlight_ = false;
    bool seventvUserLookupFinished_ = false;
    QString nameHistoryLogin_;
    std::vector<TwitchNameHistoryEntry> nameHistoryEntries_;
    QPointer<QMenu> nameHistoryMenu_;
    uint64_t nameHistoryRequestGeneration_ = 0;
    bool nameHistoryLoading_ = false;
    bool nameHistoryLoaded_ = false;
    ChannelPtr usercardMessagesChannel_;
    QString usercardMessagesCursor_;
    QString usercardMessagesError_;
    uint64_t usercardMessagesRequestGeneration_ = 0;
    bool usercardMessagesLoading_ = false;
    bool usercardMessagesHasNextPage_ = true;
    bool usercardMessagesLazyLoadEnabled_ = false;
    bool usercardMessagesAuthRequired_ = false;

    QString kickUserSlug_;
    QPointer<QWidget> moderationReasonPopup_;

    // The channel the popup was opened from (e.g. /mentions or #forsen). Can be a special channel.
    ChannelPtr channel_;

    // The channel the messages are rendered from (e.g. #forsen). Can be a special channel, but will try to not be where possible.
    ChannelPtr underlyingChannel_;

    pajlada::Signals::NoArgSignal userStateChanged_;

    std::unique_ptr<pajlada::Signals::ScopedConnection> refreshConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        twitchUserStateConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        twitchTargetRoleConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        twitchRoomIdConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        youtubeModerationStateConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        userDataUpdatedConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        usercardScrollConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        usercardMessageReplacementConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        usercardMessagesClearedConnection_;
    pajlada::Signals::ScopedConnection currentUserChangedConnection_;
    pajlada::Signals::ScopedConnection kickCurrentUserChangedConnection_;
    pajlada::Signals::ScopedConnection youtubeCurrentUserChangedConnection_;
    pajlada::Signals::ScopedConnection tiktokCurrentUserChangedConnection_;

    // If we should close the dialog automatically if the user clicks out
    // Set based on the "Automatically close usercard when it loses focus" setting
    // Pinned status is tracked in DraggablePopup::isPinned_.
    const bool closeAutomatically_;

    class TimeoutWidget;
    struct {
        PixmapButton *avatarButton = nullptr;
        PixmapButton *localizedNameCopyButton = nullptr;
        QHBoxLayout *identityHeader = nullptr;
        QHBoxLayout *handleLayout = nullptr;
        QWidget *handleRow = nullptr;
        int localizedNameIndex = 0;

        Label *nameLabel = nullptr;
        LabelButton *nameHistoryButton = nullptr;
        Label *localizedNameLabel = nullptr;
        Label *bioLabel = nullptr;
        Label *pronounsLabel = nullptr;
        Label *followerCountLabel = nullptr;
        Label *createdDateLabel = nullptr;
        Label *userIDLabel = nullptr;
        QLabel *bannedAvatarLabel = nullptr;
        QWidget *followageRow = nullptr;
        QLabel *followageIcon = nullptr;
        Label *followageLabel = nullptr;
        QWidget *subageRow = nullptr;
        QLabel *subageIcon = nullptr;
        Label *subageLabel = nullptr;
        Label *chatterCountLabel = nullptr;
        Label *lastLiveLabel = nullptr;
        QWidget *userColorRow = nullptr;
        QWidget *userColorSwatch = nullptr;
        Label *userColorLabel = nullptr;
        Label *statusLabel = nullptr;

        QWidget *identityBadgeRow = nullptr;
        QWidget *identityBadges = nullptr;
        QWidget *identityPaintRow = nullptr;
        QWidget *identityPaint = nullptr;
        DrawnButton *userActions = nullptr;

        LiveIndicator *liveIndicator = nullptr;

        MarkdownLabel *notesPreview = nullptr;

        Label *noMessagesLabel = nullptr;
        ChannelView *latestMessages = nullptr;
        LabelButton *loadMoreMessages = nullptr;
        QStackedWidget *activityStack = nullptr;
        QWidget *messagesPage = nullptr;
        ModeratorCommentsView *commentsView = nullptr;
        UserLogsView *logsView = nullptr;
        UserRolesView *rolesView = nullptr;

        LabelButton *usercardLabel = nullptr;
        LabelButton *commentsLabel = nullptr;
        LabelButton *userlogsLabel = nullptr;
        LabelButton *sevenTVUserLabel = nullptr;
        LabelButton *rolesLabel = nullptr;
        LabelButton *notesActionLabel = nullptr;
        LabelButton *blockActionLabel = nullptr;
        LabelButton *hideActionLabel = nullptr;
        LabelButton *ignoreHighlightsActionLabel = nullptr;
        LabelButton *crossBanActionLabel = nullptr;
        LabelButton *crossUnbanActionLabel = nullptr;
        QPushButton *followButton = nullptr;
        LabelButton *switchAvatars = nullptr;

        TimeoutWidget *timeoutWidget = nullptr;
    } ui_;

    QMovie *seventvAvatar_ = nullptr;
    bool isTwitchAvatarShown_ = true;
    QPixmap avatarPixmap_;
    QPointer<EditUserNotesDialog> editUserNotesDialog_;
    QString editUserNotesTargetId_;

    bool isKick_ = false;
    bool isYouTube_ = false;
    bool isTikTok_ = false;
    bool youtubeTargetIsOwner_ = false;
    bool youtubeTargetIsModerator_ = false;
    uint64_t kickUserID_ = 0;

    std::map<int, std::pair<std::function<void()>, std::function<bool()>>>
        mnemonicActions_;

    class TimeoutWidget : public BaseWidget
    {
    public:
        TimeoutWidget();

        pajlada::Signals::Signal<UsercardModerationRequest> buttonClicked;

        void setMinTimeout(int minSecs);
        void setUnbanEnabled(bool enabled);
        void setReasonPromptsEnabled(bool enabled);
        void refreshActionTooltips();

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        struct ActionWidget {
            QWidget *widget = nullptr;
            QString target;
            bool supportsReasonPrompt = false;
        };

        void rebuildActions();

        std::vector<std::pair<QWidget *, int>> timeoutButtons;
        std::vector<ActionWidget> actionWidgets_;
        QWidget *unbanButton_ = nullptr;
        QWidget *content_ = nullptr;
        QHBoxLayout *rootLayout_ = nullptr;
        int minTimeoutSeconds_ = 0;
        bool unbanEnabled_ = true;
        bool reasonPromptsEnabled_ = true;
    };
};

}  // namespace chatterino
