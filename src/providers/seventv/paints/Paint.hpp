#pragma once

#include "providers/seventv/paints/PaintDropShadow.hpp"

#include <QBrush>
#include <QFont>
#include <QMarginsF>

#include <utility>
#include <vector>

namespace chatterino {

class Paint
{
public:
    virtual QBrush asBrush(QColor userColor, QRectF drawingRect) const = 0;
    virtual const std::vector<PaintDropShadow> &getDropShadows() const = 0;
    virtual bool animated() const = 0;
    virtual bool loaded() const;
    virtual bool failed() const;
    virtual void ensureLoaded(bool retry = false) const;

    QPixmap getPixmap(const QString &text, const QFont &font, QColor userColor,
                      QSizeF size, float scale, float dpr,
                      QMarginsF contentMargins = {},
                      bool separateTrailingColon = true) const;
    QString getPixmapCacheKey(const QString &text, const QFont &font,
                              QColor userColor, QSizeF size, float scale,
                              float dpr, QMarginsF contentMargins = {},
                              bool separateTrailingColon = true) const;
    QMarginsF getShadowMargins(float scale) const;

    Paint(QString name, QString id)
        : id(std::move(id))
        , name_(std::move(name))
    {
    }
    virtual ~Paint() = default;

    Paint(const Paint &) = default;
    Paint(Paint &&) = delete;
    Paint &operator=(const Paint &) = default;
    Paint &operator=(Paint &&) = delete;

    const QString &getName() const;
    QString getTooltip() const;

    QString id;

protected:
    static QColor overlayColors(QColor background, QColor foreground);
    static qreal offsetRepeatingStopPosition(qreal position,
                                             const QGradientStops &stops);
    virtual qint64 sourcePixmapCacheKey() const;

private:
    QString name_;
};

}
