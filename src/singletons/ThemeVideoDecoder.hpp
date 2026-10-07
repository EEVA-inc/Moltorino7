#pragma once

#include <QImage>
#include <QString>

#include <memory>
#include <stop_token>

namespace chatterino {

inline constexpr int THEME_VIDEO_DIMENSION = 1024;
inline constexpr int THEME_VIDEO_PIXELS = 1024 * 576;
inline constexpr int THEME_VIDEO_DURATION_MS = 120000;
inline constexpr int THEME_BACKGROUND_BYTES = 50 * 1024 * 1024;

bool isThemeVideo(const QString &source);

class ThemeVideoDecoder
{
public:
    explicit ThemeVideoDecoder(const QString &source);
    ~ThemeVideoDecoder();
    ThemeVideoDecoder(const ThemeVideoDecoder &) = delete;
    ThemeVideoDecoder &operator=(const ThemeVideoDecoder &) = delete;

    bool open();
    bool decodeNext();
    bool copyFrame(QImage &image) const;
    int frameDuration() const;
    int frameCount() const;
    QString error() const;

private:
    struct Private;
    std::unique_ptr<Private> data_;
};

QImage themeVideoPoster(const QString &source, int maxDimension);
QString validateThemeVideo(const QString &source,
                           std::stop_token cancellation = {});

}
