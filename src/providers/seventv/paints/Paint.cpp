#include "providers/seventv/paints/Paint.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <private/qpixmapfilter_p.h>
#include <QFontMetricsF>
#include <QLabel>
#include <QPainter>

#include <algorithm>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

namespace {

qreal textBaseline(const QFont &font, const QRectF &rect)
{
    const QFontMetricsF metrics(font);

    return rect.bottom() - metrics.descent();
}

}  // namespace

QPixmap Paint::getPixmap(const QString &text, const QFont &font,
                         QColor userColor, QSizeF size, float scale,
                         float dpr, QMarginsF contentMargins,
                         bool separateTrailingColon) const
{
    contentMargins = {
        std::max<qreal>(0, contentMargins.left()),
        std::max<qreal>(0, contentMargins.top()),
        std::max<qreal>(0, contentMargins.right()),
        std::max<qreal>(0, contentMargins.bottom()),
    };
    auto contentSize = QSizeF(
        size.width() - contentMargins.left() - contentMargins.right(),
        size.height() - contentMargins.top() - contentMargins.bottom());
    if (contentSize.width() <= 0 || contentSize.height() <= 0)
    {
        contentMargins = {};
        contentSize = size;
    }

    QPixmap pixmap((size * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    const bool hasContentMargins = contentMargins.left() > 0 ||
                                   contentMargins.top() > 0 ||
                                   contentMargins.right() > 0 ||
                                   contentMargins.bottom() > 0;
    QPixmap contentPixmap;
    auto *contentTarget = &pixmap;
    if (hasContentMargins)
    {
        contentPixmap = QPixmap((contentSize * dpr).toSize());
        contentPixmap.setDevicePixelRatio(dpr);
        contentPixmap.fill(Qt::transparent);
        contentTarget = &contentPixmap;
    }

    QPainter contentPainter(contentTarget);
    contentPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    contentPainter.setFont(font);

    const QRectF contentRect(QPointF{}, contentSize);

    // NOTE: draw colon separately from the nametag
    // otherwise the paint would extend onto the colon
    bool drawColon = false;
    QRectF nametagBoundingRect = contentRect;
    QString nametagText = text;
    if (separateTrailingColon && nametagText.endsWith(':'))
    {
        drawColon = true;
        nametagText = nametagText.chopped(1);
        const auto textBounds = contentPainter.boundingRect(
            QRectF(0, 0, 10000, 10000), nametagText,
            QTextOption(Qt::AlignLeft | Qt::AlignTop));
        nametagBoundingRect.setWidth(
            std::min(textBounds.width(), contentRect.width()));
    }

    QPen pen;
    const QBrush brush = this->asBrush(userColor, nametagBoundingRect);
    pen.setBrush(brush);
    contentPainter.setPen(pen);

    const auto baseline = textBaseline(font, nametagBoundingRect);
    contentPainter.drawText(QPointF(nametagBoundingRect.left(), baseline),
                            nametagText);
    contentPainter.end();

    if (hasContentMargins)
    {
        QPainter pixmapPainter(&pixmap);
        pixmapPainter.drawPixmap(
            QPointF(contentMargins.left(), contentMargins.top()),
            contentPixmap);
    }

    if (!this->getDropShadows().empty() &&
        getSettings()->displaySevenTVPaintShadows)
    {
        QPixmap outMap((size * dpr).toSize());
        outMap.setDevicePixelRatio(dpr);
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }
            outMap.fill(Qt::transparent);

            {
                QPainter outPainter(&outMap);
                auto scaled = shadow.scaled(
                    scale / static_cast<float>(outMap.devicePixelRatio()));

                QPixmapDropShadowFilter filter;
                scaled.apply(filter);
                filter.draw(&outPainter, {0, 0}, pixmap);
            }
            outMap.swap(pixmap);
        }
    }

    if (drawColon)
    {
        auto colonColor = getApp()->getThemes()->messages.textColors.regular;

        QPainter pixmapPainter(&pixmap);

        pixmapPainter.setPen(QPen(colonColor));
        pixmapPainter.setFont(font);

        pixmapPainter.drawText(
            QPointF(contentMargins.left() + nametagBoundingRect.right(),
                    contentMargins.top() + baseline),
            u":"_s);
    }

    return pixmap;
}

QColor Paint::overlayColors(QColor background, QColor foreground)
{
    auto alpha = foreground.alphaF();

    auto r = ((1 - alpha) * static_cast<float>(background.red())) +
             (alpha * static_cast<float>(foreground.red()));
    auto g = ((1 - alpha) * static_cast<float>(background.green())) +
             (alpha * static_cast<float>(foreground.green()));
    auto b = ((1 - alpha) * static_cast<float>(background.blue())) +
             (alpha * static_cast<float>(foreground.blue()));

    return {static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
}

qreal Paint::offsetRepeatingStopPosition(const qreal position,
                                         const QGradientStops &stops)
{
    const qreal gradientStart = stops.first().first;
    const qreal gradientEnd = stops.last().first;
    const qreal gradientLength = gradientEnd - gradientStart;
    const qreal offsetPosition = (position - gradientStart) / gradientLength;

    return offsetPosition;
}

const QString &Paint::getName() const
{
    return this->name_;
}

QString Paint::getTooltip() const
{
    if (this->name_.isEmpty())
    {
        return {};
    }

    return u"<span>Paint: %1</span>"_s.arg(this->name_.toHtmlEscaped());
}

bool Paint::loaded() const
{
    return true;
}

bool Paint::failed() const
{
    return false;
}

void Paint::ensureLoaded(bool) const
{
}

qint64 Paint::sourcePixmapCacheKey() const
{
    return 0;
}

QString Paint::getPixmapCacheKey(const QString &text, const QFont &font,
                                 QColor userColor, QSizeF size, float scale,
                                 float dpr, QMarginsF contentMargins,
                                 bool separateTrailingColon) const
{
    contentMargins = {
        std::max<qreal>(0, contentMargins.left()),
        std::max<qreal>(0, contentMargins.top()),
        std::max<qreal>(0, contentMargins.right()),
        std::max<qreal>(0, contentMargins.bottom()),
    };

    QString key;
    const auto fontKey = font.key();
    key.reserve(this->id.size() + text.size() + fontKey.size() + 112);
    const auto appendString = [&key](QStringView value) {
        key += QString::number(value.size());
        key += u':';
        key += value;
        key += u'/';
    };

    appendString(this->id);
    appendString(text);
    appendString(fontKey);
    key += QString::number(this->sourcePixmapCacheKey()) + u'/' +
           QString::number(userColor.rgba()) + u'/' +
           QString::number(qRound64(size.width() * dpr)) + u'x' +
           QString::number(qRound64(size.height() * dpr)) + u'/' +
           QString::number(qRound64(scale * 1000)) + u'/' +
           QString::number(qRound64(dpr * 1000)) + u'/' +
           QString::number(qRound64(contentMargins.left() * dpr)) + u',' +
           QString::number(qRound64(contentMargins.top() * dpr)) + u',' +
           QString::number(qRound64(contentMargins.right() * dpr)) + u',' +
           QString::number(qRound64(contentMargins.bottom() * dpr)) + u'/' +
           (separateTrailingColon ? u'1' : u'0') + u'/' +
           (getSettings()->displaySevenTVPaintShadows ? u'1' : u'0');
    return key;
}

QMarginsF Paint::getShadowMargins(float scale) const
{
    QMarginsF result;
    if (!getSettings()->displaySevenTVPaintShadows)
    {
        return result;
    }

    for (const auto &shadow : this->getDropShadows())
    {
        const auto margins = shadow.margins(scale);

        result.setLeft(result.left() + margins.left());
        result.setTop(result.top() + margins.top());
        result.setRight(result.right() + margins.right());
        result.setBottom(result.bottom() + margins.bottom());
    }
    return result;
}

}  // namespace chatterino
