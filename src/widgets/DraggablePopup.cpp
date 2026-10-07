// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/DraggablePopup.hpp"

#include "buttons/SvgButton.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/InvisibleSizeGrip.hpp"
#include "widgets/Label.hpp"
#include "widgets/MarkdownLabel.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScreen>
#include <QShowEvent>
#include <QWindow>

#include <algorithm>
#include <chrono>

namespace chatterino {

namespace {

bool isInteractiveDragTarget(QWidget *widget, const QWidget *popup)
{
    for (auto *current = widget; current != nullptr && current != popup;
         current = current->parentWidget())
    {
        const bool tooltipLabel =
            dynamic_cast<const Label *>(current) != nullptr &&
            dynamic_cast<const MarkdownLabel *>(current) == nullptr;
        if (current->focusPolicy() != Qt::NoFocus ||
            (current->hasMouseTracking() && !tooltipLabel))
        {
            return true;
        }
    }
    return false;
}

constexpr FlagsEnum<BaseWindow::Flags> POPUP_FLAGS{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    BaseWindow::Dialog,
#endif
    BaseWindow::EnableCustomFrame,
};
constexpr FlagsEnum<BaseWindow::Flags> POPUP_FLAGS_CLOSE_AUTOMATICALLY{
#ifdef Q_OS_MACOS
    BaseWindow::Dialog,
#endif
    BaseWindow::EnableCustomFrame,
    BaseWindow::Frameless,
    BaseWindow::FramelessDraggable,
};

}

DraggablePopup::DraggablePopup(bool closeAutomatically, QWidget *parent)
    : BaseWindow(
          (closeAutomatically ? POPUP_FLAGS_CLOSE_AUTOMATICALLY : POPUP_FLAGS) |
              BaseWindow::DisableLayoutSave |
              BaseWindow::ClearBuffersOnDpiChange |
              BaseWindow::CloseOnMinimize | BaseWindow::DisableMaximize,
          parent)
    , lifetimeHack_(std::make_shared<bool>(false))
    , saveSizeTimer_(this)
    , closeAutomatically_(closeAutomatically)
    , dragTimer_(this)

{
    this->saveSizeTimer_.setSingleShot(true);
    this->saveSizeTimer_.setInterval(500);
    this->saveSizeTimer_.callOnTimeout(this, &DraggablePopup::savePopupSize);
    if (closeAutomatically)
    {
        this->windowDeactivateAction = WindowDeactivateAction::Delete;
    }
    else
    {
        this->setAttribute(Qt::WA_DeleteOnClose);
    }

    this->dragTimer_.callOnTimeout(
        [this, hack = std::weak_ptr<bool>(this->lifetimeHack_)] {
            if (!hack.lock())
            {

                return;
            }

            if (!this->isMoving_)
            {
                return;
            }

            this->move(this->requestedDragPos_);
        });
}

void DraggablePopup::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::MouseButton::LeftButton)
    {
        return;
    }

    auto *target = QApplication::widgetAt(event->globalPosition().toPoint());
    if (target != nullptr && this->isAncestorOf(target) &&
        isInteractiveDragTarget(target, this))
    {
        event->accept();
        return;
    }

    if (!this->windowHandle() || !this->windowHandle()->startSystemMove())
    {
        this->dragTimer_.start(std::chrono::milliseconds(17));
        this->startPosDrag_ = event->pos();
        this->movingRelativePos = event->position();
    }
}

DraggablePopup::~DraggablePopup()
{
    this->savePopupSize();
}

void DraggablePopup::enableResize(QSizeSetting &setting, QSize defaultSize,
                                  bool rememberSize)
{
    this->sizeSetting_ = &setting;
    this->defaultSize_ = defaultSize;
    this->rememberSize_ = rememberSize;
    if (rememberSize)
    {
        this->customSize_ = setting.getValue();
    }
    this->sizeGrip_ = new InvisibleSizeGrip(this);
    this->sizeGrip_->setObjectName("PopupSizeGrip");
    this->sizeGrip_->installEventFilter(this);
    getSettings()->allowPopupResize.connect(
        [this](bool enabled) {
            this->customSize_ = enabled && this->rememberSize_
                                    ? this->sizeSetting_->getValue()
                                    : QSize{};
            if (this->suggestedSize_.isValid())
            {
                this->applyPopupSize(this->suggestedSize_ * this->scale());
            }
            this->positionSizeGrip();
        },
        this->resizeConnections_, false);

    if (defaultSize.isValid() || this->hasCustomSize())
    {
        this->applyPopupSize(defaultSize * this->scale());
    }
}

bool DraggablePopup::hasCustomSize() const
{
    return getSettings()->allowPopupResize && this->customSize_.width() > 0 &&
           this->customSize_.height() > 0;
}

QSize DraggablePopup::preferredSize(QSize fallback) const
{
    if (!this->hasCustomSize())
    {
        return fallback;
    }

    return this->customSize_.boundedTo(QSize(10000, 10000)) * this->scale();
}

void DraggablePopup::applyPopupSize(QSize suggestedSize)
{
    if (this->isMaximized() || this->isFullScreen() || this->isMinimized())
    {
        return;
    }
    const QScopedValueRollback guard(this->applyingSize_, true);
    this->suggestedSize_ = suggestedSize / this->scale();
    auto minimum = this->defaultSize_.isValid()
                       ? this->defaultSize_ * this->scale()
                       : suggestedSize;
    auto wanted = this->preferredSize(suggestedSize).expandedTo(minimum);
    if (auto *screen = this->screen())
    {
        const auto frame = (this->frameGeometry().size() - this->size())
                               .expandedTo(QSize(0, 0));
        const int margin = 8;
        const auto available = (screen->availableGeometry().size() - frame -
                                QSize(margin, margin) * 2)
                                   .expandedTo(QSize(1, 1));
        minimum = minimum.boundedTo(available);
        wanted = wanted.boundedTo(available);
    }
    if (getSettings()->allowPopupResize)
    {
        this->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        this->setMinimumSize(minimum.expandedTo(QSize(1, 1)));
        this->resize(wanted.expandedTo(QSize(1, 1)));
    }
    else
    {
        this->setFixedSize(wanted.expandedTo(QSize(1, 1)));
    }
}

void DraggablePopup::positionSizeGrip()
{
    if (this->sizeGrip_)
    {
        const int edge = std::max(12, qRound(16 * this->scale()));
        this->sizeGrip_->setGeometry(this->width() - edge,
                                     this->height() - edge, edge, edge);
        this->sizeGrip_->raise();
        this->sizeGrip_->setVisible(getSettings()->allowPopupResize &&
                                    !this->isMaximized() &&
                                    !this->isFullScreen());
    }
}

void DraggablePopup::resizeEvent(QResizeEvent *event)
{
    BaseWindow::resizeEvent(event);
    this->positionSizeGrip();
    if (this->sizeSetting_ && getSettings()->allowPopupResize &&
        this->isVisible() && !this->applyingSize_ && !this->isMaximized() &&
        !this->isMinimized() && !this->isFullScreen() &&
        (this->resizing_ || event->spontaneous()))
    {
        this->customSize_ = this->size() / this->scale();
        if (this->rememberSize_)
        {
            *this->sizeSetting_ = this->customSize_;
            this->sizeDirty_ = true;
            this->saveSizeTimer_.start();
        }
    }
}

void DraggablePopup::showEvent(QShowEvent *event)
{
    BaseWindow::showEvent(event);
    if (this->sizeSetting_)
    {
        this->applyPopupSize(this->suggestedSize_.isValid()
                                 ? this->suggestedSize_ * this->scale()
                                 : this->size());
        this->positionSizeGrip();
    }
}

void DraggablePopup::scaleChangedEvent(float scale)
{
    BaseWindow::scaleChangedEvent(scale);
    if (this->hasCustomSize())
    {
        QTimer::singleShot(0, this, [this] {
            this->applyPopupSize(this->suggestedSize_ * this->scale());
            this->positionSizeGrip();
        });
    }
}

bool DraggablePopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->sizeGrip_)
    {
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        {
            this->resizing_ = true;
        }
        else if (event->type() == QEvent::MouseButtonRelease)
        {
            this->resizing_ = false;
            this->savePopupSize();
        }
    }
    return BaseWindow::eventFilter(watched, event);
}

bool DraggablePopup::event(QEvent *event)
{
    if (event->type() == QEvent::DeferredDelete)
    {
        return BaseWindow::event(event);
    }
    const bool handled = BaseWindow::event(event);
    if (event->type() == QEvent::WindowStateChange)
    {
        this->positionSizeGrip();
    }
    if (this->sizeSetting_ && event->type() == QEvent::ScreenChangeInternal)
    {
        QTimer::singleShot(0, this, [this] {
            const auto previousSize = this->size();
            this->applyPopupSize(this->suggestedSize_ * this->scale());
            if (this->isVisible() && this->size() != previousSize)
            {
                this->moveTo(this->pos(),
                             widgets::BoundsChecking::DesiredPosition);
            }
        });
    }
    return handled;
}

void DraggablePopup::savePopupSize()
{
    if (this->sizeDirty_)
    {
        this->sizeDirty_ = false;
        getSettings()->requestSave();
    }
}

void DraggablePopup::mouseReleaseEvent(QMouseEvent *event)
{
    this->dragTimer_.stop();
    this->isMoving_ = false;
}

void DraggablePopup::mouseMoveEvent(QMouseEvent *event)
{

    auto movePos = event->pos() - this->startPosDrag_;
    if (this->isMoving_ || movePos.manhattanLength() > 10.0)
    {
        this->requestedDragPos_ =
            (event->globalPosition() - this->movingRelativePos).toPoint();
        this->isMoving_ = true;
    }
}

void DraggablePopup::togglePinned()
{
    if (!this->pinButton_)
    {
        return;
    }
    this->isPinned_ = !this->isPinned_;
    if (this->isPinned_)
    {
        this->windowDeactivateAction = WindowDeactivateAction::Nothing;
        this->pinButton_->setSource(this->pinEnabledSource_);
    }
    else
    {
        this->windowDeactivateAction = WindowDeactivateAction::Delete;
        this->pinButton_->setSource(this->pinDisabledSource_);
    }
}
Button *DraggablePopup::createPinButton()
{
    this->pinButton_ = new SvgButton(this->pinDisabledSource_, this, {3, 3});
    this->pinButton_->setScaleIndependentSize(18, 18);
    this->pinButton_->setToolTip("Pin Window");

    QObject::connect(this->pinButton_, &Button::leftClicked, this,
                     &DraggablePopup::togglePinned);
    return this->pinButton_;
}

bool DraggablePopup::ensurePinned()
{
    if (this->closeAutomatically_ && this->pinButton_ && !this->isPinned_)
    {
        this->togglePinned();
        return true;
    }
    return false;
}

}
