// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightResult.hpp"

namespace chatterino {

QString highlightMatchStyleName(HighlightMatchStyle style)
{
    switch (style)
    {
        case HighlightMatchStyle::None:
            return QStringLiteral("None");
        case HighlightMatchStyle::Outline:
            return QStringLiteral("Outline");
        case HighlightMatchStyle::Fill:
            return QStringLiteral("Fill");
        case HighlightMatchStyle::OutlineAndFill:
            return QStringLiteral("Outline and fill");
        case HighlightMatchStyle::Underline:
            return QStringLiteral("Underline");
    }
    return QStringLiteral("Outline");
}

QString highlightMatchAppearanceName(HighlightMatchStyle style, bool hasPaint)
{
    if (style == HighlightMatchStyle::None)
    {
        return hasPaint ? QStringLiteral("7TV paint") : QStringLiteral("None");
    }

    auto name = highlightMatchStyleName(style);
    if (hasPaint)
    {
        name += QStringLiteral(", 7TV");
    }
    return name;
}

QString highlightMatchAppearanceTooltip(const QColor &color,
                                        HighlightMatchStyle style,
                                        const QString &paintID)
{
    auto tooltip =
        QStringLiteral("Exact marker: %1").arg(highlightMatchStyleName(style));
    if (style != HighlightMatchStyle::None)
    {
        tooltip += QStringLiteral("\nMarker color: %1")
                       .arg(color.name(QColor::HexArgb));
    }
    if (!paintID.isEmpty())
    {
        tooltip += QStringLiteral("\n7TV text paint: %1").arg(paintID);
    }
    return tooltip;
}

HighlightMatchStyle highlightMatchStyleFromName(const QString &name)
{
    if (name.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::None;
    }
    if (name.compare(QStringLiteral("Fill"), Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::Fill;
    }
    if (name.compare(QStringLiteral("Outline and fill"),
                     Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::OutlineAndFill;
    }
    if (name.compare(QStringLiteral("Underline"), Qt::CaseInsensitive) == 0)
    {
        return HighlightMatchStyle::Underline;
    }
    return HighlightMatchStyle::Outline;
}

QColor defaultHighlightMatchColor(QColor messageHighlightColor)
{
    if (!messageHighlightColor.isValid())
    {
        messageHighlightColor = QColor(127, 63, 73);
    }
    messageHighlightColor.setAlpha(150);
    return messageHighlightColor;
}

QColor defaultNewHighlightMatchColor()
{
    return QColor(255, 255, 255, 235);
}

QColor defaultAutoModMatchColor()
{
    return QColor(255, 177, 64, 128);
}

HighlightResult::HighlightResult(bool _alert, bool _playSound,
                                 std::optional<QUrl> _customSoundUrl,
                                 std::shared_ptr<QColor> _color,
                                 bool _showInMentions)
    : alert(_alert)
    , playSound(_playSound)
    , customSoundUrl(std::move(_customSoundUrl))
    , color(std::move(_color))
    , showInMentions(_showInMentions)
{
}

HighlightResult HighlightResult::emptyResult()
{
    return {
        false, false, std::nullopt, nullptr, false,
    };
}

bool HighlightResult::operator==(const HighlightResult &other) const
{
    if (this->alert != other.alert)
    {
        return false;
    }
    if (this->playSound != other.playSound)
    {
        return false;
    }
    if (this->customSoundUrl != other.customSoundUrl)
    {
        return false;
    }

    if (static_cast<bool>(this->color) != static_cast<bool>(other.color))
    {
        return false;
    }
    if (this->color && *this->color != *other.color)
    {
        return false;
    }

    if (this->showInMentions != other.showInMentions)
    {
        return false;
    }

    return true;
}

bool HighlightResult::operator!=(const HighlightResult &other) const
{
    return !(*this == other);
}

bool HighlightResult::empty() const
{
    return !this->alert && !this->playSound &&
           !this->customSoundUrl.has_value() && !this->color &&
           !this->showInMentions;
}

bool HighlightResult::full() const
{
    return this->alert && this->playSound && this->customSoundUrl.has_value() &&
           this->color && this->showInMentions;
}

std::ostream &operator<<(std::ostream &os, const HighlightResult &result)
{
    os << "Alert: " << (result.alert ? "Yes" : "No") << ", "
       << "Play sound: " << (result.playSound ? "Yes" : "No") << " ("
       << (result.customSoundUrl
               ? result.customSoundUrl->toString().toStdString()
               : "")
       << ")"
       << ", "
       << "Color: " << (result.color ? result.color->name().toStdString() : "")
       << ", "
       << "Show in mentions: " << (result.showInMentions ? "Yes" : "No")
       << ", Exact matches: " << result.matches.size();
    return os;
}

}
