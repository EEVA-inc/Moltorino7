#pragma once

#include "singletons/ThemeWallpaper.hpp"

#include <QSize>
#include <QStringList>

namespace chatterino {

struct ThemeVideoInfo {
    QString error;
    double duration = 0;
    QSize outputSize;
    double framesPerSecond = 24;
};

bool isThemeVideoInput(const QString &source);
QStringList themeVideoInputExtensions();
ThemeVideoInfo inspectThemeVideo(const QString &source,
                                 std::stop_token cancellation = {});

ThemeWallpaperData prepareThemeVideo(const QString &source,
                                     std::stop_token cancellation = {},
                                     int seconds = 0);

}
