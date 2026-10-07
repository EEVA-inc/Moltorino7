// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/ImageSet.hpp"

#include "messages/Image.hpp"
#include "singletons/Settings.hpp"

#include <QJsonObject>

namespace chatterino {

ImageSet::ImageSet()
    : imageX1_(Image::getEmpty())
    , imageX2_(Image::getEmpty())
    , imageX3_(Image::getEmpty())
    , imageX4_(Image::getEmpty())
{
}

ImageSet::ImageSet(const ImagePtr &image1, const ImagePtr &image2,
                   const ImagePtr &image3, const ImagePtr &image4)
    : imageX1_(image1)
    , imageX2_(image2)
    , imageX3_(image3)
    , imageX4_(image4 ? image4 : Image::getEmpty())
{
}

ImageSet::ImageSet(const Url &image1, const Url &image2, const Url &image3,
                   const Url &image4)
    : imageX1_(Image::fromUrl(image1, 1))
    , imageX2_(image2.string.isEmpty() ? Image::getEmpty()
                                       : Image::fromUrl(image2, 0.5))
    , imageX3_(image3.string.isEmpty() ? Image::getEmpty()
                                       : Image::fromUrl(image3, 0.25))
    , imageX4_(image4.string.isEmpty() ? Image::getEmpty()
                                       : Image::fromUrl(image4, 0.125))
{
}

void ImageSet::setImage1(const ImagePtr &image)
{
    this->imageX1_ = image ? image : Image::getEmpty();
}

void ImageSet::setImage2(const ImagePtr &image)
{
    this->imageX2_ = image ? image : Image::getEmpty();
}

void ImageSet::setImage3(const ImagePtr &image)
{
    this->imageX3_ = image ? image : Image::getEmpty();
}

void ImageSet::setImage4(const ImagePtr &image)
{
    this->imageX4_ = image ? image : Image::getEmpty();
}

const ImagePtr &ImageSet::getImage1() const
{
    return this->imageX1_;
}

const ImagePtr &ImageSet::getImage2() const
{
    return this->imageX2_;
}

const ImagePtr &ImageSet::getImage3() const
{
    return this->imageX3_;
}

const ImagePtr &ImageSet::getImage4() const
{
    return this->imageX4_;
}

const std::shared_ptr<Image> &getImagePriv(const ImageSet &set, float scale,
                                           ImageSet::ScaleMode scaleMode)
{
    if (scaleMode == ImageSet::ScaleMode::Emote)
    {
        scale *= getSettings()->emoteScale;
    }

    int quality = 1;

    if (scale > 4.001f && !set.getImage4()->isEmpty())
    {
        return set.getImage4();
    }

    if (scale > 2.001f)
    {
        quality = 3;
    }
    else if (scale > 1.001f)
    {
        quality = 2;
    }

    if (!set.getImage3()->isEmpty() && quality == 3)
    {
        return set.getImage3();
    }

    if (!set.getImage2()->isEmpty() && quality >= 2)
    {
        return set.getImage2();
    }

    return set.getImage1();
}

const ImagePtr &ImageSet::getImageOrLoaded(float scale,
                                           ScaleMode scaleMode) const
{
    auto &&result = getImagePriv(*this, scale, scaleMode);

    result->load();

    if (result->loaded())
    {
        return result;
    }
    else if (this->imageX3_ && !this->imageX3_->isEmpty() &&
             this->imageX3_->loaded())
    {
        return this->imageX3_;
    }
    else if (this->imageX2_ && !this->imageX2_->isEmpty() &&
             this->imageX2_->loaded())
    {
        return this->imageX2_;
    }
    else if (this->imageX1_->loaded())
    {
        return this->imageX1_;
    }
    else if (!this->imageX4_->isEmpty() && this->imageX4_->loaded())
    {
        return this->imageX4_;
    }
    else
    {
        return result;
    }
}

const ImagePtr &ImageSet::getImageOrLoadedNoLoad(float scale,
                                                 ScaleMode scaleMode) const
{
    auto &&result = getImagePriv(*this, scale, scaleMode);

    if (!result->isEmpty() && result->loaded())
    {
        return result;
    }
    else if (this->imageX3_ && !this->imageX3_->isEmpty() &&
             this->imageX3_->loaded())
    {
        return this->imageX3_;
    }
    else if (this->imageX2_ && !this->imageX2_->isEmpty() &&
             this->imageX2_->loaded())
    {
        return this->imageX2_;
    }
    else if (this->imageX1_ && !this->imageX1_->isEmpty() &&
             this->imageX1_->loaded())
    {
        return this->imageX1_;
    }
    else if (!this->imageX4_->isEmpty() && this->imageX4_->loaded())
    {
        return this->imageX4_;
    }
    else
    {
        return result;
    }
}

const ImagePtr &ImageSet::getImage(float scale, ScaleMode scaleMode) const
{
    return getImagePriv(*this, scale, scaleMode);
}

bool ImageSet::operator==(const ImageSet &other) const
{
    return std::tie(this->imageX1_, this->imageX2_, this->imageX3_,
                    this->imageX4_) == std::tie(other.imageX1_, other.imageX2_,
                                                other.imageX3_, other.imageX4_);
}

bool ImageSet::operator!=(const ImageSet &other) const
{
    return !this->operator==(other);
}

QJsonObject ImageSet::toJson() const
{
    QJsonObject obj;
    if (!this->imageX1_->isEmpty())
    {
        obj[u"1x"] = this->imageX1_->url().string;
    }
    if (!this->imageX2_->isEmpty())
    {
        obj[u"2x"] = this->imageX2_->url().string;
    }
    if (!this->imageX3_->isEmpty())
    {
        obj[u"3x"] = this->imageX3_->url().string;
    }
    if (!this->imageX4_->isEmpty())
    {
        obj[u"4x"] = this->imageX4_->url().string;
    }
    return obj;
}

}
