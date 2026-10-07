#include "providers/seventv/paints/PaintDropShadow.hpp"

#include "singletons/Settings.hpp"

#include <private/qpixmapfilter_p.h>

#include <algorithm>

namespace chatterino {

PaintDropShadow::PaintDropShadow(float xOffset, float yOffset, float radius,
                                 QColor color)
    : xOffset_(xOffset)
    , yOffset_(yOffset)
    , radius_(radius)
    , color_(color)
{
}

bool PaintDropShadow::isValid() const
{
    return radius_ > 0;
}

PaintDropShadow PaintDropShadow::scaled(float scale) const
{
    return {this->xOffset_ * scale, this->yOffset_ * scale,
            this->radius_ * scale, this->color_};
}

void PaintDropShadow::apply(QPixmapDropShadowFilter &effect) const
{
    effect.setOffset({this->xOffset_, this->yOffset_});
    auto radius = this->radius_;
    if (getSettings()->largeSevenTVPaintShadows)
    {

        radius *= 3;
    }
    effect.setBlurRadius(radius);
    effect.setColor(this->color_);
}

QMarginsF PaintDropShadow::margins(float scale) const
{
    if (!this->isValid() || scale <= 0.F)
    {
        return {};
    }

    const auto xOffset = static_cast<qreal>(this->xOffset_ * scale);
    const auto yOffset = static_cast<qreal>(this->yOffset_ * scale);
    auto radius = static_cast<qreal>(this->radius_ * scale);
    if (getSettings()->largeSevenTVPaintShadows)
    {
        radius *= 3;
    }

    return {
        std::max<qreal>(0, radius - xOffset),
        std::max<qreal>(0, radius - yOffset),
        std::max<qreal>(0, radius + xOffset),
        std::max<qreal>(0, radius + yOffset),
    };
}

}
