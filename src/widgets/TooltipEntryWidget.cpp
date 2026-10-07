// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/TooltipEntryWidget.hpp"

#include <QVBoxLayout>

namespace chatterino {

TooltipEntryWidget::TooltipEntryWidget(QWidget *parent)
    : TooltipEntryWidget(nullptr, "", 0, 0, parent)
{
}

TooltipEntryWidget::TooltipEntryWidget(ImagePtr image, const QString &text,
                                       int customWidth, int customHeight,
                                       QWidget *parent)
    : QWidget(parent)
    , image_(image)
    , customSize(customWidth, customHeight)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    this->setLayout(layout);

    this->displayImage_ = new QLabel();
    this->displayImage_->setAlignment(Qt::AlignHCenter);
    this->displayImage_->setStyleSheet("background: transparent");
    this->displayImage_->hide();
    this->displayText_ = new QLabel(text);
    this->displayText_->setAlignment(Qt::AlignHCenter);
    this->displayText_->setStyleSheet("background: transparent");

    layout->addWidget(this->displayImage_);
    layout->addWidget(this->displayText_);
}

void TooltipEntryWidget::setWordWrap(bool wrap)
{
    this->displayText_->setWordWrap(wrap);
}

void TooltipEntryWidget::setImageScale(int w, int h)
{
    if (this->customSize == QSize{w, h})
    {
        return;
    }
    this->customSize = QSize{w, h};
    this->displayedFrameKey_ = 0;
    this->refreshPixmap();
}

void TooltipEntryWidget::setText(const QString &text)
{
    this->displayText_->setText(text);
}

void TooltipEntryWidget::setImage(ImagePtr image)
{
    if (this->image_ == image)
    {
        return;
    }

    this->clearImage();
    this->image_ = std::move(image);
    this->refreshPixmap();
}

void TooltipEntryWidget::clearImage()
{
    this->displayImage_->hide();
    this->displayImage_->clear();
    this->image_ = nullptr;
    this->customSize = {};
    this->displayedFrameKey_ = 0;
    this->displayedPixelRatio_ = 0;
    this->attemptRefresh_ = false;
}

bool TooltipEntryWidget::refreshPixmap()
{
    if (!this->image_)
    {
        return false;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (!pixmap)
    {
        this->attemptRefresh_ = true;
        return false;
    }
    const auto pixelRatio = this->devicePixelRatioF();
    const auto frameKey = pixmap->cacheKey();
    if (frameKey == this->displayedFrameKey_ &&
        pixelRatio == this->displayedPixelRatio_)
    {
        this->attemptRefresh_ = false;
        return true;
    }
    if (!this->customSize.isEmpty())
    {
        *pixmap = pixmap->scaled(this->customSize * pixelRatio,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    pixmap->setDevicePixelRatio(pixelRatio);
    this->displayImage_->setPixmap(*pixmap);
    this->displayedFrameKey_ = frameKey;
    this->displayedPixelRatio_ = pixelRatio;
    this->attemptRefresh_ = false;
    this->displayImage_->show();

    return true;
}

bool TooltipEntryWidget::animated() const
{
    return this->image_ && this->image_->animated();
}

bool TooltipEntryWidget::hasImage() const
{
    return this->image_ != nullptr;
}

bool TooltipEntryWidget::attemptRefresh() const
{
    return this->attemptRefresh_;
}

}
