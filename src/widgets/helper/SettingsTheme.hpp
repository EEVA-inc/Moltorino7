#pragma once

#include <QColor>
#include <QString>

namespace chatterino {

struct SettingsTheme {
    QColor background;
    QColor surface;
    QColor raisedSurface;
    QColor text;
    QColor mutedText;
    QColor accent;
    QColor selectionSurface;
    QString interfaceFontFamily;
    int interfaceFontSize;
};

inline const SettingsTheme &settingsTheme()
{
    static const SettingsTheme value{
        .background = QColor(QStringLiteral("#0f0e0d")),
        .surface = QColor(QStringLiteral("#191817")),
        .raisedSurface = QColor(QStringLiteral("#22211f")),
        .text = QColor(QStringLiteral("#e8e7e4")),
        .mutedText = QColor(QStringLiteral("#9c9b97")),
        .accent = QColor(QStringLiteral("#f3922b")),
        .selectionSurface = QColor(QStringLiteral("#2f261c")),
        .interfaceFontFamily = QStringLiteral("Gabarito"),
        .interfaceFontSize = 10,
    };
    return value;
}

}
