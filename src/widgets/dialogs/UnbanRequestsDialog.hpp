#pragma once

#include "ForwardDecl.hpp"
#include "providers/twitch/api/TwitchGql.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;

namespace chatterino {

class UnbanRequestListModel;

class UnbanRequestsDialog final : public QDialog
{
public:
    UnbanRequestsDialog(QString channelId, QString channelLogin,
                        std::weak_ptr<Channel> outputChannel,
                        QWidget *parent = nullptr);

    static void showDialog(const QString &channelId,
                           const QString &channelLogin,
                           std::weak_ptr<Channel> outputChannel,
                           QWidget *parent = nullptr);

private:
    struct RelationshipContext {
        QString followedAt;
        QString subTier;
        QString giftSource;
        int totalSubMonths = 0;
        bool isSubHidden = false;
        bool isSubbed = false;
        bool isGifted = false;
        bool giftIsAnonymous = false;
    };

    struct DetailCache {
        GqlUnbanRequestUserContext userContext;
        RelationshipContext relationship;
        QPixmap avatar;
        QString avatarUrl;
        QString contextError;
        QString chatError;
        QString commentsError;
        QVector<GqlUsercardMessage> chatMessages;
        QSet<QString> seenChatMessageIds;
        QString chatCursor;
        QVector<GqlModeratorComment> comments;
        QSet<QString> seenCommentIds;
        QString commentsCursor;
        bool contextLoaded = false;
        bool contextLoading = false;
        bool relationshipLoaded = false;
        bool relationshipLoading = false;
        bool relationshipFailed = false;
        bool avatarLoading = false;
        bool chatLoaded = false;
        bool chatLoading = false;
        bool chatHasNextPage = false;
        bool commentsLoaded = false;
        bool commentsLoading = false;
        bool commentsHasNextPage = false;
    };

    void loadFirstPage();
    void loadNextPage();
    void selectCurrentRequest();
    void showRequest(const GqlUnbanRequest &request);
    void clearRequest();
    DetailCache &cacheFor(const QString &requestId);
    DetailCache *currentCache();
    void touchCache(const QString &requestId);
    void prefetchNextRequest(int currentRow);
    void applyUserContext(const GqlUnbanRequestUserContext &context);
    void loadUserContext(const GqlUnbanRequest &request);
    void loadRelationshipContext(const GqlUnbanRequest &request);
    void updateViewerHistoryLine();
    void loadAvatar(const QString &requestId, const QString &url,
                    const QString &fallbackName);
    void loadChatHistory(bool older = false);
    void loadChatHistoryFor(const GqlUnbanRequest &request, bool older = false);
    void rebuildChatHistory(bool preserveScrollPosition = false);
    void loadComments(bool older = false);
    void rebuildComments();
    void resolveCurrentRequest(bool approve);
    void updateQueueSummary();
    void updateActionState();
    void refreshDetailStatus();
    void setQueueStatus(const QString &text, bool error = false);
    void setDetailStatus(const QString &text, bool error = false);
    void applyTheme();
    QString moderationToken(const QString &action);
    void publishResult(const QString &text) const;

    QString channelId_;
    QString channelLogin_;
    std::weak_ptr<Channel> outputChannel_;
    QString nextCursor_;
    QString selectedRequestId_;
    QString actionError_;
    QString preferredRequestId_;
    QSet<QString> seenRequestIds_;
    QHash<QString, DetailCache> detailCache_;
    QHash<QString, QString> moderatorNoteDrafts_;
    QStringList detailCacheOrder_;
    int totalRequests_ = 0;
    int queueGeneration_ = 0;
    quint64 authGeneration_ = 0;
    QString authToken_;
    bool queueLoading_ = false;
    bool hasNextPage_ = false;
    bool rebuildingChatHistory_ = false;
    bool actionInFlight_ = false;
    bool queueStatusIsError_ = false;
    bool detailStatusIsError_ = false;

    QLabel *countLabel_{};
    QLabel *queueStatusLabel_{};
    QComboBox *sort_{};
    QPushButton *refreshButton_{};
    QListView *requestList_{};
    UnbanRequestListModel *requestModel_{};
    QPushButton *loadMoreRequests_{};
    QStackedWidget *detailStack_{};
    QLabel *avatar_{};
    QLabel *displayName_{};
    QLabel *login_{};
    QLabel *accountCreated_{};
    QLabel *banStatus_{};
    QLabel *historyCounts_{};
    QLabel *requestDate_{};
    QLabel *requestText_{};
    QLabel *detailStatus_{};
    QLabel *noteCount_{};
    QTabWidget *detailTabs_{};
    QTableWidget *chatHistory_{};
    QTableWidget *commentsTable_{};
    QPushButton *loadOlderComments_{};
    QPlainTextEdit *moderatorNote_{};
    QPushButton *denyButton_{};
    QPushButton *approveButton_{};
    pajlada::Signals::SignalHolder themeConnections_;
    pajlada::Signals::SignalHolder authConnections_;

    static std::vector<QPointer<UnbanRequestsDialog>> activeDialogs_;
};

}
