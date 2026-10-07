#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPoint>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cstdint>
#include <optional>

class QPainterPath;
class QPainter;

namespace chatterino {

enum class ThemeFoundation : uint8_t {
    ChatterinoClassic,
    MoltorinoPolished,
};

enum class ThemeSurfaceDepth : uint8_t {
    Flat,
    Balanced,
    Layered,
};

enum class ThemeAccentStrength : uint8_t {
    Subtle,
    Balanced,
    Bold,
};

enum class ThemeCornerStyle : uint8_t {
    Classic,
    Soft,
    Rounded,
};

enum class ThemeTabShape : uint8_t {
    Connected,
    Individual,
};

enum class ThemeWallpaperMode : uint8_t {
    Fill,
    Fit,
    Center,
    Tile,
    Stretch,
};

constexpr bool themeUsesClassicSplitFrame(ThemeFoundation foundation)
{
    return foundation == ThemeFoundation::ChatterinoClassic;
}

qreal themeTabCornerShoulder(qreal radius, qreal height);

QPainterPath themeTopTabPath(const QRectF &rect, qreal radius, bool roundLeft,
                             bool roundRight);

struct ThemeCustomizationProfile {
    static constexpr int CURRENT_VERSION = 14;

    QString name;
    QString baseTheme = QStringLiteral("Dark");

    QColor background = QColor(QStringLiteral("#111111"));
    QColor chatBackground = QColor(QStringLiteral("#191919"));
    QColor surface = QColor(QStringLiteral("#222222"));
    QColor raisedSurface = QColor(QStringLiteral("#303030"));
    QColor text = QColor(QStringLiteral("#eeeeee"));
    QColor chatText = QColor(QStringLiteral("#eeeeee"));
    bool separateChatText = false;
    QColor mutedText = QColor(QStringLiteral("#999999"));

    QColor systemText;
    QColor timestampText;
    QColor accent = QColor(QStringLiteral("#4fc3f7"));

    ThemeFoundation foundation = ThemeFoundation::MoltorinoPolished;
    ThemeSurfaceDepth surfaceDepth = ThemeSurfaceDepth::Balanced;

    int panelContrast = 50;
    ThemeAccentStrength accentStrength = ThemeAccentStrength::Balanced;
    ThemeCornerStyle cornerStyle = ThemeCornerStyle::Soft;
    ThemeTabShape tabShape = ThemeTabShape::Connected;
    int tabCornerRadius = 3;
    int tabSpacing = 1;
    int inactiveTabContrast = 72;
    int channelBarContrast = 52;
    bool roundChat = false;
    int chatCornerRadius = 6;
    bool chatBorder = true;
    QColor chatBorderColor = QColor(QStringLiteral("#999999"));
    int chatBorderWidth = 1;
    int chatBorderOpacity = 22;

    bool useThemeMessageRows = true;
    bool alternateMessageRows = false;
    int alternateMessageOpacity = 24;
    int alternateMessageContrast = 42;

    int highlightOpacityAdjustment = 0;

    bool messageShadow = false;
    bool messageShadowEmotes = true;
    QColor messageShadowColor = QColor(Qt::black);
    int messageShadowOpacity = 58;
    int messageShadowOffsetX = 1;
    int messageShadowOffsetY = 1;
    int messageShadowBlur = 2;

    QString wallpaperSource;
    QString wallpaperId;
    ThemeWallpaperMode wallpaperMode = ThemeWallpaperMode::Fill;
    int wallpaperOpacity = 100;
    QColor wallpaperOverlayColor = QColor(Qt::black);
    int wallpaperOverlayOpacity = 42;
    int wallpaperBlur = 0;
    int wallpaperZoom = 100;
    int wallpaperFocalX = 50;
    int wallpaperFocalY = 50;

    bool useThemeFonts = true;
    bool useThemeFontSizes = true;
    QString chatFontFamily;
    int chatFontSize = 0;
    int chatFontWeight = 0;
    QString usernameFontFamily;
    int usernameFontSize = 0;
    int usernameFontWeight = 0;
    QString interfaceFontFamily;
    int interfaceFontSize = 0;

    bool operator==(const ThemeCustomizationProfile &) const = default;

    bool isValid() const;
    QColor effectiveChatText() const;
    bool hasWallpaper() const;
    int cornerRadius() const;
    QPoint messageShadowOffset() const;

    void clearDisabledFontOverrides();
};

struct ThemePaletteSelection {
    bool background = false;
    bool chatBackground = false;
    bool surface = false;
    bool raisedSurface = false;
    bool accent = false;

    bool any() const;
};

enum class ThemePaletteMode : uint8_t {
    Dark,
    Light,
};

std::optional<ThemeCustomizationProfile> customizationProfileFromTheme(
    const QJsonObject &theme);

std::optional<ThemeCustomizationProfile> customizationProfileFromBluzyrinoTheme(
    const QJsonObject &theme, const QString &name);

std::optional<ThemeCustomizationProfile>
    customizationProfileFromBluzyrinoSettings(const QJsonObject &settings);

QJsonObject buildCustomizedTheme(const ThemeCustomizationProfile &profile);

QJsonObject makeShareableCustomizedTheme(const QJsonObject &theme,
                                         bool *wallpaperRemoved = nullptr,
                                         bool includeWallpaper = true);

double colorContrastRatio(const QColor &foreground, const QColor &background);

std::optional<ThemeCustomizationProfile> buildThemePalette(
    const ThemeCustomizationProfile &profile,
    const ThemePaletteSelection &selection, ThemePaletteMode mode);

std::optional<ThemeCustomizationProfile> builtInCustomizationProfile(
    QStringView id);

QImage loadThemeWallpaper(const QString &source, int maxDimension = 1536);

QImage blurThemeWallpaper(QImage image, int radius);

QImage blurThemeShadow(QImage image, int radius);

void buildThemeChatFramePaths(const QRectF &rect, qreal radius, qreal width,
                              QPainterPath &outside, QPainterPath &innerBorder);

void paintThemeChatFrame(QPainter &painter, const QRectF &rect, qreal radius,
                         qreal width, const QColor &topSurface,
                         const QColor &bottomSurface, const QColor &borderColor,
                         int borderOpacity,
                         const QPainterPath *cachedOutside = nullptr,
                         const QPainterPath *cachedInnerBorder = nullptr);

QColor blendThemeHighlight(const QColor &base, QColor highlight,
                           int opacityAdjustment);

struct ThemeWallpaperLayout {
    QRectF destination;
    QRectF source;
};

ThemeWallpaperLayout layoutThemeWallpaper(const QSizeF &imageSize,
                                          const QRectF &target,
                                          ThemeWallpaperMode mode, int focalX,
                                          int focalY, int zoom = 100);

}
