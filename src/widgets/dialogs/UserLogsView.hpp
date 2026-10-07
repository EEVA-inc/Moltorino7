#pragma once

#include "providers/twitch/TwitchUserLogs.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>

class QComboBox;
class QLineEdit;
class QListView;
class QPushButton;
class QTimer;
class QToolButton;
class QUrl;

namespace chatterino {

class Label;
class UserLogsModel;

class UserLogsView final : public QWidget
{
public:
    explicit UserLogsView(QWidget *parent = nullptr);

    void setContext(const QString &channel, const QString &user);
    void activate();
    void deactivate();
    void refreshStyle(float scale);

private:
    enum class Mode {
        Period,
        AllHistory,
    };
    enum class RetryAction {
        None,
        Periods,
        Period,
        Search,
    };

    void reset();
    void loadPeriods();
    void loadSelectedPeriod();
    void searchAllHistory();
    void loadSearchPage(QString query, qsizetype offset);
    void returnToPeriod();
    void retry();
    void selectPeriod(int index);
    void rebuildVisibleMessages(bool preserveScroll = false,
                                bool clearError = true);
    void refreshTimestampStyle();
    void refreshPeriodWidths();
    void updateControls();
    void updateStatus();
    void openOnWeb() const;
    void resolveMessageLink(const twitch_user_logs::Message &message,
                            std::function<void(QUrl)> callback);
    void copySelectedMessage(bool includeTimestamp = false) const;
    void showMessageMenu(const QPoint &position);

    QString channel_;
    QString user_;
    QString allHistoryQuery_;
    QString error_;
    QVector<twitch_user_logs::Period> periods_;
    twitch_user_logs::Page periodPage_;
    twitch_user_logs::Page searchPage_;
    QVector<const twitch_user_logs::Message *> visibleMessages_;
    uint64_t generation_{};
    qsizetype searchOffset_{};
    qsizetype retrySearchOffset_{};
    int selectedPeriod_ = -1;
    Mode mode_ = Mode::Period;
    RetryAction retryAction_ = RetryAction::None;
    bool activated_{};
    bool periodsLoaded_{};
    bool periodPageLoaded_{};
    bool loadingPeriods_{};
    bool loadingPage_{};
    bool searchingAllHistory_{};
    QString retrySearchQuery_;

    QToolButton *olderButton_{};
    QToolButton *newerButton_{};
    QComboBox *periodBox_{};
    Label *statusLabel_{};
    QPushButton *webButton_{};
    QLineEdit *searchInput_{};
    QPushButton *allHistoryButton_{};
    QPushButton *retryButton_{};
    QListView *list_{};
    UserLogsModel *model_{};
    QTimer *filterTimer_{};
    bool showDate_ = true;
    bool newestAtBottom_ = true;
    QString dateStyle_;
    QString timeStyle_;
    pajlada::Signals::SignalHolder settingConnections_;
};

}
