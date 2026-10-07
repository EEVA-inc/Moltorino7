// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/NotebookTab.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/DraggedSplit.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <boost/bind/bind.hpp>
#include <boost/container_hash/hash.hpp>
#include <QAbstractAnimation>
#include <QApplication>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace chatterino {
namespace {
// Translates the given rectangle by an amount in the direction to appear like the tab is selected.
// For example, if location is Top, the rectangle will be translated in the negative Y direction,
// or "up" on the screen, by amount.
void translateRectForLocation(QRect &rect, NotebookTabLocation location,
                              int amount)
{
    switch (location)
    {
        case NotebookTabLocation::Top:
            rect.translate(0, -amount);
            break;
        case NotebookTabLocation::Left:
            rect.translate(-amount, 0);
            break;
        case NotebookTabLocation::Right:
            rect.translate(amount, 0);
            break;
        case NotebookTabLocation::Bottom:
            rect.translate(0, amount);
            break;
    }
}

float getCompactDivider(TabStyle tabStyle)
{
    switch (tabStyle)
    {
        case TabStyle::Compact:
            return 1.5;
        case TabStyle::Normal:
        default:
            return 1.0;
    }
}

float getCompactReducer(TabStyle tabStyle)
{
    switch (tabStyle)
    {
        case TabStyle::Compact:
            return 4.0;
        case TabStyle::Normal:
        default:
            return 0.0;
    }
}

bool colorsMatch(const std::shared_ptr<QColor> &lhs,
                 const std::shared_ptr<QColor> &rhs)
{
    if (lhs == nullptr || rhs == nullptr)
    {
        return lhs == rhs;
    }

    return *lhs == *rhs;
}

QColor tabColorFill(QColor color, bool selected, bool windowFocused)
{
    auto alpha = color.alpha();
    const auto cap = selected ? 110 : 85;
    alpha = std::clamp(alpha, 0, cap);
    if (!windowFocused)
    {
        alpha = alpha * 2 / 3;
    }

    color.setAlpha(alpha);
    return color;
}

QColor tabHighlightLineColor(QColor color, bool windowFocused)
{
    color.setAlpha(windowFocused ? 230 : 150);
    return color;
}

QIcon tabColorIcon(QColor color)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    color.setAlpha(210);
    painter.setBrush(color);
    painter.setPen(QColor(255, 255, 255, 70));
    painter.drawRoundedRect(QRectF(3, 3, 12, 12), 3, 3);

    return QIcon(pixmap);
}

QColor opaqueTabColor(QColor color)
{
    color.setAlpha(255);
    return color;
}
struct GroupHeaderGeometry {
    int edgeInset{};
    int iconSize{};
    int iconGap{};
    int arrowInset{};
    int arrowSize{};
    int statusStep{};
    int trailing{};

    int textLeft(bool hasIcon) const
    {
        return this->edgeInset + (hasIcon ? this->iconSize + this->iconGap : 0);
    }
};

GroupHeaderGeometry groupHeaderGeometry(float scale, int statusCount)
{
    GroupHeaderGeometry result{
        .edgeInset = std::max(2, static_cast<int>(std::round(5 * scale))),
        .iconSize = std::max(5, static_cast<int>(std::round(11 * scale))),
        .iconGap = std::max(1, static_cast<int>(std::round(3 * scale))),
        .arrowInset = std::max(4, static_cast<int>(std::round(8 * scale))),
        .arrowSize = std::max(1, static_cast<int>(std::round(3 * scale))),
        .statusStep = std::max(4, static_cast<int>(std::round(9 * scale))),
    };
    const auto contentGap =
        std::max(1, static_cast<int>(std::round(3 * scale)));
    result.trailing = result.arrowInset + result.arrowSize + contentGap +
                      statusCount * result.statusStep;
    return result;
}

int groupHeaderStatusCount(bool selected, HighlightState highlightState)
{
    return selected && highlightState != HighlightState::None ? 1 : 0;
}

QImage trimTransparentMargins(QImage image)
{
    if (image.isNull() || !image.hasAlphaChannel())
    {
        return image;
    }

    image = image.convertToFormat(QImage::Format_ARGB32);
    auto left = image.width();
    auto top = image.height();
    auto right = -1;
    auto bottom = -1;
    for (auto y = 0; y < image.height(); ++y)
    {
        const auto *line =
            reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (auto x = 0; x < image.width(); ++x)
        {
            if (qAlpha(line[x]) <= 8)
            {
                continue;
            }
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }

    if (right < left || bottom < top)
    {
        return image;
    }

    const QRect content(left, top, right - left + 1, bottom - top + 1);
    return content == image.rect() ? image : image.copy(content);
}

QPainterPath tabBackgroundPath(const QRectF &rect, NotebookTabLocation location,
                               qreal radius, bool roundStart, bool roundEnd)
{
    if (location == NotebookTabLocation::Top)
    {
        return themeTopTabPath(rect, radius, roundStart, roundEnd);
    }

    QPainterPath path;
    if (radius <= 0 || (!roundStart && !roundEnd))
    {
        path.addRect(rect);
        return path;
    }

    const auto left = rect.left();
    const auto right = rect.right();
    const auto top = rect.top();
    const auto bottom = rect.bottom();

    const auto shoulder = themeTabCornerShoulder(radius, rect.height());
    constexpr qreal K = 0.58;
    switch (location)
    {
        case NotebookTabLocation::Top:
            break;
        case NotebookTabLocation::Bottom:
            path.moveTo(left, top);
            path.lineTo(left, roundStart ? bottom - shoulder : bottom);
            if (roundStart)
            {
                path.cubicTo(left, bottom - shoulder * (1.0 - K),
                             left + radius * (1.0 - K), bottom, left + radius,
                             bottom);
            }
            path.lineTo(roundEnd ? right - radius : right, bottom);
            if (roundEnd)
            {
                path.cubicTo(right - radius * (1.0 - K), bottom, right,
                             bottom - shoulder * (1.0 - K), right,
                             bottom - shoulder);
            }
            path.lineTo(right, top);
            break;
        case NotebookTabLocation::Left:
            path.moveTo(right, top);
            path.lineTo(roundStart ? left + shoulder : left, top);
            if (roundStart)
            {
                path.cubicTo(left + shoulder * (1.0 - K), top, left,
                             top + radius * (1.0 - K), left, top + radius);
            }
            path.lineTo(left, roundEnd ? bottom - radius : bottom);
            if (roundEnd)
            {
                path.cubicTo(left, bottom - radius * (1.0 - K),
                             left + shoulder * (1.0 - K), bottom,
                             left + shoulder, bottom);
            }
            path.lineTo(right, bottom);
            break;
        case NotebookTabLocation::Right:
            path.moveTo(left, top);
            path.lineTo(roundStart ? right - shoulder : right, top);
            if (roundStart)
            {
                path.cubicTo(right - shoulder * (1.0 - K), top, right,
                             top + radius * (1.0 - K), right, top + radius);
            }
            path.lineTo(right, roundEnd ? bottom - radius : bottom);
            if (roundEnd)
            {
                path.cubicTo(right, bottom - radius * (1.0 - K),
                             right - shoulder * (1.0 - K), bottom,
                             right - shoulder, bottom);
            }
            path.lineTo(left, bottom);
            break;
    }
    path.closeSubpath();
    return path;
}
}  // namespace

QPainterPath groupIconPath(const QString &icon, const QRectF &rect);
QPixmap loadGroupIconPixmap(const QString &path);

NotebookTab::NotebookTab(Notebook *notebook, Role role)
    : Button(notebook)
    , positionChangedAnimation_(this, "pos")
    , notebook_(notebook)
    , role_(role)
    , menu_(this)
{
    this->setContentCacheEnabled(false);
    this->setAcceptDrops(true);

    this->positionChangedAnimation_.setEasingCurve(
        QEasingCurve(QEasingCurve::InCubic));

    getSettings()->showTabCloseButton.connect(
        [this] {
            this->tabSizeChanged();
        },
        this->managedConnections_);
    getSettings()->hideTabCloseButtonWhenLocked.connect(
        [this] {
            this->tabSizeChanged();
        },
        this->managedConnections_);
    getSettings()->keepTabWidthWhenLocked.connect(
        [this] {
            this->tabSizeChanged();
        },
        this->managedConnections_);
    getSettings()->tabStyle.connect(
        [this] {
            this->tabSizeChanged();
        },
        this->managedConnections_);
    getSettings()->showTabLive.connect(
        [this](auto, auto) {
            this->update();
        },
        this->managedConnections_);
    getSettings()->thinTabLines.connect(
        [this] {
            this->update();
        },
        this->managedConnections_);
    getSettings()->colorTabHighlightsByMessage.connect(
        [this](auto, auto) {
            this->update();
        },
        this->managedConnections_);

    this->setMouseTracking(true);

    if (this->role_ == Role::GroupHeader)
    {
        this->setToolTip(
            "Click to expand or collapse this group. Right click for more "
            "options.");
        return;
    }

    this->menu_.addAction("Rename Tab", [this]() {
        this->showRenameDialog();
    });

    if (auto *recordings = getApp()->getChatRecordings())
    {
        auto *action =
            this->menu_.addAction("Record tab", this, [this, recordings] {
                recordings->toggle(dynamic_cast<SplitContainer *>(this->page));
            });
        connect(
            &this->menu_, &QMenu::aboutToShow, this,
            [this, recordings, action] {
                auto *tab = dynamic_cast<SplitContainer *>(this->page);
                const bool active = recordings->isActive(tab);
                action->setText(active ? "Stop tab recording" : "Record tab");
                action->setEnabled(active ? recordings->status(tab) != "Saving"
                                          : recordings->canStart(tab));
            });
        connect(recordings, &ChatRecordingController::stateChanged, this,
                [this] {
                    this->updateSize();
                    this->update();
                });
    }

    // XXX: this doesn't update after changing hotkeys

    this->menu_.addAction("Close Tab",
                          getApp()->getHotkeys()->getDisplaySequence(
                              HotkeyCategory::Window, "removeTab"),
                          [this]() {
                              this->notebook_->removePage(this->page);
                          });

    this->closeMultipleTabsMenu_ = new QMenu("Close Multiple Tabs", this);

    this->menu_.addMenu(this->closeMultipleTabsMenu_);
    getSettings()->tabDirection.connect(
        [this](int val) {
            this->recreateCloseMultipleTabsMenu(
                static_cast<NotebookTabLocation>(val));
        },
        this->signalHolder_);

    this->menu_.addAction(
        "Popup Tab",
        getApp()->getHotkeys()->getDisplaySequence(HotkeyCategory::Window,
                                                   "popup", {{"window"}}),
        [this]() {
            if (auto *container = dynamic_cast<SplitContainer *>(this->page))
            {
                container->popup();
            }
        });

    this->menu_.addAction("Duplicate Tab", [this]() {
        this->notebook_->duplicatePage(this->page);
    });

    this->tabGroupMenu_ = this->menu_.addMenu("Tab group");
    QObject::connect(this->tabGroupMenu_, &QMenu::aboutToShow, this, [this] {
        this->notebook_->populateTabGroupMenu(this->tabGroupMenu_, this->page);
    });

    this->alwaysVisibleAction_ = new QAction("Always visible", &this->menu_);
    this->alwaysVisibleAction_->setCheckable(true);
    this->alwaysVisibleAction_->setToolTip(
        "Keep this tab visible when Only show live tabs is enabled.");
    QObject::connect(this->alwaysVisibleAction_, &QAction::toggled, this,
                     [this](bool checked) {
                         this->setAlwaysVisible(checked);
                     });
    this->menu_.addAction(this->alwaysVisibleAction_);

    this->highlightNewMessagesAction_ =
        new QAction("Mark Tab as Unread on New Messages", &this->menu_);
    this->highlightNewMessagesAction_->setCheckable(true);
    this->highlightNewMessagesAction_->setChecked(this->highlightEnabled_);
    QObject::connect(this->highlightNewMessagesAction_, &QAction::triggered,
                     [this](bool checked) {
                         this->highlightEnabled_ = checked;
                     });
    this->menu_.addAction(this->highlightNewMessagesAction_);

    auto *tabColorMenu = this->menu_.addMenu("Tab Color");
    const std::vector<std::pair<QString, QColor>> tabColorPresets = {
        {"Blue", QColor(91, 157, 255)},
        {"Orange", QColor(255, 148, 67)},
        {"Red", QColor(244, 91, 91)},
        {"Yellow", QColor(244, 190, 72)},
        {"Green", QColor(76, 196, 120)},
        {"Purple", QColor(172, 123, 255)},
        {"Pink", QColor(238, 95, 161)},
        {"Cyan", QColor(73, 205, 214)},
    };
    for (const auto &[name, color] : tabColorPresets)
    {
        auto *action = tabColorMenu->addAction(tabColorIcon(color), name);
        QObject::connect(action, &QAction::triggered, this,
                         [this, color] {
                             this->setCustomTabColor(color);
                         });
    }

    tabColorMenu->addSeparator();
    auto *customColorAction =
        tabColorMenu->addAction("Custom Color...", this, [this] {
            auto initialColor = this->hasCustomTabColor()
                                    ? opaqueTabColor(this->getCustomTabColor())
                                    : QColor(255, 148, 67);
            auto *dialog = new ColorPickerDialog(initialColor, this);
            QObject::connect(dialog, &ColorPickerDialog::colorConfirmed, this,
                             [this](const QColor &color) {
                                 if (color.isValid())
                                 {
                                     this->setCustomTabColor(color);
                                 }
                             });
            dialog->show();
        });
    customColorAction->setIcon(tabColorIcon(QColor(255, 148, 67)));

    auto *resetTabColorAction =
        tabColorMenu->addAction("Reset to Default", this, [this] {
            this->resetCustomTabColor();
        });
    QObject::connect(tabColorMenu, &QMenu::aboutToShow, this,
                     [this, resetTabColorAction] {
                         resetTabColorAction->setEnabled(
                             this->hasCustomTabColor());
                     });

    this->menu_.addSeparator();

    this->notebook_->addNotebookActionsToMenu(&this->menu_, false);
}

void NotebookTab::recreateCloseMultipleTabsMenu(
    const NotebookTabLocation tabLocation)
{
    this->closeMultipleTabsMenu_->clear();

    this->closeMultipleTabsMenu_->addAction("Close All Visible Tabs", [this]() {
        auto reply = QMessageBox::question(
            this, "Close All Visible Tabs",
            "Are you sure you want to close all visible tabs?",
            QMessageBox::Yes | QMessageBox::Cancel);

        if (reply != QMessageBox::Yes)
        {
            return;
        }

        for (int i = this->notebook_->getPageCount() - 1; i >= 0; --i)
        {
            auto *page = this->notebook_->getPageAt(i);

            auto *container = dynamic_cast<SplitContainer *>(page);
            if (!container)
            {
                continue;
            }

            auto *tab = container->getTab();
            if (!tab || !tab->isVisible())
            {
                continue;
            }

            this->notebook_->removePage(page);
        }
    });

    QString beforeSelectedName;
    QString afterSelectedName;
    switch (tabLocation)
    {
        case Top:
        case Bottom:
            beforeSelectedName = "Left";
            afterSelectedName = "Right";
            break;
        case Left:
        case Right:
            beforeSelectedName = "Top";
            afterSelectedName = "Bottom";
            break;
    }

    this->closeTabsBeforeSelectedAction_ =
        this->closeMultipleTabsMenu_->addAction(
            "Close Visible Tabs to " + beforeSelectedName,
            [this, beforeSelectedName]() {
                auto reply = QMessageBox::question(
                    this, "Close Visible Tabs to " + beforeSelectedName,
                    "Are you sure you want to close all visible tabs to the " +
                        beforeSelectedName.toLower() + "?",
                    QMessageBox::Yes | QMessageBox::Cancel);

                if (reply != QMessageBox::Yes)
                {
                    return;
                }

                std::vector<QWidget *> pagesToRemove;
                for (int i = 0; i < this->notebook_->getPageCount(); ++i)
                {
                    auto *page = this->notebook_->getPageAt(i);
                    if (page == this->page)
                    {
                        break;
                    }

                    auto *container = dynamic_cast<SplitContainer *>(page);
                    if (!container)
                    {
                        continue;
                    }

                    auto *tab = container->getTab();
                    if (!tab || !tab->isVisible())
                    {
                        continue;
                    }

                    pagesToRemove.push_back(page);
                }

                for (auto *page : pagesToRemove)
                {
                    this->notebook_->removePage(page);
                }
            });

    this->closeTabsAfterSelectedAction_ =
        this->closeMultipleTabsMenu_->addAction(
            "Close Visible Tabs to " + afterSelectedName,
            [this, afterSelectedName]() {
                auto reply = QMessageBox::question(
                    this, "Close Visible Tabs to " + afterSelectedName,
                    "Are you sure you want to close all visible tabs to the " +
                        afterSelectedName + "?",
                    QMessageBox::Yes | QMessageBox::Cancel);

                if (reply != QMessageBox::Yes)
                {
                    return;
                }

                for (int i = this->notebook_->getPageCount() - 1; i >= 0; --i)
                {
                    auto *p = this->notebook_->getPageAt(i);
                    if (p == this->page)
                    {
                        break;
                    }

                    auto *container = dynamic_cast<SplitContainer *>(p);
                    if (!container)
                    {
                        continue;
                    }

                    auto *tab = container->getTab();
                    if (!tab || !tab->isVisible())
                    {
                        continue;
                    }

                    this->notebook_->removePage(p);
                }
            });

    this->closeMultipleTabsMenu_->addAction(
        "Close Other Visible Tabs", [this]() {
            auto reply = QMessageBox::question(
                this, "Close Other Visible Tabs",
                "Are you sure you want to close all other visible tabs?",
                QMessageBox::Yes | QMessageBox::Cancel);

            if (reply != QMessageBox::Yes)
            {
                return;
            }

            for (int i = this->notebook_->getPageCount() - 1; i >= 0; --i)
            {
                auto *p = this->notebook_->getPageAt(i);
                if (p == this->page)
                {
                    continue;
                }

                auto *container = dynamic_cast<SplitContainer *>(p);
                if (!container)
                {
                    continue;
                }

                auto *tab = container->getTab();
                if (!tab || !tab->isVisible())
                {
                    continue;
                }

                this->notebook_->removePage(p);
            }
        });
}

void NotebookTab::showRenameDialog()
{
    const QPointer<NotebookTab> self(this);
    QPointer<QDialog> dialog = new QDialog(this);

    auto *vbox = new QVBoxLayout;

    auto *lineEdit = new QLineEdit;
    lineEdit->setText(this->getCustomTitle());
    lineEdit->setPlaceholderText(this->getDefaultTitle());
    lineEdit->selectAll();

    vbox->addWidget(new QLabel("Name:"));
    vbox->addWidget(lineEdit);
    vbox->addStretch(1);

    auto *buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    vbox->addWidget(buttonBox);
    dialog->setLayout(vbox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, [dialog] {
        dialog->accept();
        dialog->close();
    });

    QObject::connect(buttonBox, &QDialogButtonBox::rejected, [dialog] {
        dialog->reject();
        dialog->close();
    });

    dialog->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    dialog->setMinimumSize(dialog->minimumSizeHint().width() + 50,
                           dialog->minimumSizeHint().height() + 10);

    dialog->setWindowFlags(
        (dialog->windowFlags() & ~(Qt::WindowContextHelpButtonHint)) |
        Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);

    dialog->setWindowTitle("Rename Tab");
    installMoltorinoDialogTheme(dialog);

    const auto result = dialog->exec();
    if (!self || !dialog)
    {
        return;
    }
    if (result == QDialog::Accepted)
    {
        QString newTitle = lineEdit->text();
        this->setCustomTitle(newTitle);
    }
    delete dialog;
}

void NotebookTab::themeChangedEvent()
{
    this->update();

    //    this->setMouseEffectColor(QColor("#999"));
    this->setMouseEffectColor(this->theme->tabs.regular.text);
}

void NotebookTab::growWidth(int width)
{
    if (this->growWidth_ != width)
    {
        this->growWidth_ = width;
        this->updateSize();
    }
    else
    {
        this->growWidth_ = width;
    }
}

int NotebookTab::normalTabWidth() const
{
    return this->normalTabWidthForHeight(this->height());
}

int NotebookTab::normalTabWidthForHeight(int height) const
{
    float scale = this->scale();
    int width = 0;

    auto metrics =
        getApp()->getFonts()->getFontMetrics(FontStyle::UiTabs, scale);

    float compactDivider = getCompactDivider(getSettings()->tabStyle);
    if (this->role_ == Role::GroupHeader)
    {
        const auto hasIcon = this->groupIcon_ != "none";
        const auto statusCount =
            groupHeaderStatusCount(this->selected_, this->highlightState_);
        const auto geometry = groupHeaderGeometry(scale, statusCount);
        width = metrics.horizontalAdvance(this->getTitle()) +
                geometry.textLeft(hasIcon) + geometry.trailing;
    }
    else if (this->reservesXButtonSpace())
    {
        width = static_cast<int>(metrics.horizontalAdvance(this->getTitle()) +
                                 (32 / compactDivider * scale));
    }
    else
    {
        width = static_cast<int>(metrics.horizontalAdvance(this->getTitle()) +
                                 (16 / compactDivider * scale));
    }

    if (this->role_ == Role::Page)
    {
        if (auto *recordings = getApp()->getChatRecordings();
            recordings &&
            recordings->isActive(dynamic_cast<SplitContainer *>(this->page)))
        {
            width += static_cast<int>(12 * scale);
        }
    }

    if (static_cast<float>(height) > 150 * scale)
    {
        width = height;
    }
    else
    {
        const auto maximum = this->role_ == Role::GroupHeader ? 260 : 150;
        width = std::clamp(width, height, static_cast<int>(maximum * scale));
    }

    return width;
}

void NotebookTab::updateSize()
{
    float scale = this->scale();
    auto height = static_cast<int>(NOTEBOOK_TAB_HEIGHT * scale);
    int width = this->normalTabWidthForHeight(height);

    if (width < this->growWidth_)
    {
        width = this->growWidth_;
    }

    if (this->width() != width || this->height() != height)
    {
        this->resize(width, height);
        this->notebook_->refresh();
    }
}

const QString &NotebookTab::getCustomTitle() const
{
    return this->customTitle_;
}

void NotebookTab::setCustomTitle(const QString &newTitle)
{
    if (this->customTitle_ != newTitle)
    {
        this->customTitle_ = newTitle;
        this->titleUpdated();
    }
}

void NotebookTab::resetCustomTitle()
{
    this->setCustomTitle(QString());
}

bool NotebookTab::hasCustomTitle() const
{
    return !this->customTitle_.isEmpty();
}

void NotebookTab::setCustomTabColor(const QColor &color)
{
    if (!color.isValid())
    {
        return;
    }

    const auto normalizedColor = opaqueTabColor(color);
    if (this->customTabColor_ != normalizedColor)
    {
        this->customTabColor_ = normalizedColor;
        this->tabColorUpdated();
    }
}

void NotebookTab::resetCustomTabColor()
{
    if (this->customTabColor_.isValid())
    {
        this->customTabColor_ = QColor();
        this->tabColorUpdated();
    }
}

bool NotebookTab::hasCustomTabColor() const
{
    return this->customTabColor_.isValid();
}

const QColor &NotebookTab::getCustomTabColor() const
{
    return this->customTabColor_;
}

void NotebookTab::setDefaultTitle(const QString &title)
{
    if (this->defaultTitle_ != title)
    {
        this->defaultTitle_ = title;

        if (this->customTitle_.isEmpty())
        {
            this->titleUpdated();
        }
    }
}

const QString &NotebookTab::getDefaultTitle() const
{
    return this->defaultTitle_;
}

const QString &NotebookTab::getTitle() const
{
    return this->customTitle_.isEmpty() ? this->defaultTitle_
                                        : this->customTitle_;
}

NotebookTab::Role NotebookTab::role() const
{
    return this->role_;
}

const QString &NotebookTab::groupId() const
{
    return this->groupId_;
}

void NotebookTab::setGroupId(const QString &groupId)
{
    this->groupId_ = groupId;
}

bool NotebookTab::setGroupHeaderState(
    const QString &title, int memberCount, bool collapsed, bool selected,
    bool live, HighlightState highlightState, const QString &colorMode,
    const QColor &color, const QString &icon, const QString &customIconPath,
    bool muted, bool openMenuOnClick, bool forceCustomIconReload)
{
    assert(this->role_ == Role::GroupHeader);

    const auto normalizedColorMode =
        colorMode == "none" || colorMode == "custom" ? colorMode
                                                     : QStringLiteral("theme");
    const auto normalizedIcon = icon == "star" || icon == "heart" ||
                                        icon == "bell" || icon == "shield" ||
                                        icon == "none" || icon == "custom"
                                    ? icon
                                    : QStringLiteral("folder");
    const auto oldStatusCount =
        groupHeaderStatusCount(this->selected_, this->highlightState_);
    const auto newStatusCount =
        groupHeaderStatusCount(selected, highlightState);
    const bool iconChanged = forceCustomIconReload ||
                             this->groupIcon_ != normalizedIcon ||
                             this->groupCustomIconPath_ != customIconPath;
    const bool sizeChanged = this->defaultTitle_ != title ||
                             this->groupMemberCount_ != memberCount ||
                             this->groupIcon_ != normalizedIcon ||
                             oldStatusCount != newStatusCount;
    this->defaultTitle_ = title;
    this->groupMemberCount_ = memberCount;
    this->groupCollapsed_ = collapsed;
    this->groupMuted_ = muted;
    this->selected_ = selected;
    this->isLive_ = live;
    this->isRerun_ = false;

    this->highlightState_ = highlightState;
    this->groupColorMode_ = normalizedColorMode;
    this->customTabColor_ =
        normalizedColorMode == "custom" && color.isValid() ? color : QColor();
    this->groupIcon_ = normalizedIcon;
    this->groupCustomIconPath_ = customIconPath;
    if (iconChanged)
    {
        this->groupCustomIcon_ = normalizedIcon == "custom"
                                     ? loadGroupIconPixmap(customIconPath)
                                     : QPixmap();
    }
    this->setAccessibleName(title);
    const auto clickHint =
        openMenuOnClick ? QStringLiteral("Click to open the tab list.")
                        : QStringLiteral("Click to %1 the group.")
                              .arg(collapsed ? QStringLiteral("expand")
                                             : QStringLiteral("collapse"));
    const auto tabCount = memberCount == 1
                              ? QStringLiteral("1 tab")
                              : QStringLiteral("%1 tabs").arg(memberCount);
    auto toolTip = QStringLiteral("%1 (%2)\n%3\nRight click for more options.")
                       .arg(title, tabCount, clickHint);
    if (muted)
    {
        toolTip.append(QStringLiteral("\nAlerts are muted for this group."));
    }
    this->setToolTip(toolTip);

    if (sizeChanged)
    {
        const auto height =
            static_cast<int>(NOTEBOOK_TAB_HEIGHT * this->scale());
        this->resize(this->normalTabWidthForHeight(height), height);
    }
    this->update();
    return sizeChanged;
}

int NotebookTab::ungroupedIndex() const
{
    return this->ungroupedIndex_;
}

void NotebookTab::setUngroupedIndex(int index)
{
    this->ungroupedIndex_ = index;
}

QPainterPath groupIconPath(const QString &icon, const QRectF &rect)
{
    QPainterPath path;
    const auto x = rect.left();
    const auto y = rect.top();
    const auto w = rect.width();
    const auto h = rect.height();

    if (icon == "folder")
    {
        path.addRoundedRect(QRectF(x, y + h * 0.24, w, h * 0.7), w * 0.12,
                            w * 0.12);
        path.addRoundedRect(
            QRectF(x + w * 0.08, y + h * 0.08, w * 0.48, h * 0.34), w * 0.1,
            w * 0.1);
    }
    else if (icon == "star")
    {
        constexpr auto POINTS = 10;
        const auto center = rect.center();
        const auto outer = std::min(w, h) * 0.48;
        const auto inner = outer * 0.45;
        for (int i = 0; i < POINTS; ++i)
        {
            const auto angle =
                -std::numbers::pi / 2.0 + i * std::numbers::pi / 5.0;
            const auto radius = i % 2 == 0 ? outer : inner;
            const QPointF point(center.x() + std::cos(angle) * radius,
                                center.y() + std::sin(angle) * radius);
            if (i == 0)
            {
                path.moveTo(point);
            }
            else
            {
                path.lineTo(point);
            }
        }
        path.closeSubpath();
    }
    else if (icon == "heart")
    {
        path.moveTo(x + w * 0.5, y + h * 0.92);
        path.cubicTo(x + w * 0.34, y + h * 0.76, x + w * 0.04, y + h * 0.55,
                     x + w * 0.08, y + h * 0.28);
        path.cubicTo(x + w * 0.12, y + h * 0.02, x + w * 0.42, y + h * 0.03,
                     x + w * 0.5, y + h * 0.23);
        path.cubicTo(x + w * 0.58, y + h * 0.03, x + w * 0.88, y + h * 0.02,
                     x + w * 0.92, y + h * 0.28);
        path.cubicTo(x + w * 0.96, y + h * 0.55, x + w * 0.66, y + h * 0.76,
                     x + w * 0.5, y + h * 0.92);
    }
    else if (icon == "bell")
    {
        path.moveTo(x + w * 0.18, y + h * 0.72);
        path.cubicTo(x + w * 0.28, y + h * 0.6, x + w * 0.25, y + h * 0.42,
                     x + w * 0.32, y + h * 0.25);
        path.cubicTo(x + w * 0.4, y + h * 0.08, x + w * 0.6, y + h * 0.08,
                     x + w * 0.68, y + h * 0.25);
        path.cubicTo(x + w * 0.75, y + h * 0.42, x + w * 0.72, y + h * 0.6,
                     x + w * 0.82, y + h * 0.72);
        path.closeSubpath();
        path.addEllipse(QRectF(x + w * 0.42, y + h * 0.76, w * 0.16, h * 0.14));
    }
    else if (icon == "shield")
    {
        path.moveTo(x + w * 0.5, y + h * 0.04);
        path.lineTo(x + w * 0.88, y + h * 0.2);
        path.lineTo(x + w * 0.82, y + h * 0.62);
        path.cubicTo(x + w * 0.77, y + h * 0.78, x + w * 0.61, y + h * 0.9,
                     x + w * 0.5, y + h * 0.96);
        path.cubicTo(x + w * 0.39, y + h * 0.9, x + w * 0.23, y + h * 0.78,
                     x + w * 0.18, y + h * 0.62);
        path.lineTo(x + w * 0.12, y + h * 0.2);
        path.closeSubpath();
    }
    return path;
}

QPixmap loadGroupIconPixmap(const QString &path)
{
    const QFileInfo source(path);
    if (!source.isFile() || source.size() <= 0 ||
        source.size() > 10 * 1024 * 1024)
    {
        return {};
    }

    QImageReader reader(source.absoluteFilePath());
    reader.setAutoTransform(true);
    const auto sourceSize = reader.size();
    if (sourceSize.isValid() &&
        (sourceSize.width() > 64 || sourceSize.height() > 64))
    {
        reader.setScaledSize(sourceSize.scaled(64, 64, Qt::KeepAspectRatio));
    }
    auto image = trimTransparentMargins(reader.read());
    if (image.isNull())
    {
        return {};
    }
    if (image.width() > 64 || image.height() > 64)
    {
        image =
            image.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return QPixmap::fromImage(std::move(image));
}

void NotebookTab::setGroupDropTarget(bool value)
{
    if (this->groupDropTarget_ == value)
    {
        return;
    }
    this->groupDropTarget_ = value;
    this->update();
}

void NotebookTab::setGroupMuted(bool value, bool notifyNotebook)
{
    if (this->groupMuted_ == value)
    {
        return;
    }

    this->groupMuted_ = value;
    if (value)
    {
        this->highlightSources_.clear();
        this->highlightColor_.reset();
        this->highlightState_ = HighlightState::None;
    }
    this->update();
    if (notifyNotebook)
    {
        this->notebook_->tabStatusChanged(this);
    }
}

bool NotebookTab::isGroupMuted() const
{
    return this->groupMuted_;
}

void NotebookTab::setVisibleEdgeFlags(bool first, bool last)
{
    if (this->isFirstVisible_ == first && this->isLastVisible_ == last)
    {
        return;
    }

    this->isFirstVisible_ = first;
    this->isLastVisible_ = last;
    this->update();
}

void NotebookTab::setAlwaysVisible(bool value)
{
    if (this->alwaysVisible_ == value)
    {
        return;
    }

    const bool shouldSelectAnotherTab =
        !value && this->isSelected() && !this->isLive() &&
        getSettings()->tabVisibility.getEnum() ==
            NotebookTabVisibility::LiveOnly;

    this->alwaysVisible_ = value;
    if (this->alwaysVisibleAction_ != nullptr &&
        this->alwaysVisibleAction_->isChecked() != value)
    {
        this->alwaysVisibleAction_->setChecked(value);
    }
    getApp()->getWindows()->queueSave();

    if (shouldSelectAnotherTab)
    {
        this->notebook_->selectNextTab();
        if (this->isSelected())
        {
            this->notebook_->select(nullptr);
        }
    }
    this->notebook_->refresh();
}

bool NotebookTab::isAlwaysVisible() const
{
    return this->alwaysVisible_;
}

bool NotebookTab::reservesXButtonSpace() const
{
    return this->role_ == Role::Page && getSettings()->showTabCloseButton &&
           this->notebook_->getAllowUserTabManagement() &&
           (!this->notebook_->isNotebookLayoutLocked() ||
            !getSettings()->hideTabCloseButtonWhenLocked ||
            getSettings()->keepTabWidthWhenLocked);
}

void NotebookTab::titleUpdated()
{
    // Queue up save because: Tab title changed
    getApp()->getWindows()->queueSave();
    this->notebook_->refresh();
    this->updateSize();
    this->update();
}

void NotebookTab::tabColorUpdated()
{
    getApp()->getWindows()->queueSave();
    this->update();
}

bool NotebookTab::isSelected() const
{
    return this->selected_;
}

void NotebookTab::removeHighlightStateChangeSources(
    const HighlightSources &toRemove)
{
    for (const auto &[source, _] : toRemove)
    {
        this->removeHighlightSource(source);
    }
}

void NotebookTab::removeHighlightSource(
    const ChannelView::ChannelViewID &source)
{
    this->highlightSources_.erase(source);
}

void NotebookTab::newHighlightSourceAdded(const ChannelView &channelViewSource)
{
    auto channelViewId = channelViewSource.getID();
    this->removeHighlightSource(channelViewId);
    this->updateHighlightStateDueSourcesChange();

    for (auto *window : getApp()->getWindows()->windows())
    {
        auto &splitNotebook = window->getNotebook();
        for (int i = 0; i < splitNotebook.getPageCount(); ++i)
        {
            auto *splitContainer =
                dynamic_cast<SplitContainer *>(splitNotebook.getPageAt(i));
            if (splitContainer)
            {
                auto *tab = splitContainer->getTab();
                if (tab && tab != this)
                {
                    tab->removeHighlightSource(channelViewId);
                    tab->updateHighlightStateDueSourcesChange();
                }
            }
        }
    }
}

void NotebookTab::updateHighlightStateDueSourcesChange()
{
    auto newState = HighlightState::None;
    std::shared_ptr<QColor> newColor;
    std::size_t newestSequence = 0;

    for (const auto &[_, source] : this->highlightSources_)
    {
        if (source.state == HighlightState::Highlighted)
        {
            newState = HighlightState::Highlighted;
            if (source.sequence >= newestSequence)
            {
                newestSequence = source.sequence;
                newColor = source.color;
            }
        }
        else if (source.state == HighlightState::NewMessage &&
                 newState != HighlightState::Highlighted)
        {
            newState = HighlightState::NewMessage;
        }
    }

    if (newState != HighlightState::Highlighted)
    {
        newColor.reset();
    }

    if (this->highlightState_ != newState ||
        !colorsMatch(this->highlightColor_, newColor))
    {
        this->highlightState_ = newState;
        this->highlightColor_ = std::move(newColor);
        this->update();
        this->notebook_->tabStatusChanged(this);
    }
}

void NotebookTab::copyHighlightStateAndSourcesFrom(const NotebookTab *sourceTab)
{
    if (this->isSelected() || this->groupMuted_)
    {
        assert(this->highlightSources_.empty());
        assert(this->highlightState_ == HighlightState::None);
        return;
    }

    this->highlightSources_ = sourceTab->highlightSources_;
    this->highlightColor_ = sourceTab->highlightColor_;
    this->lastHighlightSequence_ = sourceTab->lastHighlightSequence_;

    if (!this->highlightEnabled_ &&
        sourceTab->highlightState_ == HighlightState::NewMessage)
    {
        this->highlightColor_.reset();
        return;
    }

    if ((this->highlightState_ == sourceTab->highlightState_ &&
         colorsMatch(this->highlightColor_, sourceTab->highlightColor_)) ||
        this->highlightState_ == HighlightState::Highlighted)
    {
        return;
    }

    this->highlightState_ = sourceTab->highlightState_;
    this->update();
    this->notebook_->tabStatusChanged(this);
}

void NotebookTab::setSelected(bool value)
{
    this->selected_ = value;

    if (value)
    {
        for (auto *window : getApp()->getWindows()->windows())
        {
            auto &splitNotebook = window->getNotebook();
            for (int i = 0; i < splitNotebook.getPageCount(); ++i)
            {
                auto *splitContainer =
                    dynamic_cast<SplitContainer *>(splitNotebook.getPageAt(i));
                if (splitContainer)
                {
                    auto *tab = splitContainer->getTab();
                    if (tab && tab != this)
                    {
                        tab->removeHighlightStateChangeSources(
                            this->highlightSources_);
                        tab->updateHighlightStateDueSourcesChange();
                    }
                }
            }
        }
    }

    this->highlightSources_.clear();
    this->highlightColor_.reset();
    this->highlightState_ = HighlightState::None;

    this->update();
    this->notebook_->tabStatusChanged(this);
}

void NotebookTab::setInLastRow(bool value)
{
    if (this->isInLastRow_ != value)
    {
        this->isInLastRow_ = value;
        this->update();
    }
}

void NotebookTab::setTabLocation(NotebookTabLocation location)
{
    if (this->tabLocation_ != location)
    {
        this->tabLocation_ = location;
        this->update();
    }
}

bool NotebookTab::setRerun(bool isRerun)
{
    if (this->isRerun_ != isRerun)
    {
        this->isRerun_ = isRerun;
        this->update();
        this->notebook_->tabStatusChanged(this);
        return true;
    }

    return false;
}

bool NotebookTab::setLive(bool isLive)
{
    if (this->isLive_ != isLive)
    {
        this->isLive_ = isLive;
        this->update();
        this->notebook_->tabStatusChanged(this);
        return true;
    }

    return false;
}

bool NotebookTab::isLive() const
{
    return this->isLive_;
}

HighlightState NotebookTab::highlightState() const
{
    return this->highlightState_;
}

void NotebookTab::setHighlightState(HighlightState newHighlightStyle)
{
    if (this->isSelected() || this->groupMuted_)
    {
        assert(this->highlightSources_.empty());
        assert(this->highlightState_ == HighlightState::None);
        return;
    }

    this->highlightSources_.clear();
    this->highlightColor_.reset();

    if (!this->highlightEnabled_ &&
        newHighlightStyle == HighlightState::NewMessage)
    {
        return;
    }

    if (this->highlightState_ == newHighlightStyle ||
        this->highlightState_ == HighlightState::Highlighted)
    {
        return;
    }

    this->highlightState_ = newHighlightStyle;
    this->update();
    this->notebook_->tabStatusChanged(this);
}

void NotebookTab::updateHighlightState(const TabHighlight &highlight,
                                       const ChannelView &channelViewSource)
{
    const auto newHighlightStyle = highlight.state;

    if (this->isSelected())
    {
        assert(this->highlightSources_.empty());
        assert(this->highlightState_ == HighlightState::None);
        return;
    }

    if (!this->shouldMessageHighlight(channelViewSource))
    {
        return;
    }

    if (!this->highlightEnabled_ &&
        newHighlightStyle == HighlightState::NewMessage)
    {
        return;
    }

    // message is highlighting unvisible tab

    auto channelViewId = channelViewSource.getID();

    switch (newHighlightStyle)
    {
        case HighlightState::Highlighted:
            // override lower states
            this->highlightSources_.insert_or_assign(
                channelViewId,
                HighlightSource{
                    .state = newHighlightStyle,
                    .color = highlight.color,
                    .sequence = ++this->lastHighlightSequence_,
                });
            break;
        case HighlightState::NewMessage: {
            // only insert if no state already there to avoid overriding
            if (!this->highlightSources_.contains(channelViewId))
            {
                this->highlightSources_.emplace(
                    channelViewId,
                    HighlightSource{
                        .state = newHighlightStyle,
                        .sequence = ++this->lastHighlightSequence_,
                    });
            }
            break;
        }
        case HighlightState::None:
            break;
    }

    this->updateHighlightStateDueSourcesChange();
}

bool NotebookTab::shouldMessageHighlight(
    const ChannelView &channelViewSource) const
{
    if (this->groupMuted_)
    {
        return false;
    }

    for (auto *window : getApp()->getWindows()->windows())
    {
        auto *visibleSplitContainer = window->getNotebook().getSelectedPage();
        if (visibleSplitContainer != nullptr)
        {
            const auto &visibleSplits = visibleSplitContainer->getSplits();
            for (const auto &visibleSplit : visibleSplits)
            {
                if (channelViewSource.getID() ==
                    visibleSplit->getChannelView().getID())
                {
                    return false;
                }
            }
        }
    }

    return true;
}

void NotebookTab::setHighlightsEnabled(const bool &newVal)
{
    if (this->highlightNewMessagesAction_)
    {
        this->highlightNewMessagesAction_->setChecked(newVal);
    }
    this->highlightEnabled_ = newVal;
}

bool NotebookTab::hasHighlightsEnabled() const
{
    return this->highlightEnabled_;
}

QRect NotebookTab::getDesiredRect() const
{
    return QRect(this->positionAnimationDesiredPoint_, this->size());
}

void NotebookTab::tabSizeChanged()
{
    this->mouseDownX_ = false;
    this->mouseOverX_ = false;
    this->updateSize();

    this->notebook_->refresh();
    this->update();
}

void NotebookTab::moveAnimated(QPoint targetPos, bool animated)
{
    this->positionAnimationDesiredPoint_ = targetPos;

    if (!animated || !this->notebook_->isVisible())
    {
        this->move(targetPos);
        return;
    }

    if (this->positionChangedAnimation_.state() ==
            QAbstractAnimation::Running &&
        this->positionChangedAnimation_.endValue() == targetPos)
    {
        return;
    }

    this->positionChangedAnimation_.stop();
    this->positionChangedAnimation_.setDuration(75);
    this->positionChangedAnimation_.setStartValue(this->pos());
    this->positionChangedAnimation_.setEndValue(targetPos);
    this->positionChangedAnimation_.start();
}

void NotebookTab::paintEvent(QPaintEvent *)
{
    auto *app = getApp();
    QPainter painter(this);
    float scale = this->scale();

    painter.setFont(app->getFonts()->getFont(FontStyle::UiTabs, scale));
    auto metrics = app->getFonts()->getFontMetrics(FontStyle::UiTabs, scale);

    int height = int(scale * NOTEBOOK_TAB_HEIGHT);

    // select the right tab colors
    Theme::TabColors colors;

    if (this->selected_)
    {
        colors = this->theme->tabs.selected;
    }
    else if (this->highlightState_ == HighlightState::Highlighted)
    {
        colors = this->theme->tabs.highlighted;
    }
    else if (this->highlightState_ == HighlightState::NewMessage)
    {
        colors = this->theme->tabs.newMessage;
    }
    else
    {
        colors = this->theme->tabs.regular;
    }

    bool windowFocused = this->window() == QApplication::activeWindow();

    QBrush tabBackground = /*this->mouseOver_ ? colors.backgrounds.hover
                                 :*/
        (windowFocused ? colors.backgrounds.regular
                       : colors.backgrounds.unfocused);

    const auto tabLineWidth = getSettings()->thinTabLines ? 1 : 2;
    auto selectionOffset = ceil((this->selected_ ? 0 : tabLineWidth) * scale);

    // fill the tab background
    auto bgRect = this->rect();
    switch (this->tabLocation_)
    {
        case NotebookTabLocation::Top:
            bgRect.setTop(selectionOffset);
            break;
        case NotebookTabLocation::Left:
            bgRect.setLeft(selectionOffset);
            break;
        case NotebookTabLocation::Right:
            bgRect.setRight(bgRect.width() - selectionOffset);
            break;
        case NotebookTabLocation::Bottom:
            bgRect.setBottom(bgRect.height() - selectionOffset);
            break;
    }

    const auto &appearance = this->theme->customization;
    const auto tabRadius =
        std::min(appearance.tabCornerRadius * scale, 10.0F * scale);
    const bool individual = appearance.tabShape == ThemeTabShape::Individual;
    auto shapedRect = QRectF(bgRect);
    if (individual)
    {
        const auto gap = appearance.tabSpacing * scale;
        if (this->tabLocation_ == NotebookTabLocation::Top ||
            this->tabLocation_ == NotebookTabLocation::Bottom)
        {
            shapedRect.adjust(gap / 2, 0, -gap / 2, 0);
        }
        else
        {
            shapedRect.adjust(0, gap / 2, 0, -gap / 2);
        }
    }
    const auto backgroundPath =
        tabBackgroundPath(shapedRect, this->tabLocation_, tabRadius,
                          individual || this->isFirstVisible_,
                          individual || this->isLastVisible_);
    painter.setRenderHint(QPainter::Antialiasing, tabRadius > 0);
    painter.fillPath(backgroundPath, tabBackground);

    QColor groupTint;
    if (this->role_ == Role::GroupHeader && this->groupColorMode_ == "theme")
    {
        groupTint = this->theme->accent;
    }
    else if (this->hasCustomTabColor())
    {
        groupTint = this->customTabColor_;
    }
    if (groupTint.isValid())
    {
        painter.fillPath(
            backgroundPath,
            tabColorFill(groupTint, this->selected_, windowFocused));
    }

    if (this->groupDropTarget_)
    {
        auto dropFill = this->theme->accent;
        dropFill.setAlpha(48);
        painter.fillPath(backgroundPath, dropFill);

        auto dropBorder = this->theme->accent;
        dropBorder.setAlpha(230);
        painter.setPen(QPen(dropBorder, std::max(1.0F, 2 * scale)));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(backgroundPath);
    }

    // draw color indicator line
    auto lineThickness =
        ceil((tabLineWidth + (this->selected_ ? 1 : 0)) * scale);
    auto lineColor = this->mouseOver_ ? colors.line.hover
                                      : (windowFocused ? colors.line.regular
                                                       : colors.line.unfocused);
    if (this->highlightState_ == HighlightState::Highlighted &&
        getSettings()->colorTabHighlightsByMessage && this->highlightColor_)
    {
        lineColor = tabHighlightLineColor(*this->highlightColor_,
                                          windowFocused || this->mouseOver_);
    }

    QRect lineRect;
    switch (this->tabLocation_)
    {
        case NotebookTabLocation::Top:
            lineRect =
                QRect(bgRect.left(), bgRect.y(), bgRect.width(), lineThickness);
            break;
        case NotebookTabLocation::Left:
            lineRect =
                QRect(bgRect.x(), bgRect.top(), lineThickness, bgRect.height());
            break;
        case NotebookTabLocation::Right:
            lineRect = QRect(bgRect.right() - lineThickness, bgRect.top(),
                             lineThickness, bgRect.height());
            break;
        case NotebookTabLocation::Bottom:
            lineRect = QRect(bgRect.left(), bgRect.bottom() - lineThickness,
                             bgRect.width(), lineThickness);
            break;
    }

    painter.save();
    painter.setClipPath(backgroundPath);
    painter.fillRect(lineRect, lineColor);
    painter.restore();

    const auto groupStatusCount =
        groupHeaderStatusCount(this->selected_, this->highlightState_);
    const auto groupGeometry = groupHeaderGeometry(scale, groupStatusCount);

    // draw live indicator
    if ((this->isLive_ || this->isRerun_) && getSettings()->showTabLive)
    {
        // Live overrides rerun
        QBrush b;
        if (this->isLive_)
        {
            painter.setPen(this->theme->tabs.liveIndicator);
            b.setColor(this->theme->tabs.liveIndicator);
        }
        else
        {
            painter.setPen(this->theme->tabs.rerunIndicator);
            b.setColor(this->theme->tabs.rerunIndicator);
        }

        painter.setRenderHint(QPainter::Antialiasing);
        b.setStyle(Qt::SolidPattern);
        painter.setBrush(b);

        auto x = this->width() - (7 * scale);
        auto y = 4 * scale;
        auto diameter = 4 * scale;
        QRect liveIndicatorRect(x, y, diameter, diameter);
        translateRectForLocation(liveIndicatorRect, this->tabLocation_,
                                 this->selected_ ? 0 : -1);
        painter.drawEllipse(liveIndicatorRect);
    }

    if (this->role_ == Role::GroupHeader && this->selected_ &&
        this->highlightState_ != HighlightState::None)
    {
        const bool liveDotVisible =
            (this->isLive_ || this->isRerun_) && getSettings()->showTabLive;
        const auto &stateColors =
            this->highlightState_ == HighlightState::Highlighted
                ? this->theme->tabs.highlighted
                : this->theme->tabs.newMessage;
        painter.setPen(Qt::NoPen);
        painter.setBrush(stateColors.line.regular);
        const auto diameter = std::max(3, static_cast<int>(4 * scale));
        const auto statusIndex = liveDotVisible ? 2 : 1;
        const auto center = this->width() - groupGeometry.arrowInset -
                            statusIndex * groupGeometry.statusStep;
        const auto x = center - diameter / 2;
        const auto y = (height - diameter) / 2;
        painter.drawEllipse(QRect(x, y, diameter, diameter));
    }

    // set the pen color
    painter.setPen(colors.text);

    float compactDivider = getCompactDivider(getSettings()->tabStyle);
    // set area for text
    int rectW =
        (!getSettings()->showTabCloseButton ? 0
                                            : int(16 * scale / compactDivider));
    QRect rect(0, 0, this->width() - rectW, height);

    // draw text
    int offset = int(scale * 4 / compactDivider);
    QRect textRect(offset, 0, this->width() - offset - offset, height);
    if (this->role_ == Role::Page)
    {
        if (auto *recordings = app->getChatRecordings())
        {
            const auto state =
                recordings->status(dynamic_cast<SplitContainer *>(this->page));
            if (!state.isEmpty())
            {
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(state == "Recording" ? QColor("#e95762")
                                                      : QColor("#d6a34a"));
                painter.drawEllipse(QRectF(textRect.left() + 2 * scale,
                                           (height - 6 * scale) / 2, 6 * scale,
                                           6 * scale));
                painter.restore();
                textRect.setLeft(textRect.left() +
                                 static_cast<int>(12 * scale));
            }
        }
    }
    const auto textPositionOffset =
        this->role_ == Role::GroupHeader
            ? -std::clamp(static_cast<int>(std::round(scale)) + 1, 1, 3)
            : (this->selected_ ? -1 : -2);
    translateRectForLocation(textRect, this->tabLocation_, textPositionOffset);

    if (this->reservesXButtonSpace() && (this->mouseOver_ || this->selected_))
    {
        textRect.setRight(textRect.right() - this->height() / 2);
    }

    if (this->role_ == Role::GroupHeader)
    {
        const auto hasIcon = this->groupIcon_ != "none";
        const auto iconSize = groupGeometry.iconSize;
        const auto iconLeft = groupGeometry.edgeInset;
        const auto centerY = textRect.center().y();
        QRect iconRect(0, 0, iconSize, iconSize);
        iconRect.moveCenter(QPoint(iconLeft + (iconSize - 1) / 2, centerY));

        if (hasIcon)
        {
            if (this->groupIcon_ == "custom" &&
                !this->groupCustomIcon_.isNull())
            {
                painter.save();
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                const auto target = this->groupCustomIcon_.size().scaled(
                    iconRect.size(), Qt::KeepAspectRatio);
                QRect targetRect(QPoint(), target);
                targetRect.moveCenter(iconRect.center());
                painter.drawPixmap(targetRect, this->groupCustomIcon_,
                                   this->groupCustomIcon_.rect());
                painter.restore();
            }
            else
            {
                auto icon = groupIconPath(this->groupIcon_ == "custom"
                                              ? QStringLiteral("folder")
                                              : this->groupIcon_,
                                          iconRect);
                painter.fillPath(icon, colors.text);
            }

            if (this->groupMuted_)
            {
                painter.save();
                painter.setPen(QPen(tabBackground, std::max(1.0F, 1.5F * scale),
                                    Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(
                    iconRect.bottomLeft() +
                        QPointF(iconSize * 0.12, -iconSize * 0.08),
                    iconRect.topRight() +
                        QPointF(-iconSize * 0.08, iconSize * 0.08));
                painter.restore();
            }

            textRect.setLeft(groupGeometry.textLeft(true));
        }
        else
        {
            textRect.setLeft(groupGeometry.textLeft(false));
        }
        textRect.setRight(this->width() - groupGeometry.trailing - 1);

        QPainterPath chevron;
        const auto arrowX = this->width() - groupGeometry.arrowInset;

        const auto arrowY = static_cast<qreal>(centerY);
        const auto arrow = groupGeometry.arrowSize;
        if (this->groupCollapsed_)
        {
            chevron.moveTo(arrowX - arrow / 2.0, arrowY - arrow);
            chevron.lineTo(arrowX + arrow / 2.0, arrowY);
            chevron.lineTo(arrowX - arrow / 2.0, arrowY + arrow);
        }
        else
        {
            chevron.moveTo(arrowX - arrow, arrowY - arrow / 2.0);
            chevron.lineTo(arrowX, arrowY + arrow / 2.0);
            chevron.lineTo(arrowX + arrow, arrowY - arrow / 2.0);
        }

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colors.text, std::max(1.0F, scale), Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(chevron);
    }

    const auto displayTitle =
        this->role_ == Role::GroupHeader
            ? metrics.elidedText(this->getTitle(), Qt::ElideRight,
                                 std::max(0, textRect.width()))
            : this->getTitle();
    int width = metrics.horizontalAdvance(displayTitle);
    const bool alignLeft = this->role_ == Role::GroupHeader ||
                           width > textRect.width();
    Qt::Alignment alignment = alignLeft ? Qt::AlignLeft | Qt::AlignVCenter
                                        : Qt::AlignHCenter | Qt::AlignVCenter;

    QTextOption option(alignment);
    option.setWrapMode(QTextOption::NoWrap);
    painter.drawText(textRect, displayTitle, option);

    // draw close x
    if (this->shouldDrawXButton())
    {
        painter.setRenderHint(QPainter::Antialiasing, false);

        QRect xRect = this->getXRect();
        if (!xRect.isNull())
        {
            painter.setBrush(QColor("#fff"));

            if (this->mouseOverX_)
            {
                painter.fillRect(xRect, QColor(0, 0, 0, 64));

                if (this->mouseDownX_)
                {
                    painter.fillRect(xRect, QColor(0, 0, 0, 64));
                }
            }

            int a = static_cast<int>(scale * 4);

            painter.drawLine(xRect.topLeft() + QPoint(a, a),
                             xRect.bottomRight() + QPoint(-a, -a));
            painter.drawLine(xRect.topRight() + QPoint(-a, a),
                             xRect.bottomLeft() + QPoint(a, -a));
        }
    }

    // draw mouse over effect
    if (!this->selected_)
    {
        this->fancyPaint(painter);
    }

    // draw line at border
    if (!this->selected_ && this->isInLastRow_)
    {
        QRect borderRect;
        switch (this->tabLocation_)
        {
            case NotebookTabLocation::Top:
                borderRect = QRect(0, this->height() - 1, this->width(), 1);
                break;
            case NotebookTabLocation::Left:
                borderRect = QRect(this->width() - 1, 0, 1, this->height());
                break;
            case NotebookTabLocation::Right:
                borderRect = QRect(0, 0, 1, this->height());
                break;
            case NotebookTabLocation::Bottom:
                borderRect = QRect(0, 0, this->width(), 1);
                break;
        }
        painter.fillRect(borderRect, app->getThemes()->window.background);
    }
}

bool NotebookTab::hasXButton() const
{
    return this->reservesXButtonSpace() &&
           (!this->notebook_->isNotebookLayoutLocked() ||
            !getSettings()->hideTabCloseButtonWhenLocked);
}

bool NotebookTab::shouldDrawXButton() const
{
    return this->hasXButton() && (this->mouseOver_ || this->selected_);
}

void NotebookTab::mousePressEvent(QMouseEvent *event)
{
    if (this->role_ == Role::GroupHeader)
    {
        if (event->button() == Qt::LeftButton)
        {
            this->mouseDown_ = true;
            this->dragMoved_ = false;
            this->dragActive_ = false;
            this->dragStartGlobal_ = event->globalPosition().toPoint();
            this->update();
        }
        else if (event->button() == Qt::RightButton)
        {
            this->notebook_->showTabGroupMenu(
                this->groupId_,
                event->globalPosition().toPoint() + QPoint(0, 8));
        }
        return;
    }

    if (event->button() == Qt::LeftButton)
    {
        this->mouseDown_ = true;
        this->dragMoved_ = false;
        this->dragActive_ = false;
        this->dragStartGlobal_ = event->globalPosition().toPoint();
        this->mouseDownX_ =
            this->hasXButton() && this->getXRect().contains(event->pos());
        const auto canDrag = !this->mouseDownX_ &&
                             this->notebook_->getAllowUserTabManagement() &&
                             !this->notebook_->isNotebookLayoutLocked();
        this->groupDragRequested_ =
            canDrag && event->modifiers().testFlag(Qt::ShiftModifier);
        this->selectionDeferred_ = canDrag && !this->selected_;

        if (!this->selectionDeferred_)
        {
            this->notebook_->select(this->page);
        }
    }

    this->update();

    if (this->notebook_->getAllowUserTabManagement())
    {
        switch (event->button())
        {
            case Qt::RightButton: {
                this->menu_.popup(event->globalPosition().toPoint() +
                                  QPoint(0, 8));

                const int visibleTabCount =
                    this->notebook_->getVisibleTabCount();
                const int selectedTabIndex =
                    this->notebook_->visibleIndexOf(this->page);

                this->closeMultipleTabsMenu_->setEnabled(visibleTabCount > 1);

                this->closeTabsBeforeSelectedAction_->setEnabled(
                    selectedTabIndex > 0);
                this->closeTabsAfterSelectedAction_->setEnabled(
                    selectedTabIndex != -1 &&
                    selectedTabIndex < (visibleTabCount - 1));
            }
            break;
            default:;
        }
    }
}

void NotebookTab::mouseReleaseEvent(QMouseEvent *event)
{
    if (this->role_ == Role::GroupHeader)
    {
        const auto toggle = event->button() == Qt::LeftButton &&
                            this->mouseDown_ && !this->dragMoved_ &&
                            this->rect().contains(event->pos());
        this->mouseDown_ = false;
        this->dragMoved_ = false;
        this->update();
        if (toggle)
        {
            this->notebook_->activateTabGroup(
                this->groupId_,
                this->mapToGlobal(this->rect().bottomLeft() + QPoint(0, 4)));
        }
        return;
    }

    const auto wasMouseDown = this->mouseDown_;
    const auto wasDragged = this->dragMoved_;
    const auto selectDeferredTab =
        event->button() == Qt::LeftButton && this->selectionDeferred_ &&
        wasMouseDown && !wasDragged && this->rect().contains(event->pos());
    const bool grouped = event->button() == Qt::LeftButton &&
                         this->mouseDown_ && this->dragActive_ &&
                         this->notebook_->commitTabGroupDrop(this->page);
    this->notebook_->cancelTabGroupDrop();
    this->unsetCursor();
    this->mouseDown_ = false;
    this->dragMoved_ = false;
    this->dragActive_ = false;
    this->groupDragRequested_ = false;
    this->selectionDeferred_ = false;
    if (grouped)
    {
        this->mouseDownX_ = false;
        this->update();
        return;
    }

    if (selectDeferredTab)
    {
        this->notebook_->select(this->page);
    }

    auto removeThisPage = [this] {
        auto reply = QMessageBox::question(
            this, "Remove this tab",
            "Are you sure that you want to remove this tab?",
            QMessageBox::Yes | QMessageBox::Cancel);

        if (reply == QMessageBox::Yes)
        {
            this->notebook_->removePage(this->page);
        }
    };

    if (event->button() == Qt::MiddleButton &&
        this->notebook_->getAllowUserTabManagement())
    {
        if (this->rect().contains(event->pos()))
        {
            removeThisPage();
        }
    }
    else
    {
        if (this->hasXButton() && this->mouseDownX_ &&
            this->getXRect().contains(event->pos()))
        {
            this->mouseDownX_ = false;

            removeThisPage();
        }
        else
        {
            this->update();
        }
    }
}

void NotebookTab::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (this->role_ == Role::GroupHeader)
    {
        this->mouseDown_ = false;
        this->dragMoved_ = true;
        event->accept();
        return;
    }

    const auto canRenameTab = this->notebook_->getAllowUserTabManagement() &&
                              getSettings()->disableTabRenamingOnClick == false;

    if (event->button() == Qt::LeftButton && canRenameTab)
    {
        this->showRenameDialog();
    }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void NotebookTab::enterEvent(QEnterEvent *event)
#else
void NotebookTab::enterEvent(QEvent *event)
#endif
{
    this->mouseOver_ = true;

    this->update();

    Button::enterEvent(event);
}

void NotebookTab::leaveEvent(QEvent *event)
{
    this->mouseOverX_ = false;
    this->mouseOver_ = false;

    this->update();

    Button::leaveEvent(event);
}

void NotebookTab::dragEnterEvent(QDragEnterEvent *event)
{
    if (this->role_ == Role::GroupHeader)
    {
        return;
    }

    if (!event->mimeData()->hasFormat("chatterino/split"))
    {
        return;
    }

    if (!isDraggingSplit())
    {
        // Ensure dragging a split from a different Chatterino instance doesn't switch tabs around
        return;
    }

    event->acceptProposedAction();

    if (this->notebook_->getAllowUserTabManagement())
    {
        this->notebook_->select(this->page);
    }
}

void NotebookTab::dropEvent(QDropEvent *event)
{
    if (this->role_ == Role::GroupHeader)
    {
        return;
    }

    if (!event->mimeData()->hasFormat("chatterino/split"))
    {
        return;
    }

    if (!isDraggingSplit())
    {
        // Ensure dragging a split from a different Chatterino instance doesn't switch tabs around
        return;
    }

    auto *draggedSplit = dynamic_cast<Split *>(event->source());
    if (!draggedSplit)
    {
        qCDebug(chatterinoWidget)
            << "Dropped something that wasn't a split onto a notebook button";
        return;
    }

    if (auto *container = dynamic_cast<SplitContainer *>(this->page))
    {
        event->acceptProposedAction();
        container->insertSplit(draggedSplit);
    }
}

void NotebookTab::mouseMoveEvent(QMouseEvent *event)
{
    const bool overX =
        this->hasXButton() && this->getXRect().contains(event->pos());
    if (overX != this->mouseOverX_)
    {
        // Over X state has been changed (we either left or entered it;
        this->mouseOverX_ = overX;
        this->update();
    }

    const auto globalPoint = event->globalPosition().toPoint();
    const auto relPoint = this->notebook_->mapFromGlobal(globalPoint);
    const auto crossedDragThreshold =
        (globalPoint - this->dragStartGlobal_).manhattanLength() >=
        QApplication::startDragDistance();

    if (this->role_ == Role::GroupHeader)
    {
        if (this->mouseDown_ && crossedDragThreshold &&
            this->notebook_->getAllowUserTabManagement() &&
            !this->notebook_->isNotebookLayoutLocked())
        {
            this->dragMoved_ = true;
            this->dragActive_ = true;
            int index = -1;
            if (this->notebook_->tabAt(relPoint, index))
            {
                this->notebook_->moveTabGroup(this->groupId_, index);
            }
        }
        Button::mouseMoveEvent(event);
        return;
    }

    if (this->mouseDown_ && crossedDragThreshold &&
        this->notebook_->getAllowUserTabManagement() &&
        !this->notebook_->isNotebookLayoutLocked())
    {
        this->dragMoved_ = true;
        this->dragActive_ = true;
        this->mouseDownX_ = false;
        int index;
        QWidget *clickedPage = this->notebook_->tabAt(relPoint, index);

        if (this->groupDragRequested_)
        {
            if (clickedPage != nullptr && clickedPage != this->page)
            {
                this->notebook_->previewTabGroupDrop(this->page, clickedPage);
                this->setCursor(Qt::DragCopyCursor);
            }
            else
            {
                this->notebook_->cancelTabGroupDrop();
                this->unsetCursor();
            }
        }
        else
        {
            this->notebook_->cancelTabGroupDrop();
            this->unsetCursor();
            if (clickedPage != nullptr && clickedPage != this->page)
            {
                this->notebook_->rearrangePage(this->page, index);
            }
        }
    }

    Button::mouseMoveEvent(event);
}

void NotebookTab::wheelEvent(QWheelEvent *event)
{
    const auto defaultMouseDelta = 120;
    const auto verticalDelta = event->angleDelta().y();
    const auto selectTab = [this](int delta) {
        delta > 0 ? this->notebook_->selectPreviousTab()
                  : this->notebook_->selectNextTab();
    };
    // If it's true
    // Then the user uses the trackpad or perhaps the most accurate mouse
    // Which has small delta.
    if (std::abs(verticalDelta) < defaultMouseDelta)
    {
        this->mouseWheelDelta_ += verticalDelta;
        if (std::abs(this->mouseWheelDelta_) >= defaultMouseDelta)
        {
            selectTab(this->mouseWheelDelta_);
            this->mouseWheelDelta_ = 0;
        }
    }
    else
    {
        selectTab(verticalDelta);
    }
}

void NotebookTab::update()
{
    Button::update();
}

QRect NotebookTab::getXRect() const
{
    QRect rect = this->rect();
    float s = this->scale();
    int size = static_cast<int>(16 * s);

    int centerAdjustment = this->tabLocation_ == NotebookTabLocation::Top
                               ? (size / 3)   // slightly off true center
                               : (size / 2);  // true center

    float compactReducer = getCompactReducer(getSettings()->tabStyle);
    QRect xRect(rect.right() - static_cast<int>((20 - compactReducer) * s),
                rect.center().y() - centerAdjustment, size, size);

    if (this->selected_)
    {
        translateRectForLocation(xRect, this->tabLocation_, 1);
    }

    return xRect;
}

}  // namespace chatterino
