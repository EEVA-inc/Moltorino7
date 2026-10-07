// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/DrawnButton.hpp"

#include "singletons/Theme.hpp"

#include <QPainter>
#include <QPainterPath>

namespace chatterino {

DrawnButton::DrawnButton(Symbol symbol_, Options options, BaseWidget *parent)
    : Button(parent)
    , symbol(symbol_)
{
    this->setContentCacheEnabled(true);

    this->setOptions(options);

    this->themeChangedEvent();
}

void DrawnButton::setOptions(Options options_)
{
    this->options = options_;

    this->invalidateContent();
}

void DrawnButton::themeChangedEvent()
{
    Button::themeChangedEvent();

    auto &o = this->symbolOptions;

    switch (this->symbol)
    {
        case Symbol::Plus: {
            o.padding = 4;
            o.thickness = 1;

            o.foreground = this->theme->tabs.regular.text;
            o.foregroundHover = this->theme->window.text;
        }
        break;

        case Symbol::Kebab: {
            o.padding = 2;
            o.thickness = 2;

            o.foreground = this->theme->tabs.regular.text;
            o.foregroundHover = this->theme->window.text;
        }
        break;

        case Symbol::FolderPlus: {
            o.padding = 4;
            o.thickness = 1;
            o.foreground = this->theme->tabs.regular.text;
            o.foregroundHover = this->theme->window.text;
        }
        break;
    }

    this->invalidateContent();
}

void DrawnButton::mouseOverUpdated()
{
    this->invalidateContent();
}

void DrawnButton::paintContent(QPainter &painter)
{
    QColor fg;
    QColor bg;

    if (this->mouseOver())
    {
        bg = this->getBackgroundHover();
        fg = this->getForegroundHover();
    }
    else
    {
        bg = this->getBackground();
        fg = this->getForeground();
    }

    if (bg.isValid())
    {
        painter.fillRect(this->rect(), bg);
    }

    auto thickness = this->getThickness();
    auto padding = this->getPadding();

    switch (this->symbol)
    {
        case Symbol::Plus: {
            QPen pen;
            pen.setColor(fg);
            pen.setWidth(thickness);
            painter.setPen(pen);

            auto innerSize = this->rect().size();
            innerSize.setHeight(innerSize.width());
            QRect inner;
            inner.setSize(innerSize);
            inner.moveCenter(this->rect().center());
            inner = inner.marginsRemoved({padding, padding, padding, padding});

            if ((inner.width() % 2) == 0)
            {
                inner.setRight(inner.right() + 1);
            }
            if ((inner.height() % 2) == 0)
            {
                inner.setTop(inner.top() - 1);
            }

            auto top = inner.top();
            auto bottom = inner.bottom();
            auto center = inner.center();
            auto left = inner.left();
            auto right = inner.right();

            QLine vertical(center.x(), top, center.x(), bottom);
            painter.drawLine(vertical);
            QLine horizontal(left, center.y(), right, center.y());
            painter.drawLine(horizontal);
        }
        break;

        case Symbol::Kebab: {
            QPen pen;
            pen.setColor(fg);
            pen.setWidth(thickness);
            painter.setPen(pen);

            QRect centerBox;
            centerBox.setSize({thickness, thickness});
            centerBox.moveCenter(this->rect().center());

            painter.fillRect(centerBox, fg);

            auto bottomBox = centerBox.translated(0, thickness + padding);
            painter.fillRect(bottomBox, fg);

            auto topBox = centerBox.translated(0, -(thickness + padding));
            painter.fillRect(topBox, fg);
        }
        break;

        case Symbol::FolderPlus: {
            painter.setRenderHint(QPainter::Antialiasing);

            auto inner = this->rect().marginsRemoved(
                {padding, padding, padding, padding});
            const auto lineWidth = std::max(1, thickness);
            QPen pen(fg, lineWidth, Qt::SolidLine, Qt::RoundCap,
                     Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);

            const auto folderTop = inner.top() + inner.height() * 0.25;
            const auto folderBottom = inner.bottom() - inner.height() * 0.06;
            const auto tabRight = inner.left() + inner.width() * 0.46;
            QPainterPath folder;
            folder.moveTo(inner.left(), folderTop);
            folder.lineTo(inner.left() + inner.width() * 0.12,
                          inner.top() + inner.height() * 0.12);
            folder.lineTo(tabRight, inner.top() + inner.height() * 0.12);
            folder.lineTo(tabRight + inner.width() * 0.12, folderTop);
            folder.lineTo(inner.right(), folderTop);
            folder.lineTo(inner.right(), folderBottom);
            folder.lineTo(inner.left(), folderBottom);
            folder.closeSubpath();
            painter.drawPath(folder);

            const auto plusCenter = QPointF(
                inner.left() + inner.width() * 0.69,
                inner.top() + inner.height() * 0.60);
            const auto arm = inner.width() * 0.16;
            painter.drawLine(QPointF(plusCenter.x() - arm, plusCenter.y()),
                             QPointF(plusCenter.x() + arm, plusCenter.y()));
            painter.drawLine(QPointF(plusCenter.x(), plusCenter.y() - arm),
                             QPointF(plusCenter.x(), plusCenter.y() + arm));
        }
        break;
    }
}

int DrawnButton::getPadding() const
{
    auto v =
        this->options.padding.value_or(this->symbolOptions.padding.value_or(0));

    return static_cast<int>(std::round(static_cast<float>(v) * this->scale()));
}

int DrawnButton::getThickness() const
{
    auto v = this->options.thickness.value_or(
        this->symbolOptions.thickness.value_or(1));

    return std::max(1, static_cast<int>(
                           std::round(static_cast<double>(v) * this->scale())));
}

QColor DrawnButton::getBackground() const
{
    auto v = this->options.background.value_or(
        this->symbolOptions.background.value_or(QColor()));

    return v;
}

QColor DrawnButton::getBackgroundHover() const
{
    auto v = this->options.backgroundHover.value_or(
        this->symbolOptions.backgroundHover.value_or(QColor()));

    return v;
}

QColor DrawnButton::getForeground() const
{
    auto v = this->options.foreground.value_or(
        this->symbolOptions.foreground.value_or(QColor()));

    assert(v.isValid());

    return v;
}

QColor DrawnButton::getForegroundHover() const
{
    auto v = this->options.foregroundHover.value_or(
        this->symbolOptions.foregroundHover.value_or(QColor()));

    assert(v.isValid());

    return v;
}

}
