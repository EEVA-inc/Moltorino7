#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "ForwardDecl.hpp"
#    include "providers/twitch/api/TwitchGql.hpp"

#    include <pajlada/signals/signalholder.hpp>
#    include <QDialog>
#    include <QPointer>
#    include <QSet>
#    include <QVector>

#    include <memory>
#    include <vector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTableView;

namespace chatterino {

class RewardRequestTableModel;

class RewardRequestQueueDialog final : public QDialog
{
public:
    RewardRequestQueueDialog(QString channelId, QString channelLogin,
                             std::weak_ptr<Channel> outputChannel,
                             QWidget *parent = nullptr);

    static void showDialog(const QString &channelId,
                           const QString &channelLogin,
                           std::weak_ptr<Channel> outputChannel,
                           QWidget *parent = nullptr);

private:
    void reloadOverview(const QString &notice = {}, bool noticeIsError = false);
    void rebuildRewardList(const QString &preferredRewardId = {});
    void selectReward(QListWidgetItem *item);
    void loadFirstPage();
    void loadNextPage();
    void updateSummary();
    void updateSelectionPreview();
    void updateActionButtons();
    void resolveRequests(GqlRewardRequestResolution resolution);
    void resolveSelectedBatches(
        QStringList remaining, GqlRewardRequestResolution resolution,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &, int)> failureCallback,
        quint64 authGeneration, int completedCount = 0);
    void setStatus(const QString &text, bool error = false);
    void applyTheme();
    QString moderationToken(const QString &action);
    void publishResult(const QString &text) const;

    QString channelId_;
    QString channelLogin_;
    std::weak_ptr<Channel> outputChannel_;
    GqlRewardRequestOverview overview_;
    QString selectedRewardId_;
    QString nextCursor_;
    QSet<QString> seenRequestIds_;
    int requestGeneration_ = 0;
    quint64 authGeneration_ = 0;
    QString authToken_;
    bool overviewLoading_ = false;
    bool pageLoading_ = false;
    bool pageLoadFailed_ = false;
    bool hasNextPage_ = false;
    bool actionInFlight_ = false;
    bool selectFirstRequestAfterReload_ = false;
    QString refreshNotice_;
    bool refreshNoticeIsError_ = false;
    bool statusIsError_ = false;

    QLabel *countLabel_{};
    QLabel *loadedLabel_{};
    QLabel *statusLabel_{};
    QLabel *requestTitle_{};
    QLabel *selectionTitle_{};
    QLabel *selectionMeta_{};
    QLabel *selectionMessage_{};
    QListWidget *rewards_{};
    QTableView *requests_{};
    RewardRequestTableModel *requestModel_{};
    QPushButton *refreshButton_{};
    QPushButton *loadMoreButton_{};
    QPushButton *completeButton_{};
    QPushButton *rejectButton_{};
    pajlada::Signals::SignalHolder themeConnections_;
    pajlada::Signals::SignalHolder authConnections_;

    static std::vector<QPointer<RewardRequestQueueDialog>> activeDialogs_;
};

}

#endif
