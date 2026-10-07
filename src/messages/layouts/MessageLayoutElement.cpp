// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayoutElement.hpp"

#include "Application.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/MessageElement.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/ThemeCustomization.hpp"
#include "util/DebugCount.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QDebug>
#include <QFontMetricsF>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSet>
#include <QTextLayout>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace {

constexpr int MODIFIER_FILTER_CACHE_KIB = 8 * 1024;

struct ModifierFilterKey {
    qint64 pixmapCacheKey{};
    uint32_t flags{};
    int phase{};

    bool operator==(const ModifierFilterKey &) const = default;
};

size_t qHash(const ModifierFilterKey &key, size_t seed = 0) noexcept
{
    return qHashMulti(seed, key.pixmapCacheKey, key.flags, key.phase);
}

struct ModifierFilterFrame {
    QPixmap pixmap;
    std::weak_ptr<chatterino::Image> source;
};

QCache<ModifierFilterKey, ModifierFilterFrame> *modifierFilterCache = nullptr;
QSet<ModifierFilterKey> *pendingModifierFilters = nullptr;
QThreadPool *modifierFilterPool = nullptr;
std::atomic_bool modifierFiltersShuttingDown{false};

void clearModifierFilterCache()
{
    modifierFiltersShuttingDown.store(true, std::memory_order_release);
    if (modifierFilterPool != nullptr)
    {
        modifierFilterPool->clear();
        modifierFilterPool->waitForDone();
        delete modifierFilterPool;
        modifierFilterPool = nullptr;
    }
    delete pendingModifierFilters;
    pendingModifierFilters = nullptr;
    delete modifierFilterCache;
    modifierFilterCache = nullptr;
}

QCache<ModifierFilterKey, ModifierFilterFrame> &getModifierFilterCache()
{
    if (modifierFilterCache == nullptr)
    {
        modifierFilterCache =
            new QCache<ModifierFilterKey, ModifierFilterFrame>(
                MODIFIER_FILTER_CACHE_KIB);
        pendingModifierFilters = new QSet<ModifierFilterKey>;
        modifierFilterPool = new QThreadPool;

        modifierFilterPool->setMaxThreadCount(1);
        modifierFilterPool->setExpiryTimeout(10'000);
        modifierFiltersShuttingDown.store(false, std::memory_order_release);

        qAddPostRoutine(clearModifierFilterCache);
    }
    return *modifierFilterCache;
}

QRectF snapRectToDevicePixels(const QRectF &rect, const QPainter &painter)
{
    const auto dpr = painter.device() ? painter.device()->devicePixelRatioF()
                                      : 1.0;
    const auto snap = [dpr](qreal value) {
        return std::round(value * dpr) / dpr;
    };

    return QRectF(snap(rect.x()), snap(rect.y()),
                  std::max(1.0 / dpr, snap(rect.width())),
                  std::max(1.0 / dpr, snap(rect.height())));
}

QMarginsF boundedNametagPaintMargins(const chatterino::Paint &paint,
                                     float scale)
{
    const auto shadow = paint.getShadowMargins(scale);
    const auto horizontalLimit = 8.0 * scale;
    const auto verticalLimit = 4.0 * scale;
    const auto bounded = [](qreal value, qreal limit) {
        return std::clamp(value, 0.0, limit);
    };
    return {
        bounded(shadow.left(), horizontalLimit),
        bounded(shadow.top(), verticalLimit),
        bounded(shadow.right(), horizontalLimit),
        bounded(shadow.bottom(), verticalLimit),
    };
}

QRectF expandedPaintRect(const QRectF &textRect, const QMarginsF &margins)
{
    return textRect.adjusted(-margins.left(), -margins.top(), margins.right(),
                             margins.bottom());
}

const QChar RTL_EMBED(0x202B);

void alignRectBottomCenter(QRectF &rect, const QRectF &reference)
{
    QPointF newCenter(reference.center().x(),
                      reference.bottom() - (rect.height() / 2.0));
    rect.moveCenter(newCenter);
}

QRectF drawPixmapWithOptionalSmoothing(
    QPainter &painter, const QRectF &target, const QPixmap &pixmap,
    chatterino::FlagsEnum<chatterino::MessageElementFlag> flags,
    bool smooth = false)
{
    smooth =
        smooth || flags.has(chatterino::MessageElementFlag::BadgeMoltorino);
    const bool wasSmooth =
        painter.testRenderHint(QPainter::SmoothPixmapTransform);

    if (smooth && !wasSmooth)
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    }

    const auto paintedRect = snapRectToDevicePixels(target, painter);
    painter.drawPixmap(paintedRect, pixmap, QRectF());

    if (smooth && !wasSmooth)
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }

    return paintedRect;
}

struct AnimatedShadowLayer {
    const QPixmap *pixmap{};
    QRectF target;
    QString cacheIdentity;
};

QRegion paintAnimatedShadow(QPainter &painter,
                            std::span<const AnimatedShadowLayer> layers,
                            const chatterino::AnimatedMessageShadow *shadow)
{
    if (shadow == nullptr || shadow->opacity <= 0 || layers.empty())
    {
        return {};
    }

    QRectF bounds;
    for (const auto &layer : layers)
    {
        if (layer.pixmap == nullptr || layer.pixmap->isNull() ||
            layer.target.isEmpty())
        {
            continue;
        }
        const auto snapped = snapRectToDevicePixels(layer.target, painter);
        bounds = bounds.isNull() ? snapped : bounds.united(snapped);
    }
    if (bounds.isEmpty())
    {
        return {};
    }

    const qreal dpr = std::max<qreal>(
        1.0, painter.device() ? painter.device()->devicePixelRatioF() : 1.0);
    const int pixelRadius =
        std::max(0, qRound(std::max<qreal>(0, shadow->blurRadius) * dpr));
    const int pixelPadding = std::max(2, pixelRadius * 2 + 2);
    const int pixelWidth =
        std::max(1, qCeil(bounds.width() * dpr) + pixelPadding * 2);
    const int pixelHeight =
        std::max(1, qCeil(bounds.height() * dpr) + pixelPadding * 2);

    QString key;
    key.reserve(96 + static_cast<qsizetype>(layers.size()) * 64);
    key += QString::number(pixelWidth) + u'x' + QString::number(pixelHeight) +
           u'/' + QString::number(pixelRadius) + u'/' +
           QString::number(shadow->color.rgba()) + u'/' +
           QString::number(qRound(dpr * 1000));
    for (const auto &layer : layers)
    {
        if (layer.pixmap == nullptr || layer.pixmap->isNull() ||
            layer.target.isEmpty())
        {
            continue;
        }
        const auto target = snapRectToDevicePixels(layer.target, painter);
        key += u'/';
        if (layer.cacheIdentity.isEmpty())
        {
            key += u'p' + QString::number(static_cast<qulonglong>(
                              layer.pixmap->cacheKey()));
        }
        else
        {
            key += u's' + QString::number(layer.cacheIdentity.size()) + u':' +
                   layer.cacheIdentity;
        }
        key += u'@' + QString::number(qRound((target.x() - bounds.x()) * dpr)) +
               u',' + QString::number(qRound((target.y() - bounds.y()) * dpr)) +
               u',' + QString::number(qRound(target.width() * dpr)) + u',' +
               QString::number(qRound(target.height() * dpr));
    }

    static QCache<QString, QImage> cache(8 * 1024);
    auto *cached = cache.object(key);
    if (cached == nullptr)
    {
        QImage image(QSize(pixelWidth, pixelHeight),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        {
            QPainter shadowPainter(&image);
            shadowPainter.setRenderHint(QPainter::SmoothPixmapTransform);
            const QPointF origin(pixelPadding / dpr - bounds.x(),
                                 pixelPadding / dpr - bounds.y());
            shadowPainter.translate(origin);
            for (const auto &layer : layers)
            {
                if (layer.pixmap == nullptr || layer.pixmap->isNull() ||
                    layer.target.isEmpty())
                {
                    continue;
                }
                shadowPainter.drawPixmap(
                    snapRectToDevicePixels(layer.target, painter),
                    *layer.pixmap, QRectF());
            }
            shadowPainter.resetTransform();
            shadowPainter.setCompositionMode(
                QPainter::CompositionMode_SourceIn);
            shadowPainter.fillRect(
                QRectF(QPointF(), image.deviceIndependentSize()),
                shadow->color);
        }
        if (pixelRadius > 0)
        {
            image = chatterino::blurThemeShadow(std::move(image), pixelRadius);
        }
        const auto costKiB =
            std::max<qsizetype>(1, (image.sizeInBytes() + 1023) / 1024);
        const int cost = static_cast<int>(
            std::min<qsizetype>(costKiB, std::numeric_limits<int>::max()));
        cache.insert(key, new QImage(std::move(image)), cost);
        cached = cache.object(key);
    }
    if (cached == nullptr)
    {
        return {};
    }

    const QPointF topLeft = bounds.topLeft() -
                            QPointF(pixelPadding / dpr, pixelPadding / dpr) +
                            shadow->offset;
    painter.save();
    painter.setOpacity(std::clamp(shadow->opacity, 0.0, 1.0));
    painter.drawImage(topLeft, *cached);
    painter.restore();
    return QRegion(
        QRectF(topLeft, cached->deviceIndependentSize()).toAlignedRect());
}

QRegion paintAnimatedShadow(QPainter &painter, const AnimatedShadowLayer &layer,
                            const chatterino::AnimatedMessageShadow *shadow)
{
    return paintAnimatedShadow(painter, std::span(&layer, 1), shadow);
}

struct ModifierLayoutGeometry {
    QSizeF size;
    QPointF contentCenter;
};

ModifierLayoutGeometry modifierLayoutGeometry(QSizeF contentSize,
                                              uint32_t flags)
{
    using namespace chatterino::emote_modifiers;

    auto transformedWidth = contentSize.width();
    auto transformedHeight = contentSize.height();
    if ((flags & BTTV_WIDE) != 0)
    {
        transformedWidth = contentSize.height() * 2.0;
    }
    else if ((flags & WIDE) != 0)
    {
        transformedWidth *= 2.0;
    }
    return {
        .size = {transformedWidth, transformedHeight},
        .contentCenter = {transformedWidth / 2.0, transformedHeight / 2.0},
    };
}

QRectF modifierVisualEnvelope(const QRectF &layoutRect, QSizeF contentSize,
                              uint32_t flags,
                              const chatterino::AnimatedMessageShadow *shadow)
{
    using namespace chatterino::emote_modifiers;

    qreal scaleX = 1.0;
    if ((flags & BTTV_WIDE) != 0 && contentSize.width() > 0)
    {
        scaleX = contentSize.height() * 2.0 / contentSize.width();
    }
    else if ((flags & WIDE) != 0)
    {
        scaleX = 2.0;
    }

    qreal scaleY = 1.0;
    if ((flags & BOUNCE) != 0)
    {
        scaleX *= 1.2;
    }
    if ((flags & JAM) != 0)
    {
        scaleX *= 1.08;
        scaleY *= 1.12;
    }
    if ((flags & (APPEAR | LEAVE)) != 0)
    {
        scaleX *= 1.04;
        scaleY *= 1.04;
    }

    auto width = contentSize.width() * std::abs(scaleX);
    auto height = contentSize.height() * std::abs(scaleY);
    if ((flags & (ROTATE_90 | ROTATE_LEFT | ROTATE_RIGHT)) != 0)
    {
        std::swap(width, height);
    }
    if ((flags & SPIN) != 0)
    {
        width = height = std::hypot(width, height);
    }
    else if ((flags & JAM) != 0)
    {
        constexpr auto MAX_ANGLE = 8.0 * 3.14159265358979323846 / 180.0;
        const auto sine = std::sin(MAX_ANGLE);
        const auto cosine = std::cos(MAX_ANGLE);
        const auto rotatedWidth = width * cosine + height * sine;
        const auto rotatedHeight = height * cosine + width * sine;
        width = rotatedWidth;
        height = rotatedHeight;
    }

    qreal left = 0.0;
    qreal right = 0.0;
    qreal top = 0.0;
    qreal bottom = 0.0;
    if ((flags & SLIDE) != 0)
    {
        left += contentSize.width() * 0.28;
        right += contentSize.width() * 0.28;
    }
    if ((flags & (APPEAR | LEAVE)) != 0)
    {
        left += contentSize.width() * 0.65;
    }
    if ((flags & (HYPER_RED | SHAKE | BTTV_SHAKE)) != 0)
    {
        left += 3.0;
        right += 3.0;
        top += 3.0;
        bottom += 3.0;
    }
    if ((flags & JAM) != 0)
    {
        left += 3.0;
        right += 3.0;
        top += 4.0;
        bottom += 4.0;
    }

    QRectF envelope(QPointF{}, QSizeF(width, height));
    envelope.moveCenter(layoutRect.center());
    envelope.adjust(-left, -top, right, bottom);
    envelope = envelope.united(layoutRect);

    if (shadow != nullptr && shadow->opacity > 0)
    {
        const auto transformScale =
            std::max<qreal>({1.0, std::abs(scaleX), std::abs(scaleY)});
        const auto padding =
            (std::max<qreal>(2.0, shadow->blurRadius * 2.0 + 2.0) +
             std::hypot(shadow->offset.x(), shadow->offset.y())) *
            transformScale;
        envelope = envelope.united(
            envelope.adjusted(-padding, -padding, padding, padding));
    }
    return envelope;
}

int modifierColorPhase(uint32_t flags, unsigned long time)
{
    using namespace chatterino::emote_modifiers;
    if ((flags & (RAINBOW | PARTY)) == 0)
    {
        return 0;
    }
    constexpr int PHASES = 12;
    const auto duration = (flags & PARTY) != 0 ? 1500UL : 2000UL;
    return static_cast<int>((time % duration) * PHASES / duration);
}

int clampColor(qreal value)
{
    return std::clamp(static_cast<int>(std::lround(value)), 0, 255);
}

void applySepia(qreal amount, int &red, int &green, int &blue)
{
    const auto sepiaRed = 0.393 * red + 0.769 * green + 0.189 * blue;
    const auto sepiaGreen = 0.349 * red + 0.686 * green + 0.168 * blue;
    const auto sepiaBlue = 0.272 * red + 0.534 * green + 0.131 * blue;
    red = clampColor(red + (sepiaRed - red) * amount);
    green = clampColor(green + (sepiaGreen - green) * amount);
    blue = clampColor(blue + (sepiaBlue - blue) * amount);
}

void applySaturation(qreal amount, int &red, int &green, int &blue)
{
    const auto luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
    red = clampColor(luminance + (red - luminance) * amount);
    green = clampColor(luminance + (green - luminance) * amount);
    blue = clampColor(luminance + (blue - luminance) * amount);
}

void applyContrast(qreal amount, int &red, int &green, int &blue)
{
    const auto adjust = [amount](int channel) {
        return clampColor((channel - 127.5) * amount + 127.5);
    };
    red = adjust(red);
    green = adjust(green);
    blue = adjust(blue);
}

QImage filterModifierImage(QImage image, uint32_t filterFlags, int phase)
{
    using namespace chatterino::emote_modifiers;
    image = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            const auto pixel = line[x];
            const auto alpha = qAlpha(pixel);
            if (alpha == 0)
            {
                continue;
            }

            int red = qRed(pixel);
            int green = qGreen(pixel);
            int blue = qBlue(pixel);
            if ((filterFlags & CURSED) != 0)
            {
                const auto gray =
                    clampColor(0.2126 * red + 0.7152 * green + 0.0722 * blue);
                red = green = blue = clampColor(gray * 0.7);
                applyContrast(2.5, red, green, blue);
            }
            if ((filterFlags & HYPER_RED) != 0)
            {
                red = clampColor(red * 0.2);
                green = clampColor(green * 0.2);
                blue = clampColor(blue * 0.2);
                applySepia(1.0, red, green, blue);
                red = clampColor(red * 2.2);
                green = clampColor(green * 2.2);
                blue = clampColor(blue * 2.2);
                applyContrast(3.0, red, green, blue);
                applySaturation(8.0, red, green, blue);
            }
            if ((filterFlags & (RAINBOW | PARTY)) != 0)
            {
                if ((filterFlags & PARTY) != 0)
                {
                    applySepia(0.5, red, green, blue);
                    applySaturation(2.5, red, green, blue);
                }
                QColor color(red, green, blue);
                int hue = 0;
                int saturation = 0;
                int value = 0;
                color.getHsv(&hue, &saturation, &value);
                hue = (std::max(0, hue) + phase * 30) % 360;
                color.setHsv(hue, saturation, value);
                red = color.red();
                green = color.green();
                blue = color.blue();
            }
            line[x] = qRgba(red, green, blue, alpha);
        }
    }
    return image;
}

void insertModifierFilterResult(const ModifierFilterKey &key, QImage image,
                                qreal devicePixelRatio,
                                const ModifierFilterKey &fallbackKey,
                                const std::weak_ptr<chatterino::Image> &source)
{
    if (modifierFiltersShuttingDown.load(std::memory_order_acquire) ||
        modifierFilterCache == nullptr || pendingModifierFilters == nullptr)
    {
        return;
    }

    pendingModifierFilters->remove(key);
    auto result = QPixmap::fromImage(std::move(image));
    result.setDevicePixelRatio(devicePixelRatio);
    const auto costKiB = std::max<qsizetype>(
        1,
        (static_cast<qsizetype>(result.width()) * result.height() * 4 + 1023) /
            1024);
    if (costKiB <= MODIFIER_FILTER_CACHE_KIB)
    {
        modifierFilterCache->insert(key,
                                    new ModifierFilterFrame{result, source},
                                    static_cast<int>(costKiB));
        if (!source.expired())
        {
            modifierFilterCache->insert(
                fallbackKey, new ModifierFilterFrame{std::move(result), source},
                static_cast<int>(costKiB));
        }
    }
}

QPixmap filteredModifierPixmap(const QPixmap &source, uint32_t flags, int phase,
                               const chatterino::ImagePtr &sourceImage)
{
    using namespace chatterino::emote_modifiers;
    const auto filterFlags = flags & (RAINBOW | PARTY | HYPER_RED | CURSED);
    if (filterFlags == 0 || source.isNull())
    {
        return source;
    }

    const ModifierFilterKey key{
        .pixmapCacheKey = source.cacheKey(),
        .flags = filterFlags,
        .phase = phase,
    };
    auto &cache = getModifierFilterCache();
    if (const auto *cached = cache.object(key))
    {
        return cached->pixmap;
    }

    const bool deferFiltering =
        sourceImage->animated() || (filterFlags & (RAINBOW | PARTY)) != 0;
    const ModifierFilterKey fallbackKey{
        .pixmapCacheKey =
            static_cast<qint64>(reinterpret_cast<quintptr>(sourceImage.get())),
        .flags = filterFlags,
        .phase = -1,
    };
    if (deferFiltering)
    {
        QPixmap fallback;
        if (const auto *last = cache.object(fallbackKey);
            last && last->source.lock() == sourceImage)
        {
            fallback = last->pixmap;
        }
        if (fallback.isNull())
        {
            constexpr int PREVIEW_DIMENSION = 128;
            auto preview = source;
            if (std::max(source.width(), source.height()) > PREVIEW_DIMENSION)
            {
                preview = source.scaled(PREVIEW_DIMENSION, PREVIEW_DIMENSION,
                                        Qt::KeepAspectRatio);
            }
            fallback = QPixmap::fromImage(
                filterModifierImage(preview.toImage(), filterFlags, phase));
            fallback.setDevicePixelRatio(source.devicePixelRatio());
            const auto costKiB = std::max(
                1, (fallback.width() * fallback.height() * 4 + 1023) / 1024);
            cache.insert(fallbackKey,
                         new ModifierFilterFrame{fallback, sourceImage},
                         costKiB);
            if (preview.size() == source.size())
            {
                cache.insert(key,
                             new ModifierFilterFrame{fallback, sourceImage},
                             costKiB);
                return fallback;
            }
        }
        if (pendingModifierFilters == nullptr ||
            modifierFilterPool == nullptr ||
            pendingModifierFilters->contains(key) ||
            pendingModifierFilters->size() >= 16 ||
            modifierFilterPool->activeThreadCount() >=
                modifierFilterPool->maxThreadCount())
        {
            return fallback;
        }

        pendingModifierFilters->insert(key);
        const auto dpr = source.devicePixelRatio();
        auto image = source.toImage();
        std::weak_ptr<chatterino::Image> weakSource = sourceImage;
        auto *context = QCoreApplication::instance();
        const bool started =
            context != nullptr &&
            modifierFilterPool->tryStart([key, image = std::move(image),
                                          filterFlags, phase, dpr, fallbackKey,
                                          weakSource, context]() mutable {
                auto filtered =
                    filterModifierImage(std::move(image), filterFlags, phase);
                if (modifierFiltersShuttingDown.load(std::memory_order_acquire))
                {
                    return;
                }
                QMetaObject::invokeMethod(
                    context,
                    [key, filtered = std::move(filtered), dpr, fallbackKey,
                     weakSource]() mutable {
                        insertModifierFilterResult(key, std::move(filtered),
                                                   dpr, fallbackKey,
                                                   weakSource);
                    },
                    Qt::QueuedConnection);
            });
        if (!started)
        {
            pendingModifierFilters->remove(key);
        }
        return fallback;
    }

    auto result = QPixmap::fromImage(
        filterModifierImage(source.toImage(), filterFlags, phase));
    result.setDevicePixelRatio(source.devicePixelRatio());
    const auto costKiB = std::max<qsizetype>(
        1,
        (static_cast<qsizetype>(result.width()) * result.height() * 4 + 1023) /
            1024);
    if (costKiB <= MODIFIER_FILTER_CACHE_KIB)
    {
        cache.insert(key, new ModifierFilterFrame{result, {}},
                     static_cast<int>(costKiB));
    }
    return result;
}

}  // namespace

namespace chatterino {

namespace {

MessageElement &clientDetectionBadgeAnchor()
{
    static TextElement anchor(
        QString{}, MessageElementFlags({MessageElementFlag::Badges,
                                        MessageElementFlag::BadgeVanity}));
    return anchor;
}

QColor mutedClientBadgeColor(QColor source)
{
    if (!source.isValid())
    {
        return QColor(36, 38, 43, 218);
    }

    constexpr qreal baseAmount = 0.70;
    constexpr qreal tintAmount = 0.25;
    constexpr qreal lightAmount = 0.05;
    return QColor(
        qRound(30 * baseAmount + source.red() * tintAmount + 255 * lightAmount),
        qRound(32 * baseAmount + source.green() * tintAmount +
               255 * lightAmount),
        qRound(36 * baseAmount + source.blue() * tintAmount +
               255 * lightAmount),
        218);
}

}

namespace {

QBrush fragmentHighlightBrush(const FragmentHighlight &highlight)
{
    if (highlight.match.source == HighlightMatchSource::AutoMod)
    {
        const auto configured =
            QColor(getSettings()->automodMatchHighlightColor.getValue());
        if (configured.isValid())
        {
            return QBrush(configured);
        }
    }
    return QBrush(highlight.match.color);
}

HighlightMatchStyle fragmentHighlightStyle(const FragmentHighlight &highlight)
{
    if (highlight.match.source != HighlightMatchSource::AutoMod)
    {
        return highlight.match.style;
    }

    const auto configured =
        getSettings()->automodMatchHighlightStyle.getValue();
    if (configured < static_cast<int>(HighlightMatchStyle::None) ||
        configured > static_cast<int>(HighlightMatchStyle::Underline))
    {
        return HighlightMatchStyle::Fill;
    }
    return static_cast<HighlightMatchStyle>(configured);
}

QRectF fragmentHighlightOutlineRect(const QRectF &rect)
{
    return rect.adjusted(0.75, 0.75, -0.75, -0.75);
}

void paintFragmentHighlightOutline(QPainter &painter, const QRectF &rect,
                                   const FragmentHighlight &highlight)
{
    const auto outline = fragmentHighlightOutlineRect(rect);
    if (!highlight.connectsBefore && !highlight.connectsAfter)
    {
        painter.drawRoundedRect(outline, 2.0, 2.0);
        return;
    }

    const auto radius =
        std::min<qreal>(2.0, std::min(outline.width(), outline.height()) / 2.0);
    QPainterPath path;
    path.moveTo(outline.left() + (highlight.connectsBefore ? 0.0 : radius),
                outline.top());
    path.lineTo(outline.right() - (highlight.connectsAfter ? 0.0 : radius),
                outline.top());
    path.moveTo(outline.right() - (highlight.connectsAfter ? 0.0 : radius),
                outline.bottom());
    path.lineTo(outline.left() + (highlight.connectsBefore ? 0.0 : radius),
                outline.bottom());

    if (!highlight.connectsBefore)
    {
        path.moveTo(outline.left() + radius, outline.top());
        path.quadTo(outline.topLeft(),
                    QPointF(outline.left(), outline.top() + radius));
        path.lineTo(outline.left(), outline.bottom() - radius);
        path.quadTo(outline.bottomLeft(),
                    QPointF(outline.left() + radius, outline.bottom()));
    }
    if (!highlight.connectsAfter)
    {
        path.moveTo(outline.right() - radius, outline.top());
        path.quadTo(outline.topRight(),
                    QPointF(outline.right(), outline.top() + radius));
        path.lineTo(outline.right(), outline.bottom() - radius);
        path.quadTo(outline.bottomRight(),
                    QPointF(outline.right() - radius, outline.bottom()));
    }
    painter.drawPath(path);
}

}

void paintFragmentHighlightBackground(QPainter &painter, const QRectF &rect,
                                      const FragmentHighlight &highlight)
{
    const auto style = fragmentHighlightStyle(highlight);
    if (rect.isEmpty() || style == HighlightMatchStyle::None ||
        (style != HighlightMatchStyle::Fill &&
         style != HighlightMatchStyle::OutlineAndFill))
    {
        return;
    }

    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(fragmentHighlightBrush(highlight));

    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addRoundedRect(rect, 2.0, 2.0);
    if (highlight.connectsBefore)
    {
        path.addRect(QRectF(rect.left(), rect.top(), 2.0, rect.height()));
    }
    if (highlight.connectsAfter)
    {
        path.addRect(
            QRectF(rect.right() - 2.0, rect.top(), 2.0, rect.height()));
    }
    painter.drawPath(path);
    painter.restore();
}

void paintFragmentHighlightForeground(QPainter &painter, const QRectF &rect,
                                      const FragmentHighlight &highlight)
{
    const auto style = fragmentHighlightStyle(highlight);
    if (rect.isEmpty() || style == HighlightMatchStyle::None ||
        style == HighlightMatchStyle::Fill)
    {
        return;
    }

    painter.save();
    QPen pen(fragmentHighlightBrush(highlight), 1.5);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (style == HighlightMatchStyle::Underline)
    {
        const auto y = rect.bottom() - 1.0;
        painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
    else
    {
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintFragmentHighlightOutline(painter, rect, highlight);
    }
    painter.restore();
}

const QRectF &MessageLayoutElement::getRect() const
{
    return this->rect_;
}

MessageLayoutElement::MessageLayoutElement(MessageElement &creator, QSizeF size)
    : rect_(QPointF{}, size)
    , creator_(creator)
{
    DebugCount::increase(DebugObject::MessageLayoutElement);
}

MessageLayoutElement::~MessageLayoutElement()
{
    DebugCount::decrease(DebugObject::MessageLayoutElement);
}

void MessageLayoutElement::setFragmentHighlight(FragmentHighlight)
{
}

const FragmentHighlight *MessageLayoutElement::fragmentHighlight() const
{
    return nullptr;
}

void MessageLayoutElement::clearFragmentHighlightContinuity(bool, bool)
{
}

QString MessageLayoutElement::getFragmentTooltip(QPointF) const
{
    if (!getSettings()->highlightMatchedFragments)
    {
        return {};
    }
    const auto *highlight = this->fragmentHighlight();
    if (highlight == nullptr ||
        highlight->match.source != HighlightMatchSource::AutoMod)
    {
        return {};
    }
    return highlight->match.ruleName;
}

MessageElement &MessageLayoutElement::getCreator() const
{
    return this->creator_;
}

void MessageLayoutElement::setPosition(QPointF point)
{
    this->rect_.moveTopLeft(point);
}

bool MessageLayoutElement::hasTrailingSpace() const
{
    return this->trailingSpace;
}

bool MessageLayoutElement::usesCompactEmoteLayout() const
{
    return this->compactEmoteLayout_;
}

size_t MessageLayoutElement::getLine() const
{
    return this->line_;
}

void MessageLayoutElement::setLine(size_t line)
{
    this->line_ = line;
}

MessageLayoutElement *MessageLayoutElement::setTrailingSpace(bool value)
{
    this->trailingSpace = value;

    return this;
}

MessageLayoutElement *MessageLayoutElement::setCompactEmoteLayout(bool enabled)
{
    this->compactEmoteLayout_ = enabled;
    return this;
}

MessageLayoutElement *MessageLayoutElement::setLink(const Link &link)
{
    this->link_ = link;
    return this;
}

MessageLayoutElement *MessageLayoutElement::setText(const QString &_text)
{
    this->text_ = _text;
    return this;
}

Link MessageLayoutElement::getLink() const
{
    if (this->link_)
    {
        return *this->link_;
    }
    return this->creator_.getLink();
}

const QString &MessageLayoutElement::getText() const
{
    return this->text_;
}

FlagsEnum<MessageElementFlag> MessageLayoutElement::getFlags() const
{
    return this->creator_.getFlags();
}

int MessageLayoutElement::getWordId() const
{
    return this->wordId_;
}

void MessageLayoutElement::setWordId(int wordId)
{
    this->wordId_ = wordId;
}

//
// IMAGE
//

ImageLayoutElement::ImageLayoutElement(MessageElement &creator, ImagePtr image,
                                       QSizeF size, qreal horizontalPadding)
    : MessageLayoutElement(
          creator,
          QSizeF(size.width() + std::max<qreal>(0.0, horizontalPadding) * 2,
                 size.height()))
    , image_(std::move(image))
    , horizontalPadding_(std::max<qreal>(0.0, horizontalPadding))
{
    this->trailingSpace = creator.hasTrailingSpace();
}

void ImageLayoutElement::setHoverImage(ImagePtr image)
{
    this->hoverImage_ = std::move(image);
}

const ImagePtr &ImageLayoutElement::getHoverImage() const
{
    return this->hoverImage_;
}

void ImageLayoutElement::setFragmentHighlight(FragmentHighlight highlight)
{
    this->fragmentHighlight_ =
        std::make_unique<FragmentHighlight>(std::move(highlight));
}

const FragmentHighlight *ImageLayoutElement::fragmentHighlight() const
{
    return this->fragmentHighlight_.get();
}

void ImageLayoutElement::clearFragmentHighlightContinuity(bool before,
                                                          bool after)
{
    if (this->fragmentHighlight_ == nullptr)
    {
        return;
    }
    if (before)
    {
        this->fragmentHighlight_->connectsBefore = false;
    }
    if (after)
    {
        this->fragmentHighlight_->connectsAfter = false;
    }
}

void ImageLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                             uint32_t to) const
{
    const auto *emoteElement =
        dynamic_cast<EmoteElement *>(&this->getCreator());
    if (emoteElement)
    {
        str += this->getText().isNull()
                   ? emoteElement->getEmote()->getCopyString()
                   : this->getText();
        str = TwitchEmotes::cleanUpEmoteCode(str);
        if (this->hasTrailingSpace() && to >= 2)
        {
            str += ' ';
        }
    }
}

size_t ImageLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void ImageLayoutElement::paint(QPainter &painter,
                               const MessageColors &messageColors)
{
    this->paint(painter, messageColors, false);
}

void ImageLayoutElement::paint(QPainter &painter,
                               const MessageColors &messageColors,
                               bool hoverAnimateOnly)
{
    this->paint(painter, messageColors, hoverAnimateOnly, false);
}

void ImageLayoutElement::paint(QPainter &painter,
                               const MessageColors & /*messageColors*/,
                               bool hoverAnimateOnly, bool isHovered)
{
    if (this->previewImage_)
    {
        return;
    }
    if (this->image_ == nullptr)
    {
        return;
    }

    if (hoverAnimateOnly)
    {
        const ImagePtr *displayImage = &this->image_;
        if (isHovered && this->hoverImage_ != nullptr)
        {
            this->hoverImage_->load();
            if (this->hoverImage_->animated())
            {
                displayImage = &this->hoverImage_;
            }
        }

        if (auto pixmap = (*displayImage)->pixmapOrLoad())
        {
            const auto rect = QRectF(this->getRect())
                                  .adjusted(this->horizontalPadding_, 0,
                                            -this->horizontalPadding_, 0);
            drawPixmapWithOptionalSmoothing(painter, rect, *pixmap,
                                            this->getFlags());
        }
        return;
    }

    auto pixmap = this->image_->pixmapOrLoad();

    if (pixmap && !this->image_->animated())
    {
        const auto rect = QRectF(this->getRect())
                              .adjusted(this->horizontalPadding_, 0,
                                        -this->horizontalPadding_, 0);
        drawPixmapWithOptionalSmoothing(painter, rect, *pixmap,
                                        this->getFlags());
    }
}

bool ImageLayoutElement::hasAnimatedContent() const
{
    return this->previewImage_ != nullptr ||
           (this->image_ != nullptr && this->image_->animated()) ||
           (this->hoverImage_ != nullptr && this->hoverImage_->animated());
}

bool ImageLayoutElement::usesOwnAnimationTimer() const
{
    return (this->previewImage_ &&
            this->previewImage_->usesOwnAnimationTimer()) ||
           (this->image_ != nullptr && this->image_->usesOwnAnimationTimer()) ||
           (this->hoverImage_ != nullptr &&
            this->hoverImage_->usesOwnAnimationTimer());
}

QRegion ImageLayoutElement::paintAnimated(QPainter &painter, qreal yOffset,
                                          const AnimatedMessageShadow *shadow)
{
    return this->paintAnimatedImage(painter, yOffset, shadow, this->image_);
}

QRegion ImageLayoutElement::paintAnimated(QPainter &painter, qreal yOffset,
                                          const AnimatedMessageShadow *shadow,
                                          bool hoverAnimateOnly, bool isHovered)
{
    if (this->previewImage_)
    {
        auto rect = this->getRect();
        rect.translate(0, yOffset);
        if (!rect.intersects(QRectF(painter.viewport())))
        {
            this->releasePickerImages();
            return {};
        }
        const ImagePtr *display = &this->previewImage_;
        const bool animate =
            getApp()->getEmotes()->getGIFTimer()->shouldAnimate();
        if (animate)
        {
            this->previewImage_->load();
        }
        else
        {
            this->previewImage_->releasePickerFrames();
            if (this->hoverImage_)
            {
                this->hoverImage_->releasePickerFrames();
            }
            display = &this->image_;
        }
        if (animate && isHovered && this->hoverImage_ &&
            this->previewImage_->animated() &&
            !this->previewImage_->usesOwnAnimationTimer())
        {
            this->hoverImage_->load();
            if (this->hoverImage_->loaded())
            {
                display = &this->hoverImage_;
            }
        }
        auto pixmap = animate ? (*display)->pixmapOrLoad()
                              : (*display)->firstPixmapOrLoad();
        if (!pixmap &&
            (this->image_->loaded() || !this->previewImage_->isLoading()))
        {
            pixmap = this->image_->firstPixmapOrLoad();
        }
        if (pixmap)
        {
            drawPixmapWithOptionalSmoothing(
                painter,
                rect.adjusted(this->horizontalPadding_, 0,
                              -this->horizontalPadding_, 0),
                *pixmap, this->getFlags());
        }
        return (*display)->animated() || (!this->previewImage_->loaded() &&
                                          !this->previewImage_->isEmpty())
                   ? QRegion(rect.toAlignedRect())
                   : QRegion{};
    }
    const auto &image = hoverAnimateOnly && isHovered && this->hoverImage_
                            ? this->hoverImage_
                            : this->image_;
    if (hoverAnimateOnly && isHovered && image != nullptr)
    {
        image->load();
        if (image->animated())
        {
            auto rect = this->getRect();
            rect.moveTop(rect.y() + yOffset);
            return rect.toAlignedRect();
        }
        return {};
    }
    return this->paintAnimatedImage(painter, yOffset, shadow, image);
}

void ImageLayoutElement::setPreviewImage(ImagePtr image)
{
    this->previewImage_ = std::move(image);
}

bool ImageLayoutElement::hasPreviewImage() const
{
    return this->previewImage_ != nullptr;
}

void ImageLayoutElement::releasePickerImages() const
{
    if (this->previewImage_)
    {
        this->image_->releasePickerFrames();
        this->previewImage_->releasePickerFrames();
        if (this->hoverImage_)
        {
            this->hoverImage_->releasePickerFrames();
        }
    }
}

QRegion ImageLayoutElement::paintAnimatedImage(
    QPainter &painter, qreal yOffset, const AnimatedMessageShadow *shadow,
    const ImagePtr &image)
{
    if (image == nullptr)
    {
        return {};
    }

    if (image->animated())
    {
        if (auto pixmap = image->pixmapOrLoad())
        {
            auto rect = this->getRect();
            rect.moveTop(rect.y() + yOffset);
            const auto imageRect = QRectF(rect).adjusted(
                this->horizontalPadding_, 0, -this->horizontalPadding_, 0);
            QRegion paintedRegion;

            if (shadow != nullptr && shadow->opacity > 0 &&
                !image->usesOwnAnimationTimer())
            {
                paintedRegion += paintAnimatedShadow(
                    painter, AnimatedShadowLayer{&*pixmap, imageRect}, shadow);
            }
            const auto paintedRect = drawPixmapWithOptionalSmoothing(
                painter, imageRect, *pixmap, this->getFlags(),
                image->usesOwnAnimationTimer());
            if (getSettings()->highlightMatchedFragments &&
                this->fragmentHighlight_)
            {
                paintFragmentHighlightForeground(painter, rect,
                                                 *this->fragmentHighlight_);
            }

            paintedRegion += QRegion(rect.toAlignedRect()) |
                             QRegion(paintedRect.toAlignedRect());
            return paintedRegion;
        }
    }
    return {};
}

int ImageLayoutElement::getMouseOverIndex(QPointF /*abs*/) const
{
    return 0;
}

qreal ImageLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index == 1)
    {
        // fourtf: remove space width
        return this->getRect().right();
    }
    else
    {
        return this->getRect().right();
    }
}

//
// LAYERED IMAGE
//

LayeredImageLayoutElement::LayeredImageLayoutElement(
    MessageElement &creator, std::vector<ImagePtr> images,
    std::vector<QSizeF> sizes, QSizeF largestSize, uint32_t modifierFlags)
    : MessageLayoutElement(
          creator, modifierLayoutGeometry(largestSize, modifierFlags).size)
    , images_(std::move(images))
    , sizes_(std::move(sizes))
    , contentSize_(largestSize)
    , modifierFlags_(modifierFlags)
{
    assert(this->images_.size() == this->sizes_.size());
    this->trailingSpace = creator.hasTrailingSpace();
}

bool LayeredImageLayoutElement::removesPreviousSpace() const
{
    return (this->modifierFlags_ & emote_modifiers::ZERO_SPACE) != 0;
}

void LayeredImageLayoutElement::setFragmentHighlight(
    FragmentHighlight highlight)
{
    this->fragmentHighlight_ =
        std::make_unique<FragmentHighlight>(std::move(highlight));
}

const FragmentHighlight *LayeredImageLayoutElement::fragmentHighlight() const
{
    return this->fragmentHighlight_.get();
}

void LayeredImageLayoutElement::clearFragmentHighlightContinuity(bool before,
                                                                 bool after)
{
    if (this->fragmentHighlight_ == nullptr)
    {
        return;
    }
    if (before)
    {
        this->fragmentHighlight_->connectsBefore = false;
    }
    if (after)
    {
        this->fragmentHighlight_->connectsAfter = false;
    }
}

void LayeredImageLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                                    uint32_t to) const
{
    const auto *layeredEmoteElement =
        dynamic_cast<LayeredEmoteElement *>(&this->getCreator());
    if (layeredEmoteElement)
    {
        // cleaning is taken care in call
        str += this->getText().isNull()
                   ? layeredEmoteElement->getCleanCopyString()
                   : this->getText();
        if (this->hasTrailingSpace() && to >= 2)
        {
            str += ' ';
        }
    }
}

size_t LayeredImageLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void LayeredImageLayoutElement::paint(QPainter &painter,
                                      const MessageColors &messageColors)
{
    this->paint(painter, messageColors, false);
}

void LayeredImageLayoutElement::paint(QPainter &painter,
                                      const MessageColors &messageColors,
                                      bool hoverAnimateOnly)
{
    this->paint(painter, messageColors, hoverAnimateOnly, false);
}

void LayeredImageLayoutElement::paint(QPainter &painter,
                                      const MessageColors & /*messageColors*/,
                                      bool hoverAnimateOnly, bool isHovered)
{
    if (hoverAnimateOnly)
    {
        const auto fullRect = QRectF(this->getRect());
        for (size_t i = 0; i < this->images_.size(); ++i)
        {
            auto &img = this->images_[i];
            if (img == nullptr)
            {
                continue;
            }

            const auto pixmap =
                isHovered ? img->pixmapOrLoad() : img->firstPixmapOrLoad();
            if (!pixmap)
            {
                continue;
            }

            auto &size = this->sizes_[i];
            QRectF destRect(0, 0, size.width(), size.height());
            alignRectBottomCenter(destRect, fullRect);
            painter.drawPixmap(snapRectToDevicePixels(destRect, painter),
                               *pixmap, QRectF());
        }
        return;
    }

    if (this->modifierFlags_ != 0)
    {
        if (!this->needsAnimatedPaint())
        {
            (void)this->paintModified(painter, 0, nullptr);
        }
        return;
    }

    auto fullRect = QRectF(this->getRect());

    for (size_t i = 0; i < this->images_.size(); ++i)
    {
        auto &img = this->images_[i];
        if (img == nullptr)
        {
            continue;
        }

        const auto pixmap = img->pixmapOrLoad();
        if (img->animated())
        {
            // As soon as we see an animated emote layer, we can stop rendering
            // the static emotes. The paintAnimated function will render any
            // static emotes layered on top of the first seen animated emote.
            return;
        }

        if (pixmap)
        {
            // Matching the web chat behavior, we center the emote within the overall
            // binding box. E.g. small overlay emotes like cvMask will sit in the direct
            // center of even wide emotes.
            auto &size = this->sizes_[i];
            QRectF destRect(0, 0, size.width(), size.height());
            alignRectBottomCenter(destRect, fullRect);

            painter.drawPixmap(snapRectToDevicePixels(destRect, painter),
                               *pixmap, QRectF());
        }
    }
}

bool LayeredImageLayoutElement::hasAnimatedContent() const
{
    return this->needsAnimatedPaint();
}

QRegion LayeredImageLayoutElement::paintAnimated(
    QPainter &painter, qreal yOffset, const AnimatedMessageShadow *shadow)
{
    if (this->modifierFlags_ != 0)
    {
        return this->needsAnimatedPaint()
                   ? this->paintModified(painter, yOffset, shadow)
                   : QRegion{};
    }

    auto fullRect = QRectF(this->getRect());
    fullRect.moveTop(fullRect.y() + yOffset);
    bool animatedFlag = false;
    QRegion paintedRegion;
    const bool paintShadow = shadow != nullptr && shadow->opacity > 0;
    std::vector<std::pair<QPixmap, QRectF>> frames;

    for (size_t i = 0; i < this->images_.size(); ++i)
    {
        auto &img = this->images_[i];
        if (img == nullptr)
        {
            continue;
        }

        // If we have a static emote layered on top of an animated emote, we need
        // to render the static emote again after animating anything below it.
        if (img->animated() || animatedFlag)
        {
            if (auto pixmap = img->pixmapOrLoad())
            {
                // Matching the web chat behavior, we center the emote within the overall
                // binding box. E.g. small overlay emotes like cvMask will sit in the direct
                // center of even wide emotes.
                auto &size = this->sizes_[i];
                QRectF destRect(0, 0, size.width(), size.height());
                alignRectBottomCenter(destRect, fullRect);

                const auto paintedRect =
                    snapRectToDevicePixels(destRect, painter);
                if (paintShadow)
                {
                    frames.emplace_back(std::move(*pixmap), paintedRect);
                }
                else
                {
                    painter.drawPixmap(paintedRect, *pixmap, QRectF());
                    paintedRegion += paintedRect.toAlignedRect();
                }
                animatedFlag = true;
            }
        }
    }
    if (paintShadow)
    {
        std::vector<AnimatedShadowLayer> shadowLayers;
        shadowLayers.reserve(frames.size());
        for (const auto &[pixmap, target] : frames)
        {
            shadowLayers.push_back({&pixmap, target});
        }
        paintedRegion += paintAnimatedShadow(painter, shadowLayers, shadow);
        for (const auto &[pixmap, target] : frames)
        {
            painter.drawPixmap(target, pixmap, QRectF());
            paintedRegion += target.toAlignedRect();
        }
    }
    if (!paintedRegion.isEmpty() && getSettings()->highlightMatchedFragments &&
        this->fragmentHighlight_)
    {
        paintFragmentHighlightForeground(painter, fullRect,
                                         *this->fragmentHighlight_);
        paintedRegion += fullRect.toAlignedRect();
    }
    return paintedRegion;
}

QRegion LayeredImageLayoutElement::paintAnimated(
    QPainter &painter, qreal yOffset, const AnimatedMessageShadow *shadow,
    bool hoverAnimateOnly, bool isHovered)
{
    if (hoverAnimateOnly)
    {
        if (isHovered && this->hasAnimatedContent())
        {
            auto rect = this->getRect();
            rect.moveTop(rect.y() + yOffset);
            return rect.toAlignedRect();
        }
        return {};
    }
    return this->paintAnimated(painter, yOffset, shadow);
}

bool LayeredImageLayoutElement::needsAnimatedPaint() const
{
    if (getSettings()->animateEmotes &&
        (this->modifierFlags_ & emote_modifiers::ANIMATED) != 0)
    {
        return true;
    }
    return std::ranges::any_of(this->images_, [](const auto &image) {
        return image != nullptr && image->animated();
    });
}

QRegion LayeredImageLayoutElement::paintModified(
    QPainter &painter, qreal yOffset, const AnimatedMessageShadow *shadow)
{
    using namespace emote_modifiers;

    auto fullRect = QRectF(this->getRect());
    fullRect.translate(0, yOffset);
    QRectF contentRect(QPointF{}, this->contentSize_);
    const auto geometry =
        modifierLayoutGeometry(this->contentSize_, this->modifierFlags_);
    contentRect.moveCenter(fullRect.topLeft() + geometry.contentCenter);

    const bool animate = getSettings()->animateEmotes;
    const auto animatedFlags = animate ? this->modifierFlags_ : 0;
    const auto time =
        animate ? getApp()->getEmotes()->getGIFTimer()->position() : 0UL;
    const auto cycle = [](unsigned long position, unsigned long duration) {
        return static_cast<qreal>(position % duration) /
               static_cast<qreal>(duration);
    };
    constexpr qreal PI = 3.14159265358979323846;

    qreal scaleX = 1.0;
    if ((this->modifierFlags_ & BTTV_WIDE) != 0 &&
        this->contentSize_.width() > 0)
    {
        scaleX = this->contentSize_.height() * 2.0 / this->contentSize_.width();
    }
    else if ((this->modifierFlags_ & WIDE) != 0)
    {
        scaleX = 2.0;
    }
    qreal scaleY = 1.0;
    if ((this->modifierFlags_ & FLIP_X) != 0)
    {
        scaleX = -scaleX;
    }
    if ((this->modifierFlags_ & FLIP_Y) != 0)
    {
        scaleY = -scaleY;
    }

    qreal rotation = 0.0;
    if ((this->modifierFlags_ & ROTATE_90) != 0)
    {
        rotation += 90.0;
    }
    if ((this->modifierFlags_ & ROTATE_LEFT) != 0)
    {
        rotation -= 90.0;
    }
    if ((this->modifierFlags_ & ROTATE_RIGHT) != 0)
    {
        rotation += 90.0;
    }
    if ((animatedFlags & SPIN) != 0)
    {
        rotation += cycle(time, 1500) * 360.0;
    }

    QPointF translation;
    if ((animatedFlags & SLIDE) != 0)
    {
        const auto duration = static_cast<unsigned long>(
            std::clamp(contentRect.width() / 32.0 * 500.0, 250.0, 2000.0));
        translation.rx() += std::sin(cycle(time, duration) * PI * 2.0) *
                            contentRect.width() * 0.28;
    }
    if ((animatedFlags & (HYPER_RED | SHAKE)) != 0)
    {
        static constexpr std::array<QPointF, 10> FFZ_SHAKE{
            QPointF{-2, 1}, QPointF{3, -2}, QPointF{-1, -3}, QPointF{2, 2},
            QPointF{-3, 0}, QPointF{1, 3},  QPointF{3, 1},   QPointF{-2, -1},
            QPointF{0, 2},  QPointF{1, -2},
        };
        translation += FFZ_SHAKE[(time / 10U) % FFZ_SHAKE.size()];
    }
    if ((animatedFlags & BTTV_SHAKE) != 0)
    {
        static constexpr std::array<QPointF, 10> BTTV_SHAKE_STEPS{
            QPointF{-1, 0}, QPointF{2, -1},  QPointF{-2, 1}, QPointF{1, 2},
            QPointF{0, -2}, QPointF{-2, -1}, QPointF{2, 1},  QPointF{-1, 2},
            QPointF{1, -1}, QPointF{0, 0},
        };
        translation += BTTV_SHAKE_STEPS[(time / 50U) % BTTV_SHAKE_STEPS.size()];
    }

    qreal opacity = 1.0;
    qreal animatedScaleX = 1.0;
    qreal animatedScaleY = 1.0;
    const bool appears = (animatedFlags & APPEAR) != 0;
    const bool leaves = (animatedFlags & LEAVE) != 0;
    const auto enter = [&](qreal progress) {
        progress = std::clamp(progress, 0.0, 1.0);
        const auto eased = 1.0 - std::pow(1.0 - progress, 3.0);
        opacity = progress;
        animatedScaleX = animatedScaleY =
            eased + std::sin(progress * PI) * 0.04;
        translation.rx() -= (1.0 - eased) * contentRect.width() * 0.65;
    };
    const auto leave = [&](qreal progress) {
        progress = std::clamp(progress, 0.0, 1.0);
        const auto remaining = 1.0 - progress;
        opacity = remaining;
        animatedScaleX = remaining * std::cos(progress * PI);
        animatedScaleY = remaining;
        translation.rx() -= progress * contentRect.width() * 0.65;
    };
    if (appears && leaves)
    {
        const auto progress = cycle(time, 6000);
        if (progress < 0.1 || progress >= 0.925)
        {
            opacity = 0.0;
            animatedScaleX = animatedScaleY = 0.0;
        }
        else if (progress < 0.325)
        {
            enter((progress - 0.1) / 0.225);
        }
        else if (progress >= 0.7)
        {
            leave((progress - 0.7) / 0.225);
        }
    }
    else if (appears)
    {
        const auto progress = cycle(time, 3000);
        if (progress < 0.2)
        {
            opacity = 0.0;
            animatedScaleX = animatedScaleY = 0.0;
        }
        else if (progress < 0.65)
        {
            enter((progress - 0.2) / 0.45);
        }
    }
    else if (leaves)
    {
        const auto progress = cycle(time, 3000);
        if (progress >= 0.85)
        {
            opacity = 0.0;
            animatedScaleX = animatedScaleY = 0.0;
        }
        else if (progress >= 0.4)
        {
            leave((progress - 0.4) / 0.45);
        }
    }
    if ((animatedFlags & JAM) != 0)
    {
        const auto beat = std::sin(cycle(time, 600) * PI * 2.0);
        scaleX *= 1.0 + beat * 0.08;
        scaleY *= 1.0 - beat * 0.12;
        translation +=
            QPointF(beat * 3.0, std::cos(cycle(time, 600) * PI * 2.0) * 4.0);
        rotation += -2.5 + beat * 5.5;
    }
    if ((animatedFlags & BOUNCE) != 0)
    {
        const auto progress = cycle(time, 500);
        const auto squash = std::sin(progress * PI);
        scaleX *= 1.0 + squash * 0.2;
        scaleY *= 1.0 - squash * 0.65;
        if (progress >= 0.25 && progress < 0.75)
        {
            scaleX = -scaleX;
        }
    }
    scaleX *= animatedScaleX;
    scaleY *= animatedScaleY;

    QTransform transform;
    auto origin = contentRect.center();
    if ((animatedFlags & BOUNCE) != 0)
    {
        origin.setY(contentRect.bottom());
    }
    transform.translate(origin.x() + translation.x(),
                        origin.y() + translation.y());
    transform.rotate(rotation);
    transform.scale(scaleX, scaleY);
    transform.translate(-origin.x(), -origin.y());

    const auto phase = modifierColorPhase(this->modifierFlags_, time);
    const bool paintShadow = shadow != nullptr && shadow->opacity > 0;
    if (!paintShadow)
    {
        bool painted = false;
        QRegion paintedRegion;
        painter.save();
        painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
        painter.setWorldTransform(transform, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (size_t i = 0; i < this->images_.size(); ++i)
        {
            const auto &image = this->images_[i];
            if (image == nullptr)
            {
                continue;
            }
            const auto pixmap =
                animate ? image->pixmapOrLoad() : image->firstPixmapOrLoad();
            if (!pixmap)
            {
                continue;
            }
            QRectF target(QPointF{}, this->sizes_[i]);
            alignRectBottomCenter(target, contentRect);
            const auto filtered = filteredModifierPixmap(
                *pixmap, this->modifierFlags_, phase, image);
            const auto snapped = snapRectToDevicePixels(target, painter);
            painter.drawPixmap(snapped, filtered, QRectF());
            paintedRegion += transform.mapRect(snapped).toAlignedRect();
            painted = true;
        }
        painter.restore();
        if (!painted)
        {
            return {};
        }

        paintedRegion += modifierVisualEnvelope(fullRect, this->contentSize_,
                                                this->modifierFlags_, nullptr)
                             .toAlignedRect();
        if (getSettings()->highlightMatchedFragments &&
            this->fragmentHighlight_)
        {
            paintFragmentHighlightForeground(painter, fullRect,
                                             *this->fragmentHighlight_);
            paintedRegion += fullRect.toAlignedRect();
        }
        return paintedRegion;
    }

    std::vector<std::pair<QPixmap, QRectF>> frames;
    frames.reserve(this->images_.size());
    for (size_t i = 0; i < this->images_.size(); ++i)
    {
        const auto &image = this->images_[i];
        if (image == nullptr)
        {
            continue;
        }
        const auto pixmap =
            animate ? image->pixmapOrLoad() : image->firstPixmapOrLoad();
        if (!pixmap)
        {
            continue;
        }
        QRectF target(QPointF{}, this->sizes_[i]);
        alignRectBottomCenter(target, contentRect);
        frames.emplace_back(
            filteredModifierPixmap(*pixmap, this->modifierFlags_, phase, image),
            target);
    }
    if (frames.empty())
    {
        return {};
    }

    QRegion paintedRegion;
    painter.save();
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
    painter.setWorldTransform(transform, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    std::vector<AnimatedShadowLayer> shadowLayers;
    shadowLayers.reserve(frames.size());
    for (const auto &[pixmap, target] : frames)
    {
        shadowLayers.push_back({&pixmap, target});
    }
    (void)paintAnimatedShadow(painter, shadowLayers, shadow);
    for (const auto &[pixmap, target] : frames)
    {
        const auto snapped = snapRectToDevicePixels(target, painter);
        painter.drawPixmap(snapped, pixmap, QRectF());
        paintedRegion += transform.mapRect(snapped).toAlignedRect();
    }
    painter.restore();

    paintedRegion += modifierVisualEnvelope(fullRect, this->contentSize_,
                                            this->modifierFlags_, shadow)
                         .toAlignedRect();

    if (getSettings()->highlightMatchedFragments && this->fragmentHighlight_)
    {
        paintFragmentHighlightForeground(painter, fullRect,
                                         *this->fragmentHighlight_);
        paintedRegion += fullRect.toAlignedRect();
    }
    return paintedRegion;
}

int LayeredImageLayoutElement::getMouseOverIndex(QPointF /*abs*/) const
{
    return 0;
}

qreal LayeredImageLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index == 1)
    {
        // fourtf: remove space width
        return this->getRect().right();
    }
    else
    {
        return this->getRect().right();
    }
}

//
// IMAGE WITH BACKGROUND
//
ImageWithBackgroundLayoutElement::ImageWithBackgroundLayoutElement(
    MessageElement &creator, ImagePtr image, QSizeF size, QColor color)
    : ImageLayoutElement(creator, std::move(image), size)
    , color_(color)
{
}

void ImageWithBackgroundLayoutElement::paint(
    QPainter &painter, const MessageColors & /*messageColors*/)
{
    if (this->image_ == nullptr)
    {
        return;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (pixmap && !this->image_->animated())
    {
        painter.fillRect(QRectF(this->getRect()), this->color_);

        // fourtf: make it use qreal values
        painter.drawPixmap(QRectF(this->getRect()), *pixmap, QRectF());
    }
}

//
// IMAGE WITH CIRCLE BACKGROUND
//
ImageWithCircleBackgroundLayoutElement::ImageWithCircleBackgroundLayoutElement(
    MessageElement &creator, ImagePtr image, const QSize &imageSize,
    QColor color, int padding)
    : ImageLayoutElement(creator, image,
                         imageSize + QSize(padding, padding) * 2)
    , color_(color)
    , imageSize_(imageSize)
    , padding_(padding)
{
}

void ImageWithCircleBackgroundLayoutElement::paint(
    QPainter &painter, const MessageColors & /*messageColors*/)
{
    if (this->image_ == nullptr)
    {
        return;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (pixmap && !this->image_->animated())
    {
        QRectF boxRect(this->getRect());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QBrush(this->color_, Qt::SolidPattern));
        painter.drawEllipse(boxRect);

        QRectF imgRect;
        imgRect.setTopLeft(boxRect.topLeft());
        imgRect.setSize(this->imageSize_);
        imgRect.translate(this->padding_, this->padding_);

        painter.drawPixmap(imgRect, *pixmap, QRectF());
    }
}

//
// TEXT
//

TextLayoutElement::TextLayoutElement(MessageElement &_creator, QString &_text,
                                     QSizeF size, QColor _color,
                                     FontStyle _style,
                                     MessageColor::Type messageColor,
                                     float _scale, float dpr)
    : MessageLayoutElement(_creator, size)
    , color_(_color)
    , style_(_style)
    , messageColor_(messageColor)
    , scale_(_scale)
    , dpr_(dpr)
{
    this->setText(_text);
}

QString TextLayoutElement::getPaintTooltip() const
{
    const auto link = this->getLink();
    const bool isNametag =
        link.type == Link::UserInfo || link.type == Link::UserWhisper;
    if (!isNametag || this->messageColor_ == MessageColor::System ||
        this->getCreator().getFlags().has(
            MessageElementFlag::NoUsernamePaint) ||
        !getSettings()->displaySevenTVPaints)
    {
        return {};
    }

    const auto paint = getApp()->getSeventvPaints()->getPaint(
        link.value.toLower(),
        this->getCreator().getFlags().has(MessageElementFlag::KickUsername));
    return paint ? paint->getTooltip() : QString{};
}

QString TextLayoutElement::getFragmentTooltip(QPointF point) const
{
    if (!getSettings()->highlightMatchedFragments ||
        this->fragmentHighlights_ == nullptr)
    {
        return {};
    }

    for (const auto &highlight : *this->fragmentHighlights_)
    {
        if (highlight.match.source != HighlightMatchSource::AutoMod ||
            highlight.match.ruleName.isEmpty())
        {
            continue;
        }

        const auto from = static_cast<size_t>(std::clamp<qsizetype>(
            highlight.match.start, 0, this->getText().size()));
        const auto to = static_cast<size_t>(std::clamp<qsizetype>(
            highlight.match.start + highlight.match.length, 0,
            this->getText().size()));
        if (to <= from)
        {
            continue;
        }

        const auto rect = this->fragmentRect(from, to);
        if (rect.contains(point))
        {
            return highlight.match.ruleName;
        }
    }
    return {};
}

void TextLayoutElement::setFragmentHighlights(
    std::vector<FragmentHighlight> highlights)
{
    if (!highlights.empty())
    {
        this->fragmentHighlights_ =
            std::make_unique<std::vector<FragmentHighlight>>(
                std::move(highlights));
    }
}

const std::vector<FragmentHighlight> &TextLayoutElement::fragmentHighlights()
    const
{
    static const std::vector<FragmentHighlight> empty;
    return this->fragmentHighlights_ ? *this->fragmentHighlights_ : empty;
}

void TextLayoutElement::clearFragmentHighlightContinuity(bool before,
                                                         bool after)
{
    if (this->fragmentHighlights_ == nullptr)
    {
        return;
    }
    for (auto &highlight : *this->fragmentHighlights_)
    {
        if (before)
        {
            highlight.connectsBefore = false;
        }
        if (after)
        {
            highlight.connectsAfter = false;
        }
    }
}

void TextLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                            uint32_t to) const
{
    str += this->getText().mid(from, to - from);

    if (this->hasTrailingSpace() && to > this->getText().length())
    {
        str += ' ';
    }
}

size_t TextLayoutElement::getSelectionIndexCount() const
{
    return this->getText().length() + (this->trailingSpace ? 1 : 0);
}

void TextLayoutElement::paint(QPainter &painter,
                              const MessageColors & /*messageColors*/)
{
    this->paintText(painter, true);
}

void TextLayoutElement::paintWithoutFragmentHighlights(QPainter &painter)
{
    this->paintText(painter, false);
}

void TextLayoutElement::paintText(QPainter &painter, bool paintHighlights)
{
    auto *app = getApp();
    QString text = this->getText();
    if (text.isRightToLeft() || this->reversedNeutral)
    {
        text.prepend(RTL_EMBED);
    }

    auto font = app->getFonts()->getFont(this->style_, this->scale_);
    auto metrics = app->getFonts()->getFontMetrics(this->style_, this->scale_);

    const auto paintFragmentHighlights = [&](bool foreground) {
        if (!paintHighlights || !getSettings()->highlightMatchedFragments)
        {
            return;
        }

        const auto *highlights = this->fragmentHighlights_.get();
        if (highlights == nullptr)
        {
            return;
        }

        for (auto it = highlights->rbegin(); it != highlights->rend(); ++it)
        {
            const auto &match = it->match;
            const auto from = static_cast<size_t>(
                std::clamp<qsizetype>(match.start, 0, this->getText().size()));
            const auto to = static_cast<size_t>(std::clamp<qsizetype>(
                match.start + match.length, 0, this->getText().size()));
            if (to <= from)
            {
                continue;
            }
            auto rect = this->fragmentRect(from, to);
            if (it->connectsAfter && this->hasTrailingSpace())
            {
                rect.setRight(rect.right() + metrics.horizontalAdvance(u' '));
            }
            if (foreground)
            {
                paintFragmentHighlightForeground(painter, rect, *it);
            }
            else
            {
                paintFragmentHighlightBackground(painter, rect, *it);
            }
        }
    };

    paintFragmentHighlights(false);

    bool isNametag = this->getLink().type == chatterino::Link::UserInfo ||
                     this->getLink().type == chatterino::Link::UserWhisper;
    bool drawPaint = isNametag && this->messageColor_ != MessageColor::System &&
                     !this->getCreator().getFlags().has(
                         MessageElementFlag::NoUsernamePaint) &&
                     getSettings()->displaySevenTVPaints;
    if (drawPaint)
    {
        auto paint = app->getSeventvPaints()->getPaint(
            this->getLink().value.toLower(),
            this->getCreator().getFlags().has(
                MessageElementFlag::KickUsername));
        if (paint)
        {
            if (paint->animated())
            {
                return;
            }

            const auto margins =
                boundedNametagPaintMargins(*paint, this->scale_);
            const auto target = expandedPaintRect(this->getRect(), margins);
            auto paintPixmap = paint->getPixmap(
                this->getText(), font, this->color_, target.size(),
                this->scale_, this->dpr_, margins);

            painter.drawPixmap(target.topLeft(), paintPixmap);
            if (paintHighlights)
            {
                this->paintMatchTextPaints(painter, 0.0, false);
            }
            paintFragmentHighlights(true);
            return;
        }
    }

    painter.setPen(this->color_);
    painter.setFont(font);

    if (this->getRect().height() > std::ceil(metrics.height()))
    {
        const auto baseline =
            this->getRect().bottom() - metrics.descent();
        painter.drawText(QPointF(this->getRect().x(), baseline), text);
    }
    else
    {
        QPointF pivot(this->getRect().x(), this->getRect().y() + metrics.ascent());
        painter.drawText(pivot, text);
    }
    if (paintHighlights)
    {
        this->paintMatchTextPaints(painter, 0.0, false);
    }
    paintFragmentHighlights(true);
}

QRegion TextLayoutElement::paintMatchTextPaints(
    QPainter &painter, qreal yOffset, bool animated,
    const AnimatedMessageShadow *shadow)
{
    QRegion paintedRegion;
    if (!getSettings()->displaySevenTVPaints ||
        !getSettings()->highlightMatchedFragments ||
        this->fragmentHighlights_ == nullptr)
    {
        return paintedRegion;
    }

    const auto font = getApp()->getFonts()->getFont(this->style_, this->scale_);
    for (auto &highlight : *this->fragmentHighlights_)
    {
        auto paint = highlight.textPaint;
        if (!paint && !highlight.match.paintID.isEmpty())
        {
            paint = getApp()->getSeventvPaints()->getPaintByID(
                highlight.match.paintID);
            highlight.textPaint = paint;
        }
        if (!paint || paint->animated() != animated)
        {
            continue;
        }

        const auto from = static_cast<size_t>(std::clamp<qsizetype>(
            highlight.match.start, 0, this->getText().size()));
        const auto to = static_cast<size_t>(std::clamp<qsizetype>(
            highlight.match.start + highlight.match.length, 0,
            this->getText().size()));
        if (to <= from)
        {
            continue;
        }

        auto textRect = this->fragmentRect(from, to);
        textRect.translate(0, yOffset);

        const auto margins = paint->getShadowMargins(this->scale_);
        const auto canvasSize =
            QSizeF(textRect.width() + margins.left() + margins.right(),
                   textRect.height() + margins.top() + margins.bottom());
        const auto matchedText = this->getText().mid(
            static_cast<qsizetype>(from), static_cast<qsizetype>(to - from));
        const auto pixmap =
            paint->getPixmap(matchedText, font, this->color_, canvasSize,
                             this->scale_, this->dpr_, margins, false);
        const auto target =
            QRectF(textRect.topLeft() - QPointF(margins.left(), margins.top()),
                   canvasSize);
        if (animated && shadow != nullptr && shadow->opacity > 0)
        {
            const auto cacheIdentity = paint->getPixmapCacheKey(
                matchedText, font, this->color_, canvasSize, this->scale_,
                this->dpr_, margins, false);
            paintedRegion += paintAnimatedShadow(
                painter, AnimatedShadowLayer{&pixmap, target, cacheIdentity},
                shadow);
        }
        painter.drawPixmap(target, pixmap, QRectF());
        paintedRegion += target.toAlignedRect();
    }
    return paintedRegion;
}

QRegion TextLayoutElement::paintAnimated(QPainter &painter, const qreal yOffset,
                                         const AnimatedMessageShadow *shadow)
{
    if (this->getRect().isEmpty())
    {
        return {};
    }

    const auto font = getApp()->getFonts()->getFont(this->style_, this->scale_);

    const bool isNametag =
        this->getLink().type == chatterino::Link::UserInfo ||
        this->getLink().type == chatterino::Link::UserWhisper;
    const bool drawPaint = isNametag &&
                           this->messageColor_ != MessageColor::System &&
                           !this->getCreator().getFlags().has(
                               MessageElementFlag::NoUsernamePaint) &&
                           getSettings()->displaySevenTVPaints;
    QRegion paintedRegion;
    if (drawPaint)
    {
        const auto paint = getApp()->getSeventvPaints()->getPaint(
            this->getLink().value.toLower(),
            this->getCreator().getFlags().has(
                MessageElementFlag::KickUsername));
        if (paint && paint->animated())
        {
            const auto margins =
                boundedNametagPaintMargins(*paint, this->scale_);
            auto target = expandedPaintRect(this->getRect(), margins);
            target.translate(0, yOffset);
            const auto paintPixmap = paint->getPixmap(
                this->getText(), font, this->color_, target.size(),
                this->scale_, this->dpr_, margins);
            if (shadow != nullptr && shadow->opacity > 0)
            {
                const auto cacheIdentity = paint->getPixmapCacheKey(
                    this->getText(), font, this->color_, target.size(),
                    this->scale_, this->dpr_, margins);
                paintedRegion += paintAnimatedShadow(
                    painter,
                    AnimatedShadowLayer{&paintPixmap, target, cacheIdentity},
                    shadow);
            }
            painter.drawPixmap(target.topLeft(), paintPixmap);
            paintedRegion += target.toAlignedRect();
        }
    }

    const auto animatedMatchRegion =
        this->paintMatchTextPaints(painter, yOffset, true, shadow);
    paintedRegion += animatedMatchRegion;
    if (!animatedMatchRegion.isEmpty() && this->fragmentHighlights_)
    {
        for (const auto &highlight : *this->fragmentHighlights_)
        {
            if (!highlight.textPaint || !highlight.textPaint->animated())
            {
                continue;
            }
            const auto from = static_cast<size_t>(std::clamp<qsizetype>(
                highlight.match.start, 0, this->getText().size()));
            const auto to = static_cast<size_t>(std::clamp<qsizetype>(
                highlight.match.start + highlight.match.length, 0,
                this->getText().size()));
            if (to <= from)
            {
                continue;
            }
            auto rect = this->fragmentRect(from, to);
            rect.translate(0, yOffset);
            paintFragmentHighlightForeground(painter, rect, highlight);
            paintedRegion += rect.toAlignedRect();
        }
    }
    return paintedRegion;
}

int TextLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() < this->getRect().left())
    {
        return 0;
    }

    auto *app = getApp();

    auto metrics = app->getFonts()->getFontMetrics(this->style_, this->scale_);
    auto x = this->getRect().left();

    const auto text = QStringView(this->getText());
    for (qsizetype i = 0; i < text.size();)
    {
        const auto units = text.at(i).isHighSurrogate() &&
                                   i + 1 < text.size() &&
                                   text.at(i + 1).isLowSurrogate()
                               ? 2
                               : 1;
        const auto width = units == 1
                               ? metrics.horizontalAdvance(text.at(i))
                               : metrics.horizontalAdvance(QString::fromRawData(
                                     text.data() + i, units));

        // accept mouse to be at only 50%+ of character width to increase index
        if (x + (width * 0.5) > abs.x())
        {
            return i;
        }

        x += width;
        i += units;
    }

    //    if (this->hasTrailingSpace() && abs.x() < this->getRect().right())
    //    {
    //        return this->getSelectionIndexCount() - 1;
    //    }

    return this->getSelectionIndexCount() - (this->hasTrailingSpace() ? 1 : 0);
}

qreal TextLayoutElement::getXFromIndex(size_t index)
{
    return this->getTextXFromIndex(index);
}

QRectF TextLayoutElement::fragmentRect(size_t from, size_t to) const
{
    auto rect = this->getRect();
    if (!this->getText().isRightToLeft() && !this->reversedNeutral)
    {
        rect.setLeft(this->getTextXFromIndex(from));
        rect.setRight(this->getTextXFromIndex(to));
        return rect;
    }

    auto text = this->getText();
    text.prepend(RTL_EMBED);
    QTextLayout layout(
        text, getApp()->getFonts()->getFont(this->style_, this->scale_));
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(rect.width());
    layout.endLayout();
    const auto start = line.cursorToX(static_cast<int>(from + 1));
    const auto end = line.cursorToX(static_cast<int>(to + 1));
    rect.setLeft(this->getRect().left() + std::min(start, end));
    rect.setRight(this->getRect().left() + std::max(start, end));
    return rect;
}

qreal TextLayoutElement::getTextXFromIndex(size_t index) const
{
    auto *app = getApp();

    auto metrics = app->getFonts()->getFontMetrics(this->style_, this->scale_);

    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index < static_cast<size_t>(this->getText().size()))
    {
        qreal x = 0;
        const auto text = QStringView(this->getText());
        for (qsizetype i = 0; i < static_cast<qsizetype>(index);)
        {
            const auto units = text.at(i).isHighSurrogate() &&
                                       i + 1 < text.size() &&
                                       text.at(i + 1).isLowSurrogate()
                                   ? 2
                                   : 1;
            if (i + units > static_cast<qsizetype>(index))
            {
                break;
            }
            x += units == 1 ? metrics.horizontalAdvance(text.at(i))
                            : metrics.horizontalAdvance(
                                  QString::fromRawData(text.data() + i, units));
            i += units;
        }
        return x + this->getRect().left();
    }
    else
    {
        return this->getRect().right();
    }
}

// TEXT ICON
TextIconLayoutElement::TextIconLayoutElement(MessageElement &creator,
                                             const QString &_line1,
                                             const QString &_line2,
                                             float _scale, QSizeF size)
    : MessageLayoutElement(creator, size)
    , scale(_scale)
    , line1(_line1)
    , line2(_line2)
{
}

void TextIconLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                                uint32_t to) const
{
}

size_t TextIconLayoutElement::getSelectionIndexCount() const
{
    return this->trailingSpace ? 2 : 1;
}

void TextIconLayoutElement::paint(QPainter &painter,
                                  const MessageColors &messageColors)
{
    auto *app = getApp();

    QFont font = app->getFonts()->getFont(FontStyle::Tiny, this->scale);

    painter.setPen(messageColors.systemText);
    painter.setFont(font);

    QTextOption option;
    option.setAlignment(Qt::AlignHCenter);

    if (this->line2.isEmpty())
    {
        painter.drawText(this->getRect(), this->line1, option);
    }
    else
    {
        painter.drawText(
            QPointF{
                this->getRect().x(),
                this->getRect().y() + (this->getRect().height() / 2),
            },
            this->line1);
        painter.drawText(
            QPointF{
                this->getRect().x(),
                this->getRect().y() + this->getRect().height(),
            },
            this->line2);
    }
}

QRegion TextIconLayoutElement::paintAnimated(QPainter & /*painter*/,
                                             qreal /*yOffset*/,
                                             const AnimatedMessageShadow *)
{
    return {};
}

int TextIconLayoutElement::getMouseOverIndex(QPointF /*abs*/) const
{
    return 0;
}

qreal TextIconLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }
    else if (index == 1)
    {
        // fourtf: remove space width
        return this->getRect().right();
    }
    else
    {
        return this->getRect().right();
    }
}

ClientDetectionLayoutElement::ClientDetectionLayoutElement(
    const QPixmap &platformIcon, QColor color, QString tooltip,
    float badgeScale)
    : MessageLayoutElement(clientDetectionBadgeAnchor(),
                           QSizeF(18.0 * badgeScale, 18.0 * badgeScale))
    , platformIcon_(&platformIcon)
    , color_(mutedClientBadgeColor(std::move(color)))
    , tooltip_(std::move(tooltip))
    , badgeScale_(badgeScale)
{
    this->setLink({});
}

QString ClientDetectionLayoutElement::getFragmentTooltip(QPointF) const
{
    return this->tooltip_;
}

void ClientDetectionLayoutElement::addCopyTextToString(QString &, uint32_t,
                                                       uint32_t) const
{
}

size_t ClientDetectionLayoutElement::getSelectionIndexCount() const
{
    return 0;
}

void ClientDetectionLayoutElement::paint(
    QPainter &painter, const MessageColors &)
{
    if (this->platformIcon_ == nullptr || this->platformIcon_->isNull())
    {
        return;
    }

    const auto rect = this->getRect();

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(this->color_);

    const auto radius = std::max<qreal>(1.5, 2.25 * this->badgeScale_);
    painter.drawRoundedRect(rect, radius, radius);
    painter.drawPixmap(rect, *this->platformIcon_,
                       QRectF(this->platformIcon_->rect()));
    painter.restore();
}

QRegion ClientDetectionLayoutElement::paintAnimated(
    QPainter &, qreal, const AnimatedMessageShadow *)
{
    return {};
}

int ClientDetectionLayoutElement::getMouseOverIndex(QPointF) const
{
    return 0;
}

qreal ClientDetectionLayoutElement::getXFromIndex(size_t index)
{
    return index == 0 ? this->getRect().left() : this->getRect().right();
}

ReplyCurveLayoutElement::ReplyCurveLayoutElement(MessageElement &creator,
                                                 qreal width, float thickness,
                                                 float radius,
                                                 float neededMargin)
    : MessageLayoutElement(creator, QSizeF(width, 0))
    , pen_(QColor("#888"), thickness, Qt::SolidLine, Qt::RoundCap)
    , radius_(radius)
    , neededMargin_(neededMargin)
{
}

void ReplyCurveLayoutElement::paint(QPainter &painter,
                                    const MessageColors & /*messageColors*/)
{
    QRectF paintRect(this->getRect());
    QPainterPath path;

    QRectF curveRect = paintRect.marginsRemoved(QMarginsF(
        this->neededMargin_, this->neededMargin_, 0, this->neededMargin_));

    // Make sure that our curveRect can always fit the radius curve
    if (curveRect.height() < this->radius_)
    {
        curveRect.setTop(curveRect.top() -
                         (this->radius_ - curveRect.height()));
    }

    QPointF bStartPoint(curveRect.left(), curveRect.top() + this->radius_);
    QPointF bEndPoint(curveRect.left() + this->radius_, curveRect.top());
    QPointF bControlPoint(curveRect.topLeft());

    // Draw line from bottom left to curve
    path.moveTo(curveRect.bottomLeft());
    path.lineTo(bStartPoint);

    // Draw curve path
    path.quadTo(bControlPoint, bEndPoint);

    // Draw line from curve to top right
    path.lineTo(curveRect.topRight());

    // Render curve
    painter.setPen(this->pen_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawPath(path);
}

QRegion ReplyCurveLayoutElement::paintAnimated(QPainter & /*painter*/,
                                               qreal /*yOffset*/,
                                               const AnimatedMessageShadow *)
{
    return {};
}

int ReplyCurveLayoutElement::getMouseOverIndex(QPointF /*abs*/) const
{
    return 0;
}

qreal ReplyCurveLayoutElement::getXFromIndex(size_t index)
{
    if (index <= 0)
    {
        return this->getRect().left();
    }

    return this->getRect().right();
}

void ReplyCurveLayoutElement::addCopyTextToString(QString &str, uint32_t from,
                                                  uint32_t to) const
{
}

size_t ReplyCurveLayoutElement::getSelectionIndexCount() const
{
    return 1;
}

}  // namespace chatterino
