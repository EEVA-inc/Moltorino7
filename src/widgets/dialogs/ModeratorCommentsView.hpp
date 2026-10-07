#pragma once

#include "providers/twitch/api/TwitchGql.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;

namespace chatterino {

class ModeratorCommentsModel;

class ModeratorCommentsView final : public QWidget
{
public:
    explicit ModeratorCommentsView(QWidget *parent = nullptr);

    void setContext(const QString &channelId, const QString &channelLogin,
                    const QString &targetId, const QString &targetLogin,
                    bool hasModerationAccess, bool targetLookupFinished);
    void activate();
    void deactivate();
    void authenticationChanged();
    void refreshStyle(float scale);

private:
    void reset();
    void retry();
    void loadInitial();
    void loadOlder();
    void loadLocalPage(bool initial);
    void loadSharedPage(bool initial);
    void loadSharingSetting();
    void appendComments(QVector<GqlModeratorComment> comments,
                        bool sharedSource);
    void rebuildList(bool preserveScroll = false);
    void updateState();
    void updateComposer();
    void submitComment();
    void deleteComment(const QString &commentId);
    void showCommentMenu(const QPoint &position);

    QString channelId_;
    QString channelLogin_;
    QString targetId_;
    QString targetLogin_;
    bool hasModerationAccess_ = false;
    bool targetLookupFinished_ = false;
    QString authToken_;
    QString localCursor_;
    QString sharedCursor_;
    QString localLoadError_;
    QString sharedLoadError_;
    QString error_;
    QString errorMessage_;
    QVector<GqlModeratorComment> localComments_;
    QVector<GqlModeratorComment> sharedComments_;
    QVector<GqlModeratorComment> comments_;
    uint64_t generation_ = 0;
    bool activated_ = false;
    bool loaded_ = false;
    bool localLoading_ = false;
    bool sharedLoading_ = false;
    bool localHasNext_ = true;
    bool sharedHasNext_ = true;
    bool localLoadFailed_ = false;
    bool sharedLoadFailed_ = false;
    bool listTruncated_ = false;
    bool sharingSettingLoading_ = false;
    bool sharingDisabled_ = true;
    bool sharingUnavailable_ = false;
    bool submitting_ = false;
    bool deleting_ = false;

    pajlada::Signals::SignalHolder authConnections_;

    QLabel *statusLabel_{};
    QPushButton *stateButton_{};
    QListView *list_{};
    ModeratorCommentsModel *model_{};
    QLineEdit *commentInput_{};
    QLabel *characterCount_{};
    QPushButton *addButton_{};
    QCheckBox *shareComment_{};
};

}
