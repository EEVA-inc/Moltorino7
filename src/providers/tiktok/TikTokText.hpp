#pragma once

#include <QString>
#include <QVector>

#include <optional>

namespace chatterino::tiktok::livetext {

struct TextAnalysis {
    qsizetype count = 0;
    QVector<qsizetype> utf16Boundaries;
};

TextAnalysis analyzeEditorText(const QString &text,
                               bool countAnsiEscapeCodes = false);

std::optional<qsizetype> incomingIndexToUtf16(const QString &content,
                                              qsizetype index,
                                              qsizetype precedingEmotes);

}
