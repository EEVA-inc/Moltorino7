#pragma once

#include "providers/twitch/TwitchRoles.hpp"

#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>

class QComboBox;
class QListView;
class QPushButton;
class QTabBar;

namespace chatterino {

class Label;
class UserRolesModel;

class UserRolesView final : public QWidget
{
public:
    explicit UserRolesView(QWidget *parent = nullptr);

    void setContext(const QString &userID, const QString &login,
                    bool userLookupFinished);
    void setRoleManagementAvailable(bool available);
    void setRoleManagementCallback(std::function<void(QWidget *)> callback);
    void activate();
    void deactivate();
    void refreshStyle(float scale);

private:
    void resetPerspective();
    void loadSummary();
    void rebuildRoleTabs();
    void selectRole(twitch_roles::Role role);
    void loadFirstPage();
    void loadMore();
    void requestPage(const QString &cursor);
    void updateControls();
    void updateStatus();
    void openOnWeb() const;
    void openEntry(int row) const;
    void showEntryMenu(const QPoint &position);

    QString userID_;
    QString login_;
    bool userLookupFinished_ = false;
    QString error_;
    twitch_roles::Perspective perspective_ =
        twitch_roles::Perspective::User;
    twitch_roles::Role selectedRole_ = twitch_roles::Role::Moderator;
    twitch_roles::Summary summary_;
    QVector<twitch_roles::Entry> entries_;
    QString nextCursor_;
    QString retryCursor_;
    qint64 total_{};
    uint64_t generation_{};
    bool activated_{};
    bool summaryLoaded_{};
    bool loadingSummary_{};
    bool loadingPage_{};

    QComboBox *perspectiveBox_{};
    QPushButton *manageButton_{};
    QPushButton *webButton_{};
    QTabBar *roleTabs_{};
    QListView *list_{};
    UserRolesModel *model_{};
    Label *statusLabel_{};
    QPushButton *retryButton_{};
    QPushButton *loadMoreButton_{};
    std::function<void(QWidget *)> roleManagementCallback_;
};

}
