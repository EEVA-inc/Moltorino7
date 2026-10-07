// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/Notebook.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/QLogging.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/InitMoltorinoUpdateButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/dialogs/TabGroupDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <boost/foreach.hpp>
#include <QActionGroup>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLayout>
#include <QList>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace chatterino {

namespace {
constexpr auto MAX_TAB_GROUPS = 128;

QString normalizeTabGroupColorMode(QString mode)
{
    mode = mode.trimmed().toLower();
    return mode == "none" || mode == "custom" ? mode : QStringLiteral("theme");
}

QString normalizeTabGroupIcon(QString icon)
{
    icon = icon.trimmed().toLower();
    static const QSet<QString> icons = {
        QStringLiteral("folder"), QStringLiteral("star"),
        QStringLiteral("heart"),  QStringLiteral("bell"),
        QStringLiteral("shield"), QStringLiteral("none"),
        QStringLiteral("custom"),
    };
    return icons.contains(icon) ? icon : QStringLiteral("folder");
}

QString tabGroupIconPath(const QString &groupId)
{
    const auto safeName =
        QString::fromLatin1(QCryptographicHash::hash(groupId.toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex()
                                .left(24)) +
        QStringLiteral(".png");
    return QDir(getApp()->getPaths().settingsDirectory)
        .filePath(QStringLiteral("TabGroupIcons/%1").arg(safeName));
}

void removeTabGroupIcon(const QString &groupId)
{
    QFile::remove(tabGroupIconPath(groupId));
}

void showThemedWarning(QWidget *parent, const QString &title,
                       const QString &text)
{
    QPointer<QMessageBox> box = new QMessageBox(QMessageBox::Warning, title,
                                                text, QMessageBox::Ok, parent);
    const auto cleanup = qScopeGuard([box] {
        delete box;
    });
    installMoltorinoDialogTheme(box);
    box->exec();
}
}

Notebook::Notebook(QWidget *parent)
    : BaseWidget(parent)
    , addButton_(new DrawnButton(DrawnButton::Symbol::Plus,
                                 {
                                     .padding = 7,
                                     .thickness = 1,
                                 },
                                 this))
    , groupButton_(new DrawnButton(DrawnButton::Symbol::FolderPlus,
                                   {
                                       .padding = 6,
                                       .thickness = 1,
                                   },
                                   this))
{
    this->addButton_->setHidden(true);
    this->addButton_->enableDrops({"chatterino/split"});
    this->groupButton_->setHidden(true);
    this->groupButton_->setToolTip("Create tab group");
    QObject::connect(this->groupButton_, &Button::leftClicked, this, [this] {
        this->openTabGroupEditor({}, this->selectedPage_);
    });

    getSettings()->showTabGroupButton.connect(
        [this](bool, auto) {
            this->updateGroupButtonVisibility();
        },
        this->settingConnections_);

    QObject::connect(
        this->addButton_, &Button::dropEvent, this, [this](QDropEvent *event) {
            auto *draggedSplit = dynamic_cast<Split *>(event->source());
            if (!draggedSplit)
            {
                qCDebug(chatterinoWidget) << "Dropped something that wasn't a "
                                             "split onto a notebook button";
                return;
            }

            event->acceptProposedAction();

            auto *page = new SplitContainer(this);
            auto *tab = this->addPage(page);
            page->setTab(tab);

            draggedSplit->setParent(page);
            page->insertSplit(draggedSplit);
        });

    this->lockNotebookLayoutAction_ = new QAction("Lock Tab Layout", this);

    // Load lock notebook layout state from settings
    this->setLockNotebookLayout(getSettings()->lockNotebookLayout.getValue());

    this->lockNotebookLayoutAction_->setCheckable(true);
    this->lockNotebookLayoutAction_->setChecked(this->lockNotebookLayout_);

    this->newTabGroupAction_ = new QAction("Create tab group…", this);
    this->newTabGroupAction_->setEnabled(false);
    QObject::connect(this->newTabGroupAction_, &QAction::triggered, this,
                     [this] {
                         this->openTabGroupEditor({}, this->selectedPage_);
                     });

    // Update lockNotebookLayout_ value anytime the user changes the checkbox state
    QObject::connect(this->lockNotebookLayoutAction_, &QAction::triggered,
                     [this](bool value) {
                         this->setLockNotebookLayout(value);
                     });

    this->toggleTopMostAction_ = new QAction("Top most window", this);
    this->toggleTopMostAction_->setCheckable(true);
    auto *window = dynamic_cast<BaseWindow *>(this->window());
    if (window)
    {
        auto updateTopMost = [this, window] {
            this->toggleTopMostAction_->setChecked(window->isTopMost());
        };
        updateTopMost();
        QObject::connect(this->toggleTopMostAction_, &QAction::triggered,
                         window, [window] {
                             window->setTopMost(!window->isTopMost());
                         });
        QObject::connect(window, &BaseWindow::topMostChanged, this,
                         updateTopMost);
    }
    else
    {
        qCWarning(chatterinoApp)
            << "Notebook must be created within a BaseWindow";
    }

    // Manually resize the add button so the initial paint uses the correct
    // width when computing the maximum width occupied per column in vertical
    // tab rendering.
    this->resizeAddButton();
}

NotebookTab *Notebook::addPage(QWidget *page, QString title, bool select)
{
    return this->addPageAt(page, -1, std::move(title), select);
}

NotebookTab *Notebook::addPageAt(QWidget *page, int position, QString title,
                                 bool select)
{
    // Queue up save because: Tab added
    getApp()->getWindows()->queueSave();

    this->ensureUngroupedOrder();

    auto *tab = new NotebookTab(this);
    tab->page = page;

    tab->setCustomTitle(title);
    tab->setTabLocation(this->tabLocation_);

    Item item;
    item.page = page;
    item.tab = tab;

    auto ungroupedPosition = static_cast<int>(this->items_.size());
    if (position >= 0 && position < this->items_.size())
    {
        ungroupedPosition = this->items_[position].tab->ungroupedIndex();
    }
    for (auto &existing : this->items_)
    {
        if (existing.tab->ungroupedIndex() >= ungroupedPosition)
        {
            existing.tab->setUngroupedIndex(existing.tab->ungroupedIndex() + 1);
        }
    }
    tab->setUngroupedIndex(ungroupedPosition);

    if (position == -1)
    {
        this->items_.push_back(item);
    }
    else
    {
        this->items_.insert(position, item);
    }

    page->hide();
    page->setParent(this);

    if (select || this->items_.count() == 1)
    {
        this->select(page);
    }

    this->updateGroupButtonVisibility();

    this->performLayout();
    tab->setVisible(this->shouldShowTab(tab));
    return tab;
}

void Notebook::removePage(QWidget *page)
{
    // Queue up save because: Tab removed
    getApp()->getWindows()->queueSave();

    int removingIndex = this->indexOf(page);
    assert(removingIndex != -1);

    if (auto *recordings = getApp()->getChatRecordings())
    {
        recordings->stop(dynamic_cast<SplitContainer *>(page));
    }

    if (this->groupDropSource_ == page || this->groupDropTarget_ == page)
    {
        this->cancelTabGroupDrop();
    }

    if (this->selectedPage_ == page)
    {
        // The page that we are removing is currently selected. We need to determine
        // the best tab to select before we remove this one. We follow a strategy used
        // by many web browsers: select the next tab. If there is no next tab, select
        // the previous tab.
        int countVisible = this->getVisibleTabCount();
        int visibleIndex = this->visibleIndexOf(page);
        assert(visibleIndex != -1);  // A selected page should always be visible

        if (this->items_.count() == 1)
        {
            // Deleting only tab, select nothing
            this->select(nullptr);
        }
        else if (countVisible == 1)
        {
            // Closing the only visible tab, try to select any tab (even if not visible)
            int nextIndex = (removingIndex + 1) % this->items_.count();
            this->select(this->items_[nextIndex].page);
        }
        else if (visibleIndex == countVisible - 1)
        {
            // Closing last visible tab, select the previous visible tab
            this->selectPreviousTab();
        }
        else
        {
            // Otherwise, select the next visible tab
            this->selectNextTab();
        }
    }

    const auto groupId = this->items_[removingIndex].tab->groupId();
    const auto ungroupedIndex =
        this->items_[removingIndex].tab->ungroupedIndex();

    // Remove page and delete resources
    this->items_[removingIndex].page->deleteLater();
    this->items_[removingIndex].tab->deleteLater();
    this->items_.removeAt(removingIndex);
    for (auto &item : this->items_)
    {
        if (item.tab->ungroupedIndex() > ungroupedIndex)
        {
            item.tab->setUngroupedIndex(item.tab->ungroupedIndex() - 1);
        }
    }

    this->updateGroupButtonVisibility();

    if (!groupId.isEmpty() && this->tabGroupMembers(groupId).isEmpty())
    {
        this->removeTabGroup(groupId, false);
    }

    this->performLayout(true);
}

void Notebook::duplicatePage(QWidget *page)
{
    auto item = this->findItem(page);
    assert(item.has_value());
    if (!item.has_value())
    {
        return;
    }

    auto *container = dynamic_cast<SplitContainer *>(item->page);
    if (!container)
    {
        return;
    }

    auto *sourceTab = item->tab;
    const auto sourceGroupId = sourceTab->groupId();

    auto *newContainer = new SplitContainer(this);
    if (!container->getSplits().empty())
    {
        auto descriptor = container->buildDescriptor();
        newContainer->applyFromDescriptor(descriptor);
    }

    const auto tabPosition = this->indexOf(page);
    auto newTabPosition = -1;
    if (tabPosition != -1)
    {
        newTabPosition = tabPosition + 1;
    }

    QString newTabTitle = "";
    if (sourceTab->hasCustomTitle())
    {
        newTabTitle = sourceTab->getCustomTitle();
    }

    auto *tab =
        this->addPageAt(newContainer, newTabPosition, newTabTitle, false);
    if (sourceTab->hasCustomTabColor())
    {
        tab->setCustomTabColor(sourceTab->getCustomTabColor());
    }
    tab->setAlwaysVisible(sourceTab->isAlwaysVisible());
    tab->copyHighlightStateAndSourcesFrom(sourceTab);

    newContainer->setTab(tab);

    if (!sourceGroupId.isEmpty())
    {
        this->updateUngroupedOrderAfterMove(newContainer, page, true);

        this->assignPageToTabGroup(newContainer, sourceGroupId, false);
    }
}

void Notebook::removeCurrentPage()
{
    if (this->selectedPage_ != nullptr)
    {
        this->removePage(this->selectedPage_);
    }
}

int Notebook::indexOf(QWidget *page) const
{
    for (int i = 0; i < this->items_.count(); i++)
    {
        if (this->items_[i].page == page)
        {
            return i;
        }
    }

    return -1;
}

int Notebook::visibleIndexOf(QWidget *page) const
{
    if (!this->tabVisibilityFilter_)
    {
        return this->indexOf(page);
    }

    int i = 0;
    for (const auto &item : this->items_)
    {
        if (item.page == page)
        {
            assert(this->tabVisibilityFilter_(item.tab));
            return i;
        }
        if (this->tabVisibilityFilter_(item.tab))
        {
            ++i;
        }
    }

    return -1;
}

int Notebook::getVisibleTabCount() const
{
    if (!this->tabVisibilityFilter_)
    {
        return this->items_.count();
    }

    int i = 0;
    for (const auto &item : this->items_)
    {
        if (this->tabVisibilityFilter_(item.tab))
        {
            ++i;
        }
    }
    return i;
}

void Notebook::select(QWidget *page, bool focusPage)
{
    if (page == this->selectedPage_)
    {
        // Nothing has changed
        return;
    }

    if (page)
    {
        // A new page has been selected, mark it as selected & focus one of its splits
        auto item = this->findItem(page);
        if (!item.has_value())
        {
            return;
        }

        page->show();

        item->tab->setSelected(true);
        item->tab->raise();

        if (focusPage)
        {
            if (item->selectedWidget != nullptr &&
                containsChild(page, item->selectedWidget))
            {
                item->selectedWidget->setFocus(Qt::MouseFocusReason);
            }
            else
            {
                if (item->selectedWidget != nullptr)
                {
                    qCDebug(chatterinoWidget) << "Notebook: selected child of "
                                                 "page doesn't exist anymore";
                }
                page->setFocus();
            }
        }
    }

    if (this->selectedPage_)
    {
        // Hide the previously selected page
        this->selectedPage_->hide();

        auto item =
            std::ranges::find_if(this->items_, [this](const auto &candidate) {
                return candidate.page == this->selectedPage_;
            });
        if (item == this->items_.end())
        {
            return;
        }
        item->tab->setSelected(false);
        item->selectedWidget = this->selectedPage_->focusWidget();
    }

    this->selectedPage_ = page;

    this->performLayout();
}

bool Notebook::containsPage(QWidget *page)
{
    return std::any_of(this->items_.begin(), this->items_.end(),
                       [page](const auto &item) {
                           return item.page == page;
                       });
}

std::optional<Notebook::Item> Notebook::findItem(QWidget *page)
{
    auto it = std::find_if(this->items_.begin(), this->items_.end(),
                           [page](const auto &item) {
                               return page == item.page;
                           });
    if (it != this->items_.end())
    {
        return *it;
    }
    return std::nullopt;
}

bool Notebook::containsChild(const QObject *obj, const QObject *child)
{
    return std::any_of(obj->children().begin(), obj->children().end(),
                       [child](const QObject *o) {
                           if (o == child)
                           {
                               return true;
                           }

                           return containsChild(o, child);
                       });
}

void Notebook::selectIndex(int index, bool focusPage)
{
    if (index < 0 || this->items_.count() <= index)
    {
        return;
    }

    this->select(this->items_[index].page, focusPage);
}

void Notebook::selectVisibleIndex(int index, bool focusPage)
{
    if (!this->tabVisibilityFilter_)
    {
        this->selectIndex(index, focusPage);
        return;
    }

    int i = 0;
    for (auto &item : this->items_)
    {
        if (this->tabVisibilityFilter_(item.tab))
        {
            if (i == index)
            {
                // found the index'th visible page
                this->select(item.page, focusPage);
                return;
            }
            ++i;
        }
    }
}

void Notebook::selectNextTab(bool focusPage)
{
    const int size = this->items_.size();
    if (size == 0)
    {
        return;
    }

    // find next tab that is permitted by filter
    auto index = this->indexOf(this->selectedPage_);
    for (int visited = 0; visited < size; ++visited)
    {
        index = (index + 1) % size;
        if (this->tabPassesVisibilityFilter(this->items_[index].tab))
        {
            this->select(this->items_[index].page, focusPage);
            return;
        }
    }
}

void Notebook::selectPreviousTab(bool focusPage)
{
    const int size = this->items_.size();

    if (size == 0)
    {
        return;
    }

    // find next previous tab that is permitted by filter
    auto index = std::max(0, this->indexOf(this->selectedPage_));
    for (int visited = 0; visited < size; ++visited)
    {
        index = index == 0 ? size - 1 : index - 1;
        if (this->tabPassesVisibilityFilter(this->items_[index].tab))
        {
            this->select(this->items_[index].page, focusPage);
            return;
        }
    }
}

void Notebook::selectLastTab(bool focusPage)
{
    if (!this->tabVisibilityFilter_)
    {
        const auto size = this->items_.size();
        if (size == 0)
        {
            return;
        }

        this->select(this->items_[size - 1].page, focusPage);
        return;
    }

    // find first tab permitted by filter starting from the end
    for (auto it = this->items_.rbegin(); it != this->items_.rend(); ++it)
    {
        if (this->tabVisibilityFilter_(it->tab))
        {
            this->select(it->page, focusPage);
            return;
        }
    }
}

int Notebook::getPageCount() const
{
    return this->items_.count();
}

QWidget *Notebook::getPageAt(int index) const
{
    return this->items_[index].page;
}

int Notebook::getSelectedIndex() const
{
    return this->indexOf(this->selectedPage_);
}

QWidget *Notebook::getSelectedPage() const
{
    return this->selectedPage_;
}

QWidget *Notebook::tabAt(QPoint point, int &index, int maxWidth)
{
    for (const auto &group : this->tabGroups_)
    {
        if (!group->header->isVisible())
        {
            continue;
        }

        auto rect = group->header->getDesiredRect();
        rect.setWidth(std::min(maxWidth, rect.width()));
        if (!rect.contains(point))
        {
            continue;
        }

        for (int memberIndex = 0; memberIndex < this->items_.size();
             ++memberIndex)
        {
            if (this->items_[memberIndex].tab->groupId() == group->id)
            {
                index = memberIndex;
                return this->items_[memberIndex].page;
            }
        }
    }

    auto i = 0;

    for (auto &item : this->items_)
    {
        if (!item.tab->isVisible())
        {
            i++;
            continue;
        }

        auto rect = item.tab->getDesiredRect();

        rect.setWidth(std::min(maxWidth, rect.width()));

        if (rect.contains(point))
        {
            index = i;
            return item.page;
        }

        i++;
    }

    index = -1;
    return nullptr;
}

void Notebook::rearrangePage(QWidget *page, int index)
{
    if (this->isNotebookLayoutLocked())
    {
        return;
    }

    // Queue up save because: Tab rearranged
    getApp()->getWindows()->queueSave();

    const auto currentIndex = this->indexOf(page);
    if (currentIndex == -1 || index < 0 || index >= this->items_.size())
    {
        return;
    }

    this->ensureUngroupedOrder();
    const auto groupId = this->items_[currentIndex].tab->groupId();
    auto *targetPage = this->items_[index].page;
    const auto movingForward = currentIndex < index;
    if (!groupId.isEmpty())
    {
        int first = this->items_.size();
        int last = -1;
        for (int i = 0; i < this->items_.size(); ++i)
        {
            if (this->items_[i].tab->groupId() == groupId)
            {
                first = std::min(first, i);
                last = std::max(last, i);
            }
        }
        index = std::clamp(index, first, last);
    }

    this->items_.move(currentIndex, index);
    if (groupId.isEmpty())
    {
        this->updateUngroupedOrderAfterMove(page, targetPage, movingForward);
    }
    this->normalizeTabGroups();

    this->performLayout(true);
}

QString Notebook::createTabGroup(QWidget *firstPage, QWidget *secondPage)
{
    if (!firstPage || !this->containsPage(firstPage) ||
        this->tabGroups_.size() >= MAX_TAB_GROUPS)
    {
        return {};
    }

    this->ensureUngroupedOrder();

    auto group = std::make_unique<TabGroup>();
    group->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    group->openMenuOnClick =
        getSettings()->newTabGroupClickAction.getValue() == "menu";
    group->header = new NotebookTab(this, NotebookTab::Role::GroupHeader);
    group->header->setGroupId(group->id);
    group->header->setTabLocation(this->tabLocation_);
    const auto id = group->id;
    this->tabGroups_.push_back(std::move(group));
    this->updateGroupButtonVisibility();

    this->assignPageToTabGroup(firstPage, id, false);
    if (secondPage && secondPage != firstPage && this->containsPage(secondPage))
    {
        this->assignPageToTabGroup(secondPage, id);
    }

    this->normalizeTabGroups();
    this->refresh();
    getApp()->getWindows()->queueSave();
    return id;
}

void Notebook::groupPageWith(QWidget *page, QWidget *targetPage)
{
    if (!page || !targetPage || page == targetPage ||
        this->isNotebookLayoutLocked())
    {
        return;
    }

    const auto targetGroup = this->pageTabGroup(targetPage);
    const auto sourceGroup = this->pageTabGroup(page);
    if (!targetGroup.isEmpty())
    {
        this->assignPageToTabGroup(page, targetGroup);
    }
    else if (!sourceGroup.isEmpty())
    {
        this->assignPageToTabGroup(targetPage, sourceGroup);
    }
    else
    {
        this->createTabGroup(targetPage, page);
    }
}

QString Notebook::pageTabGroup(QWidget *page) const
{
    for (const auto &item : this->items_)
    {
        if (item.page == page)
        {
            return item.tab->groupId();
        }
    }
    return {};
}

void Notebook::assignPageToTabGroup(QWidget *page, const QString &groupId,
                                    bool moveNextToGroup)
{
    auto *group = this->findTabGroup(groupId);
    auto item = this->findItem(page);
    if (!group || !item || item->tab->groupId() == groupId)
    {
        return;
    }

    this->ensureUngroupedOrder();
    const auto oldGroupId = item->tab->groupId();
    item->tab->setGroupId(groupId);
    item->tab->setGroupMuted(group->muted, false);

    if (moveNextToGroup)
    {
        const auto from = this->indexOf(page);
        auto destination = -1;
        for (int i = 0; i < this->items_.size(); ++i)
        {
            if (this->items_[i].page != page &&
                this->items_[i].tab->groupId() == groupId)
            {
                destination = i;
            }
        }
        if (destination != -1)
        {
            this->items_.move(
                from, from < destination ? destination : destination + 1);
        }
    }

    if (!oldGroupId.isEmpty() && this->tabGroupMembers(oldGroupId).isEmpty())
    {
        this->removeTabGroup(oldGroupId, false);
    }

    this->updateTabGroupHeader(*group);
    this->updateGroupButtonVisibility();
    this->refresh();
    getApp()->getWindows()->queueSave();
}

void Notebook::removePageFromTabGroup(QWidget *page)
{
    auto item = this->findItem(page);
    if (!item || item->tab->groupId().isEmpty())
    {
        return;
    }

    const auto oldGroupId = item->tab->groupId();
    item->tab->setGroupId({});
    item->tab->setGroupMuted(false);
    this->restoreUngroupedOrder();
    this->normalizeTabGroups();
    if (this->tabGroupMembers(oldGroupId).isEmpty())
    {
        this->removeTabGroup(oldGroupId, false);
    }
    else if (auto *group = this->findTabGroup(oldGroupId))
    {
        this->updateTabGroupHeader(*group);
    }
    this->refresh();
    getApp()->getWindows()->queueSave();
}

void Notebook::ungroupTabGroup(const QString &groupId)
{
    this->removeTabGroup(groupId, true);
}

void Notebook::populateTabGroupMenu(QMenu *menu, QWidget *page)
{
    menu->clear();
    const auto locked = this->isNotebookLayoutLocked();

    auto *newGroup = menu->addAction("Create tab group…");
    newGroup->setEnabled(!locked && this->tabGroups_.size() < MAX_TAB_GROUPS);
    QObject::connect(newGroup, &QAction::triggered, this, [this, page] {
        this->openTabGroupEditor({}, page);
    });

    if (!this->tabGroups_.empty())
    {
        menu->addSeparator();
    }
    const auto current = this->pageTabGroup(page);
    for (const auto &group : this->tabGroups_)
    {
        const auto memberCount = this->tabGroupMembers(group->id).size();
        auto *action =
            menu->addAction(this->tabGroupDisplayName(*group, memberCount));
        action->setCheckable(true);
        action->setChecked(current == group->id);
        action->setEnabled(!locked);
        QObject::connect(action, &QAction::triggered, this,
                         [this, page, id = group->id](bool checked) {
                             if (checked)
                             {
                                 this->assignPageToTabGroup(page, id);
                             }
                             else
                             {
                                 this->removePageFromTabGroup(page);
                             }
                         });
    }

    if (!current.isEmpty())
    {
        menu->addSeparator();
        auto *remove = menu->addAction("Remove from group");
        remove->setEnabled(!locked);
        QObject::connect(remove, &QAction::triggered, this, [this, page] {
            this->removePageFromTabGroup(page);
        });
    }
}

void Notebook::showTabGroupMenu(const QString &groupId, const QPoint &globalPos)
{
    auto *group = this->findTabGroup(groupId);
    if (!group)
    {
        return;
    }

    QPointer<QMenu> menu = new QMenu(this);
    const auto cleanup = qScopeGuard([menu] {
        delete menu;
    });
    menu->addAction(group->collapsed ? "Expand group" : "Collapse group", this,
                    [this, groupId] {
                        this->toggleTabGroup(groupId);
                    });

    auto *openTab = menu->addMenu("Open tab");
    const auto members = this->tabGroupMembers(groupId);
    for (const auto *member : members)
    {
        auto *action = openTab->addAction(this->tabGroupMemberLabel(*member));
        action->setCheckable(true);
        action->setChecked(member->page == this->selectedPage_);
        QObject::connect(action, &QAction::triggered, this,
                         [this, page = QPointer<QWidget>(member->page)] {
                             if (page)
                             {
                                 this->select(page);
                             }
                         });
    }

    auto *mute = menu->addAction("Mute group alerts");
    mute->setCheckable(true);
    mute->setChecked(group->muted);
    mute->setToolTip(
        "Silence sounds and desktop alerts, and hide unread markers. Each tab "
        "keeps its own alert settings.");
    QObject::connect(
        mute, &QAction::toggled, this, [this, groupId](bool muted) {
            auto *current = this->findTabGroup(groupId);
            if (!current || current->muted == muted)
            {
                return;
            }
            current->muted = muted;
            for (auto *member : this->tabGroupMembers(groupId))
            {
                member->tab->setGroupMuted(muted, false);
            }
            const auto sizeChanged = this->updateTabGroupHeader(*current);
            if (sizeChanged)
            {
                this->refresh();
            }
            getApp()->getWindows()->queueSave();
        });

    auto *clickMenu = menu->addMenu("When clicked");
    auto *clickActions = new QActionGroup(clickMenu);
    clickActions->setExclusive(true);
    auto *expandAction = clickMenu->addAction("Expand tabs");
    auto *listAction = clickMenu->addAction("Open tab list");
    for (auto *action : {expandAction, listAction})
    {
        action->setCheckable(true);
        clickActions->addAction(action);
    }
    expandAction->setChecked(!group->openMenuOnClick);
    listAction->setChecked(group->openMenuOnClick);
    QObject::connect(expandAction, &QAction::triggered, this, [this, groupId] {
        if (auto *current = this->findTabGroup(groupId))
        {
            current->openMenuOnClick = false;
            this->updateTabGroupHeader(*current);
            getApp()->getWindows()->queueSave();
        }
    });
    QObject::connect(listAction, &QAction::triggered, this, [this, groupId] {
        if (auto *current = this->findTabGroup(groupId))
        {
            current->openMenuOnClick = true;
            this->updateTabGroupHeader(*current);
            getApp()->getWindows()->queueSave();
        }
    });

    menu->addSeparator();
    auto *rename = menu->addAction("Rename group…", this, [this, groupId] {
        this->renameTabGroup(groupId);
    });
    rename->setEnabled(!this->isNotebookLayoutLocked());

    auto *edit = menu->addAction("Edit tabs…", this, [this, groupId] {
        this->openTabGroupEditor(groupId);
    });
    edit->setEnabled(!this->isNotebookLayoutLocked());

    auto *colorMenu = menu->addMenu("Color");
    auto *colorActions = new QActionGroup(colorMenu);
    colorActions->setExclusive(true);
    auto *themeColor = colorMenu->addAction("Theme accent");
    themeColor->setCheckable(true);
    themeColor->setChecked(group->colorMode == "theme");
    colorActions->addAction(themeColor);
    QObject::connect(themeColor, &QAction::triggered, this, [this, groupId] {
        if (auto *current = this->findTabGroup(groupId))
        {
            current->colorMode = "theme";
            current->color = QColor();
            this->updateTabGroupHeader(*current);
            getApp()->getWindows()->queueSave();
        }
    });

    auto *noColor = colorMenu->addAction("No color");
    noColor->setCheckable(true);
    noColor->setChecked(group->colorMode == "none");
    colorActions->addAction(noColor);
    QObject::connect(noColor, &QAction::triggered, this, [this, groupId] {
        if (auto *current = this->findTabGroup(groupId))
        {
            current->colorMode = "none";
            current->color = QColor();
            this->updateTabGroupHeader(*current);
            getApp()->getWindows()->queueSave();
        }
    });
    colorMenu->addSeparator();
    const std::vector<std::pair<QString, QColor>> colors = {
        {"Orange", QColor(255, 148, 67)}, {"Blue", QColor(91, 157, 255)},
        {"Green", QColor(76, 196, 120)},  {"Purple", QColor(172, 123, 255)},
        {"Pink", QColor(238, 95, 161)},   {"Cyan", QColor(73, 205, 214)},
    };
    for (const auto &[name, color] : colors)
    {
        auto *colorAction = colorMenu->addAction(name);
        colorAction->setCheckable(true);
        colorAction->setChecked(group->colorMode == "custom" &&
                                group->color.isValid() &&
                                group->color.rgb() == color.rgb());
        colorActions->addAction(colorAction);
        QObject::connect(colorAction, &QAction::triggered, this,
                         [this, groupId, color] {
                             if (auto *current = this->findTabGroup(groupId))
                             {
                                 current->colorMode = "custom";
                                 current->color = color;
                                 this->updateTabGroupHeader(*current);
                                 getApp()->getWindows()->queueSave();
                             }
                         });
    }
    colorMenu->addSeparator();
    colorMenu->addAction("Custom color…", this, [this, groupId] {
        auto *current = this->findTabGroup(groupId);
        if (!current)
        {
            return;
        }
        auto initial =
            current->color.isValid() ? current->color : getTheme()->accent;
        auto *dialog = new ColorPickerDialog(initial, this);
        QObject::connect(dialog, &ColorPickerDialog::colorConfirmed, this,
                         [this, groupId](const QColor &color) {
                             if (auto *current = this->findTabGroup(groupId);
                                 current && color.isValid())
                             {
                                 current->colorMode = "custom";
                                 current->color = color;
                                 this->updateTabGroupHeader(*current);
                                 getApp()->getWindows()->queueSave();
                             }
                         });
        dialog->show();
    });

    auto *iconMenu = menu->addMenu("Icon");
    auto *iconActions = new QActionGroup(iconMenu);
    iconActions->setExclusive(true);
    const std::array<std::pair<const char *, const char *>, 6> icons = {{
        {"Folder", "folder"},
        {"Star", "star"},
        {"Heart", "heart"},
        {"Bell", "bell"},
        {"Shield", "shield"},
        {"No icon", "none"},
    }};
    for (const auto &[label, value] : icons)
    {
        auto *action = iconMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(group->icon == value);
        iconActions->addAction(action);
        QObject::connect(action, &QAction::triggered, this,
                         [this, groupId, icon = QString::fromLatin1(value)] {
                             if (auto *current = this->findTabGroup(groupId))
                             {
                                 if (current->icon == "custom")
                                 {
                                     removeTabGroupIcon(current->id);
                                 }
                                 current->icon = icon;
                                 current->customIconPath.clear();
                                 const auto sizeChanged =
                                     this->updateTabGroupHeader(*current);
                                 if (sizeChanged)
                                 {
                                     this->refresh();
                                 }
                                 getApp()->getWindows()->queueSave();
                             }
                         });
    }
    iconMenu->addSeparator();
    auto *customIcon = iconMenu->addAction("Custom image…");
    customIcon->setCheckable(true);
    customIcon->setChecked(group->icon == "custom");
    iconActions->addAction(customIcon);
    QObject::connect(customIcon, &QAction::triggered, this, [this, groupId] {
        auto *current = this->findTabGroup(groupId);
        if (!current)
        {
            return;
        }
        const QPointer<Notebook> self(this);
        const auto path = QFileDialog::getOpenFileName(
            this, "Choose a group icon", {},
            "Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)");
        if (!self)
        {
            return;
        }
        current = this->findTabGroup(groupId);
        if (path.isEmpty() || !current)
        {
            return;
        }
        if (!this->setTabGroupCustomIcon(*current, path))
        {
            showThemedWarning(
                this, "Group icon",
                "Could not use that image. Choose a PNG, JPG, WebP, or "
                "BMP image.");
            return;
        }
        const auto sizeChanged = this->updateTabGroupHeader(*current);
        if (sizeChanged)
        {
            this->refresh();
        }
        getApp()->getWindows()->queueSave();
    });

    menu->addSeparator();
    auto *ungroup = menu->addAction("Ungroup tabs", this, [this, groupId] {
        this->ungroupTabGroup(groupId);
    });
    ungroup->setEnabled(!this->isNotebookLayoutLocked());

    auto *close = menu->addAction("Close all tabs…", this, [this, groupId] {
        const auto members = this->tabGroupMembers(groupId);
        if (members.isEmpty())
        {
            return;
        }
        QList<QPointer<QWidget>> pages;
        for (const auto *member : members)
        {
            pages.push_back(member->page);
        }
        const auto oneTab = members.size() == 1;
        QPointer<QMessageBox> confirmation = new QMessageBox(
            QMessageBox::Question, "Close group tabs",
            oneTab ? QStringLiteral("Close the tab in this group?")
                   : QStringLiteral("Close all %1 tabs in this group?")
                         .arg(members.size()),
            QMessageBox::Cancel, this);
        const auto cleanup = qScopeGuard([confirmation] {
            delete confirmation;
        });
        auto *closeButton = confirmation->addButton(
            oneTab ? "Close tab" : "Close tabs", QMessageBox::AcceptRole);
        confirmation->setDefaultButton(QMessageBox::Cancel);
        installMoltorinoDialogTheme(confirmation);
        confirmation->exec();
        if (!confirmation || confirmation->clickedButton() != closeButton)
        {
            return;
        }

        for (const auto &page : pages)
        {
            if (page && this->containsPage(page) &&
                this->pageTabGroup(page) == groupId)
            {
                this->removePage(page);
            }
        }
    });
    close->setEnabled(!this->isNotebookLayoutLocked());

    menu->exec(globalPos);
}

void Notebook::openTabGroupEditor(const QString &groupId, QWidget *initialPage)
{
    if (this->isNotebookLayoutLocked() || this->items_.isEmpty())
    {
        return;
    }

    this->ensureUngroupedOrder();
    auto *group = groupId.isEmpty() ? nullptr : this->findTabGroup(groupId);
    if (groupId.isEmpty() && this->tabGroups_.size() >= MAX_TAB_GROUPS)
    {
        return;
    }
    if (!groupId.isEmpty() && !group)
    {
        return;
    }

    std::vector<TabGroupDialogEntry> entries;
    entries.reserve(this->items_.size());
    for (const auto &item : this->items_)
    {
        QString otherGroupName;
        const auto itemGroupId = item.tab->groupId();
        if (!itemGroupId.isEmpty() && itemGroupId != groupId)
        {
            if (const auto *otherGroup = this->findTabGroup(itemGroupId))
            {
                otherGroupName = this->tabGroupDisplayName(
                    *otherGroup, this->tabGroupMembers(itemGroupId).size());
            }
        }
        entries.push_back({
            .page = item.page,
            .title = item.tab->getTitle(),
            .channelTitle = item.tab->getDefaultTitle(),
            .otherGroupName = std::move(otherGroupName),
            .selected = group
                            ? item.tab->groupId() == groupId
                            : item.page == initialPage && itemGroupId.isEmpty(),
        });
    }

    const QPointer<Notebook> self(this);
    QPointer<TabGroupDialog> dialog = new TabGroupDialog(
        group ? group->name : QString(), entries, group == nullptr,
        group ? group->icon : QStringLiteral("folder"),
        group ? group->customIconPath : QString(), this);
    const auto result = dialog->exec();
    if (!self || !dialog)
    {
        return;
    }
    if (result != QDialog::Accepted)
    {
        delete dialog;
        return;
    }
    auto chosenPages = dialog->selectedPages();
    const auto name = dialog->groupName();
    const auto icon = dialog->groupIcon();
    const auto iconPath = dialog->customIconPath();
    delete dialog;
    chosenPages.removeIf([this](auto *page) {
        return !this->containsPage(page);
    });
    if (chosenPages.isEmpty())
    {
        return;
    }

    QString targetId = groupId;
    group = this->findTabGroup(groupId);
    if (groupId.isEmpty())
    {
        targetId = this->createTabGroup(chosenPages.front());
        group = this->findTabGroup(targetId);
    }
    if (!group)
    {
        return;
    }

    QSet<QWidget *> chosen;
    for (auto *page : chosenPages)
    {
        chosen.insert(page);
    }

    for (auto &item : this->items_)
    {
        if (chosen.contains(item.page))
        {
            item.tab->setGroupId(targetId);
            item.tab->setGroupMuted(group->muted, false);
        }
        else if (item.tab->groupId() == targetId)
        {
            item.tab->setGroupId({});
            item.tab->setGroupMuted(false);
        }
    }
    group->name = name;
    const auto chosenIcon = normalizeTabGroupIcon(icon);
    if (chosenIcon == "custom")
    {
        if (!this->setTabGroupCustomIcon(*group, iconPath))
        {
            if (group->icon == "custom" && group->customIconPath.isEmpty())
            {
                group->icon = "folder";
            }
            showThemedWarning(
                this, "Group icon",
                "Could not use that image. The current icon is unchanged.");
            if (!self)
            {
                return;
            }
            group = this->findTabGroup(targetId);
            if (!group)
            {
                return;
            }
        }
    }
    else
    {
        if (group->icon == "custom")
        {
            removeTabGroupIcon(group->id);
        }
        group->icon = chosenIcon;
        group->customIconPath.clear();
    }

    this->restoreUngroupedOrder();
    this->normalizeTabGroups();
    for (auto it = this->tabGroups_.begin(); it != this->tabGroups_.end();)
    {
        if ((*it)->id != targetId && this->tabGroupMembers((*it)->id).isEmpty())
        {
            removeTabGroupIcon((*it)->id);
            (*it)->header->hide();
            (*it)->header->deleteLater();
            it = this->tabGroups_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    this->updateTabGroupHeader(*group);
    this->updateGroupButtonVisibility();
    this->refresh();
    getApp()->getWindows()->queueSave();
}

void Notebook::toggleTabGroup(const QString &groupId)
{
    if (auto *group = this->findTabGroup(groupId))
    {
        group->collapsed = !group->collapsed;
        this->refresh();
        getApp()->getWindows()->queueSave();
    }
}

void Notebook::moveTabGroup(const QString &groupId, int index)
{
    if (this->isNotebookLayoutLocked() || index < 0 ||
        index >= this->items_.size())
    {
        return;
    }

    auto firstSourceIndex = this->items_.size();
    for (int i = 0; i < this->items_.size(); ++i)
    {
        if (this->items_[i].tab->groupId() == groupId)
        {
            firstSourceIndex = i;
            break;
        }
    }
    if (firstSourceIndex == this->items_.size())
    {
        return;
    }

    const bool movingForward = firstSourceIndex < index;
    const auto targetGroupId = this->items_[index].tab->groupId();
    if (targetGroupId == groupId)
    {
        return;
    }
    if (!targetGroupId.isEmpty())
    {
        if (movingForward)
        {
            while (index + 1 < this->items_.size() &&
                   this->items_[index + 1].tab->groupId() == targetGroupId)
            {
                ++index;
            }
            ++index;
        }
        else
        {
            while (index > 0 &&
                   this->items_[index - 1].tab->groupId() == targetGroupId)
            {
                --index;
            }
        }
    }
    else if (movingForward)
    {
        ++index;
    }

    QList<Item> members;
    int removedBeforeTarget = 0;
    for (int i = this->items_.size() - 1; i >= 0; --i)
    {
        if (this->items_[i].tab->groupId() == groupId)
        {
            if (i < index)
            {
                ++removedBeforeTarget;
            }
            members.prepend(this->items_.takeAt(i));
        }
    }
    if (members.isEmpty())
    {
        return;
    }

    index = std::clamp(index - removedBeforeTarget, 0,
                       static_cast<int>(this->items_.size()));
    for (const auto &member : members)
    {
        this->items_.insert(index++, member);
    }
    this->performLayout(true);
    getApp()->getWindows()->queueSave();
}

void Notebook::tabStatusChanged(NotebookTab *tab)
{
    if (!tab || tab->role() != NotebookTab::Role::Page ||
        tab->groupId().isEmpty())
    {
        return;
    }
    if (auto *group = this->findTabGroup(tab->groupId()))
    {
        if (this->updateTabGroupHeader(*group))
        {
            this->refresh();
        }
    }
}

Notebook::TabGroup *Notebook::findTabGroup(const QString &id)
{
    const auto it =
        std::ranges::find_if(this->tabGroups_, [&id](const auto &group) {
            return group->id == id;
        });
    return it == this->tabGroups_.end() ? nullptr : it->get();
}

const Notebook::TabGroup *Notebook::findTabGroup(const QString &id) const
{
    const auto it =
        std::ranges::find_if(this->tabGroups_, [&id](const auto &group) {
            return group->id == id;
        });
    return it == this->tabGroups_.end() ? nullptr : it->get();
}

QList<Notebook::Item *> Notebook::tabGroupMembers(const QString &id)
{
    QList<Item *> members;
    for (auto &item : this->items_)
    {
        if (item.tab->groupId() == id)
        {
            members.push_back(&item);
        }
    }
    return members;
}

QList<const Notebook::Item *> Notebook::tabGroupMembers(const QString &id) const
{
    QList<const Item *> members;
    for (const auto &item : this->items_)
    {
        if (item.tab->groupId() == id)
        {
            members.push_back(&item);
        }
    }
    return members;
}

QString Notebook::tabGroupDisplayName(const TabGroup &group, int members) const
{
    if (!group.name.isEmpty())
    {
        return group.name;
    }
    return members == 1 ? QStringLiteral("1 tab")
                        : QStringLiteral("%1 tabs").arg(members);
}

bool Notebook::updateTabGroupHeader(TabGroup &group)
{
    const auto members = this->tabGroupMembers(group.id);
    return this->updateTabGroupHeader(group, members);
}

bool Notebook::updateTabGroupHeader(TabGroup &group,
                                    const QList<Item *> &members)
{
    bool selected = false;
    bool live = false;
    auto highlight = HighlightState::None;
    for (const auto *member : members)
    {
        selected |= member->page == this->selectedPage_;
        live |= member->tab->isLive();
        if (member->tab->highlightState() == HighlightState::Highlighted)
        {
            highlight = HighlightState::Highlighted;
        }
        else if (highlight == HighlightState::None &&
                 member->tab->highlightState() == HighlightState::NewMessage)
        {
            highlight = HighlightState::NewMessage;
        }
    }

    const auto title = this->tabGroupDisplayName(group, members.size());
    return group.header->setGroupHeaderState(
        title, members.size(), group.collapsed, group.collapsed && selected,
        live, highlight, group.colorMode, group.color, group.icon,
        group.customIconPath, group.muted, group.openMenuOnClick,
        std::exchange(group.customIconDirty, false));
}

void Notebook::activateTabGroup(const QString &groupId, const QPoint &globalPos)
{
    const auto *group = this->findTabGroup(groupId);
    if (!group)
    {
        return;
    }

    if (group->openMenuOnClick)
    {
        this->showTabGroupQuickSwitcher(groupId, globalPos);
    }
    else
    {
        this->toggleTabGroup(groupId);
    }
}

void Notebook::previewTabGroupDrop(QWidget *sourcePage, QWidget *targetPage)
{
    if (!sourcePage || !targetPage || sourcePage == targetPage ||
        !this->containsPage(sourcePage) || !this->containsPage(targetPage) ||
        !this->getAllowUserTabManagement() || this->isNotebookLayoutLocked())
    {
        this->cancelTabGroupDrop();
        return;
    }

    const auto sourceGroup = this->pageTabGroup(sourcePage);
    const auto targetGroup = this->pageTabGroup(targetPage);
    if (!sourceGroup.isEmpty() && sourceGroup == targetGroup)
    {
        this->cancelTabGroupDrop();
        return;
    }

    NotebookTab *visual = nullptr;
    if (!targetGroup.isEmpty())
    {
        if (auto *group = this->findTabGroup(targetGroup))
        {
            visual = group->header;
        }
    }
    if (!visual)
    {
        if (auto item = this->findItem(targetPage))
        {
            visual = item->tab;
        }
    }
    if (!visual)
    {
        this->cancelTabGroupDrop();
        return;
    }

    if (this->groupDropVisual_ != visual)
    {
        if (this->groupDropVisual_)
        {
            this->groupDropVisual_->setGroupDropTarget(false);
        }
        this->groupDropVisual_ = visual;
        this->groupDropVisual_->setGroupDropTarget(true);
    }
    this->groupDropSource_ = sourcePage;
    this->groupDropTarget_ = targetPage;
}

bool Notebook::commitTabGroupDrop(QWidget *sourcePage)
{
    if (!sourcePage || this->groupDropSource_ != sourcePage ||
        !this->groupDropTarget_)
    {
        return false;
    }

    auto *targetPage = this->groupDropTarget_.data();
    this->cancelTabGroupDrop();
    this->groupPageWith(sourcePage, targetPage);
    return true;
}

void Notebook::cancelTabGroupDrop()
{
    if (this->groupDropVisual_)
    {
        this->groupDropVisual_->setGroupDropTarget(false);
    }
    this->groupDropVisual_ = nullptr;
    this->groupDropSource_.clear();
    this->groupDropTarget_.clear();
}

void Notebook::showTabGroupQuickSwitcher(const QString &groupId,
                                         const QPoint &globalPos)
{
    auto *group = this->findTabGroup(groupId);
    if (!group)
    {
        return;
    }

    const auto members = this->tabGroupMembers(groupId);
    QPointer<QMenu> menu = new QMenu(this);
    const auto cleanup = qScopeGuard([menu] {
        delete menu;
    });
    auto *heading =
        menu->addSection(this->tabGroupDisplayName(*group, members.size()));
    heading->setEnabled(false);
    for (const auto *member : members)
    {
        auto label = this->tabGroupMemberLabel(*member);
        if (member->tab->isLive())
        {
            label.prepend(QStringLiteral("\u25cf "));
        }
        auto *action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(member->page == this->selectedPage_);
        QObject::connect(action, &QAction::triggered, this,
                         [this, page = QPointer<QWidget>(member->page)] {
                             if (page)
                             {
                                 this->select(page);
                             }
                         });
    }
    menu->exec(globalPos);
}

QString Notebook::tabGroupMemberLabel(const Item &item) const
{
    const auto title = item.tab->getTitle().trimmed();
    const auto channel = item.tab->getDefaultTitle().trimmed();
    if (channel.isEmpty() || channel.compare(title, Qt::CaseInsensitive) == 0 ||
        !getSettings()->showTabGroupChannelNames.getValue())
    {
        return title.isEmpty() ? QStringLiteral("Untitled tab") : title;
    }
    return QStringLiteral("%1 \u00b7 %2")
        .arg(title.isEmpty() ? QStringLiteral("Untitled tab") : title, channel);
}

void Notebook::renameTabGroup(const QString &groupId)
{
    auto *group = this->findTabGroup(groupId);
    if (!group || this->isNotebookLayoutLocked())
    {
        return;
    }

    QPointer<QInputDialog> dialog = new QInputDialog(this);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    dialog->setWindowTitle("Rename tab group");
    dialog->setLabelText("Group name");
    dialog->setTextValue(group->name);
    dialog->setOkButtonText("Save");
    dialog->setMinimumWidth(380);
    if (auto *input = dialog->findChild<QLineEdit *>())
    {
        input->setMaxLength(64);
        input->setPlaceholderText("Leave blank to use the tab count");
        input->selectAll();
    }
    installMoltorinoDialogTheme(dialog);
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return;
    }

    group = this->findTabGroup(groupId);
    if (!group)
    {
        return;
    }
    group->name = dialog->textValue().trimmed().left(64);
    this->updateTabGroupHeader(*group);
    this->refresh();
    getApp()->getWindows()->queueSave();
}

bool Notebook::setTabGroupCustomIcon(TabGroup &group, const QString &sourcePath)
{
    const QFileInfo source(sourcePath);
    if (!source.isFile() || source.size() <= 0 ||
        source.size() > 10 * 1024 * 1024)
    {
        return false;
    }

    QImageReader reader(source.absoluteFilePath());
    reader.setAutoTransform(true);
    const auto sourceSize = reader.size();
    if (sourceSize.isValid() &&
        (sourceSize.width() > 256 || sourceSize.height() > 256))
    {
        reader.setScaledSize(sourceSize.scaled(256, 256, Qt::KeepAspectRatio));
    }
    auto image = reader.read();
    if (image.isNull())
    {
        return false;
    }
    if (image.width() > 256 || image.height() > 256)
    {
        image = image.scaled(256, 256, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    const auto destination = tabGroupIconPath(group.id);
    if (!source.canonicalFilePath().isEmpty() &&
        QFileInfo(destination).canonicalFilePath() ==
            source.canonicalFilePath())
    {
        group.icon = "custom";
        group.customIconPath = destination;
        group.customIconDirty = true;
        return true;
    }

    QDir settings(getApp()->getPaths().settingsDirectory);
    if (!settings.mkpath("TabGroupIcons"))
    {
        return false;
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") ||
        !output.commit())
    {
        output.cancelWriting();
        return false;
    }

    group.icon = "custom";
    group.customIconPath = destination;
    group.customIconDirty = true;
    return true;
}

bool Notebook::tabPassesVisibilityFilter(const NotebookTab *tab) const
{
    return !this->tabVisibilityFilter_ || this->tabVisibilityFilter_(tab);
}

void Notebook::removeTabGroup(const QString &groupId, bool keepMembers)
{
    const auto it =
        std::ranges::find_if(this->tabGroups_, [&groupId](const auto &group) {
            return group->id == groupId;
        });
    if (it == this->tabGroups_.end())
    {
        return;
    }

    if (keepMembers)
    {
        for (auto &item : this->items_)
        {
            if (item.tab->groupId() == groupId)
            {
                item.tab->setGroupId({});
                item.tab->setGroupMuted(false);
            }
        }
    }
    (*it)->header->hide();
    (*it)->header->deleteLater();
    removeTabGroupIcon((*it)->id);
    this->tabGroups_.erase(it);
    if (keepMembers)
    {
        this->restoreUngroupedOrder();
        this->normalizeTabGroups();
    }
    this->updateGroupButtonVisibility();
    this->refresh();
    getApp()->getWindows()->queueSave();
}

void Notebook::normalizeTabGroups()
{
    QList<Item> normalized;
    QSet<QString> emitted;
    const auto original = this->items_;
    normalized.reserve(original.size());

    QSet<QString> validGroups;
    for (const auto &group : this->tabGroups_)
    {
        validGroups.insert(group->id);
    }
    QHash<QString, QList<Item>> membersByGroup;
    for (const auto &item : original)
    {
        const auto groupId = item.tab->groupId();
        if (!groupId.isEmpty() && validGroups.contains(groupId))
        {
            membersByGroup[groupId].push_back(item);
        }
    }

    for (auto item : original)
    {
        const auto groupId = item.tab->groupId();
        if (groupId.isEmpty() || !validGroups.contains(groupId))
        {
            if (!groupId.isEmpty())
            {
                item.tab->setGroupId({});
            }
            normalized.push_back(item);
            continue;
        }
        if (emitted.contains(groupId))
        {
            continue;
        }
        emitted.insert(groupId);
        for (const auto &candidate : membersByGroup[groupId])
        {
            normalized.push_back(candidate);
        }
    }
    this->items_ = std::move(normalized);
}

void Notebook::ensureUngroupedOrder()
{
    QSet<int> indices;
    QList<std::pair<int, NotebookTab *>> validIndices;
    QList<NotebookTab *> repairIndices;
    auto largestIndex = -1;
    for (auto &item : this->items_)
    {
        const auto index = item.tab->ungroupedIndex();
        if (index < 0 || indices.contains(index))
        {
            repairIndices.push_back(item.tab);
            continue;
        }
        indices.insert(index);
        validIndices.push_back({index, item.tab});
        largestIndex = std::max(largestIndex, index);
    }

    if (repairIndices.isEmpty() &&
        largestIndex == static_cast<int>(this->items_.size()) - 1)
    {
        return;
    }

    std::ranges::stable_sort(validIndices,
                             [](const auto &left, const auto &right) {
                                 return left.first < right.first;
                             });
    auto nextIndex = 0;
    for (const auto &[index, tab] : validIndices)
    {
        std::ignore = index;
        tab->setUngroupedIndex(nextIndex++);
    }
    for (auto *tab : repairIndices)
    {
        tab->setUngroupedIndex(nextIndex++);
    }
}

void Notebook::restoreUngroupedOrder()
{
    this->ensureUngroupedOrder();
    QList<Item> ungroupedItems;
    ungroupedItems.reserve(this->items_.size());
    for (const auto &item : this->items_)
    {
        if (item.tab->groupId().isEmpty())
        {
            ungroupedItems.push_back(item);
        }
    }
    std::ranges::stable_sort(
        ungroupedItems, [](const Item &left, const Item &right) {
            return left.tab->ungroupedIndex() < right.tab->ungroupedIndex();
        });

    auto restored = ungroupedItems.begin();
    for (auto &item : this->items_)
    {
        if (item.tab->groupId().isEmpty())
        {
            item = *restored++;
        }
    }
}

void Notebook::updateUngroupedOrderAfterMove(QWidget *page, QWidget *targetPage,
                                             bool afterTarget)
{
    if (!page || !targetPage || page == targetPage)
    {
        return;
    }

    QList<Item *> homeOrder;
    homeOrder.reserve(this->items_.size());
    for (auto &item : this->items_)
    {
        homeOrder.push_back(&item);
    }
    std::ranges::stable_sort(
        homeOrder, [](const Item *left, const Item *right) {
            return left->tab->ungroupedIndex() < right->tab->ungroupedIndex();
        });

    const auto pageIt = std::ranges::find(homeOrder, page, &Item::page);
    const auto targetIt = std::ranges::find(homeOrder, targetPage, &Item::page);
    if (pageIt == homeOrder.end() || targetIt == homeOrder.end())
    {
        return;
    }

    auto *moved = *pageIt;
    homeOrder.erase(pageIt);
    auto newTarget = std::ranges::find(homeOrder, targetPage, &Item::page);
    if (afterTarget)
    {
        ++newTarget;
    }
    homeOrder.insert(newTarget, moved);

    for (int i = 0; i < homeOrder.size(); ++i)
    {
        homeOrder[i]->tab->setUngroupedIndex(i);
    }
}

void Notebook::syncUngroupedOrderToItems()
{
    for (int i = 0; i < this->items_.size(); ++i)
    {
        this->items_[i].tab->setUngroupedIndex(i);
    }
}

std::vector<Notebook::TabGroupSnapshot> Notebook::tabGroups() const
{
    std::vector<TabGroupSnapshot> result;
    result.reserve(this->tabGroups_.size());
    QSet<QString> nonEmptyGroupIds;
    for (const auto &item : this->items_)
    {
        if (!item.tab->groupId().isEmpty())
        {
            nonEmptyGroupIds.insert(item.tab->groupId());
        }
    }
    for (const auto &group : this->tabGroups_)
    {
        if (!nonEmptyGroupIds.contains(group->id))
        {
            continue;
        }
        result.push_back({
            .id = group->id,
            .name = group->name,
            .colorMode = group->colorMode,
            .color = group->color,
            .icon = group->icon,
            .customIconPath = group->customIconPath,
            .collapsed = group->collapsed,
            .muted = group->muted,
            .openMenuOnClick = group->openMenuOnClick,
        });
    }
    return result;
}

void Notebook::restoreTabGroup(const TabGroupSnapshot &snapshot)
{
    const auto id = snapshot.id.left(64);
    if (id.isEmpty() || this->findTabGroup(id) ||
        this->tabGroups_.size() >= MAX_TAB_GROUPS)
    {
        return;
    }

    auto group = std::make_unique<TabGroup>();
    group->id = id;
    group->name = snapshot.name.left(64);
    group->colorMode = normalizeTabGroupColorMode(snapshot.colorMode);
    group->color = snapshot.color;
    if (group->colorMode == "custom" && !group->color.isValid())
    {
        group->colorMode = "theme";
    }
    if (group->colorMode != "custom")
    {
        group->color = QColor();
    }
    group->icon = normalizeTabGroupIcon(snapshot.icon);
    if (group->icon == "custom")
    {
        const auto internalPath = tabGroupIconPath(group->id);
        const QFileInfo internalIcon(internalPath);
        QImageReader internalReader(internalPath);
        if (internalIcon.isFile() && internalIcon.size() > 0 &&
            internalIcon.size() <= 10 * 1024 * 1024 && internalReader.canRead())
        {
            group->customIconPath = internalPath;
        }
        else
        {
            removeTabGroupIcon(group->id);
            auto legacyPath = snapshot.customIconPath.left(1024);
            if (!legacyPath.isEmpty() && QDir::isRelativePath(legacyPath))
            {
                legacyPath = QDir(getApp()->getPaths().settingsDirectory)
                                 .filePath(legacyPath);
            }
            if (legacyPath.isEmpty() ||
                !this->setTabGroupCustomIcon(*group, legacyPath))
            {
                group->icon = "folder";
            }
        }
    }
    if (group->icon != "custom")
    {
        group->customIconPath.clear();
    }
    group->collapsed = snapshot.collapsed;
    group->muted = snapshot.muted;
    group->openMenuOnClick = snapshot.openMenuOnClick;
    group->header = new NotebookTab(this, NotebookTab::Role::GroupHeader);
    group->header->setGroupId(group->id);
    group->header->setTabLocation(this->tabLocation_);
    group->header->hide();
    this->tabGroups_.push_back(std::move(group));
}

void Notebook::restorePageTabGroup(QWidget *page, const QString &groupId,
                                   int ungroupedIndex)
{
    const auto *group = this->findTabGroup(groupId);
    if (auto item = this->findItem(page); item && group)
    {
        item->tab->setUngroupedIndex(ungroupedIndex);
        item->tab->setGroupId(groupId);
        item->tab->setGroupMuted(group->muted, false);
    }
}

void Notebook::finishRestoringTabGroups()
{
    this->ensureUngroupedOrder();
    this->normalizeTabGroups();
    for (auto it = this->tabGroups_.begin(); it != this->tabGroups_.end();)
    {
        if (this->tabGroupMembers((*it)->id).isEmpty())
        {
            removeTabGroupIcon((*it)->id);
            (*it)->header->deleteLater();
            it = this->tabGroups_.erase(it);
        }
        else
        {
            for (auto *member : this->tabGroupMembers((*it)->id))
            {
                member->tab->setGroupMuted((*it)->muted, false);
            }
            this->updateTabGroupHeader(**it);
            ++it;
        }
    }
    this->updateGroupButtonVisibility();
    this->refresh();
}

void Notebook::updateGroupButtonVisibility()
{
    const bool canCreate = this->showTabs_ && this->allowUserTabManagement_ &&
                           !this->lockNotebookLayout_ &&
                           this->items_.size() >= 2 &&
                           this->tabGroups_.size() < MAX_TAB_GROUPS;
    const bool visible = this->showAddButton_ && canCreate &&
                         getSettings()->showTabGroupButton.getValue();
    if (this->newTabGroupAction_)
    {
        this->newTabGroupAction_->setEnabled(canCreate);
    }
    if (this->groupButton_->isHidden() == visible)
    {
        this->groupButton_->setHidden(!visible);
        this->refresh();
    }
}

bool Notebook::getAllowUserTabManagement() const
{
    return this->allowUserTabManagement_;
}

void Notebook::setAllowUserTabManagement(bool value)
{
    this->allowUserTabManagement_ = value;
    this->updateGroupButtonVisibility();
}

bool Notebook::getShowTabs() const
{
    return this->showTabs_;
}

void Notebook::setShowTabs(bool value)
{
    this->showTabs_ = value;

    this->setShowAddButton(value);
    this->performLayout();

    // show a popup upon hiding tabs
    if (!value && getSettings()->informOnTabVisibilityToggle.getValue())
    {
        this->showTabVisibilityInfoPopup();
    }
}

void Notebook::showTabVisibilityInfoPopup()
{
    auto unhideSeq = getApp()->getHotkeys()->getDisplaySequence(
        HotkeyCategory::Window, "setTabVisibility", {std::vector<QString>()});
    if (unhideSeq.isEmpty())
    {
        unhideSeq = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"toggle"}});
    }
    if (unhideSeq.isEmpty())
    {
        unhideSeq = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"on"}});
    }
    QString hotkeyInfo = "(currently unbound)";
    if (!unhideSeq.isEmpty())
    {
        hotkeyInfo =
            "(" + unhideSeq.toString(QKeySequence::SequenceFormat::NativeText) +
            ")";
    }
    QMessageBox msgBox(this->window());
    msgBox.window()->setWindowTitle("Chatterino - hidden tabs");
    msgBox.setText("You've just hidden your tabs.");
    msgBox.setInformativeText(
        "You can toggle tabs by using the keyboard shortcut " + hotkeyInfo +
        " or right-clicking the tab area and selecting \"Toggle "
        "visibility of tabs\".");
    msgBox.addButton(QMessageBox::Ok);
    auto *dsaButton =
        msgBox.addButton("Don't show again", QMessageBox::YesRole);

    msgBox.setDefaultButton(QMessageBox::Ok);

    installMoltorinoDialogTheme(&msgBox);
    msgBox.exec();

    if (msgBox.clickedButton() == dsaButton)
    {
        getSettings()->informOnTabVisibilityToggle.setValue(false);
    }
}

void Notebook::refresh()
{
    if (this->refreshPaused_)
    {
        this->refreshRequested_ = true;
        return;
    }

    this->performLayout();
}

bool Notebook::getShowAddButton() const
{
    return this->showAddButton_;
}

void Notebook::setShowAddButton(bool value)
{
    this->showAddButton_ = value;

    this->addButton_->setHidden(!value);
    this->updateGroupButtonVisibility();

    this->refresh();
}

void Notebook::resizeAddButton()
{
    int h = static_cast<int>((NOTEBOOK_TAB_HEIGHT - 1) * this->scale());
    this->addButton_->setFixedSize(h, h);
    this->groupButton_->setFixedSize(h, h);
}

void Notebook::scaleChangedEvent(float /*scale*/)
{
    this->resizeAddButton();
    this->refreshPaused_ = true;
    this->refreshRequested_ = false;
    for (auto &i : this->items_)
    {
        i.tab->updateSize();
    }
    for (auto &group : this->tabGroups_)
    {
        group->header->updateSize();
    }
    this->refreshPaused_ = false;
    if (this->refreshRequested_)
    {
        this->refresh();
    }
}

void Notebook::resizeEvent(QResizeEvent *)
{
    this->performLayout();
}

void Notebook::performLayout(bool animated)
{
    std::vector<Item> filteredItems;
    filteredItems.reserve(this->items_.size() + this->tabGroups_.size());

    QHash<QString, QList<Item *>> membersByGroup;
    QHash<QString, TabGroup *> groupsById;
    for (auto &group : this->tabGroups_)
    {
        groupsById.insert(group->id, group.get());
    }
    for (auto &item : this->items_)
    {
        if (!item.tab->groupId().isEmpty() &&
            groupsById.contains(item.tab->groupId()))
        {
            membersByGroup[item.tab->groupId()].push_back(&item);
        }
    }

    QSet<QString> emittedGroups;
    for (auto &item : this->items_)
    {
        const auto groupId = item.tab->groupId();
        auto *group = groupsById.value(groupId, nullptr);
        if (groupId.isEmpty() || !group)
        {
            if (this->tabPassesVisibilityFilter(item.tab))
            {
                filteredItems.push_back(item);
            }
            continue;
        }

        if (emittedGroups.contains(groupId))
        {
            continue;
        }
        emittedGroups.insert(groupId);

        const auto &members = membersByGroup[groupId];
        const auto hasVisibleMember =
            std::ranges::any_of(members, [this](const auto *member) {
                return this->tabPassesVisibilityFilter(member->tab);
            });
        if (!hasVisibleMember)
        {
            continue;
        }

        this->updateTabGroupHeader(*group, members);
        filteredItems.push_back({.tab = group->header});
        if (!group->collapsed)
        {
            for (const auto *member : members)
            {
                if (this->tabPassesVisibilityFilter(member->tab))
                {
                    filteredItems.push_back(*member);
                }
            }
        }
    }

    QSet<NotebookTab *> displayedTabs;
    displayedTabs.reserve(static_cast<qsizetype>(filteredItems.size()));
    for (auto &item : filteredItems)
    {
        displayedTabs.insert(item.tab);
    }
    for (auto &item : this->items_)
    {
        item.tab->setVisible(this->showTabs_ &&
                             displayedTabs.contains(item.tab));
    }
    for (auto &group : this->tabGroups_)
    {
        group->header->setVisible(this->showTabs_ &&
                                  displayedTabs.contains(group->header));
    }

    for (std::size_t i = 0; i < filteredItems.size(); ++i)
    {
        filteredItems[i].tab->setVisibleEdgeFlags(
            i == 0, i + 1 == filteredItems.size());
    }

    const auto scale = this->scale();
    const auto tabHeight = int(NOTEBOOK_TAB_HEIGHT * scale);
    const LayoutContext ctx{
        .left = static_cast<int>(2 * this->scale()),
        .right = this->width(),
        .bottom = this->height(),
        .scale = scale,
        .tabHeight = tabHeight,
        .minimumTabAreaSpace = static_cast<int>(tabHeight * 0.5),
        .addButtonWidth = (this->addButton_->isHidden() ? 0 : tabHeight) +
                          (this->groupButton_->isHidden() ? 0 : tabHeight),
        .lineThickness = static_cast<int>(2 * scale),
        .tabSpacer = std::max(1, static_cast<int>(scale)),
        .buttonWidth = tabHeight,
        .buttonHeight = tabHeight - 1,
        .items = filteredItems,
    };

    if (this->tabLocation_ == NotebookTabLocation::Top ||
        this->tabLocation_ == NotebookTabLocation::Bottom)
    {
        this->performHorizontalLayout(ctx, animated);
    }
    else
    {
        this->performVerticalLayout(ctx, animated);
    }

    if (this->showTabs_)
    {
        // raise elements
        for (auto &i : filteredItems)
        {
            i.tab->raise();
        }

        if (!this->groupButton_->isHidden())
        {
            this->groupButton_->raise();
        }
        if (!this->addButton_->isHidden())
        {
            this->addButton_->raise();
        }
    }
}

void Notebook::performHorizontalLayout(const LayoutContext &ctx, bool animated)
{
    const auto isBottom = this->tabLocation_ == NotebookTabLocation::Bottom;
    const auto reverse = isBottom ? -1 : 1;

    auto x = ctx.left;
    auto y = isBottom ? ctx.bottom - ctx.tabHeight - ctx.tabSpacer : 0;
    auto consumedButtonHeights = 0;

    // set size of custom buttons (settings, user, ...)
    for (auto *btn : this->customButtons_)
    {
        // We use isHidden here since the layout can happen when the button has
        // been added but before it's shown
        if (btn->isHidden())
        {
            continue;
        }

        btn->setFixedSize(ctx.buttonWidth, ctx.buttonHeight);
        btn->move(x, y);
        x += ctx.buttonWidth;

        consumedButtonHeights = ctx.tabHeight;
    }

    if (this->showTabs_)
    {
        // layout tabs
        /// Notebook tabs need to know if they are in the last row.
        auto *firstInBottomRow =
            ctx.items.empty() ? nullptr : &ctx.items.front();

        for (std::size_t itemIndex = 0; itemIndex < ctx.items.size();
             ++itemIndex)
        {
            auto &item = ctx.items[itemIndex];
            /// Break line if element doesn't fit.
            auto isFirst = itemIndex == 0;
            auto isLast = itemIndex + 1 == ctx.items.size();

            auto requiredWidth = item.tab->width();
            if (item.tab->role() == NotebookTab::Role::GroupHeader &&
                itemIndex + 1 < ctx.items.size() &&
                ctx.items[itemIndex + 1].tab->groupId() == item.tab->groupId())
            {
                requiredWidth +=
                    ctx.tabSpacer + ctx.items[itemIndex + 1].tab->width();
            }

            auto fitsInLine = ((isLast ? ctx.addButtonWidth : 0) + x +
                               requiredWidth) <= this->width();

            if (!isFirst && !fitsInLine)
            {
                y += item.tab->height() * reverse;
                x = ctx.left;
                firstInBottomRow = &item;
            }

            /// Layout tab
            item.tab->growWidth(0);
            item.tab->moveAnimated(QPoint(x, y), animated);
            x += item.tab->width() + ctx.tabSpacer;
        }

        /// Update which tabs are in the last row
        auto inLastRow = false;
        for (const auto &item : ctx.items)
        {
            if (&item == firstInBottomRow)
            {
                inLastRow = true;
            }
            item.tab->setInLastRow(inLastRow);
        }

        // move misc buttons
        if (!this->groupButton_->isHidden())
        {
            this->groupButton_->move(x, y);
            x += this->groupButton_->width();
        }
        if (!this->addButton_->isHidden())
        {
            this->addButton_->move(x, y);
        }

        if (!isBottom)
        {
            y += ctx.tabHeight;
        }
    }

    if (isBottom)
    {
        int consumedBottomSpace = std::max(
            {ctx.bottom - y, consumedButtonHeights, ctx.minimumTabAreaSpace});
        int tabsStart = ctx.bottom - consumedBottomSpace - ctx.lineThickness;

        if (this->lineOffset_ != tabsStart)
        {
            this->lineOffset_ = tabsStart;
            this->update();
        }

        // set page bounds
        if (this->selectedPage_ != nullptr)
        {
            this->selectedPage_->move(0, 0);
            this->selectedPage_->resize(this->width(), tabsStart);
            this->selectedPage_->raise();
        }
    }
    else
    {
        y = std::max({y, consumedButtonHeights, ctx.minimumTabAreaSpace});

        if (this->lineOffset_ != y)
        {
            this->lineOffset_ = y;
            this->update();
        }

        /// Increment for the line at the bottom
        y += int(2 * ctx.scale);

        // set page bounds
        if (this->selectedPage_ != nullptr)
        {
            this->selectedPage_->move(0, y);
            this->selectedPage_->resize(this->width(), this->height() - y);
            this->selectedPage_->raise();
        }
    }
}

void Notebook::performVerticalLayout(const LayoutContext &ctx, bool animated)
{
    int x = 0;
    int y = 0;
    int consumedButtonWidths = 0;

    const bool isRight = this->tabLocation_ == NotebookTabLocation::Right;

    if (isRight)
    {
        x = ctx.right;

        // set size of custom buttons (settings, user, ...)
        for (auto btnIt = this->customButtons_.rbegin();
             btnIt != this->customButtons_.rend(); ++btnIt)
        {
            auto *btn = *btnIt;
            if (btn->isHidden())
            {
                continue;
            }

            x -= ctx.buttonWidth;
            btn->setFixedSize(ctx.buttonWidth, ctx.buttonHeight);
            btn->move(x, y);
        }

        consumedButtonWidths = ctx.right - x;
        x = ctx.right;
    }
    else
    {
        x = ctx.left;

        // set size of custom buttons (settings, user, ...)
        for (auto *btn : this->customButtons_)
        {
            if (btn->isHidden())
            {
                continue;
            }

            btn->setFixedSize(ctx.buttonWidth, ctx.buttonHeight);
            btn->move(x, y);
            x += ctx.buttonWidth;
        }

        consumedButtonWidths = x;
        x = ctx.left;
    }

    if (this->visibleButtonCount() > 0)
    {
        y = ctx.tabHeight + ctx.lineThickness;  // account for divider line
    }

    const int top = y + ctx.tabSpacer;  // add margin

    y = top;

    // zneix: if we were to remove buttons when tabs are hidden
    // stuff below to "set page bounds" part should be in conditional statement
    int tabsPerColumn =
        (this->height() - top) / (ctx.tabHeight + ctx.tabSpacer);
    if (tabsPerColumn == 0)  // window hasn't properly rendered yet
    {
        return;
    }
    QList<DrawnButton *> endButtons;
    if (!this->groupButton_->isHidden())
    {
        endButtons.push_back(this->groupButton_);
    }
    if (!this->addButton_->isHidden())
    {
        endButtons.push_back(this->addButton_);
    }

    const auto tabCount = static_cast<int>(ctx.items.size());
    const int count = tabCount + endButtons.size();
    int columnCount = ceil((float)count / tabsPerColumn);

    // only add width of all the tabs if they are not hidden
    if (this->showTabs_)
    {
        for (int col = 0; col < columnCount; col++)
        {
            bool isLastColumn = col == columnCount - 1;
            auto largestWidth = 0;
            const int itemStart = col * tabsPerColumn;
            const int itemEnd = std::min((col + 1) * tabsPerColumn, count);

            for (int i = itemStart; i < itemEnd; i++)
            {
                if (i < tabCount)
                {
                    largestWidth = std::max(ctx.items[i].tab->normalTabWidth(),
                                            largestWidth);
                }
                else
                {
                    largestWidth = std::max(largestWidth,
                                            endButtons[i - tabCount]->width());
                }
            }

            if (isLastColumn)
            {
                if (isRight)
                {
                    int distanceFromRight = this->width() - x;
                    largestWidth = std::max(
                        largestWidth, consumedButtonWidths - distanceFromRight);
                }
                else
                {
                    largestWidth =
                        std::max(largestWidth, consumedButtonWidths - x);
                }
            }

            if (isRight)
            {
                x -= largestWidth + ctx.lineThickness;
            }

            for (int i = itemStart; i < itemEnd; i++)
            {
                if (i < tabCount)
                {
                    auto item = ctx.items[i];

                    /// Layout tab
                    item.tab->growWidth(largestWidth);
                    item.tab->moveAnimated(QPoint(x, y), animated);
                    item.tab->setInLastRow(isLastColumn);
                }
                else
                {
                    endButtons[i - tabCount]->move(x, y);
                }
                y += ctx.tabHeight + ctx.tabSpacer;
            }

            if (!isRight)
            {
                x += largestWidth + ctx.lineThickness;
            }

            y = top;
        }
    }

    if (isRight)
    {
        // subtract another lineThickness to account for vertical divider
        x -= ctx.lineThickness;
        int consumedRightSpace = std::max(
            {ctx.right - x, consumedButtonWidths, ctx.minimumTabAreaSpace});
        int tabsStart = ctx.right - consumedRightSpace;

        if (this->lineOffset_ != tabsStart)
        {
            this->lineOffset_ = tabsStart;
            this->update();
        }

        // set page bounds
        if (this->selectedPage_ != nullptr)
        {
            this->selectedPage_->move(0, 0);
            this->selectedPage_->resize(tabsStart, this->height());
            this->selectedPage_->raise();
        }
    }
    else
    {
        x = std::max({x, consumedButtonWidths, ctx.minimumTabAreaSpace});

        if (this->lineOffset_ != x - ctx.lineThickness)
        {
            this->lineOffset_ = x - ctx.lineThickness;
            this->update();
        }

        // set page bounds
        if (this->selectedPage_ != nullptr)
        {
            this->selectedPage_->move(x, 0);
            this->selectedPage_->resize(this->width() - x, this->height());
            this->selectedPage_->raise();
        }
    }
}

void Notebook::mousePressEvent(QMouseEvent *event)
{
    this->update();

    switch (event->button())
    {
        case Qt::RightButton: {
            event->accept();

            if (!this->menu_)
            {
                this->menu_ = new QMenu(this);
                this->addNotebookActionsToMenu(this->menu_);
            }
            this->menu_->popup(event->globalPosition().toPoint() +
                               QPoint(0, 8));
        }
        break;
        default:;
    }
}

void Notebook::setTabLocation(NotebookTabLocation location)
{
    if (location != this->tabLocation_)
    {
        this->tabLocation_ = location;

        // Update all tabs
        for (const auto &item : this->items_)
        {
            item.tab->setTabLocation(location);
        }

        for (const auto &group : this->tabGroups_)
        {
            group->header->setTabLocation(location);
        }

        this->performLayout();
    }
}

void Notebook::paintEvent(QPaintEvent *event)
{
    auto scale = this->scale();

    QPainter painter(this);
    if (this->tabLocation_ == NotebookTabLocation::Top ||
        this->tabLocation_ == NotebookTabLocation::Bottom)
    {
        /// horizontal line
        painter.fillRect(0, this->lineOffset_, this->width(), int(2 * scale),
                         this->theme->tabs.dividerLine);
    }
    else if (this->tabLocation_ == NotebookTabLocation::Left ||
             this->tabLocation_ == NotebookTabLocation::Right)
    {
        if (this->visibleButtonCount() > 0)
        {
            if (this->tabLocation_ == NotebookTabLocation::Left)
            {
                painter.fillRect(0, int(NOTEBOOK_TAB_HEIGHT * scale),
                                 this->lineOffset_, int(2 * scale),
                                 this->theme->tabs.dividerLine);
            }
            else
            {
                painter.fillRect(this->lineOffset_,
                                 int(NOTEBOOK_TAB_HEIGHT * scale),
                                 this->width() - this->lineOffset_,
                                 int(2 * scale), this->theme->tabs.dividerLine);
            }
        }

        /// vertical line
        painter.fillRect(this->lineOffset_, 0, int(2 * scale), this->height(),
                         this->theme->tabs.dividerLine);
    }
}

bool Notebook::isNotebookLayoutLocked() const
{
    return this->lockNotebookLayout_;
}

void Notebook::setLockNotebookLayout(bool value)
{
    if (this->lockNotebookLayout_ == value)
    {
        return;
    }

    this->lockNotebookLayout_ = value;
    this->lockNotebookLayoutAction_->setChecked(value);
    getSettings()->lockNotebookLayout.setValue(value);
    this->updateGroupButtonVisibility();

    this->refreshPaused_ = true;
    this->refreshRequested_ = false;
    for (auto &item : this->items_)
    {
        if (item.tab)
        {
            item.tab->tabSizeChanged();
        }
    }
    this->refreshPaused_ = false;
    this->refreshRequested_ = false;
    this->performLayout();
}

void Notebook::addNotebookActionsToMenu(QMenu *menu, bool includeNewGroupAction)
{
    menu->addAction(this->lockNotebookLayoutAction_);
    if (includeNewGroupAction)
    {
        menu->addAction(this->newTabGroupAction_);
    }

    menu->addAction(this->toggleTopMostAction_);
}

NotebookTab *Notebook::getTabFromPage(QWidget *page)
{
    for (auto &it : this->items_)
    {
        if (it.page == page)
        {
            return it.tab;
        }
    }

    return nullptr;
}

size_t Notebook::visibleButtonCount() const
{
    size_t i = 0;
    for (auto *btn : this->customButtons_)
    {
        if (!btn->isHidden())
        {
            ++i;
        }
    }
    return i;
}

void Notebook::setTabVisibilityFilter(TabVisibilityFilter filter)
{
    if (filter)
    {
        // Wrap tab filter to always accept selected tabs. This prevents confusion
        // when jumping to hidden tabs with the quick switcher, for example.
        filter = [originalFilter = std::move(filter)](const NotebookTab *tab) {
            return tab->isSelected() || originalFilter(tab);
        };
    }

    this->tabVisibilityFilter_ = std::move(filter);
    this->performLayout();
}

bool Notebook::shouldShowTab(const NotebookTab *tab) const
{
    if (!this->showTabs_)
    {
        return false;
    }

    if (this->tabVisibilityFilter_)
    {
        return this->tabVisibilityFilter_(tab);
    }

    return true;
}

void Notebook::sortTabsAlphabetically()
{
    assert(!this->isNotebookLayoutLocked() &&
           "sortTabsAlphabetically called while notebook layout is locked");

    QHash<QString, int> memberCounts;
    for (const auto &item : this->items_)
    {
        if (!item.tab->groupId().isEmpty())
        {
            ++memberCounts[item.tab->groupId()];
        }
    }
    QHash<QString, QString> groupSortKeys;
    for (const auto &group : this->tabGroups_)
    {
        groupSortKeys.insert(
            group->id,
            this->tabGroupDisplayName(*group, memberCounts.value(group->id)));
    }

    struct SortBlock {
        QString title;
        QList<Item> items;
    };

    std::vector<SortBlock> blocks;
    blocks.reserve(this->items_.size());
    QSet<QString> emittedGroups;
    for (const auto &item : this->items_)
    {
        const auto groupId = item.tab->groupId();
        if (groupId.isEmpty())
        {
            blocks.push_back({item.tab->getTitle(), {item}});
            continue;
        }
        if (emittedGroups.contains(groupId))
        {
            continue;
        }
        emittedGroups.insert(groupId);

        SortBlock block{.title = groupSortKeys.value(groupId)};
        for (const auto &candidate : this->items_)
        {
            if (candidate.tab->groupId() == groupId)
            {
                block.items.push_back(candidate);
            }
        }
        std::ranges::stable_sort(block.items, [](const Item &a, const Item &b) {
            return a.tab->getTitle().compare(b.tab->getTitle(),
                                             Qt::CaseInsensitive) < 0;
        });
        blocks.push_back(std::move(block));
    }

    std::ranges::stable_sort(
        blocks, [](const SortBlock &a, const SortBlock &b) {
            return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
        });

    QList<Item> sortedItems;
    sortedItems.reserve(this->items_.size());
    for (auto &block : blocks)
    {
        sortedItems.append(std::move(block.items));
    }
    this->items_ = std::move(sortedItems);
    this->syncUngroupedOrderToItems();

    getApp()->getWindows()->queueSave();
    this->performLayout(true);
}

SplitNotebook::SplitNotebook(Window *parent)
    : Notebook(parent)
{
    QObject::connect(this->addButton_, &Button::leftClicked, [this]() {
        QTimer::singleShot(80, this, [this] {
            this->addPage(true);
        });
    });

    // add custom buttons if they are not in the parent window frame
    if (!parent->hasCustomWindowFrame())
    {
        this->addCustomButtons();
    }

    auto *tabVisibilityActionGroup = new QActionGroup(this);
    tabVisibilityActionGroup->setExclusionPolicy(
        QActionGroup::ExclusionPolicy::Exclusive);

    this->showAllTabsAction = new QAction("Show all tabs", this);
    this->showAllTabsAction->setCheckable(true);
    this->showAllTabsAction->setShortcut(
        getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"on"}}));
    QObject::connect(this->showAllTabsAction, &QAction::triggered, this,
                     [this] {
                         this->setShowTabs(true);
                         getSettings()->tabVisibility.setValue(
                             NotebookTabVisibility::AllTabs);
                         this->showAllTabsAction->setChecked(true);
                     });
    tabVisibilityActionGroup->addAction(this->showAllTabsAction);

    this->onlyShowLiveTabsAction = new QAction("Only show live tabs", this);
    this->onlyShowLiveTabsAction->setCheckable(true);
    this->onlyShowLiveTabsAction->setShortcut(
        getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"liveOnly"}}));
    QObject::connect(this->onlyShowLiveTabsAction, &QAction::triggered, this,
                     [this] {
                         this->setShowTabs(true);
                         getSettings()->tabVisibility.setValue(
                             NotebookTabVisibility::LiveOnly);
                         this->onlyShowLiveTabsAction->setChecked(true);
                     });
    tabVisibilityActionGroup->addAction(this->onlyShowLiveTabsAction);

    this->hideAllTabsAction = new QAction("Hide all tabs", this);
    this->hideAllTabsAction->setCheckable(true);
    this->hideAllTabsAction->setShortcut(
        getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"off"}}));
    QObject::connect(this->hideAllTabsAction, &QAction::triggered, this,
                     [this] {
                         this->setShowTabs(false);
                         getSettings()->tabVisibility.setValue(
                             NotebookTabVisibility::AllTabs);
                         this->hideAllTabsAction->setChecked(true);
                     });
    tabVisibilityActionGroup->addAction(this->hideAllTabsAction);

    this->sortTabsAlphabeticallyAction_ =
        new QAction("Sort Tabs Alphabetically", this);
    if (this->isNotebookLayoutLocked())
    {
        this->sortTabsAlphabeticallyAction_->setEnabled(false);
    }
    QObject::connect(this->sortTabsAlphabeticallyAction_, &QAction::triggered,
                     [this] {
                         this->sortTabsAlphabetically();
                     });

    switch (getSettings()->tabVisibility.getEnum())
    {
        case NotebookTabVisibility::AllTabs: {
            this->showAllTabsAction->setChecked(true);
        }
        break;

        case NotebookTabVisibility::LiveOnly: {
            this->onlyShowLiveTabsAction->setChecked(true);
        }
        break;
    }

    getSettings()->tabVisibility.connect(
        [this](int val, auto) {
            auto visibility = NotebookTabVisibility(val);
            // Set the correct TabVisibilityFilter for the given visibility setting.
            // Note that selected tabs are always shown regardless of what the tab
            // filter returns, so no need to include `tab->isSelected()` in the
            // predicate. See Notebook::setTabVisibilityFilter.
            switch (visibility)
            {
                case NotebookTabVisibility::LiveOnly:
                    this->setTabVisibilityFilter([](const NotebookTab *tab) {
                        return tab->isLive() || tab->isAlwaysVisible();
                    });
                    break;
                case NotebookTabVisibility::AllTabs:
                default:
                    this->setTabVisibilityFilter(nullptr);
                    break;
            }
        },
        this->signalHolder_, true);

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->selectSplit, [this](Split *split) {
            for (auto &&item : this->items())
            {
                if (auto *sc = dynamic_cast<SplitContainer *>(item.page))
                {
                    auto &&splits = sc->getSplits();
                    if (std::find(splits.begin(), splits.end(), split) !=
                        splits.end())
                    {
                        this->select(item.page);
                        split->setFocus();
                        break;
                    }
                }
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->selectSplitContainer,
        [this](SplitContainer *sc) {
            this->select(sc);
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->scrollToMessageSignal,
        [this](const MessagePtr &message) {
            for (auto &&item : this->items())
            {
                if (auto *sc = dynamic_cast<SplitContainer *>(item.page))
                {
                    for (auto *split : sc->getSplits())
                    {
                        auto type = split->getChannel()->getType();
                        if (type != Channel::Type::TwitchMentions &&
                            type != Channel::Type::TwitchAutomod)
                        {
                            if (split->getChannelView().scrollToMessage(
                                    message))
                            {
                                return;
                            }
                        }
                    }
                }
            }
        });
}

void SplitNotebook::addNotebookActionsToMenu(QMenu *menu,
                                             bool includeNewGroupAction)
{
    Notebook::addNotebookActionsToMenu(menu, includeNewGroupAction);

    menu->addAction(this->sortTabsAlphabeticallyAction_);

    auto *submenu = menu->addMenu("Tab visibility");
    submenu->addAction(this->showAllTabsAction);
    submenu->addAction(this->onlyShowLiveTabsAction);
    submenu->addAction(this->hideAllTabsAction);
}

void SplitNotebook::toggleTabVisibility()
{
    if (this->getShowTabs())
    {
        this->hideAllTabsAction->trigger();
    }
    else
    {
        this->showAllTabsAction->trigger();
    }
}

void SplitNotebook::showEvent(QShowEvent * /*event*/)
{
    if (auto *page = this->getSelectedPage())
    {
        auto *split = page->getSelectedSplit();
        if (!split)
        {
            split = page->findChild<Split *>();
        }

        if (split)
        {
            split->scheduleDeferredTwitchRefresh();
            split->setFocus(Qt::OtherFocusReason);
        }
    }
}

void SplitNotebook::addCustomButtons()
{
    // settings
    auto *settingsBtn = this->addCustomButton<SvgButton>(SvgButton::Src{
        .dark = ":/buttons/settings-darkMode.svg",
        .light = ":/buttons/settings-lightMode.svg",
    });

    settingsBtn->setPadding({0, 0});

    // This is to ensure you can't lock yourself out of the settings
    if (getApp()->getArgs().safeMode)
    {
        settingsBtn->setVisible(true);
    }
    else
    {
        settingsBtn->setVisible(
            !getSettings()->hidePreferencesButton.getValue());

        getSettings()->hidePreferencesButton.connect(
            [this, settingsBtn](bool hide) {
                auto oldVisibility = settingsBtn->isVisible();
                auto newVisibility = !hide;
                settingsBtn->setVisible(newVisibility);
                if (oldVisibility != newVisibility)
                {
                    this->performLayout();
                }
            },
            this->signalHolder_, false);
    }

    QObject::connect(settingsBtn, &Button::leftClicked, [this] {
        getApp()->getWindows()->showSettingsDialog(this);
    });

    // account
    auto *userBtn = this->addCustomButton<SvgButton>(SvgButton::Src{
        .dark = ":/buttons/account-darkMode.svg",
        .light = ":/buttons/account-lightMode.svg",
    });

    userBtn->setPadding({0, 0});

    userBtn->setVisible(!getSettings()->hideUserButton.getValue());
    getSettings()->hideUserButton.connect(
        [this, userBtn](bool hide) {
            auto oldVisibility = userBtn->isVisible();
            auto newVisibility = !hide;
            userBtn->setVisible(newVisibility);
            if (oldVisibility != newVisibility)
            {
                this->performLayout();
            }
        },
        this->signalHolder_, false);

    QObject::connect(userBtn, &Button::leftClicked, [this, userBtn] {
        getApp()->getWindows()->showAccountSelectPopup(
            this->mapToGlobal(userBtn->rect().bottomRight()));
    });

    // updates
    auto *updateBtn = this->addCustomButton<PixmapButton>();

    initMoltorinoUpdateButton(
        *updateBtn,
        [this] {
            this->performLayout(false);
        },
        this->signalHolder_);

    // streamer mode
    this->streamerModeIcon_ = this->addCustomButton<PixmapButton>();
    QObject::connect(this->streamerModeIcon_, &Button::leftClicked, [this] {
        getApp()->getWindows()->showSettingsDialog(
            this, SettingsDialogPreference::StreamerMode);
    });
    QObject::connect(getApp()->getStreamerMode(), &IStreamerMode::changed, this,
                     &SplitNotebook::updateStreamerModeIcon);
    this->updateStreamerModeIcon();

    this->performLayout(false);
}

void SplitNotebook::updateStreamerModeIcon()
{
    if (this->streamerModeIcon_ == nullptr)
    {
        return;
    }
    // A duplicate of this code is in Window class
    // That copy handles the TitleBar icon in Window (main window on Windows)
    // This one is the one near splits (on linux and mac or non-main windows on Windows)
    if (getTheme()->isLightTheme())
    {
        this->streamerModeIcon_->setPixmap(
            getResources().buttons.streamerModeEnabledLight);
    }
    else
    {
        this->streamerModeIcon_->setPixmap(
            getResources().buttons.streamerModeEnabledDark);
    }

    auto oldVisibility = this->streamerModeIcon_->isVisible();
    auto newVisibility = getApp()->getStreamerMode()->isEnabled();

    this->streamerModeIcon_->setVisible(newVisibility);

    if (oldVisibility != newVisibility)
    {
        this->performLayout();
    }
}

void SplitNotebook::themeChangedEvent()
{
    this->updateStreamerModeIcon();
}

SplitContainer *SplitNotebook::addPage(bool select)
{
    auto *container = new SplitContainer(this);
    auto *tab = Notebook::addPage(container, QString(), select);
    container->setTab(tab);
    tab->setParent(this);
    return container;
}

SplitContainer *SplitNotebook::getOrAddSelectedPage()
{
    auto *selectedPage = this->getSelectedPage();

    if (selectedPage)
    {
        return dynamic_cast<SplitContainer *>(selectedPage);
    }

    return this->addPage();
}

SplitContainer *SplitNotebook::getSelectedPage()
{
    return dynamic_cast<SplitContainer *>(Notebook::getSelectedPage());
}

void SplitNotebook::select(QWidget *page, bool focusPage)
{
    // If there's a previously selected page, go through its splits and
    // update their "last read message" indicator
    if (auto *selectedPage = this->getSelectedPage())
    {
        if (auto *splitContainer = dynamic_cast<SplitContainer *>(selectedPage))
        {
            for (auto *split : splitContainer->getSplits())
            {
                split->updateLastReadMessage();
            }
        }
    }

    this->Notebook::select(page, focusPage);

    if (auto *selectedPage = this->getSelectedPage())
    {
        auto *split = selectedPage->getSelectedSplit();
        if (!split)
        {
            split = selectedPage->findChild<Split *>();
        }

        if (split)
        {
            split->scheduleDeferredTwitchRefresh();
            split->refreshSelectedYouTube();
        }
    }
}

void SplitNotebook::forEachSplit(const std::function<void(Split *)> &cb)
{
    for (const auto &item : this->items())
    {
        auto *page = dynamic_cast<SplitContainer *>(item.page);
        if (!page)
        {
            continue;
        }
        for (auto *split : page->getSplits())
        {
            cb(split);
        }
    }
}

void SplitNotebook::setLockNotebookLayout(bool value)
{
    Notebook::setLockNotebookLayout(value);
    this->sortTabsAlphabeticallyAction_->setEnabled(!value);
}

}  // namespace chatterino
