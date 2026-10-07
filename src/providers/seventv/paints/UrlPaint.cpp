#include "providers/seventv/paints/UrlPaint.hpp"

#include <QPainter>

#include <utility>

namespace chatterino {

UrlPaint::UrlPaint(QString name, QString id, ImagePtr image,
                   std::vector<PaintDropShadow> dropShadows)
    : Paint(std::move(name), std::move(id))
    , image_(std::move(image))
    , dropShadows_(std::move(dropShadows))
{
}

bool UrlPaint::animated() const
{
    return image_->animated();
}

QBrush UrlPaint::asBrush(const QColor userColor, const QRectF drawingRect) const
{
    if (auto paintPixmap = this->image_->pixmapOrLoad())
    {
        QPixmap target(drawingRect.size().toSize());
        target.fill(userColor);
        {
            QPainter painter(&target);
            painter.drawPixmap(target.rect(), *paintPixmap,
                               paintPixmap->rect());
        }

        return {target};
    }

    return {userColor};
}

const std::vector<PaintDropShadow> &UrlPaint::getDropShadows() const
{
    return this->dropShadows_;
}

bool UrlPaint::loaded() const
{
    return image_->loaded();
}

bool UrlPaint::failed() const
{
    return image_->isEmpty();
}

void UrlPaint::ensureLoaded(bool retry) const
{
    if (retry)
    {
        this->image_->retryLoad();
    }
    else
    {
        this->image_->load();
    }
}

qint64 UrlPaint::sourcePixmapCacheKey() const
{
    if (const auto pixmap = this->image_->pixmapOrLoad())
    {
        return pixmap->cacheKey();
    }
    return 0;
}

}
