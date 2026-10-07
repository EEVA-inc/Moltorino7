// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <vector>
#include <optional>
#include <ostream>

namespace chatterino {

enum class HighlightMatchSource : std::uint8_t {
    Phrase,
    WordList,
    AutoMod,
};

enum class HighlightMatchStyle : std::uint8_t {
    None,
    Outline,
    Fill,
    OutlineAndFill,
    Underline,
};

QString highlightMatchStyleName(HighlightMatchStyle style);
QString highlightMatchAppearanceName(HighlightMatchStyle style, bool hasPaint);
QString highlightMatchAppearanceTooltip(const QColor &color,
                                        HighlightMatchStyle style,
                                        const QString &paintID);
HighlightMatchStyle highlightMatchStyleFromName(const QString &name);
QColor defaultHighlightMatchColor(QColor messageHighlightColor);
QColor defaultNewHighlightMatchColor();
QColor defaultAutoModMatchColor();

struct HighlightMatch {
    qsizetype start{};
    qsizetype length{};
    QColor color;
    QString ruleName;
    QString pattern;
    HighlightMatchSource source{HighlightMatchSource::Phrase};
    HighlightMatchStyle style{HighlightMatchStyle::Outline};
    QString paintID;

    bool operator==(const HighlightMatch &other) const = default;
};

struct HighlightResult {
    HighlightResult(bool _alert, bool _playSound,
                    std::optional<QUrl> _customSoundUrl,
                    std::shared_ptr<QColor> _color, bool _showInMentions);

    static HighlightResult emptyResult();

    bool alert{false};

    bool playSound{false};

    std::optional<QUrl> customSoundUrl{};

    std::shared_ptr<QColor> color{};

    bool showInMentions{false};

    std::vector<HighlightMatch> matches;

    bool operator==(const HighlightResult &other) const;
    bool operator!=(const HighlightResult &other) const;

    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool full() const;

    friend std::ostream &operator<<(std::ostream &os,
                                    const HighlightResult &result);
};

}
