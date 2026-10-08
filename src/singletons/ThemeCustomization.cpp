#include "singletons/ThemeCustomization.hpp"

#include "singletons/Settings.hpp"
#include "singletons/ThemeVideoDecoder.hpp"
#include "singletons/ThemeWallpaper.hpp"

#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace chatterino {
namespace {

constexpr int MAX_WALLPAPER_DIMENSION = 1536;

QColor mix(const QColor &a, const QColor &b, qreal amount)
{
    amount = std::clamp(amount, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() * (1.0 - amount) + b.redF() * amount,
                            a.greenF() * (1.0 - amount) + b.greenF() * amount,
                            a.blueF() * (1.0 - amount) + b.blueF() * amount,
                            a.alphaF() * (1.0 - amount) + b.alphaF() * amount);
}

QColor alpha(QColor color, int opacityPercent)
{
    color.setAlphaF(std::clamp(opacityPercent, 0, 100) / 100.0);
    return color;
}

QString encoded(const QColor &color)
{
    return color.name(color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
}

QJsonObject tabState(const QColor &text, const QColor &regular,
                     const QColor &hover, const QColor &unfocused,
                     const QColor &line)
{
    return {
        {u"text"_s, encoded(text)},
        {u"backgrounds"_s, QJsonObject{{u"regular"_s, encoded(regular)},
                                       {u"hover"_s, encoded(hover)},
                                       {u"unfocused"_s, encoded(unfocused)}}},
        {u"line"_s, QJsonObject{{u"regular"_s, encoded(line)},
                                {u"hover"_s, encoded(line)},
                                {u"unfocused"_s, encoded(alpha(line, 70))}}},
    };
}

QJsonObject validatedThemeColors(const QJsonObject &source,
                                 const QJsonObject &shape)
{
    QJsonObject result;
    for (auto it = shape.begin(); it != shape.end(); ++it)
    {
        const auto value = source.value(it.key());
        if (it->isObject())
        {
            const auto child =
                validatedThemeColors(value.toObject(), it->toObject());
            if (!child.isEmpty())
            {
                result.insert(it.key(), child);
            }
        }
        else if (value.isString() && QColor(value.toString()).isValid())
        {
            result.insert(it.key(), value);
        }
    }
    return result;
}

QJsonObject preserveUnchangedThemeColors(QJsonObject generated,
                                         const QJsonObject &reference,
                                         const QJsonObject &original)
{
    for (auto it = generated.begin(); it != generated.end(); ++it)
    {
        if (it->isObject())
        {
            it.value() = preserveUnchangedThemeColors(
                it->toObject(), reference.value(it.key()).toObject(),
                original.value(it.key()).toObject());
        }
        else
        {
            const auto saved = original.value(it.key());
            if (QColor(saved.toString()).isValid() &&
                QColor(it->toString()) ==
                    QColor(reference.value(it.key()).toString()))
            {
                it.value() = saved;
            }
        }
    }
    return generated;
}

QString depthName(ThemeSurfaceDepth value)
{
    switch (value)
    {
        case ThemeSurfaceDepth::Flat:
            return u"flat"_s;
        case ThemeSurfaceDepth::Layered:
            return u"layered"_s;
        case ThemeSurfaceDepth::Balanced:
            return u"balanced"_s;
    }
    return u"balanced"_s;
}

QString foundationName(ThemeFoundation value)
{
    return value == ThemeFoundation::ChatterinoClassic ? u"classic"_s
                                                       : u"polished"_s;
}

QString accentName(ThemeAccentStrength value)
{
    switch (value)
    {
        case ThemeAccentStrength::Subtle:
            return u"subtle"_s;
        case ThemeAccentStrength::Bold:
            return u"bold"_s;
        case ThemeAccentStrength::Balanced:
            return u"balanced"_s;
    }
    return u"balanced"_s;
}

QString cornerName(ThemeCornerStyle value)
{
    switch (value)
    {
        case ThemeCornerStyle::Classic:
            return u"classic"_s;
        case ThemeCornerStyle::Rounded:
            return u"rounded"_s;
        case ThemeCornerStyle::Soft:
            return u"soft"_s;
    }
    return u"soft"_s;
}

QString tabShapeName(ThemeTabShape value)
{
    return value == ThemeTabShape::Individual ? u"individual"_s
                                              : u"connected"_s;
}

QString wallpaperModeName(ThemeWallpaperMode value)
{
    switch (value)
    {
        case ThemeWallpaperMode::Fit:
            return u"fit"_s;
        case ThemeWallpaperMode::Center:
            return u"center"_s;
        case ThemeWallpaperMode::Tile:
            return u"tile"_s;
        case ThemeWallpaperMode::Stretch:
            return u"stretch"_s;
        case ThemeWallpaperMode::Fill:
            return u"fill"_s;
    }
    return u"fill"_s;
}

ThemeSurfaceDepth parseDepth(const QString &value)
{
    if (value == u"flat"_s)
    {
        return ThemeSurfaceDepth::Flat;
    }
    if (value == u"layered"_s)
    {
        return ThemeSurfaceDepth::Layered;
    }
    return ThemeSurfaceDepth::Balanced;
}

ThemeFoundation parseFoundation(const QString &value)
{
    return value == u"classic"_s ? ThemeFoundation::ChatterinoClassic
                                 : ThemeFoundation::MoltorinoPolished;
}

ThemeAccentStrength parseAccent(const QString &value)
{
    if (value == u"subtle"_s)
    {
        return ThemeAccentStrength::Subtle;
    }
    if (value == u"bold"_s)
    {
        return ThemeAccentStrength::Bold;
    }
    return ThemeAccentStrength::Balanced;
}

ThemeCornerStyle parseCorner(const QString &value)
{
    if (value == u"classic"_s)
    {
        return ThemeCornerStyle::Classic;
    }
    if (value == u"rounded"_s)
    {
        return ThemeCornerStyle::Rounded;
    }
    return ThemeCornerStyle::Soft;
}

ThemeTabShape parseTabShape(const QString &value)
{
    return value == u"individual"_s ? ThemeTabShape::Individual
                                    : ThemeTabShape::Connected;
}

QPoint legacyShadowOffset(const QString &value, int distance)
{
    QPoint direction{1, 1};
    if (value == u"down"_s)
    {
        direction = {0, 1};
    }
    else if (value == u"down-left"_s)
    {
        direction = {-1, 1};
    }
    else if (value == u"right"_s)
    {
        direction = {1, 0};
    }
    else if (value == u"left"_s)
    {
        direction = {-1, 0};
    }
    else if (value == u"up-right"_s)
    {
        direction = {1, -1};
    }
    else if (value == u"up"_s)
    {
        direction = {0, -1};
    }
    else if (value == u"up-left"_s)
    {
        direction = {-1, -1};
    }
    return direction * std::clamp(distance, 1, 4);
}

ThemeWallpaperMode parseWallpaperMode(const QString &value)
{
    if (value == u"fit"_s)
    {
        return ThemeWallpaperMode::Fit;
    }
    if (value == u"center"_s)
    {
        return ThemeWallpaperMode::Center;
    }
    if (value == u"tile"_s)
    {
        return ThemeWallpaperMode::Tile;
    }
    if (value == u"stretch"_s)
    {
        return ThemeWallpaperMode::Stretch;
    }
    return ThemeWallpaperMode::Fill;
}

QColor readColor(const QJsonObject &object, const QString &key,
                 const QColor &fallback)
{
    const QColor result(object.value(key).toString());
    return result.isValid() ? result : fallback;
}

qreal luminanceChannel(qreal value)
{
    return value <= 0.04045 ? value / 12.92
                            : qPow((value + 0.055) / 1.055, 2.4);
}

QColor opaque(QColor color)
{
    return {color.red(), color.green(), color.blue()};
}

bool meetsContrast(const QColor &color,
                   std::initializer_list<QColor> backgrounds, double minimum)
{
    return std::ranges::all_of(backgrounds, [&](const QColor &background) {
        return colorContrastRatio(color, background) >= minimum;
    });
}

QColor moveTextToContrast(QColor color, const QColor &target,
                          std::initializer_list<QColor> backgrounds,
                          double minimum)
{
    color = opaque(color);
    if (meetsContrast(color, backgrounds, minimum))
    {
        return color;
    }
    for (int step = 1; step <= 16; ++step)
    {
        const auto candidate = opaque(mix(color, target, step / 16.0));
        if (meetsContrast(candidate, backgrounds, minimum))
        {
            return candidate;
        }
    }
    return opaque(target);
}

QColor moveBackgroundToContrast(QColor background, const QColor &text,
                                double minimum)
{
    background = opaque(background);
    if (colorContrastRatio(text, background) >= minimum)
    {
        return background;
    }
    const QColor target = text.lightnessF() >= 0.5 ? Qt::black : Qt::white;
    for (int step = 1; step <= 16; ++step)
    {
        const auto candidate = opaque(mix(background, target, step / 16.0));
        if (colorContrastRatio(text, candidate) >= minimum)
        {
            return candidate;
        }
    }
    return target;
}

struct BackgroundContrastRequirement {
    QColor foreground;
    double minimum;
};

QColor moveBackgroundToContrast(
    QColor background, const QColor &target,
    std::initializer_list<BackgroundContrastRequirement> requirements)
{
    const auto authoredAlpha = background.alpha();
    const auto withAuthoredAlpha = [authoredAlpha](QColor color) {
        color = opaque(color);
        color.setAlpha(authoredAlpha);
        return color;
    };
    background = withAuthoredAlpha(background);
    const auto isReadable = [&](const QColor &candidate) {
        return std::ranges::all_of(requirements, [&](const auto &requirement) {
            const auto regularContrast =
                colorContrastRatio(requirement.foreground, target);
            return colorContrastRatio(requirement.foreground, candidate) >=
                   std::min(requirement.minimum, regularContrast);
        });
    };
    if (isReadable(background))
    {
        return background;
    }
    for (int step = 1; step <= 24; ++step)
    {
        const auto candidate =
            withAuthoredAlpha(mix(background, target, step / 24.0));
        if (isReadable(candidate))
        {
            return candidate;
        }
    }
    return withAuthoredAlpha(target);
}

QColor movePaletteBackgroundToContrast(QColor background, const QColor &text,
                                       double minimum)
{
    background = opaque(background);
    if (colorContrastRatio(text, background) >= minimum)
    {
        return background;
    }

    qreal hue = background.hsvHueF();
    qreal saturation = std::max(background.hsvSaturationF(), 0.0F);
    const qreal value = background.valueF();
    if (hue < 0)
    {
        hue = 0;
        saturation = 0;
    }

    const bool needsLighterBackground = text.lightnessF() < 0.5;
    for (int step = 1; step <= 48; ++step)
    {
        const qreal amount = step / 48.0;
        const qreal targetValue = needsLighterBackground ? 1.0 : 0.0;
        const auto candidate = opaque(QColor::fromHsvF(
            hue, saturation, value + (targetValue - value) * amount));
        if (colorContrastRatio(text, candidate) >= minimum)
        {
            return candidate;
        }
    }

    if (needsLighterBackground)
    {
        for (int step = 1; step <= 48; ++step)
        {
            const qreal amount = step / 48.0;
            const auto candidate =
                opaque(QColor::fromHsvF(hue, saturation * (1.0 - amount), 1.0));
            if (colorContrastRatio(text, candidate) >= minimum)
            {
                return candidate;
            }
        }
        return QColor(Qt::white);
    }
    return QColor(Qt::black);
}

template <typename Range>
double minimumContrast(const QColor &foreground, const Range &backgrounds)
{
    double result = std::numeric_limits<double>::max();
    for (const auto &background : backgrounds)
    {
        result = std::min(result, colorContrastRatio(foreground, background));
    }
    return result;
}

template <typename Range>
QColor preferredTextColor(const Range &backgrounds)
{
    const QColor white(Qt::white);
    const QColor black(Qt::black);
    return minimumContrast(white, backgrounds) >=
                   minimumContrast(black, backgrounds)
               ? white
               : black;
}

template <typename Range>
QColor moveTextToContrast(QColor color, const QColor &target,
                          const Range &backgrounds, double minimum)
{
    color = opaque(color);
    if (minimumContrast(color, backgrounds) >= minimum)
    {
        return color;
    }
    for (int step = 1; step <= 24; ++step)
    {
        const auto candidate = opaque(mix(color, target, step / 24.0));
        if (minimumContrast(candidate, backgrounds) >= minimum)
        {
            return candidate;
        }
    }
    return opaque(target);
}

QColor relatedTone(const QColor &seed, bool light, qreal value)
{
    qreal hue = seed.hsvHueF();
    qreal saturation = seed.hsvSaturationF();
    if (hue < 0)
    {
        hue = 0;
        saturation = 0;
    }

    saturation = std::clamp(saturation * (light ? 0.78 : 0.82), 0.0,
                            light ? 0.88 : 0.82);
    return opaque(
        QColor::fromHsvF(hue, saturation, std::clamp(value, 0.0, 1.0)));
}

QImage boxBlurPass(const QImage &source, int radius, bool horizontal)
{
    if (radius <= 0 || source.isNull())
    {
        return source;
    }

    QImage output(source.size(), QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(source.devicePixelRatio());
    const int width = source.width();
    const int height = source.height();
    const int window = radius * 2 + 1;
    const int major = horizontal ? height : width;
    const int minor = horizontal ? width : height;

    for (int line = 0; line < major; ++line)
    {
        qint64 alphaSum = 0;
        qint64 redSum = 0;
        qint64 greenSum = 0;
        qint64 blueSum = 0;
        const auto pixelAt = [&](int position) {
            const int x =
                horizontal ? std::clamp(position, 0, width - 1) : line;
            const int y =
                horizontal ? line : std::clamp(position, 0, height - 1);
            return source.pixel(x, y);
        };
        for (int offset = -radius; offset <= radius; ++offset)
        {
            const auto pixel = pixelAt(offset);
            alphaSum += qAlpha(pixel);
            redSum += qRed(pixel);
            greenSum += qGreen(pixel);
            blueSum += qBlue(pixel);
        }
        for (int position = 0; position < minor; ++position)
        {
            const auto blurred = qRgba(redSum / window, greenSum / window,
                                       blueSum / window, alphaSum / window);
            if (horizontal)
            {
                output.setPixel(position, line, blurred);
            }
            else
            {
                output.setPixel(line, position, blurred);
            }

            const auto leaving = pixelAt(position - radius);
            const auto entering = pixelAt(position + radius + 1);
            alphaSum += qAlpha(entering) - qAlpha(leaving);
            redSum += qRed(entering) - qRed(leaving);
            greenSum += qGreen(entering) - qGreen(leaving);
            blueSum += qBlue(entering) - qBlue(leaving);
        }
    }
    return output;
}

void boxBlurAlphaPass(const std::vector<uint16_t> &source,
                      std::vector<uint16_t> &output, int width, int height,
                      int radius, bool horizontal)
{
    if (radius <= 0)
    {
        output = source;
        return;
    }

    const int window = radius * 2 + 1;
    const int major = horizontal ? height : width;
    const int minor = horizontal ? width : height;

    for (int line = 0; line < major; ++line)
    {
        qint64 alphaSum = 0;
        const auto alphaAt = [&](int position) {
            const int x =
                horizontal ? std::clamp(position, 0, width - 1) : line;
            const int y =
                horizontal ? line : std::clamp(position, 0, height - 1);
            return source[size_t(y) * size_t(width) + size_t(x)];
        };
        for (int offset = -radius; offset <= radius; ++offset)
        {
            alphaSum += alphaAt(offset);
        }
        for (int position = 0; position < minor; ++position)
        {
            const int x = horizontal ? position : line;
            const int y = horizontal ? line : position;
            output[size_t(y) * size_t(width) + size_t(x)] =
                uint16_t(alphaSum / window);

            alphaSum +=
                alphaAt(position + radius + 1) - alphaAt(position - radius);
        }
    }
}

}

QImage loadThemeWallpaper(const QString &source, int maxDimension)
{
    if (isThemeVideo(source))
    {
        return themeVideoPoster(source, maxDimension);
    }
    if (source.trimmed().isEmpty())
    {
        return {};
    }

    QImageReader reader(source);
    reader.setAutoTransform(true);
    const auto sourceSize = reader.size();
    const auto format = reader.format().toLower();
    if (!sourceSize.isValid() ||
        qint64(sourceSize.width()) * sourceSize.height() > 32LL * 1024 * 1024 ||
        (format != "png" && format != "jpeg" && format != "webp" &&
         format != "gif" && format != "bmp"))
    {
        return {};
    }
    maxDimension = std::clamp(maxDimension, 1, MAX_WALLPAPER_DIMENSION);
    if (sourceSize.width() > maxDimension ||
        sourceSize.height() > maxDimension)
    {
        reader.setScaledSize(
            sourceSize.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio));
    }

    auto image = reader.read();
    if (!image.isNull() &&
        (image.width() > maxDimension || image.height() > maxDimension))
    {
        image = image.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image;
}

QImage blurThemeWallpaper(QImage image, int radius)
{
    radius = std::clamp(radius, 0, 30);
    if (image.isNull() || radius == 0)
    {
        return image;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    const int passRadius = std::max(1, (radius + 1) / 2);
    for (int pass = 0; pass < 2; ++pass)
    {
        image = boxBlurPass(image, passRadius, true);
        image = boxBlurPass(image, passRadius, false);
    }
    return image;
}

QImage blurThemeShadow(QImage image, int radius)
{
    radius = std::clamp(radius, 0, 32);
    if (image.isNull() || radius == 0)
    {
        return image;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto dpr = image.devicePixelRatio();
    const int width = image.width();
    const int height = image.height();
    QColor shadowColor(Qt::black);
    int strongestAlpha = 0;

    std::vector<uint16_t> alphaMask(size_t(width) * size_t(height));
    std::vector<uint16_t> scratch(alphaMask.size());
    for (int y = 0; y < height; ++y)
    {
        const auto *line =
            reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x)
        {
            const int alpha = qAlpha(line[x]);
            alphaMask[size_t(y) * size_t(width) + size_t(x)] =
                uint16_t(alpha * 257);

            if (alpha > strongestAlpha)
            {
                strongestAlpha = alpha;
                shadowColor = image.pixelColor(x, y);
            }
        }
    }

    const qreal sigma = 0.55 + radius * 0.72;
    constexpr int PASS_COUNT = 3;
    const qreal idealWidth = qSqrt((12.0 * sigma * sigma / PASS_COUNT) + 1.0);
    int narrowWidth = qFloor(idealWidth);
    if ((narrowWidth & 1) == 0)
    {
        --narrowWidth;
    }
    narrowWidth = std::max(1, narrowWidth);
    const int wideWidth = narrowWidth + 2;
    const qreal narrowPasses =
        (12.0 * sigma * sigma - PASS_COUNT * narrowWidth * narrowWidth -
         4.0 * PASS_COUNT * narrowWidth - 3.0 * PASS_COUNT) /
        (-4.0 * narrowWidth - 4.0);
    const int useNarrow = std::clamp(qRound(narrowPasses), 0, PASS_COUNT);

    for (int pass = 0; pass < PASS_COUNT; ++pass)
    {
        const int boxWidth = pass < useNarrow ? narrowWidth : wideWidth;
        const int passRadius = (boxWidth - 1) / 2;
        if (passRadius == 0)
        {
            continue;
        }
        boxBlurAlphaPass(alphaMask, scratch, width, height, passRadius, true);
        boxBlurAlphaPass(scratch, alphaMask, width, height, passRadius, false);
    }

    QImage blurred(image.size(), QImage::Format_ARGB32_Premultiplied);
    blurred.setDevicePixelRatio(dpr);
    for (int y = 0; y < height; ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(blurred.scanLine(y));
        for (int x = 0; x < width; ++x)
        {
            const auto alpha =
                (alphaMask[size_t(y) * size_t(width) + size_t(x)] + 128) / 257;
            line[x] = qPremultiply(qRgba(shadowColor.red(), shadowColor.green(),
                                         shadowColor.blue(), alpha));
        }
    }
    return blurred;
}

void buildThemeChatFramePaths(const QRectF &rect, qreal radius, qreal width,
                              QPainterPath &outside, QPainterPath &innerBorder)
{
    outside = {};
    innerBorder = {};
    if (rect.isEmpty() || radius <= 0)
    {
        return;
    }

    radius = std::min(radius, std::min(rect.width(), rect.height()) / 2.0);
    width = std::clamp(width, 0.0, radius);

    QPainterPath outer;
    outer.addRoundedRect(rect, radius, radius);
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(rect);
    outside.addPath(outer);

    if (width <= 0)
    {
        return;
    }

    const QRectF innerRect = rect.adjusted(width, width, -width, -width);
    QPainterPath inner;
    if (!innerRect.isEmpty())
    {
        const auto innerRadius = std::max<qreal>(0, radius - width);
        inner.addRoundedRect(innerRect, innerRadius, innerRadius);
    }
    innerBorder.setFillRule(Qt::OddEvenFill);
    innerBorder.addPath(outer);
    innerBorder.addPath(inner);
}

void paintThemeChatFrame(QPainter &painter, const QRectF &rect, qreal radius,
                         qreal width, const QColor &topSurface,
                         const QColor &bottomSurface, const QColor &borderColor,
                         int borderOpacity, const QPainterPath *cachedOutside,
                         const QPainterPath *cachedInnerBorder)
{
    if (rect.isEmpty() || radius <= 0)
    {
        return;
    }
    borderOpacity = std::clamp(borderOpacity, 0, 100);

    QPainterPath localOutside;
    QPainterPath localInnerBorder;
    if (!cachedOutside || !cachedInnerBorder)
    {
        buildThemeChatFramePaths(rect, radius, width, localOutside,
                                 localInnerBorder);
        cachedOutside = &localOutside;
        cachedInnerBorder = &localInnerBorder;
    }

    QLinearGradient outsideGradient(rect.topLeft(), rect.bottomLeft());
    outsideGradient.setColorAt(0.0, topSurface);
    outsideGradient.setColorAt(1.0, bottomSurface);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.fillPath(*cachedOutside, outsideGradient);
    if (borderOpacity > 0 && borderColor.isValid() &&
        !cachedInnerBorder->isEmpty())
    {
        auto color = borderColor;
        color.setAlphaF(color.alphaF() * borderOpacity / 100.0);
        painter.fillPath(*cachedInnerBorder, color);
    }
    painter.restore();
}

QColor blendThemeHighlight(const QColor &base, QColor highlight,
                           int opacityAdjustment)
{
    if (opacityAdjustment != 0)
    {
        highlight.setAlphaF(
            std::clamp(highlight.alphaF() +
                           std::clamp(opacityAdjustment, -100, 100) / 100.0,
                       0.0, 1.0));
    }
    const qreal sourceAlpha = highlight.alphaF();
    const qreal baseAlpha = base.alphaF();
    const qreal outputAlpha = sourceAlpha + baseAlpha * (1.0 - sourceAlpha);
    if (outputAlpha <= 0.0)
    {
        return Qt::transparent;
    }

    const qreal baseContribution = baseAlpha * (1.0 - sourceAlpha);
    QColor result;
    result.setRgbF(
        (highlight.redF() * sourceAlpha + base.redF() * baseContribution) /
            outputAlpha,
        (highlight.greenF() * sourceAlpha + base.greenF() * baseContribution) /
            outputAlpha,
        (highlight.blueF() * sourceAlpha + base.blueF() * baseContribution) /
            outputAlpha,
        outputAlpha);
    return result;
}

ThemeWallpaperLayout layoutThemeWallpaper(const QSizeF &imageSize,
                                          const QRectF &target,
                                          ThemeWallpaperMode mode, int focalX,
                                          int focalY, int zoom)
{
    ThemeWallpaperLayout result{target, QRectF(QPointF(), imageSize)};
    if (imageSize.isEmpty() || target.isEmpty() ||
        mode == ThemeWallpaperMode::Tile)
    {
        return result;
    }

    if (mode == ThemeWallpaperMode::Stretch)
    {
        const auto size = imageSize / (std::clamp(zoom, 100, 300) / 100.0);
        result.source = QRectF(QPointF((imageSize.width() - size.width()) *
                                           std::clamp(focalX, 0, 100) / 100.0,
                                       (imageSize.height() - size.height()) *
                                           std::clamp(focalY, 0, 100) / 100.0),
                               size);
        return result;
    }

    qreal scale = 1;
    if (mode != ThemeWallpaperMode::Center)
    {
        const auto x = target.width() / imageSize.width();
        const auto y = target.height() / imageSize.height();
        scale =
            mode == ThemeWallpaperMode::Fit ? std::min(x, y) : std::max(x, y);
    }
    scale *= std::clamp(zoom, 100, 300) / 100.0;
    if (mode == ThemeWallpaperMode::Center && zoom <= 100)
    {
        focalX = focalY = 50;
    }
    const auto size = imageSize * scale;

    const auto offset = [](qreal spare, int focus) {
        return spare * (spare < 0 ? std::clamp(focus, 0, 100) / 100.0 : 0.5);
    };
    const QRectF placed(
        target.topLeft() +
            QPointF(offset(target.width() - size.width(), focalX),
                    offset(target.height() - size.height(), focalY)),
        size);
    result.destination = placed.intersected(target);
    result.source =
        QRectF((result.destination.topLeft() - placed.topLeft()) / scale,
               result.destination.size() / scale);
    return result;
}

qreal themeTabCornerShoulder(qreal radius, qreal height)
{
    return std::min(radius, height);
}

QPainterPath themeTopTabPath(const QRectF &rect, qreal radius, bool roundLeft,
                             bool roundRight)
{
    QPainterPath path;
    if (radius <= 0 || (!roundLeft && !roundRight))
    {
        path.addRect(rect);
        return path;
    }

    radius = std::min({radius, rect.width() / 2.0, rect.height()});
    constexpr qreal K = 0.5522847498;
    path.moveTo(rect.left(), rect.bottom());
    path.lineTo(rect.left(), roundLeft ? rect.top() + radius : rect.top());
    if (roundLeft)
    {
        path.cubicTo(rect.left(), rect.top() + radius * (1.0 - K),
                     rect.left() + radius * (1.0 - K), rect.top(),
                     rect.left() + radius, rect.top());
    }
    path.lineTo(roundRight ? rect.right() - radius : rect.right(), rect.top());
    if (roundRight)
    {
        path.cubicTo(rect.right() - radius * (1.0 - K), rect.top(),
                     rect.right(), rect.top() + radius * (1.0 - K),
                     rect.right(), rect.top() + radius);
    }
    path.lineTo(rect.right(), rect.bottom());
    path.closeSubpath();
    return path;
}

bool ThemeCustomizationProfile::isValid() const
{
    return !this->name.trimmed().isEmpty() && this->background.isValid() &&
           this->chatBackground.isValid() && this->surface.isValid() &&
           this->raisedSurface.isValid() && this->text.isValid() &&
           this->effectiveChatText().isValid() && this->mutedText.isValid() &&
           this->accent.isValid() && this->chatBorderColor.isValid() &&
           this->messageShadowColor.isValid() &&
           this->wallpaperOverlayColor.isValid();
}

QColor ThemeCustomizationProfile::effectiveChatText() const
{
    return this->separateChatText && this->chatText.isValid() ? this->chatText
                                                              : this->text;
}

bool ThemePaletteSelection::any() const
{
    return this->background || this->chatBackground || this->surface ||
           this->raisedSurface || this->accent;
}

bool ThemeCustomizationProfile::hasWallpaper() const
{
    return !this->wallpaperSource.trimmed().isEmpty() ||
           isThemeWallpaperId(this->wallpaperId);
}

int ThemeCustomizationProfile::cornerRadius() const
{
    return std::clamp(this->chatCornerRadius, 0, 16);
}

QPoint ThemeCustomizationProfile::messageShadowOffset() const
{
    return {std::clamp(this->messageShadowOffsetX, -8, 8),
            std::clamp(this->messageShadowOffsetY, -8, 8)};
}

void ThemeCustomizationProfile::clearDisabledFontOverrides()
{
    if (!this->useThemeFonts)
    {
        this->chatFontFamily.clear();
        this->usernameFontFamily.clear();
        this->interfaceFontFamily.clear();
        this->chatFontWeight = 0;
        this->usernameFontWeight = 0;
    }
    if (!this->useThemeFontSizes)
    {
        this->chatFontSize = 0;
        this->usernameFontSize = 0;
        this->interfaceFontSize = 0;
    }
}

ThemeCustomizationProfile customizationProfileFromClassicTheme(
    const QJsonObject &theme, const QString &name, const QString &key)
{
    const auto colors = theme.value(u"colors"_s).toObject();
    const auto window = colors.value(u"window"_s).toObject();
    const auto tabs = colors.value(u"tabs"_s).toObject();
    const auto regularTab = tabs.value(u"regular"_s).toObject();
    const auto selectedTab = tabs.value(u"selected"_s).toObject();
    const auto text = colors.value(u"messages"_s)
                          .toObject()
                          .value(u"textColors"_s)
                          .toObject();
    ThemeCustomizationProfile profile;
    profile.name = name;
    profile.useThemeFonts = false;
    profile.useThemeFontSizes = false;
    profile.baseTheme = key;
    profile.background = readColor(window, u"background"_s, profile.background);
    profile.chatBackground = readColor(colors.value(u"splits"_s).toObject(),
                                       u"background"_s, profile.chatBackground);
    profile.surface = readColor(regularTab.value(u"backgrounds"_s).toObject(),
                                u"regular"_s, profile.surface);
    profile.raisedSurface =
        readColor(selectedTab.value(u"backgrounds"_s).toObject(), u"regular"_s,
                  profile.raisedSurface);
    profile.text = readColor(window, u"text"_s, profile.text);
    profile.chatText = readColor(text, u"regular"_s, profile.text);
    profile.separateChatText = profile.chatText != profile.text;
    profile.systemText = readColor(text, u"system"_s, profile.mutedText);
    profile.mutedText = readColor(regularTab, u"text"_s, profile.systemText);
    profile.timestampText = readColor(text, u"timestamp"_s, profile.systemText);
    profile.accent = readColor(colors, u"accent"_s, profile.accent);
    profile.foundation = ThemeFoundation::ChatterinoClassic;
    profile.useThemeMessageRows = false;
    profile.cornerStyle = ThemeCornerStyle::Classic;
    profile.tabCornerRadius = 0;
    profile.chatCornerRadius = 0;
    profile.roundChat = false;
    if (key == u"Dark" || key == u"Black" || key == u"Light" || key == u"White")
    {
        const auto generated =
            buildCustomizedTheme(profile).value(u"colors"_s).toObject();
        profile.originalColors = validatedThemeColors(
            theme.value(u"colors"_s).toObject(), generated);
        profile.originalGeneratedColors = generated;
    }
    return profile;
}

std::optional<ThemeCustomizationProfile> customizationProfileFromTheme(
    const QJsonObject &theme)
{
    const auto metadata = theme.value(u"metadata"_s).toObject();
    const auto customization = metadata.value(u"moltorino"_s).toObject();
    const int version = customization.value(u"version"_s).toInt();
    if (customization.isEmpty() || version < 1 ||
        version > ThemeCustomizationProfile::CURRENT_VERSION)
    {
        return std::nullopt;
    }

    ThemeCustomizationProfile profile;
    profile.name = metadata.value(u"name"_s).toString(u"Custom theme"_s);
    profile.baseTheme = metadata.value(u"fallbackTheme"_s).toString(u"Dark"_s);
    const auto colors = customization.value(u"colors"_s).toObject();
    profile.background = readColor(colors, u"background"_s, profile.background);
    profile.chatBackground =
        readColor(colors, u"chatBackground"_s, profile.chatBackground);
    profile.surface = readColor(colors, u"surface"_s, profile.surface);
    profile.raisedSurface =
        readColor(colors, u"raisedSurface"_s, profile.raisedSurface);
    profile.text = readColor(colors, u"text"_s, profile.text);
    profile.separateChatText =
        version >= 11 &&
        customization.value(u"separateChatText"_s).toBool(false);
    profile.chatText = version >= 11
                           ? readColor(colors, u"chatText"_s, profile.text)
                           : profile.text;
    if (!profile.separateChatText)
    {
        profile.chatText = profile.text;
    }
    profile.mutedText = readColor(colors, u"mutedText"_s, profile.mutedText);
    profile.systemText = readColor(colors, u"systemText"_s, profile.mutedText);
    profile.timestampText =
        readColor(colors, u"timestampText"_s, profile.systemText);
    profile.accent = readColor(colors, u"accent"_s, profile.accent);

    profile.foundation =
        version >= 2
            ? parseFoundation(customization.value(u"foundation"_s).toString())
            : ThemeFoundation::MoltorinoPolished;
    profile.surfaceDepth =
        parseDepth(customization.value(u"surfaceDepth"_s).toString());
    if (version >= 9)
    {
        profile.panelContrast = std::clamp(
            customization.value(u"panelContrast"_s).toInt(50), 0, 100);
    }
    else
    {
        profile.panelContrast =
            profile.surfaceDepth == ThemeSurfaceDepth::Flat      ? 15
            : profile.surfaceDepth == ThemeSurfaceDepth::Layered ? 85
                                                                 : 50;
    }
    profile.accentStrength =
        parseAccent(customization.value(u"accentStrength"_s).toString());
    profile.cornerStyle =
        parseCorner(customization.value(u"cornerStyle"_s).toString());
    profile.tabShape =
        parseTabShape(customization.value(u"tabShape"_s).toString());
    profile.tabCornerRadius =
        std::clamp(customization.value(u"tabCornerRadius"_s).toInt(3), 0, 10);
    profile.tabSpacing =
        std::clamp(customization.value(u"tabSpacing"_s).toInt(1), 0, 4);
    profile.inactiveTabContrast = std::clamp(
        customization.value(u"inactiveTabContrast"_s).toInt(72), 0, 100);
    profile.channelBarContrast = std::clamp(
        customization.value(u"channelBarContrast"_s).toInt(52), 0, 100);
    profile.roundChat = customization.value(u"roundChat"_s).toBool(false);
    if (version >= 4)
    {
        profile.chatCornerRadius = std::clamp(
            customization.value(u"chatCornerRadius"_s).toInt(6), 0, 16);
    }
    else
    {
        switch (profile.cornerStyle)
        {
            case ThemeCornerStyle::Classic:
                profile.chatCornerRadius = 0;
                break;
            case ThemeCornerStyle::Soft:
                profile.chatCornerRadius = 4;
                break;
            case ThemeCornerStyle::Rounded:
                profile.chatCornerRadius = 8;
                break;
        }
    }
    if (version >= 8)
    {
        const auto border = customization.value(u"chatBorder"_s).toObject();
        profile.chatBorder = border.value(u"enabled"_s).toBool(true);
        profile.chatBorderColor =
            readColor(border, u"color"_s, profile.chatBorderColor);
        profile.chatBorderWidth =
            std::clamp(border.value(u"width"_s).toInt(1), 1, 4);
        profile.chatBorderOpacity =
            std::clamp(border.value(u"opacity"_s).toInt(22), 0, 100);
    }
    else
    {
        const int legacyStrength = std::clamp(
            customization.value(u"chatFrameOpacity"_s).toInt(100), 0, 100);
        profile.chatBorder = legacyStrength > 0;
        profile.chatBorderColor = profile.mutedText;
        profile.chatBorderWidth = 1;
        profile.chatBorderOpacity = qRound(legacyStrength * 0.22);
    }
    if (version >= 5)
    {
        const auto rows = customization.value(u"messageRows"_s).toObject();
        profile.useThemeMessageRows = rows.value(u"managed"_s).toBool(true);
        profile.alternateMessageRows = rows.value(u"enabled"_s).toBool(false);
        profile.alternateMessageOpacity =
            std::clamp(rows.value(u"opacity"_s).toInt(24), 0, 100);
        profile.alternateMessageContrast =
            std::clamp(rows.value(u"contrast"_s).toInt(42), 0, 100);
    }
    else
    {
        profile.useThemeMessageRows = true;
        profile.alternateMessageRows = false;
    }
    profile.highlightOpacityAdjustment = std::clamp(
        customization.value(u"highlightOpacityAdjustment"_s).toInt(0), -100,
        100);
    profile.messageShadow =
        customization.value(u"messageShadow"_s).toBool(false);
    profile.messageShadowEmotes =
        customization.value(u"messageShadowEmotes"_s).toBool(true);
    profile.messageShadowColor =
        readColor(customization, u"messageShadowColor"_s, QColor(Qt::black));
    profile.messageShadowOpacity = std::clamp(
        customization.value(u"messageShadowOpacity"_s).toInt(58), 0, 100);
    if (version >= 5)
    {
        profile.messageShadowOffsetX = std::clamp(
            customization.value(u"messageShadowOffsetX"_s).toInt(1), -8, 8);
        profile.messageShadowOffsetY = std::clamp(
            customization.value(u"messageShadowOffsetY"_s).toInt(1), -8, 8);
        profile.messageShadowBlur = std::clamp(
            customization.value(u"messageShadowBlur"_s).toInt(2), 0, 8);
    }
    else
    {
        const auto legacyOffset = legacyShadowOffset(
            customization.value(u"messageShadowDirection"_s).toString(),
            customization.value(u"messageShadowDistance"_s).toInt(1));
        profile.messageShadowOffsetX = legacyOffset.x();
        profile.messageShadowOffsetY = legacyOffset.y();
        const int legacySoftness = std::clamp(
            customization.value(u"messageShadowSoftness"_s).toInt(1), 0, 2);
        profile.messageShadowBlur = legacySoftness == 0   ? 0
                                    : legacySoftness == 1 ? 2
                                                          : 5;
    }

    const auto wallpaper = customization.value(u"wallpaper"_s).toObject();
    profile.wallpaperSource = wallpaper.value(u"source"_s).toString();
    const auto wallpaperId = wallpaper.value(u"id"_s).toString();
    if (isThemeWallpaperId(wallpaperId))
    {
        profile.wallpaperId = wallpaperId;
    }
    profile.wallpaperMode =
        parseWallpaperMode(wallpaper.value(u"mode"_s).toString());
    profile.wallpaperOpacity =
        std::clamp(wallpaper.value(u"opacity"_s).toInt(100), 0, 100);
    profile.wallpaperOverlayColor =
        readColor(wallpaper, u"overlayColor"_s, QColor(Qt::black));
    profile.wallpaperOverlayOpacity =
        std::clamp(version >= 8 ? wallpaper.value(u"overlayOpacity"_s).toInt(42)
                                : wallpaper.value(u"darken"_s).toInt(42),
                   0, 100);
    profile.wallpaperBlur =
        std::clamp(wallpaper.value(u"blur"_s).toInt(0), 0, 30);
    profile.wallpaperZoom =
        std::clamp(wallpaper.value(u"zoom"_s).toInt(100), 100, 300);
    profile.wallpaperFocalX =
        std::clamp(wallpaper.value(u"focalX"_s).toInt(50), 0, 100);
    profile.wallpaperFocalY =
        std::clamp(wallpaper.value(u"focalY"_s).toInt(50), 0, 100);

    const auto typography = customization.value(u"typography"_s).toObject();

    profile.useThemeFonts = typography.value(u"useThemeFonts"_s).toBool(true);
    profile.useThemeFontSizes =
        typography.value(u"useThemeFontSizes"_s).toBool(true);
    profile.chatFontFamily = typography.value(u"chatFamily"_s).toString();
    profile.chatFontSize =
        std::clamp(typography.value(u"chatSize"_s).toInt(0), 0, 40);
    profile.chatFontWeight =
        std::clamp(typography.value(u"chatWeight"_s).toInt(0), 0, 900);
    if (version >= 7)
    {
        profile.usernameFontFamily =
            typography.value(u"usernameFamily"_s).toString();
        profile.usernameFontSize =
            std::clamp(typography.value(u"usernameSize"_s).toInt(0), 0, 40);
    }
    profile.usernameFontWeight =
        std::clamp(typography.value(u"usernameWeight"_s).toInt(0), 0, 900);
    profile.interfaceFontFamily =
        typography.value(u"interfaceFamily"_s).toString();
    profile.interfaceFontSize =
        std::clamp(typography.value(u"interfaceSize"_s).toInt(0), 0, 30);

    const bool legacyDefaultFonts =
        version < 12 && profile.name == u"Moltorino" &&
        profile.chatFontFamily == u"Gabarito" &&
        profile.usernameFontFamily == u"Gabarito" &&
        profile.interfaceFontFamily == u"Gabarito" &&
        profile.chatFontWeight == 600 && profile.usernameFontWeight == 700;
    if (legacyDefaultFonts)
    {
        profile.useThemeFonts =
            typography.value(u"useThemeFonts"_s).toBool(false);
        if (profile.chatFontSize == 14 && profile.usernameFontSize == 14 &&
            profile.interfaceFontSize == 10)
        {
            profile.useThemeFontSizes =
                typography.value(u"useThemeFontSizes"_s).toBool(false);
        }
    }
    if (version >= 16 &&
        customization.value(u"originalColors"_s).isObject() &&
        customization.value(u"originalGeneratedColors"_s).isObject())
    {
        const auto shape =
            buildCustomizedTheme(profile).value(u"colors"_s).toObject();
        profile.originalColors = validatedThemeColors(
            customization.value(u"originalColors"_s).toObject(), shape);
        profile.originalGeneratedColors = validatedThemeColors(
            customization.value(u"originalGeneratedColors"_s).toObject(),
            shape);
    }
    return profile.isValid() ? std::optional(profile) : std::nullopt;
}

QJsonObject buildCustomizedTheme(const ThemeCustomizationProfile &input)
{
    auto profile = input;
    profile.chatText = profile.effectiveChatText();
    if (!profile.systemText.isValid())
    {
        profile.systemText = profile.mutedText;
    }
    if (!profile.timestampText.isValid())
    {
        profile.timestampText = profile.systemText;
    }
    const bool light = colorContrastRatio(Qt::black, profile.background) >
                       colorContrastRatio(Qt::white, profile.background);
    const QColor black = Qt::black;
    const QColor white = Qt::white;
    const QColor depthTarget = light ? black : white;
    const qreal panelContrast =
        std::clamp(profile.panelContrast, 0, 100) / 100.0;
    const qreal depth = 0.025 + panelContrast * 0.16;
    const qreal accentMix =
        profile.accentStrength == ThemeAccentStrength::Subtle ? 0.35
        : profile.accentStrength == ThemeAccentStrength::Bold ? 0.9
                                                              : 0.62;

    const qreal flatten = std::max(0.0, (0.5 - panelContrast) * 2.0);
    const qreal deepen = std::max(0.0, (panelContrast - 0.5) * 2.0);
    const QColor effectiveSurface =
        mix(mix(profile.surface, profile.background, flatten * 0.58),
            depthTarget, deepen * 0.055);
    const QColor effectiveRaisedSurface =
        mix(mix(profile.raisedSurface, effectiveSurface, flatten * 0.48),
            depthTarget, deepen * 0.08);
    const QColor hover = mix(effectiveSurface, depthTarget, depth);
    const QColor selected =
        mix(effectiveRaisedSurface, profile.accent,
            profile.accentStrength == ThemeAccentStrength::Bold ? 0.20 : 0.10);
    const QColor border = mix(effectiveSurface, depthTarget, depth * 1.7);
    const QColor accentLine = mix(profile.mutedText, profile.accent, accentMix);
    const qreal inactiveContrast = profile.inactiveTabContrast / 100.0;
    const QColor inactiveTab =
        mix(profile.background, effectiveSurface, inactiveContrast);
    const QColor inactiveSelected =
        mix(profile.background, effectiveRaisedSurface, inactiveContrast);
    const qreal headerContrast = profile.channelBarContrast / 100.0;
    const QColor channelBar =
        mix(effectiveSurface, effectiveRaisedSurface, headerContrast * 0.52);
    const QColor focusedChannelBar =
        profile.foundation == ThemeFoundation::MoltorinoPolished ? channelBar
                                                                 : selected;
    const QColor messageRegular = profile.hasWallpaper()
                                      ? alpha(profile.chatBackground, 0)
                                      : profile.chatBackground;
    const qreal rowContrast = profile.alternateMessageContrast / 100.0;
    const QColor alternateTarget = mix(profile.chatBackground, effectiveSurface,
                                       0.15 + rowContrast * 0.70);
    QColor solidMessageAlternate = profile.chatBackground;
    if (profile.alternateMessageRows)
    {
        const auto proposedAlternate =
            profile.hasWallpaper()
                ? alternateTarget
                : mix(profile.chatBackground, alternateTarget,
                      profile.alternateMessageOpacity / 100.0);
        solidMessageAlternate =
            moveBackgroundToContrast(proposedAlternate, profile.chatBackground,
                                     {{profile.chatText, 4.5},
                                      {profile.systemText, 3.0},
                                      {profile.timestampText, 2.5}});
    }
    const QColor messageAlternate =
        !profile.alternateMessageRows ? messageRegular
        : profile.hasWallpaper()
            ? alpha(solidMessageAlternate, profile.alternateMessageOpacity)
            : solidMessageAlternate;

    const QColor chatLink = moveTextToContrast(
        mix(profile.accent, profile.chatText, 0.12), profile.chatText,
        {profile.chatBackground, solidMessageAlternate}, 3.0);
    const QJsonObject textColors{
        {u"caret"_s, encoded(profile.chatText)},
        {u"chatPlaceholder"_s, encoded(alpha(profile.mutedText, 72))},
        {u"link"_s, encoded(chatLink)},
        {u"regular"_s, encoded(profile.chatText)},
        {u"system"_s, encoded(profile.systemText)},
        {u"timestamp"_s, encoded(profile.timestampText)},
    };

    QJsonObject colors{
        {u"accent"_s, encoded(profile.accent)},
        {u"window"_s,
         QJsonObject{{u"background"_s, encoded(profile.background)},
                     {u"text"_s, encoded(profile.text)}}},
        {u"messages"_s,
         QJsonObject{
             {u"backgrounds"_s,
              QJsonObject{{u"regular"_s, encoded(messageRegular)},
                          {u"alternate"_s, encoded(messageAlternate)}}},
             {u"disabled"_s, encoded(alpha(profile.chatBackground, 58))},
             {u"selection"_s, encoded(alpha(profile.accent, 34))},
             {u"highlightAnimationStart"_s, encoded(alpha(profile.accent, 48))},
             {u"highlightAnimationEnd"_s, encoded(alpha(profile.chatText, 0))},
             {u"textColors"_s, textColors},
         }},
        {u"overlayMessages"_s,
         QJsonObject{
             {u"backgrounds"_s,
              QJsonObject{{u"regular"_s, u"transparent"_s},
                          {u"alternate"_s,
                           encoded(alpha(profile.chatBackground, 20))}}},
             {u"disabled"_s, encoded(alpha(profile.chatBackground, 42))},
             {u"selection"_s, encoded(alpha(profile.accent, 34))},
             {u"textColors"_s, textColors},
             {u"background"_s, encoded(profile.chatBackground)},
         }},
        {u"scrollbars"_s,
         QJsonObject{
             {u"background"_s, u"transparent"_s},
             {u"thumb"_s, encoded(mix(effectiveSurface, profile.text, 0.30))},
             {u"thumbSelected"_s,
              encoded(mix(effectiveSurface, profile.text, 0.44))}}},
        {u"splits"_s,
         QJsonObject{
             {u"background"_s, encoded(profile.chatBackground)},
             {u"dropPreview"_s, encoded(alpha(profile.accent, 28))},
             {u"dropPreviewBorder"_s, encoded(profile.accent)},
             {u"dropTargetRect"_s, encoded(alpha(profile.accent, 12))},
             {u"dropTargetRectBorder"_s, encoded(profile.accent)},
             {u"messageSeperator"_s, encoded(border)},
             {u"resizeHandle"_s, encoded(alpha(profile.accent, 58))},
             {u"resizeHandleBackground"_s, encoded(alpha(profile.accent, 18))},
             {u"header"_s,
              QJsonObject{{u"background"_s, encoded(channelBar)},
                          {u"border"_s, encoded(border)},
                          {u"focusedBackground"_s, encoded(focusedChannelBar)},
                          {u"focusedBorder"_s, encoded(border)},
                          {u"focusedText"_s, encoded(profile.text)},
                          {u"text"_s, encoded(profile.text)}}},
             {u"input"_s,
              QJsonObject{
                  {u"background"_s, encoded(effectiveSurface)},
                  {u"backgroundPulse"_s,
                   encoded(mix(effectiveSurface, QColor("#245424"), 0.35))},
                  {u"searchFailText"_s, u"#ff5757"_s},
                  {u"searchHighlightBackground"_s,
                   encoded(alpha(profile.accent, 36))},
                  {u"text"_s, encoded(profile.text)}}},
         }},
        {u"tabs"_s,
         QJsonObject{
             {u"liveIndicator"_s, u"#ff3b4f"_s},
             {u"rerunIndicator"_s, u"#d8c94d"_s},
             {u"dividerLine"_s, encoded(border)},
             {u"regular"_s, tabState(profile.mutedText, inactiveTab, hover,
                                     inactiveTab, border)},
             {u"newMessage"_s,
              tabState(profile.text, inactiveTab, hover, inactiveTab,
                       mix(profile.mutedText, profile.accent, 0.24))},
             {u"highlighted"_s,
              tabState(profile.text, inactiveTab, hover, inactiveTab,
                       mix(QColor("#ee6166"), profile.accent, 0.30))},
             {u"selected"_s, tabState(profile.text, selected, hover,
                                      inactiveSelected, accentLine)},
         }},
    };

    QJsonObject customization{
        {u"version"_s, ThemeCustomizationProfile::CURRENT_VERSION},
        {u"foundation"_s, foundationName(profile.foundation)},
        {u"surfaceDepth"_s,
         depthName(profile.panelContrast < 34   ? ThemeSurfaceDepth::Flat
                   : profile.panelContrast > 66 ? ThemeSurfaceDepth::Layered
                                                : ThemeSurfaceDepth::Balanced)},
        {u"panelContrast"_s, profile.panelContrast},
        {u"accentStrength"_s, accentName(profile.accentStrength)},
        {u"cornerStyle"_s, cornerName(profile.cornerStyle)},
        {u"tabShape"_s, tabShapeName(profile.tabShape)},
        {u"tabCornerRadius"_s, profile.tabCornerRadius},
        {u"tabSpacing"_s, profile.tabSpacing},
        {u"inactiveTabContrast"_s, profile.inactiveTabContrast},
        {u"channelBarContrast"_s, profile.channelBarContrast},
        {u"separateChatText"_s, profile.separateChatText},
        {u"roundChat"_s, profile.roundChat},
        {u"chatCornerRadius"_s, profile.chatCornerRadius},
        {u"chatBorder"_s,
         QJsonObject{{u"enabled"_s, profile.chatBorder},
                     {u"color"_s, encoded(profile.chatBorderColor)},
                     {u"width"_s, profile.chatBorderWidth},
                     {u"opacity"_s, profile.chatBorderOpacity}}},
        {u"messageRows"_s,
         QJsonObject{{u"managed"_s, profile.useThemeMessageRows},
                     {u"enabled"_s, profile.alternateMessageRows},
                     {u"opacity"_s, profile.alternateMessageOpacity},
                     {u"contrast"_s, profile.alternateMessageContrast}}},
        {u"highlightOpacityAdjustment"_s, profile.highlightOpacityAdjustment},
        {u"messageShadow"_s, profile.messageShadow},
        {u"messageShadowEmotes"_s, profile.messageShadowEmotes},
        {u"messageShadowColor"_s, encoded(profile.messageShadowColor)},
        {u"messageShadowOpacity"_s, profile.messageShadowOpacity},
        {u"messageShadowOffsetX"_s, profile.messageShadowOffsetX},
        {u"messageShadowOffsetY"_s, profile.messageShadowOffsetY},
        {u"messageShadowBlur"_s, profile.messageShadowBlur},
        {u"colors"_s,
         QJsonObject{{u"background"_s, encoded(profile.background)},
                     {u"chatBackground"_s, encoded(profile.chatBackground)},
                     {u"surface"_s, encoded(profile.surface)},
                     {u"raisedSurface"_s, encoded(profile.raisedSurface)},
                     {u"text"_s, encoded(profile.text)},
                     {u"chatText"_s, encoded(profile.chatText)},
                     {u"mutedText"_s, encoded(profile.mutedText)},
                     {u"systemText"_s, encoded(profile.systemText)},
                     {u"timestampText"_s, encoded(profile.timestampText)},
                     {u"accent"_s, encoded(profile.accent)}}},
        {u"wallpaper"_s,
         QJsonObject{
             {u"source"_s, profile.wallpaperSource},
             {u"id"_s, profile.wallpaperId},
             {u"mode"_s, wallpaperModeName(profile.wallpaperMode)},
             {u"opacity"_s, profile.wallpaperOpacity},
             {u"overlayColor"_s, encoded(profile.wallpaperOverlayColor)},
             {u"overlayOpacity"_s, profile.wallpaperOverlayOpacity},
             {u"blur"_s, profile.wallpaperBlur},
             {u"zoom"_s, profile.wallpaperZoom},
             {u"focalX"_s, profile.wallpaperFocalX},
             {u"focalY"_s, profile.wallpaperFocalY}}},
        {u"typography"_s,
         QJsonObject{{u"useThemeFonts"_s, profile.useThemeFonts},
                     {u"useThemeFontSizes"_s, profile.useThemeFontSizes},
                     {u"chatFamily"_s, profile.chatFontFamily},
                     {u"chatSize"_s, profile.chatFontSize},
                     {u"chatWeight"_s, profile.chatFontWeight},
                     {u"usernameFamily"_s, profile.usernameFontFamily},
                     {u"usernameSize"_s, profile.usernameFontSize},
                     {u"usernameWeight"_s, profile.usernameFontWeight},
                     {u"interfaceFamily"_s, profile.interfaceFontFamily},
                     {u"interfaceSize"_s, profile.interfaceFontSize}}},
    };

    if (!profile.originalColors.isEmpty() &&
        !profile.originalGeneratedColors.isEmpty())
    {
        const auto original =
            validatedThemeColors(profile.originalColors, colors);
        const auto reference =
            validatedThemeColors(profile.originalGeneratedColors, colors);
        customization.insert(u"originalColors"_s, original);
        customization.insert(u"originalGeneratedColors"_s, reference);
        colors = preserveUnchangedThemeColors(colors, reference, original);
    }

    return {
        {u"$schema"_s, u"../../docs/ChatterinoTheme.schema.json"_s},
        {u"metadata"_s,
         QJsonObject{
             {u"name"_s, profile.name},
             {u"iconTheme"_s, light ? u"dark"_s : u"light"_s},
             {u"fallbackTheme"_s,
              light && profile.originalColors.isEmpty() ? u"Light"_s
                                                       : profile.baseTheme},
             {u"moltorino"_s, customization}}},
        {u"colors"_s, colors},
    };
}

QJsonObject makeShareableCustomizedTheme(const QJsonObject &theme,
                                         bool *wallpaperRemoved,
                                         bool includeWallpaper)
{
    auto result = theme;
    auto metadata = result.value(u"metadata"_s).toObject();
    auto customization = metadata.value(u"moltorino"_s).toObject();
    auto wallpaper = customization.value(u"wallpaper"_s).toObject();
    const auto source = wallpaper.value(u"source"_s).toString();
    const auto id = wallpaper.value(u"id"_s).toString();
    const bool validId = includeWallpaper && isThemeWallpaperId(id);
    if (!validId)
    {
        wallpaper.remove(u"id"_s);
    }
    const bool validBundledWallpaper =
        includeWallpaper && source.startsWith(u":/themes/wallpapers/"_s) &&
        !loadThemeWallpaper(source).isNull();
    const bool remove = !source.isEmpty() && !validBundledWallpaper;
    if (remove)
    {
        wallpaper.insert(u"source"_s, QString());
    }
    customization.insert(u"wallpaper"_s, wallpaper);
    metadata.insert(u"moltorino"_s, customization);
    result.insert(u"metadata"_s, metadata);

    if (auto profile = customizationProfileFromTheme(result))
    {
        result = buildCustomizedTheme(*profile);
    }
    if (wallpaperRemoved)
    {
        *wallpaperRemoved = remove && !validId;
    }
    return result;
}

double colorContrastRatio(const QColor &foreground, const QColor &background)
{
    const auto luminance = [](const QColor &color) {
        return 0.2126 * luminanceChannel(color.redF()) +
               0.7152 * luminanceChannel(color.greenF()) +
               0.0722 * luminanceChannel(color.blueF());
    };
    const double a = luminance(foreground);
    const double b = luminance(background);
    const double lighter = std::max(a, b);
    const double darker = std::min(a, b);
    return (lighter + 0.05) / (darker + 0.05);
}

std::optional<ThemeCustomizationProfile> customizationProfileFromBluzyrinoTheme(
    const QJsonObject &theme, const QString &name)
{
    if (theme.value(u"format"_s).toString() != u"bluzyrino-theme"_s)
    {
        return std::nullopt;
    }
    const int version = theme.value(u"version"_s).toInt();
    if (version < 1 || version > 2)
    {
        return std::nullopt;
    }

    QColor primary(theme.value(u"primary"_s).toString());
    QColor secondary(theme.value(u"secondary"_s).toString());
    QColor sourceText(theme.value(u"text"_s).toString());
    if (!primary.isValid() || !secondary.isValid() || !sourceText.isValid())
    {
        return std::nullopt;
    }
    primary = opaque(primary);
    secondary = opaque(secondary);
    sourceText = opaque(sourceText);

    const QColor white(Qt::white);
    const QColor black(Qt::black);
    const double whiteMinimum = std::min(colorContrastRatio(white, primary),
                                         colorContrastRatio(white, secondary));
    const double blackMinimum = std::min(colorContrastRatio(black, primary),
                                         colorContrastRatio(black, secondary));
    const bool lightText = whiteMinimum >= blackMinimum;
    const QColor textTarget = lightText ? white : black;
    const QColor depthTarget = lightText ? black : white;

    primary = moveBackgroundToContrast(primary, textTarget, 4.5);
    secondary = moveBackgroundToContrast(secondary, textTarget, 4.5);

    ThemeCustomizationProfile profile;
    profile.name = name.simplified().left(80);
    if (profile.name.isEmpty())
    {
        profile.name = u"Bluzyrino theme"_s;
    }
    profile.baseTheme = lightText ? u"Dark"_s : u"Light"_s;
    profile.background = primary;
    profile.chatBackground = moveBackgroundToContrast(
        mix(primary, depthTarget, 0.06), textTarget, 4.5);
    profile.surface = moveBackgroundToContrast(mix(primary, secondary, 0.76),
                                               textTarget, 4.5);
    profile.raisedSurface = secondary;
    if (colorContrastRatio(profile.surface, profile.raisedSurface) < 1.08)
    {
        profile.raisedSurface = moveBackgroundToContrast(
            mix(secondary, textTarget, 0.10), textTarget, 4.5);
    }

    profile.text =
        moveTextToContrast(sourceText, textTarget,
                           {profile.background, profile.chatBackground,
                            profile.surface, profile.raisedSurface},
                           4.5);
    profile.accent = moveTextToContrast(
        secondary, textTarget,
        {profile.background, profile.surface, profile.raisedSurface}, 3.0);
    profile.mutedText = moveTextToContrast(
        mix(profile.text, profile.chatBackground, 0.44), textTarget,
        {profile.background, profile.chatBackground, profile.surface,
         profile.raisedSurface},
        3.0);

    QColor sourceSystem;
    QColor sourceTimestamp;
    if (version >= 2)
    {
        sourceSystem = QColor(theme.value(u"systemText"_s).toString());
        sourceTimestamp = QColor(theme.value(u"timestampText"_s).toString());
    }
    profile.systemText = moveTextToContrast(
        sourceSystem.isValid() ? opaque(sourceSystem)
                               : mix(profile.text, profile.accent, 0.24),
        textTarget, {profile.chatBackground}, 3.0);
    profile.timestampText = moveTextToContrast(
        sourceTimestamp.isValid()
            ? opaque(sourceTimestamp)
            : mix(profile.text, profile.chatBackground, 0.50),
        textTarget, {profile.chatBackground}, 2.5);

    profile.foundation = ThemeFoundation::MoltorinoPolished;
    profile.panelContrast = 44;
    profile.roundChat = true;
    profile.chatCornerRadius = 6;
    profile.chatBorderColor = profile.mutedText;
    profile.chatBorderOpacity = 18;
    profile.alternateMessageRows = true;
    profile.alternateMessageOpacity = 12;
    profile.alternateMessageContrast = 30;
    profile.chatText = profile.text;
    profile.separateChatText = false;
    return profile.isValid() ? std::optional(profile) : std::nullopt;
}

std::optional<ThemeCustomizationProfile>
    customizationProfileFromBluzyrinoSettings(const QJsonObject &settings)
{
    const auto theme =
        settings.value(u"bluzyrino"_s).toObject().value(u"theme"_s).toObject();
    const auto appearance = settings.value(u"appearance"_s).toObject();
    const auto wallpaper = appearance.value(u"backgroundImage"_s).toObject();
    if (!settings.value(u"bluzyrino"_s).toObject().value(u"theme"_s).isObject())
    {
        return std::nullopt;
    }

    QJsonObject palette;

    for (const auto &[key, value] :
         {std::pair{u"primary"_s, u"#101A2B"_s},
          std::pair{u"secondary"_s, u"#17365F"_s},
          std::pair{u"text"_s, u"#EAF4FF"_s},
          std::pair{u"systemText"_s, u"#EAF4FF"_s},
          std::pair{u"timestampText"_s, u"#EAF4FF"_s}})
    {
        palette.insert(
            key, theme.contains(key) ? theme.value(key) : QJsonValue(value));
    }
    palette.insert(u"format"_s, u"bluzyrino-theme"_s);
    palette.insert(u"version"_s, 2);
    const auto adapted =
        customizationProfileFromBluzyrinoTheme(palette, u"Bluzyrino"_s);
    if (!adapted)
    {
        return std::nullopt;
    }
    auto profile = *adapted;
    profile.name = u"Bluzyrino"_s;

    profile.useThemeFonts = false;
    profile.useThemeFontSizes = false;
    profile.wallpaperSource.clear();
    profile.wallpaperId.clear();
    profile.wallpaperMode =
        wallpaper.value(u"mode"_s).toString(u"Stretch"_s) == u"Stretch"_s
            ? ThemeWallpaperMode::Stretch
            : ThemeWallpaperMode::Fill;
    profile.wallpaperOpacity =
        std::clamp(wallpaper.value(u"opacity"_s).toInt(49), 0, 100);
    profile.wallpaperOverlayOpacity = 0;
    profile.wallpaperBlur = 0;
    profile.wallpaperZoom = 100;
    const auto position = wallpaper.value(u"position"_s).toString();
    profile.wallpaperFocalX = position == u"Left"_s    ? 0
                              : position == u"Right"_s ? 100
                                                       : 50;
    profile.wallpaperFocalY = position == u"Top"_s      ? 0
                              : position == u"Bottom"_s ? 100
                                                        : 50;

    profile.useThemeMessageRows = true;
    profile.alternateMessageRows = appearance.value(u"messages"_s)
                                       .toObject()
                                       .value(u"alternateMessageBackground"_s)
                                       .toBool(false);
    return profile.isValid() ? std::optional(profile) : std::nullopt;
}

std::optional<ThemeCustomizationProfile> buildThemePalette(
    const ThemeCustomizationProfile &input,
    const ThemePaletteSelection &selection, ThemePaletteMode mode)
{
    if (!selection.any())
    {
        return std::nullopt;
    }

    auto profile = input;
    const auto chosen = [](bool keep, const QColor &color) {
        return keep && color.isValid() ? std::optional(color) : std::nullopt;
    };
    const auto keptBackground = chosen(selection.background, input.background);
    const auto keptChatBackground =
        chosen(selection.chatBackground, input.chatBackground);
    const auto keptSurface = chosen(selection.surface, input.surface);
    const auto keptRaised =
        chosen(selection.raisedSurface, input.raisedSurface);
    const auto keptAccent = chosen(selection.accent, input.accent);
    if ((selection.background && !keptBackground) ||
        (selection.chatBackground && !keptChatBackground) ||
        (selection.surface && !keptSurface) ||
        (selection.raisedSurface && !keptRaised) ||
        (selection.accent && !keptAccent))
    {
        return std::nullopt;
    }

    const QColor seed = keptBackground               ? *keptBackground
                        : keptSurface                ? *keptSurface
                        : keptRaised                 ? *keptRaised
                        : keptChatBackground         ? *keptChatBackground
                        : keptAccent                 ? *keptAccent
                        : input.background.isValid() ? input.background
                                                     : QColor(u"#202020"_s);

    const bool lightInterface = mode == ThemePaletteMode::Light;

    profile.background = keptBackground.value_or(
        relatedTone(seed, lightInterface, lightInterface ? 0.92 : 0.08));
    profile.surface = keptSurface.value_or(
        relatedTone(seed, lightInterface, lightInterface ? 0.84 : 0.14));
    profile.raisedSurface = keptRaised.value_or(
        relatedTone(seed, lightInterface, lightInterface ? 0.75 : 0.21));

    std::vector<QColor> keptInterfaceSurfaces;
    if (keptBackground)
    {
        keptInterfaceSurfaces.push_back(*keptBackground);
    }
    if (keptSurface)
    {
        keptInterfaceSurfaces.push_back(*keptSurface);
    }
    if (keptRaised)
    {
        keptInterfaceSurfaces.push_back(*keptRaised);
    }

    profile.text =
        keptInterfaceSurfaces.empty()
            ? (lightInterface ? QColor(Qt::black) : QColor(Qt::white))
            : preferredTextColor(keptInterfaceSurfaces);

    const auto adjustInterfaceSurface = [&](QColor &color, bool keep) {
        if (!keep)
        {
            color = movePaletteBackgroundToContrast(color, profile.text, 4.5);
        }
    };
    adjustInterfaceSurface(profile.background, selection.background);
    adjustInterfaceSurface(profile.surface, selection.surface);
    adjustInterfaceSurface(profile.raisedSurface, selection.raisedSurface);

    const std::array interfaceSurfaces{profile.background, profile.surface,
                                       profile.raisedSurface};
    profile.mutedText =
        moveTextToContrast(mix(profile.text, profile.surface, 0.42),
                           profile.text, interfaceSurfaces, 3.0);

    bool lightChat;
    if (keptChatBackground)
    {
        const std::array backgrounds{*keptChatBackground};
        lightChat = preferredTextColor(backgrounds) == QColor(Qt::black);
    }
    else
    {
        lightChat = lightInterface;
    }
    profile.chatBackground = keptChatBackground.value_or(
        relatedTone(seed, lightChat, lightChat ? 0.96 : 0.07));

    if (keptChatBackground)
    {
        const std::array backgrounds{profile.chatBackground};
        profile.chatText = preferredTextColor(backgrounds);
    }
    else
    {
        profile.chatText = lightChat ? QColor(Qt::black) : QColor(Qt::white);
        profile.chatBackground = movePaletteBackgroundToContrast(
            profile.chatBackground, profile.chatText, 4.5);
    }

    profile.separateChatText = profile.chatText != profile.text;

    if (keptAccent)
    {
        profile.accent = *keptAccent;
    }
    else
    {
        qreal hue = seed.hsvHueF();
        if (hue < 0)
        {
            hue = input.accent.hsvHueF();
        }
        if (hue < 0)
        {
            hue = 0.58;
        }
        const qreal saturation =
            std::clamp(std::max(seed.hsvSaturationF(), 0.52F), 0.0F, 0.92F);
        const bool darkInterfaceText = profile.text.lightnessF() < 0.5;
        profile.accent = opaque(
            QColor::fromHsvF(hue, saturation, darkInterfaceText ? 0.48 : 0.94));
        profile.accent = moveTextToContrast(profile.accent, profile.text,
                                            interfaceSurfaces, 3.0);
    }
    profile.systemText =
        moveTextToContrast(mix(profile.chatText, profile.accent, 0.20),
                           profile.chatText, {profile.chatBackground}, 3.0);
    profile.timestampText =
        moveTextToContrast(mix(profile.chatText, profile.chatBackground, 0.46),
                           profile.chatText, {profile.chatBackground}, 2.5);
    return profile;
}

std::optional<ThemeCustomizationProfile> builtInCustomizationProfile(
    QStringView id)
{
    ThemeCustomizationProfile profile;
    if (id == u"moltorino-midnight")
    {
        profile.useThemeFonts = false;
        profile.useThemeFontSizes = false;
        profile.name = u"Moltorino"_s;
        profile.background = QColor(u"#0f0e0d"_s);
        profile.chatBackground = QColor(u"#131210"_s);
        profile.surface = QColor(u"#191817"_s);
        profile.raisedSurface = QColor(u"#22211f"_s);
        profile.text = QColor(u"#e8e7e4"_s);
        profile.mutedText = QColor(u"#9c9b97"_s);
        profile.accent = QColor(u"#f3922b"_s);
        profile.foundation = ThemeFoundation::MoltorinoPolished;
        profile.surfaceDepth = ThemeSurfaceDepth::Flat;
        profile.panelContrast = 25;
        profile.accentStrength = ThemeAccentStrength::Balanced;
        profile.tabShape = ThemeTabShape::Connected;
        profile.tabCornerRadius = 5;
        profile.tabSpacing = 0;
        profile.inactiveTabContrast = 66;
        profile.channelBarContrast = 38;
        profile.roundChat = true;
        profile.chatCornerRadius = 8;
        profile.chatBorderColor = QColor(u"#8b8984"_s);
        profile.chatBorderOpacity = 18;
        profile.alternateMessageRows = true;
        profile.alternateMessageOpacity = 14;
        profile.alternateMessageContrast = 28;
        profile.chatFontFamily = QStringLiteral(DEFAULT_FONT_FAMILY);
        profile.chatFontSize = 14;
        profile.chatFontWeight = QFont::Normal;
        profile.usernameFontFamily = QStringLiteral(DEFAULT_FONT_FAMILY);
        profile.usernameFontSize = 14;
        profile.usernameFontWeight = QFont::DemiBold;
        profile.interfaceFontFamily = QStringLiteral(DEFAULT_FONT_FAMILY);
        profile.interfaceFontSize = 10;
    }
    else if (id == u"sakura-dream")
    {
        profile.name = u"Sakura"_s;
        profile.baseTheme = u"Light"_s;
        profile.background = QColor(u"#f39aba"_s);
        profile.chatBackground = QColor(u"#ffd9e7"_s);
        profile.surface = QColor(u"#ef84ab"_s);
        profile.raisedSurface = QColor(u"#ffb6cf"_s);
        profile.text = QColor(u"#3f1f2d"_s);
        profile.mutedText = QColor(u"#775262"_s);
        profile.accent = QColor(u"#bd2f67"_s);
        profile.foundation = ThemeFoundation::MoltorinoPolished;
        profile.surfaceDepth = ThemeSurfaceDepth::Flat;
        profile.panelContrast = 20;
        profile.accentStrength = ThemeAccentStrength::Balanced;
        profile.tabShape = ThemeTabShape::Individual;
        profile.tabCornerRadius = 3;
        profile.tabSpacing = 2;
        profile.inactiveTabContrast = 69;
        profile.channelBarContrast = 49;
        profile.wallpaperSource = u":/themes/wallpapers/sakura-bloom-v2.jpg"_s;
        profile.wallpaperOverlayColor = QColor(u"#f58db5"_s);
        profile.wallpaperOverlayOpacity = 46;
        profile.wallpaperFocalX = 82;
        profile.wallpaperFocalY = 76;
        profile.alternateMessageRows = true;
        profile.alternateMessageOpacity = 13;
        profile.alternateMessageContrast = 24;
        profile.messageShadow = true;
        profile.messageShadowOpacity = 37;
        profile.messageShadowOffsetX = 0;
        profile.messageShadowOffsetY = 0;
        profile.messageShadowBlur = 1;
        profile.cornerStyle = ThemeCornerStyle::Rounded;
        profile.roundChat = true;
        profile.chatCornerRadius = 13;
        profile.chatBorderColor = QColor(u"#a92157"_s);
        profile.chatBorderOpacity = 30;
        profile.chatFontFamily = u"Segoe UI"_s;
        profile.chatFontSize = 13;

        profile.chatFontWeight = 600;
        profile.usernameFontFamily = u"Segoe UI"_s;
        profile.usernameFontSize = 13;
        profile.usernameFontWeight = 700;
        profile.interfaceFontFamily = u"Segoe UI"_s;
        profile.interfaceFontSize = 10;
    }
    else if (id == u"aurora-calm")
    {
        profile.name = u"Garden"_s;
        profile.background = QColor(u"#08100d"_s);
        profile.chatBackground = QColor(u"#0b1511"_s);
        profile.surface = QColor(u"#15251d"_s);
        profile.raisedSurface = QColor(u"#20362a"_s);
        profile.text = QColor(u"#eff8f1"_s);
        profile.mutedText = QColor(u"#9db5a5"_s);
        profile.accent = QColor(u"#8addaa"_s);
        profile.foundation = ThemeFoundation::MoltorinoPolished;
        profile.surfaceDepth = ThemeSurfaceDepth::Layered;
        profile.panelContrast = 76;
        profile.accentStrength = ThemeAccentStrength::Subtle;
        profile.tabShape = ThemeTabShape::Connected;
        profile.tabCornerRadius = 6;
        profile.tabSpacing = 0;
        profile.inactiveTabContrast = 64;
        profile.channelBarContrast = 42;
        profile.wallpaperSource = u":/themes/wallpapers/midnight-garden.jpg"_s;
        profile.wallpaperOverlayColor = QColor(u"#082215"_s);
        profile.wallpaperOverlayOpacity = 52;
        profile.alternateMessageRows = true;
        profile.alternateMessageOpacity = 14;
        profile.alternateMessageContrast = 38;
        profile.wallpaperFocalX = 79;
        profile.wallpaperFocalY = 13;
        profile.messageShadow = true;
        profile.messageShadowOpacity = 52;
        profile.messageShadowBlur = 3;
        profile.cornerStyle = ThemeCornerStyle::Soft;
        profile.roundChat = true;
        profile.chatCornerRadius = 9;
        profile.chatBorderColor = QColor(u"#8bb49a"_s);
        profile.chatBorderOpacity = 22;
        profile.chatFontFamily = u"Inter"_s;
        profile.chatFontSize = 13;
        profile.chatFontWeight = 500;
        profile.usernameFontFamily = u"Inter"_s;
        profile.usernameFontSize = 13;
        profile.usernameFontWeight = 700;
        profile.interfaceFontFamily = u"Segoe UI"_s;
        profile.interfaceFontSize = 10;
    }
    else if (id == u"ember-arcade")
    {
        profile.name = u"Arcade"_s;
        profile.background = QColor(u"#070814"_s);
        profile.chatBackground = QColor(u"#090b18"_s);
        profile.surface = QColor(u"#171833"_s);
        profile.raisedSurface = QColor(u"#242552"_s);
        profile.text = QColor(u"#faf8ff"_s);
        profile.mutedText = QColor(u"#aaa8c8"_s);
        profile.accent = QColor(u"#22d7ff"_s);
        profile.foundation = ThemeFoundation::MoltorinoPolished;
        profile.surfaceDepth = ThemeSurfaceDepth::Flat;
        profile.panelContrast = 22;
        profile.accentStrength = ThemeAccentStrength::Bold;
        profile.tabShape = ThemeTabShape::Individual;
        profile.tabCornerRadius = 0;
        profile.tabSpacing = 1;
        profile.inactiveTabContrast = 58;
        profile.channelBarContrast = 57;
        profile.wallpaperSource.clear();
        profile.alternateMessageRows = true;
        profile.alternateMessageOpacity = 13;
        profile.alternateMessageContrast = 46;
        profile.messageShadow = true;
        profile.messageShadowOpacity = 68;
        profile.messageShadowBlur = 3;
        profile.cornerStyle = ThemeCornerStyle::Rounded;
        profile.roundChat = true;
        profile.chatCornerRadius = 12;
        profile.chatBorderColor = QColor(u"#a971ff"_s);
        profile.chatBorderOpacity = 30;
        profile.chatFontFamily = u"Consolas"_s;
        profile.chatFontSize = 13;
        profile.chatFontWeight = 600;
        profile.usernameFontFamily = u"Consolas"_s;
        profile.usernameFontSize = 13;
        profile.usernameFontWeight = 700;
        profile.interfaceFontFamily = u"Segoe UI"_s;
        profile.interfaceFontSize = 10;
    }
    else if (id == u"violet-dusk")
    {
        profile.name = u"Afterhours"_s;
        profile.background = QColor(u"#1a1110"_s);
        profile.chatBackground = QColor(u"#241714"_s);
        profile.surface = QColor(u"#39231e"_s);
        profile.raisedSurface = QColor(u"#53332a"_s);
        profile.text = QColor(u"#fff1df"_s);
        profile.mutedText = QColor(u"#c9a894"_s);
        profile.accent = QColor(u"#ff9a52"_s);
        profile.foundation = ThemeFoundation::MoltorinoPolished;
        profile.surfaceDepth = ThemeSurfaceDepth::Layered;
        profile.panelContrast = 72;
        profile.accentStrength = ThemeAccentStrength::Balanced;
        profile.tabShape = ThemeTabShape::Connected;
        profile.tabCornerRadius = 6;
        profile.tabSpacing = 0;
        profile.inactiveTabContrast = 62;
        profile.channelBarContrast = 48;
        profile.wallpaperSource = u":/themes/wallpapers/cozy-afterhours.jpg"_s;
        profile.wallpaperOverlayColor = QColor(u"#171514"_s);
        profile.wallpaperOverlayOpacity = 65;
        profile.wallpaperBlur = 12;
        profile.wallpaperFocalX = 74;
        profile.wallpaperFocalY = 56;
        profile.alternateMessageRows = true;
        profile.alternateMessageOpacity = 15;
        profile.alternateMessageContrast = 40;
        profile.messageShadow = true;
        profile.messageShadowOpacity = 58;
        profile.messageShadowBlur = 3;
        profile.cornerStyle = ThemeCornerStyle::Soft;
        profile.roundChat = true;
        profile.chatCornerRadius = 7;
        profile.chatBorderColor = QColor(u"#d18a63"_s);
        profile.chatBorderOpacity = 26;
        profile.chatFontFamily = u"Google Sans"_s;
        profile.chatFontSize = 13;
        profile.chatFontWeight = 900;
        profile.usernameFontFamily = u"Google Sans"_s;
        profile.usernameFontSize = 14;
        profile.usernameFontWeight = 800;
        profile.interfaceFontFamily = u"Segoe UI"_s;
        profile.interfaceFontSize = 10;
    }
    else
    {
        return std::nullopt;
    }
    if (!profile.systemText.isValid())
    {
        profile.systemText = profile.mutedText;
    }
    if (!profile.timestampText.isValid())
    {
        profile.timestampText = profile.systemText;
    }
    if (!profile.separateChatText)
    {
        profile.chatText = profile.text;
    }
    return profile;
}

}
