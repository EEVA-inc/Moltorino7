#include "providers/seventv/paints/LinearGradientPaint.hpp"

#include <cmath>
#include <utility>

namespace chatterino {

LinearGradientPaint::LinearGradientPaint(
    QString name, QString id, std::optional<QColor> color, QGradientStops stops,
    bool repeat, float angle, std::vector<PaintDropShadow> dropShadows)
    : Paint(std::move(name), std::move(id))
    , color_(color)
    , stops_(std::move(stops))
    , repeat_(repeat)
    , angle_(angle)
    , dropShadows_(std::move(dropShadows))
{
}

bool LinearGradientPaint::animated() const
{
    return false;
}

QBrush LinearGradientPaint::asBrush(const QColor userColor,
                                    const QRectF drawingRect) const
{
    if (this->stops_.empty())
    {
        return {userColor};
    }
    if (this->stops_.size() == 1 ||
        this->stops_.back().first <= this->stops_.front().first)
    {
        return {overlayColors(userColor, this->stops_.back().second)};
    }

    auto angle = std::isfinite(this->angle_)
                     ? std::fmod(this->angle_, 360.F)
                     : 0.F;
    if (angle < 0.F)
    {
        angle += 360.F;
    }
    QPointF startPoint = drawingRect.bottomLeft();
    QPointF endPoint = drawingRect.topRight();

    const int angleStep = int(angle / 90);
    if (angleStep == 1)
    {
        startPoint = drawingRect.topLeft();
        endPoint = drawingRect.bottomRight();
    }
    if (angleStep == 2)
    {
        startPoint = drawingRect.topRight();
        endPoint = drawingRect.bottomLeft();
    }
    if (angleStep == 3)
    {
        startPoint = drawingRect.bottomRight();
        endPoint = drawingRect.topLeft();
    }

    QLineF gradientAxis;
    gradientAxis.setP1(drawingRect.center());
    gradientAxis.setAngle(90.0F - angle);

    QLineF colorStartAxis;
    colorStartAxis.setP1(startPoint);
    colorStartAxis.setAngle(-angle);

    QLineF colorStopAxis;
    colorStopAxis.setP1(endPoint);
    colorStopAxis.setAngle(-angle);

    QPointF gradientStart;
    QPointF gradientEnd;
    gradientAxis.intersects(colorStartAxis, &gradientStart);
    gradientAxis.intersects(colorStopAxis, &gradientEnd);

    if (this->repeat_)
    {
        QLineF gradientLine(gradientStart, gradientEnd);
        gradientStart = gradientLine.pointAt(this->stops_.front().first);
        gradientEnd = gradientLine.pointAt(this->stops_.back().first);
    }

    QLinearGradient gradient(gradientStart, gradientEnd);

    auto spread =
        this->repeat_ ? QGradient::RepeatSpread : QGradient::PadSpread;
    gradient.setSpread(spread);

    for (const auto &[position, color] : this->stops_)
    {
        auto combinedColor =
            LinearGradientPaint::overlayColors(userColor, color);
        auto offsetPosition =
            this->repeat_ ? LinearGradientPaint::offsetRepeatingStopPosition(
                                position, this->stops_)
                          : position;
        gradient.setColorAt(offsetPosition, combinedColor);
    }

    return {gradient};
}

const std::vector<PaintDropShadow> &LinearGradientPaint::getDropShadows() const
{
    return this->dropShadows_;
}

}
