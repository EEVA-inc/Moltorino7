#pragma once

#include <QByteArray>
#include <QString>

#include <stop_token>

namespace chatterino {

bool isThemeWallpaperId(const QString &id);
QString themeWallpaperFileFilter();

struct ThemeWallpaperData {
    QByteArray bytes;
    QString error;
    QString extension = QStringLiteral("webp");
};

ThemeWallpaperData prepareThemeWallpaper(const QByteArray &bytes,
                                         std::stop_token cancellation = {});
ThemeWallpaperData prepareThemeWallpaper(const QString &source,
                                         std::stop_token cancellation = {});
QString storeThemeWallpaper(const ThemeWallpaperData &data,
                            const QString &themesDirectory, QString *error);
QString themeWallpaperUploadId(const QString &managedSource);
QString findThemeWallpaper(const QString &id, const QString &themesDirectory);
bool saveThemeWallpaperUploadId(const QString &managedSource,
                                const QString &id);

}
