// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SettingsDialogTab.hpp"

#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/SettingsTheme.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QPainter>
#include <QStyleOption>

#include <cmath>

namespace chatterino {

SettingsDialogTab::SettingsDialogTab(SettingsDialog *_dialog,
                                     std::function<SettingsPage *()> _lazyPage,
                                     const QString &name, QString imageFileName,
                                     SettingsTabId id, float iconOpticalScale)
    : BaseWidget(_dialog)
    , dialog_(_dialog)
    , lazyPage_(std::move(_lazyPage))
    , id_(id)
    , name_(name)
    , iconOpticalScale_(iconOpticalScale)
{
    this->ui_.labelText = name;
    this->ui_.icon.addFile(imageFileName);

    this->setCursor(QCursor(Qt::PointingHandCursor));

    this->themeChangedEvent();
}

void SettingsDialogTab::setSelected(bool _selected)
{
    if (this->selected_ == _selected)
    {
        return;
    }

    //    height: <checkbox-size>px;

    this->selected_ = _selected;
    this->themeChangedEvent();
    this->update();
    this->selectedChanged(this->selected_);
}

void SettingsDialogTab::themeChangedEvent()
{
    const auto &appearance = settingsTheme();
    this->setStyleSheet(QStringLiteral("background:%1; color:%2;")
                            .arg(this->selected_
                                     ? appearance.selectionSurface.name()
                                     : QStringLiteral("transparent"),
                                 this->selected_ ? appearance.accent.name()
                                                 : appearance.text.name()));
}

SettingsPage *SettingsDialogTab::page()
{
    if (this->page_)
    {
        return this->page_;
    }

    this->page_ = this->lazyPage_();
    this->page_->setTab(this);
    return this->page_;
}

SettingsPage *SettingsDialogTab::createdPage() const
{
    return this->page_;
}

void SettingsDialogTab::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    QStyleOption opt;
    opt.initFrom(this);

    this->style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);

    const auto baseIconSize =
        static_cast<int>(std::lround(20.F * this->scale()));
    const auto iconSize =
        static_cast<int>(std::lround(baseIconSize * this->iconOpticalScale_));
    const auto iconPad = (this->height() - iconSize) / 2;
    QPixmap pixmap = this->ui_.icon.pixmap(QSize(iconSize, iconSize));

    painter.drawPixmap(iconPad, iconPad, pixmap);

    const auto basePad = (this->height() - baseIconSize) / 2;
    const auto textPad = (3 * basePad) + baseIconSize;

    this->style()->drawItemText(
        &painter, QRect(textPad, 0, this->width() - textPad, this->height()),
        Qt::AlignLeft | Qt::AlignVCenter, this->palette(), false,
        this->ui_.labelText);
}

void SettingsDialogTab::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }

    this->dialog_->selectTab(this);

    this->setFocus();
}

const QString &SettingsDialogTab::name() const
{
    return this->name_;
}

SettingsTabId SettingsDialogTab::id() const
{
    return this->id_;
}

}  // namespace chatterino
