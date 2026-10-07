#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/TooltipWidget.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/connection.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QElapsedTimer>
#include <QHash>
#include <QMenu>
#include <QPoint>
#include <QSet>

#include <memory>
#include <vector>

class QSpacerItem;

namespace chatterino {

class SvgButton;
class DrawnButton;
class LabelButton;
class Label;
class Split;
class AutoModReviewBar;
struct Message;
struct TwitchUser;

class SplitHeader final : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitHeader(Split *split);
    ~SplitHeader() override;

    void setAddButtonVisible(bool value);
    void setAutoModReviewBar(AutoModReviewBar *bar);

    void updateChannelText();
    void updateIcons();

    void updateRoomModes();

protected:
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent *event) override;
#else
    void enterEvent(QEvent *event) override;
#endif
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    TooltipWidget *ensureTooltipWidget();
    void hideTooltip();
    void releaseTooltip();
    void updateAutoModLayout();
    void resetSharedChatFilter();
    void recordSharedChatSource(const std::shared_ptr<const Message> &message);
    void updateSharedChatButton();
    void applySharedChatFilter();
    std::unique_ptr<QMenu> createSharedChatMenu();
    void initializeLayout();
    std::unique_ptr<QMenu> createMainMenu();
    std::unique_ptr<QMenu> createChatModeMenu();

    void resetThumbnail();

    void handleChannelChanged();
    void toggleFollow();

    Split *const split_{};
    QString tooltipText_{};
    TooltipWidget *tooltipWidget_{};
    bool isLive_{false};
    QString thumbnail_;
    QString thumbnailSource_;
    QElapsedTimer lastThumbnail_;
    std::chrono::steady_clock::time_point lastReloadedChannelEmotes_;
    std::chrono::steady_clock::time_point lastReloadedSubEmotes_;

    DrawnButton *dropdownButton_{};
    Label *titleLabel_{};
    QSpacerItem *autoModHeaderBalance_{};

    LabelButton *modeButton_{};
    SvgButton *recordingButton_{};
    LabelButton *sharedChatButton_{};
    QAction *modeActionSetEmote{};
    QAction *modeActionSetSub{};
    QAction *modeActionSetSlow{};
    QAction *modeActionSetR9k{};
    QAction *modeActionSetFollowers{};

    SvgButton *followButton_{};
    SvgButton *manageChannelButton_{};
    SvgButton *moderationButton_{};
    SvgButton *chattersButton_{};
    DrawnButton *addButton_{};
    AutoModReviewBar *autoModReviewBar_{};
    QHash<QString, std::shared_ptr<TwitchUser>> sharedChatSources_;
    QSet<QString> hiddenSharedChatSources_;
    bool sharedChatFilterActive_ = false;

    QPoint dragStart_{};
    bool dragging_{false};
    bool doubleClicked_{false};
    bool menuVisible_{false};

    pajlada::Signals::SignalHolder managedConnections_;
    pajlada::Signals::SignalHolder channelConnections_;

public Q_SLOTS:
    void reloadChannelEmotes();
    void reloadSubscriberEmotes();
    void reconnect();
};

}
