#include "singletons/ThemeWallpaper.hpp"

#include "singletons/ThemeVideoDecoder.hpp"
#include "singletons/ThemeVideoImport.hpp"

#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>

#include <cstring>

namespace chatterino {
namespace {

const QList<QByteArray> WALLPAPER_IMAGE_FORMATS{
    "png", "jpeg", "jpg", "webp", "gif",  "bmp",  "tiff",
    "tif", "tga",  "ico", "cur",  "icns", "pbm",  "pgm",
    "ppm", "xbm",  "xpm", "wbmp", "avif", "heif", "heic"};

}

QString themeWallpaperFileFilter()
{
    QStringList images;
    for (const auto &format : QImageReader::supportedImageFormats())
    {
        if (WALLPAPER_IMAGE_FORMATS.contains(format.toLower()))
        {
            images.append(QStringLiteral("*.") + QString::fromLatin1(format));
        }
    }
    images.removeDuplicates();
    images.sort();
    QStringList videos;
    for (const auto &extension : themeVideoInputExtensions())
    {
        if (extension != QStringLiteral("gif"))
        {
            videos.append(QStringLiteral("*.") + extension);
        }
    }
    return QStringLiteral("Images and videos (%1);;Images (%2);;Videos (%3)")
        .arg((images + videos).join(u' '), images.join(u' '),
             videos.join(u' '));
}

bool isThemeWallpaperId(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9_-]{5,64}\\z"));
    return pattern.match(id).hasMatch();
}

ThemeWallpaperData prepareThemeWallpaper(const QByteArray &bytes,
                                         std::stop_token cancellation)
{
    if (bytes.isEmpty() || bytes.size() > 50 * 1024 * 1024)
    {
        return {{}, QStringLiteral("Choose a background smaller than 50 MB.")};
    }
    if (cancellation.stop_requested())
    {
        return {{}, QStringLiteral("Background preparation cancelled.")};
    }
    if (bytes.startsWith(QByteArray::fromHex("1a45dfa3")))
    {
        QTemporaryFile file;
        if (!file.open() || file.write(bytes) != bytes.size() || !file.flush())
        {
            return {{},
                    QStringLiteral("Moltorino couldn't read that background.")};
        }
        const auto error = validateThemeVideo(file.fileName(), cancellation);
        return error.isEmpty()
                   ? ThemeWallpaperData{bytes, {}, QStringLiteral("webm")}
                   : ThemeWallpaperData{{}, error};
    }
    QBuffer input;
    input.setData(bytes);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input);
    reader.setAutoTransform(true);
    const auto format = reader.format().toLower();
    const auto size = reader.size();
    if (!WALLPAPER_IMAGE_FORMATS.contains(format) || !size.isValid() ||
        qint64(size.width()) * size.height() > 32LL * 1024 * 1024)
    {
        return {{},
                QStringLiteral("Choose a supported image under 32 "
                               "megapixels.")};
    }
    if (format == "gif" && reader.imageCount() > 1)
    {
        QTemporaryFile file(QDir::tempPath() +
                            QStringLiteral("/moltorino-background-XXXXXX.gif"));
        if (!file.open() || file.write(bytes) != bytes.size() || !file.flush())
        {
            return {{},
                    QStringLiteral("Moltorino couldn't read that background.")};
        }
        return prepareThemeVideo(file.fileName(), cancellation);
    }
    const auto scaled = size.scaled(1536, 1536, Qt::KeepAspectRatio);
    if (size.width() > 1536 || size.height() > 1536)
    {
        reader.setScaledSize(scaled);
    }
    auto image = reader.read();
    if (image.isNull())
    {
        return {{}, QStringLiteral("Moltorino couldn't read that background.")};
    }
    if (image.width() > 1536 || image.height() > 1536)
    {
        image = image.scaled(1536, 1536, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    if (image.colorSpace().isValid())
    {
        image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    }

    QImage clean(image.size(), image.hasAlphaChannel() ? QImage::Format_RGBA8888
                                                       : QImage::Format_RGB888);
    image = image.convertToFormat(clean.format());
    if (clean.isNull() || image.isNull())
    {
        return {
            {},
            QStringLiteral("Not enough memory to prepare that background.")};
    }
    for (int y = 0; y < clean.height(); ++y)
    {
        memcpy(clean.scanLine(y), image.constScanLine(y),
               size_t(clean.width()) * (image.hasAlphaChannel() ? 4 : 3));
    }
    const auto encode = [&clean](int quality) {
        QByteArray bytes;
        QBuffer output(&bytes);
        output.open(QIODevice::ReadWrite);
        QImageWriter writer(&output, "webp");
        writer.setQuality(quality);
        if (!writer.write(clean) || !output.seek(0) ||
            QImageReader(&output, "webp").size() != clean.size())
        {
            return QByteArray();
        }
        return bytes;
    };

    auto lossless = encode(100);
    auto compact = encode(95);
    if (!lossless.isEmpty() &&
        (compact.isEmpty() || lossless.size() <= compact.size()))
    {
        compact = std::move(lossless);
    }
    if (compact.isEmpty())
    {
        return {{},
                QStringLiteral("Moltorino couldn't prepare that background.")};
    }
    return {std::move(compact), {}};
}

ThemeWallpaperData prepareThemeWallpaper(const QString &source,
                                         std::stop_token cancellation)
{
    if (cancellation.stop_requested())
    {
        return {{}, QStringLiteral("Background preparation cancelled.")};
    }
    if (isThemeVideoInput(source) &&
        QFileInfo(source).suffix().compare(QStringLiteral("gif"),
                                           Qt::CaseInsensitive) != 0)
    {
        if (isThemeVideo(source) && QFileInfo(source).fileName().startsWith(
                                        QStringLiteral("wallpaper-")))
        {
            QFile video(source);
            if (video.open(QIODevice::ReadOnly) &&
                video.size() <= THEME_BACKGROUND_BYTES)
            {
                const auto bytes = video.read(THEME_BACKGROUND_BYTES + 1);
                if (bytes.size() > THEME_BACKGROUND_BYTES || !video.atEnd() ||
                    video.error() != QFile::NoError)
                {
                    return {{},
                            QStringLiteral(
                                "Moltorino couldn't read that background.")};
                }
                const auto digest =
                    QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                        .toHex()
                        .left(16);
                if (QFileInfo(source).fileName() ==
                    QStringLiteral("wallpaper-%1.webm")
                        .arg(QString::fromLatin1(digest)))
                {
                    return prepareThemeWallpaper(bytes, cancellation);
                }
            }
        }
        return prepareThemeVideo(source, cancellation);
    }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 50 * 1024 * 1024)
    {
        return {{},
                QStringLiteral("Moltorino couldn't read that background, or it "
                               "exceeds 50 MB.")};
    }
    const auto bytes = file.read(THEME_BACKGROUND_BYTES + 1);
    if (bytes.size() > THEME_BACKGROUND_BYTES || !file.atEnd() ||
        file.error() != QFile::NoError)
    {
        return {{}, QStringLiteral("Moltorino couldn't read that background.")};
    }
    const auto digest =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    if (QFileInfo(source).fileName() ==
        QStringLiteral("wallpaper-%1.webp").arg(QString::fromLatin1(digest)))
    {
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "webp");
        const auto size = reader.size();
        if (size.isValid() && size.width() <= 1536 && size.height() <= 1536 &&
            reader.canRead())
        {
            return {bytes, {}};
        }
    }
    return prepareThemeWallpaper(bytes, cancellation);
}

QString storeThemeWallpaper(const ThemeWallpaperData &data,
                            const QString &themesDirectory, QString *error)
{
    if (!data.error.isEmpty() || data.bytes.isEmpty() ||
        data.bytes.size() > THEME_BACKGROUND_BYTES ||
        (data.extension != QStringLiteral("webp") &&
         data.extension != QStringLiteral("webm")))
    {
        *error =
            data.error.isEmpty()
                ? QStringLiteral("That background is invalid or exceeds 50 MB.")
                : data.error;
        return {};
    }
    QDir directory(
        QDir(themesDirectory).filePath(QStringLiteral("Wallpapers")));
    const auto digest =
        QCryptographicHash::hash(data.bytes, QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    const auto path = directory.filePath(
        QStringLiteral("wallpaper-%1.%2")
            .arg(QString::fromLatin1(digest), data.extension));
    if (!directory.mkpath(QStringLiteral(".")))
    {
        *error =
            QStringLiteral("Moltorino couldn't create its background folder.");
        return {};
    }
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) &&
        existing.size() == data.bytes.size() &&
        existing.read(THEME_BACKGROUND_BYTES + 1) == data.bytes)
    {
        return path;
    }
    existing.close();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(data.bytes) != data.bytes.size() || !file.commit())
    {
        *error = QStringLiteral("Moltorino couldn't save that background.");
        return {};
    }
    return path;
}

QString themeWallpaperUploadId(const QString &managedSource)
{
    QFile file(managedSource + QStringLiteral(".kappa"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64)
    {
        return {};
    }
    const auto id = QString::fromLatin1(file.read(65));
    return isThemeWallpaperId(id) ? id : QString();
}

bool saveThemeWallpaperUploadId(const QString &managedSource, const QString &id)
{
    if (!isThemeWallpaperId(id))
    {
        return false;
    }
    QSaveFile file(managedSource + QStringLiteral(".kappa"));
    const auto bytes = id.toLatin1();
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes) == bytes.size() && file.commit();
}

QString findThemeWallpaper(const QString &id, const QString &themesDirectory)
{
    if (!isThemeWallpaperId(id))
    {
        return {};
    }
    const QDir directory(
        QDir(themesDirectory).filePath(QStringLiteral("Wallpapers")));
    for (const auto &entry :
         directory.entryInfoList({QStringLiteral("wallpaper-*.webp.kappa"),
                                  QStringLiteral("wallpaper-*.webm.kappa")},
                                 QDir::Files | QDir::NoSymLinks))
    {
        const auto path = entry.absoluteFilePath().chopped(6);
        if (themeWallpaperUploadId(path) == id && QFile::exists(path))
        {
            if (isThemeVideo(path))
            {
                ThemeVideoDecoder decoder(path);
                if (decoder.open())
                {
                    return path;
                }
                continue;
            }
            const auto prepared = prepareThemeWallpaper(path);
            if (prepared.error.isEmpty())
            {
                return path;
            }
        }
    }
    return {};
}

}
