// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/NotebookEnums.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QHash>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace chatterino {

class Button;
class PixmapButton;
class Window;
class DrawnButton;
class NotebookTab;
class SplitContainer;
class Split;

using TabVisibilityFilter = std::function<bool(const NotebookTab *)>;

class Notebook : public BaseWidget
{
    Q_OBJECT

public:
    struct TabGroupSnapshot {
        QString id;
        QString name;
        QString colorMode = QStringLiteral("theme");
        QColor color;
        QString icon = QStringLiteral("folder");
        QString customIconPath;
        bool collapsed = false;
        bool muted = false;
        bool openMenuOnClick = false;
    };

    explicit Notebook(QWidget *parent);
    ~Notebook() override = default;

    NotebookTab *addPage(QWidget *page, QString title = QString(),
                         bool select = false);

    NotebookTab *addPageAt(QWidget *page, int position,
                           QString title = QString(), bool select = false);
    void removePage(QWidget *page);
    void duplicatePage(QWidget *page);
    void removeCurrentPage();

    int indexOf(QWidget *page) const;

    int visibleIndexOf(QWidget *page) const;

    int getVisibleTabCount() const;

    virtual void select(QWidget *page, bool focusPage = true);

    void selectIndex(int index, bool focusPage = true);

    void selectVisibleIndex(int index, bool focusPage = true);

    void selectNextTab(bool focusPage = true);

    void selectPreviousTab(bool focusPage = true);

    void selectLastTab(bool focusPage = true);

    int getPageCount() const;
    QWidget *getPageAt(int index) const;
    int getSelectedIndex() const;
    QWidget *getSelectedPage() const;

    QWidget *tabAt(QPoint point, int &index, int maxWidth = 2000000000);
    void rearrangePage(QWidget *page, int index);

    QString createTabGroup(QWidget *firstPage, QWidget *secondPage = nullptr);
    void groupPageWith(QWidget *page, QWidget *targetPage);
    void removePageFromTabGroup(QWidget *page);
    void ungroupTabGroup(const QString &groupId);
    QString pageTabGroup(QWidget *page) const;
    void populateTabGroupMenu(QMenu *menu, QWidget *page);
    void showTabGroupMenu(const QString &groupId, const QPoint &globalPos);
    void activateTabGroup(const QString &groupId, const QPoint &globalPos);
    void toggleTabGroup(const QString &groupId);
    void moveTabGroup(const QString &groupId, int index);
    void previewTabGroupDrop(QWidget *sourcePage, QWidget *targetPage);
    bool commitTabGroupDrop(QWidget *sourcePage);
    void cancelTabGroupDrop();
    void tabStatusChanged(NotebookTab *tab);

    std::vector<TabGroupSnapshot> tabGroups() const;
    void restoreTabGroup(const TabGroupSnapshot &group);
    void restorePageTabGroup(QWidget *page, const QString &groupId,
                             int ungroupedIndex = -1);
    void finishRestoringTabGroups();

    bool getAllowUserTabManagement() const;
    void setAllowUserTabManagement(bool value);

    bool getShowAddButton() const;
    void setShowAddButton(bool value);

    void setTabLocation(NotebookTabLocation location);

    bool isNotebookLayoutLocked() const;
    virtual void setLockNotebookLayout(bool value);

    virtual void addNotebookActionsToMenu(QMenu *menu,
                                          bool includeNewGroupAction = true);

    void refresh();

protected:
    bool getShowTabs() const;
    void setShowTabs(bool value);

    void scaleChangedEvent(float scale_) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;

    DrawnButton *addButton_;
    DrawnButton *groupButton_;

    template <typename T>
    T *addCustomButton(auto &&...args)
    {
        auto *btn = new T(std::forward<decltype(args)>(args)..., this);
        this->customButtons_.push_back(btn);

        return btn;
    }

    struct Item {
        NotebookTab *tab{};
        QWidget *page{};
        QWidget *selectedWidget{};
    };

    const QList<Item> items()
    {
        return this->items_;
    }

    void setTabVisibilityFilter(TabVisibilityFilter filter);

    bool shouldShowTab(const NotebookTab *tab) const;

    void performLayout(bool animate = false);

    void sortTabsAlphabetically();

private:
    struct TabGroup {
        QString id;
        QString name;
        QString colorMode = QStringLiteral("theme");
        QColor color;
        QString icon = QStringLiteral("folder");
        QString customIconPath;
        bool customIconDirty = false;
        bool collapsed = false;
        bool muted = false;
        bool openMenuOnClick = false;
        NotebookTab *header = nullptr;
    };

    struct LayoutContext {
        int left = 0;
        int right = 0;
        int bottom = 0;
        float scale = 0;
        int tabHeight = 0;
        int minimumTabAreaSpace = 0;
        int addButtonWidth = 0;
        int lineThickness = 0;
        int tabSpacer = 0;

        int buttonWidth = 0;
        int buttonHeight = 0;

        std::span<Item> items;
    };

    void performHorizontalLayout(const LayoutContext &ctx, bool animated);
    void performVerticalLayout(const LayoutContext &ctx, bool animated);

    void showTabVisibilityInfoPopup();

    void resizeAddButton();

    void updateGroupButtonVisibility();
    void openTabGroupEditor(const QString &groupId = {},
                            QWidget *initialPage = nullptr);
    void renameTabGroup(const QString &groupId);
    void showTabGroupQuickSwitcher(const QString &groupId,
                                   const QPoint &globalPos);
    QString tabGroupMemberLabel(const Item &item) const;
    bool setTabGroupCustomIcon(TabGroup &group, const QString &sourcePath);

    TabGroup *findTabGroup(const QString &id);
    const TabGroup *findTabGroup(const QString &id) const;
    QList<Item *> tabGroupMembers(const QString &id);
    QList<const Item *> tabGroupMembers(const QString &id) const;
    void assignPageToTabGroup(QWidget *page, const QString &groupId,
                              bool moveNextToGroup = true);
    void removeTabGroup(const QString &groupId, bool keepMembers);
    void normalizeTabGroups();
    void ensureUngroupedOrder();
    void restoreUngroupedOrder();
    void updateUngroupedOrderAfterMove(QWidget *page, QWidget *targetPage,
                                       bool afterTarget);
    void syncUngroupedOrderToItems();
    bool updateTabGroupHeader(TabGroup &group);
    bool updateTabGroupHeader(TabGroup &group, const QList<Item *> &members);
    QString tabGroupDisplayName(const TabGroup &group, int members) const;
    bool tabPassesVisibilityFilter(const NotebookTab *tab) const;

    bool containsPage(QWidget *page);
    std::optional<Item> findItem(QWidget *page);

    static bool containsChild(const QObject *obj, const QObject *child);
    NotebookTab *getTabFromPage(QWidget *page);

    size_t visibleButtonCount() const;

    QList<Item> items_;
    std::vector<std::unique_ptr<TabGroup>> tabGroups_;
    QPointer<QWidget> groupDropSource_;
    QPointer<QWidget> groupDropTarget_;
    QPointer<NotebookTab> groupDropVisual_;
    QMenu *menu_ = nullptr;
    QWidget *selectedPage_ = nullptr;

    std::vector<Button *> customButtons_;

    bool allowUserTabManagement_ = false;
    bool showTabs_ = true;
    bool showAddButton_ = false;
    int lineOffset_ = 20;
    bool lockNotebookLayout_ = false;

    pajlada::Signals::SignalHolder settingConnections_;
    bool refreshPaused_ = false;
    bool refreshRequested_ = false;

    NotebookTabLocation tabLocation_ = NotebookTabLocation::Top;

    QAction *lockNotebookLayoutAction_;
    QAction *newTabGroupAction_{};
    QAction *toggleTopMostAction_;

    TabVisibilityFilter tabVisibilityFilter_;
};

class SplitNotebook : public Notebook
{
public:
    SplitNotebook(Window *parent);

    SplitContainer *addPage(bool select = false);
    SplitContainer *getOrAddSelectedPage();

    SplitContainer *getSelectedPage();
    void select(QWidget *page, bool focusPage = true) override;
    void themeChangedEvent() override;

    void addNotebookActionsToMenu(QMenu *menu,
                                  bool includeNewGroupAction = true) override;

    void forEachSplit(const std::function<void(Split *)> &cb);

    void toggleTabVisibility();

    QAction *showAllTabsAction;
    QAction *onlyShowLiveTabsAction;
    QAction *hideAllTabsAction;

protected:
    void showEvent(QShowEvent *event) override;

private:
    QAction *sortTabsAlphabeticallyAction_;

    void addCustomButtons();

    pajlada::Signals::SignalHolder signalHolder_;

    PixmapButton *streamerModeIcon_{};
    void updateStreamerModeIcon();

    void setLockNotebookLayout(bool value) override;
};

}
