// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Fonts.hpp"

#include "Application.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"

#include <QDebug>
#include <QFontDatabase>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <climits>
#include <mutex>

namespace chatterino {

int getUsernameBoldness()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

    static constexpr std::array<std::array<int, 2>, 9> legacyToOpenTypeMap{{
        {0, QFont::Thin},
        {12, QFont::ExtraLight},
        {25, QFont::Light},
        {50, QFont::Normal},
        {57, QFont::Medium},
        {63, QFont::DemiBold},
        {75, QFont::Bold},
        {81, QFont::ExtraBold},
        {87, QFont::Black},
    }};

    const int target = getSettings()->boldScale.getValue();

    int result = QFont::Medium;
    int closestDist = INT_MAX;

    for (const auto [weightOld, weightNew] : legacyToOpenTypeMap)
    {
        const int dist = qAbs(weightOld - target);
        if (dist < closestDist)
        {
            result = weightNew;
            closestDist = dist;
        }
        else
        {

            break;
        }
    }

    return result;
#else
    return getSettings()->boldScale.getValue();
#endif
}

}

namespace {

using namespace chatterino;

float fontSize(FontStyle style)
{
    auto chatSize = [] {
        const auto themed = getTheme()->customization.chatFontSize;
        if (themed > 0)
        {
            return static_cast<float>(themed);
        }
        return static_cast<float>(getSettings()->chatFontSize);
    };
    switch (style)
    {
        case FontStyle::ChatSmall:
            return 0.6F * chatSize();
        case FontStyle::ChatMediumSmall:
            return 0.8F * chatSize();
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumBold:
        case FontStyle::ChatMediumItalic:
        case FontStyle::TimestampMedium:
            return chatSize();
        case FontStyle::ChatUsername:
            return getTheme()->customization.usernameFontSize > 0
                       ? static_cast<float>(
                             getTheme()->customization.usernameFontSize)
                       : chatSize();
        case FontStyle::ChatLarge:
            return 1.2F * chatSize();
        case FontStyle::ChatVeryLarge:
            return 1.4F * chatSize();

        case FontStyle::Tiny:
            return 8;
        case FontStyle::UiMedium:
        case FontStyle::UiMediumBold:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return getTheme()->customization.interfaceFontSize > 0
                       ? static_cast<float>(
                             getTheme()->customization.interfaceFontSize)
                       : 9;
    }

    assert(false);
    return 9;
}

int fontWeight(FontStyle style)
{
    switch (style)
    {
        case FontStyle::ChatSmall:
        case FontStyle::ChatMediumSmall:
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumItalic:
        case FontStyle::ChatLarge:
        case FontStyle::ChatVeryLarge:
        case FontStyle::TimestampMedium:
            return getTheme()->customization.chatFontWeight > 0
                       ? getTheme()->customization.chatFontWeight
                       : getSettings()->chatFontWeight.getValue();

        case FontStyle::ChatMediumBold:
            return getUsernameBoldness();

        case FontStyle::ChatUsername:
            return getTheme()->customization.usernameFontWeight > 0
                       ? getTheme()->customization.usernameFontWeight
                       : getUsernameBoldness();

        case FontStyle::Tiny:
        case FontStyle::UiMedium:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return QFont::Normal;

        case FontStyle::UiMediumBold:
            return QFont::Bold;
    }

    assert(false);
    return QFont::Normal;
}

bool isItalic(FontStyle style)
{
    switch (style)
    {
        case FontStyle::Tiny:
        case FontStyle::ChatSmall:
        case FontStyle::ChatMediumSmall:
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumBold:
        case FontStyle::ChatUsername:
        case FontStyle::ChatLarge:
        case FontStyle::ChatVeryLarge:
        case FontStyle::TimestampMedium:
        case FontStyle::UiMedium:
        case FontStyle::UiMediumBold:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return false;

        case FontStyle::ChatMediumItalic:
            return true;
    }

    assert(false);
    return false;
}

QString fontFamily(FontStyle style)
{
    switch (style)
    {
        case FontStyle::Tiny:
            return QStringLiteral("Monospace");

        case FontStyle::ChatSmall:
        case FontStyle::ChatMediumSmall:
        case FontStyle::ChatMedium:
        case FontStyle::ChatMediumBold:
        case FontStyle::ChatMediumItalic:
        case FontStyle::ChatLarge:
        case FontStyle::ChatVeryLarge:
        case FontStyle::TimestampMedium:
            return getTheme()->customization.chatFontFamily.isEmpty()
                       ? getSettings()->chatFontFamily.getValue()
                       : getTheme()->customization.chatFontFamily;

        case FontStyle::ChatUsername:
            if (!getTheme()->customization.usernameFontFamily.isEmpty())
            {
                return getTheme()->customization.usernameFontFamily;
            }
            return fontFamily(FontStyle::ChatMedium);

        case FontStyle::UiMedium:
        case FontStyle::UiMediumBold:
        case FontStyle::UiTabs:
        case FontStyle::EndType:
            return getTheme()->customization.interfaceFontFamily.isEmpty()
                       ? QStringLiteral(DEFAULT_FONT_FAMILY)
                       : getTheme()->customization.interfaceFontFamily;
    }

    assert(false);
    return QStringLiteral(DEFAULT_FONT_FAMILY);
}

}

namespace chatterino {

bool registerBundledFonts()
{
    static std::once_flag once;
    static bool available = false;

    std::call_once(once, [] {
        const std::array paths{
            QStringLiteral(":/fonts/Gabarito/Gabarito-Regular.ttf"),
            QStringLiteral(":/fonts/Gabarito/Gabarito-Medium.ttf"),
            QStringLiteral(":/fonts/Gabarito/Gabarito-SemiBold.ttf"),
            QStringLiteral(":/fonts/Gabarito/Gabarito-Bold.ttf"),
            QStringLiteral(":/fonts/Gabarito/Gabarito-ExtraBold.ttf"),
            QStringLiteral(":/fonts/Gabarito/Gabarito-Black.ttf"),
        };
        QStringList registeredFamilies;
        size_t registeredFaceCount = 0;
        for (const auto &path : paths)
        {
            const auto id = QFontDatabase::addApplicationFont(path);
            if (id < 0)
            {
                qWarning() << "Could not load Moltorino's bundled font face:"
                           << path;
                continue;
            }
            ++registeredFaceCount;
            registeredFamilies.append(
                QFontDatabase::applicationFontFamilies(id));
        }

        const bool hasBaseFamily = std::ranges::any_of(
            registeredFamilies, [](const QString &registeredFamily) {
                return registeredFamily.compare(QStringLiteral("Gabarito"),
                                                Qt::CaseInsensitive) == 0;
            });
        available = registeredFaceCount == paths.size() && hasBaseFamily;
        if (!available)
        {
            qWarning() << "The bundled Gabarito faces did not all register:"
                       << registeredFamilies;
        }
    });

    return available;
}

QFont makeResolvedFont(const QString &family, qreal pointSize, int weight,
                       bool italic)
{
    const bool bundledGabaritoAvailable = registerBundledFonts();

    QString resolvedFamily = family;
    QString resolvedStyle;
    auto resolvedWeight =
        QFont::Weight(std::clamp(weight, int(QFont::Thin), int(QFont::Black)));

    const bool isBundledGabarito =
        bundledGabaritoAvailable &&
        (family.compare(QStringLiteral("Gabarito"), Qt::CaseInsensitive) == 0 ||
         family.startsWith(QStringLiteral("Gabarito "), Qt::CaseInsensitive));
    if (isBundledGabarito)
    {
        resolvedFamily = QStringLiteral("Gabarito");
        static const std::array styles{
            QStringLiteral("Regular"),   QStringLiteral("Medium"),
            QStringLiteral("SemiBold"),  QStringLiteral("Bold"),
            QStringLiteral("ExtraBold"), QStringLiteral("Black")};
        static const std::array families{QString(),
                                         QStringLiteral("Gabarito Medium"),
                                         QStringLiteral("Gabarito SemiBold"),
                                         QString(),
                                         QStringLiteral("Gabarito ExtraBold"),
                                         QStringLiteral("Gabarito Black")};
        static constexpr std::array weights{QFont::Normal,    QFont::Medium,
                                            QFont::DemiBold,  QFont::Bold,
                                            QFont::ExtraBold, QFont::Black};
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const int face = std::clamp((int(resolvedWeight) + 50) / 100 - 4, 0, 5);
#else
        const auto closest =
            std::ranges::min_element(weights, [resolvedWeight](auto a, auto b) {
                return qAbs(int(a) - int(resolvedWeight)) <
                       qAbs(int(b) - int(resolvedWeight));
            });
        const auto face = closest - weights.begin();
#endif
        const auto &legacyFamily = families[face];
        resolvedStyle = styles[face];
        resolvedWeight = weights[face];
        if (!legacyFamily.isEmpty())
        {
            const auto availableFamilies = QFontDatabase::families();
            if (std::ranges::any_of(availableFamilies,
                                    [&legacyFamily](const QString &candidate) {
                                        return candidate.compare(
                                                   legacyFamily,
                                                   Qt::CaseInsensitive) == 0;
                                    }))
            {
                resolvedFamily = legacyFamily;
                resolvedStyle.clear();
                resolvedWeight = QFont::Normal;
            }
        }
    }

    QFont font(resolvedFamily);
    font.setPointSizeF(pointSize);
    font.setWeight(resolvedWeight);
    if (!resolvedStyle.isEmpty())
    {
        font.setStyleName(resolvedStyle);
    }
    font.setItalic(italic);

    QWidget widget;
    widget.setFont(font);
    return widget.font();
}

QFont makeResolvedFont(const QFont &base, int weight)
{
    const bool pixelSized = base.pointSizeF() <= 0 && base.pixelSize() > 0;
    const auto size = pixelSized ? base.pixelSize() : base.pointSizeF();

    auto resolved = makeResolvedFont(base.family(), std::max<qreal>(1, size),
                                     weight, base.italic());
    if (pixelSized)
    {
        resolved.setPixelSize(base.pixelSize());
    }
    resolved.setStretch(base.stretch());
    resolved.setKerning(base.kerning());
    resolved.setStyleHint(base.styleHint(), base.styleStrategy());
    resolved.setHintingPreference(base.hintingPreference());
    resolved.setCapitalization(base.capitalization());
    resolved.setLetterSpacing(base.letterSpacingType(), base.letterSpacing());
    resolved.setWordSpacing(base.wordSpacing());
    resolved.setUnderline(base.underline());
    resolved.setOverline(base.overline());
    resolved.setStrikeOut(base.strikeOut());
    resolved.setFixedPitch(base.fixedPitch());
    return resolved;
}

const FontAlignmentMetrics &Fonts::getUsernameAlignmentMetrics(float scale)
{
    assertInGuiThread();

    const auto cached = this->usernameAlignmentsByScale_.find(scale);
    if (cached != this->usernameAlignmentsByScale_.end())
    {
        return cached->second;
    }

    const auto &metrics =
        this->getOrCreateFontData(FontStyle::ChatUsername, scale).metrics;
    const auto inserted = this->usernameAlignmentsByScale_.emplace(
        scale,
        FontAlignmentMetrics{
            .uppercaseCenterAboveBottom =
                metrics.descent() -
                metrics.tightBoundingRect(QStringLiteral("M")).center().y(),
            .lowercaseCenterAboveBottom =
                metrics.descent() -
                metrics.tightBoundingRect(QStringLiteral("m")).center().y(),
        });
    return inserted.first->second;
}

Fonts::Fonts(Settings &settings)
{
    registerBundledFonts();
    this->fontsByType_.resize(size_t(FontStyle::EndType));

    auto invalidateFonts = [this] {
        assertInGuiThread();

        for (auto &map : this->fontsByType_)
        {
            map.clear();
        }
        this->usernameAlignmentsByScale_.clear();
        this->fontChanged.invoke();
    };
    this->fontChangedListener.setCB(invalidateFonts);
    this->fontChangedListener.addSetting(settings.chatFontFamily);
    this->fontChangedListener.addSetting(settings.chatFontSize);
    this->fontChangedListener.addSetting(settings.chatFontWeight);
    this->fontChangedListener.addSetting(settings.boldScale);
    this->themeConnections_.managedConnect(getTheme()->updated, invalidateFonts);
}

QFont Fonts::getFont(FontStyle type, float scale)
{
    return this->getOrCreateFontData(type, scale).font;
}

QFontMetricsF Fonts::getFontMetrics(FontStyle type, float scale)
{
    return this->getOrCreateFontData(type, scale).metrics;
}

Fonts::FontData &Fonts::getOrCreateFontData(FontStyle type, float scale)
{
    assertInGuiThread();

    assert(type < FontStyle::EndType);

    auto &map = this->fontsByType_[size_t(type)];

    auto it = map.find(scale);
    if (it != map.end())
    {

        return it->second;
    }

    auto result = map.emplace(scale, Fonts::createFontData(type, scale));
    assert(result.second);

    return result.first->second;
}

Fonts::FontData Fonts::createFontData(FontStyle type, float scale)
{
    auto font = makeResolvedFont(fontFamily(type), fontSize(type) * scale,
                                 fontWeight(type), isItalic(type));

    switch (type)
    {
        case FontStyle::TimestampMedium: {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

            auto tag = QFont::Tag("tnum");
            font.setFeature(tag, 1);
#endif
        }
        break;

        default:
            break;
    }

    return font;
}

}
