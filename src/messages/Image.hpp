// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/DebugCount.hpp"

#include <QList>
#include <QPixmap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace chatterino {

class Image;
class ImageExpirationPool;

}

namespace chatterino::detail {

class StreamingGif;
class PickerImageReservation;

struct Frame {
    QPixmap image;
    int duration;
};

class Frames
{
public:
    Frames();
    Frames(QList<Frame> &&frames);
    ~Frames();

    Frames(const Frames &) = delete;
    Frames &operator=(const Frames &) = delete;

    Frames(Frames &&) = delete;
    Frames &operator=(Frames &&) = delete;

    void clear();
    bool empty() const;
    bool animated() const;
    std::optional<QPixmap> current();
    std::optional<QPixmap> first() const;

private:
    friend class chatterino::Image;
    friend class chatterino::ImageExpirationPool;

    int64_t memoryUsage() const;
    void synchronizeAnimation();
    void synchronizeAnimation(unsigned long currentPosition);
    void processOffset();
    QList<Frame> items_;
    QList<Frame>::size_type index_{0};
    uint64_t durationOffset_{0};
    unsigned long totalDuration_{0};
    unsigned long lastTimerPosition_{0};
};

QList<Frame> readFrames(QImageReader &reader, const Url &url,
                        bool firstFrameOnly = false, int maxFrameDimension = 0,
                        int minimumFrameDuration = 0);
void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed,
                  std::uint64_t loadGeneration,
                  std::shared_ptr<PickerImageReservation> reservation = {});

}

namespace chatterino {

class Image;
using ImagePtr = std::shared_ptr<Image>;

class Image : public std::enable_shared_from_this<Image>
{
public:

    static constexpr int maxBytesRam = 20 * 1024 * 1024;

    ~Image();

    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    Image(Image &&) = delete;
    Image &operator=(Image &&) = delete;

    static ImagePtr fromUrl(const Url &url, qreal scale = 1,
                            QSize expectedSize = {});

    static ImagePtr fromUrlWithMaxFrameDimension(const Url &url,
                                                 int maxFrameDimension,
                                                 qreal scale = 1,
                                                 QSize expectedSize = {});

    static ImagePtr fromStreamingGifUrl(const Url &url, int maxFrameDimension,
                                        qreal scale = 1,
                                        QSize expectedSize = {});

    static bool takeStreamingGifRepaint(unsigned long position);
    static ImagePtr fromResourcePixmap(const QPixmap &pixmap, qreal scale = 1);
    static ImagePtr getEmpty();

    static ImagePtr fromAutoscaledUrl(const Url &url, uint16_t autoScale);

    static void releaseUnusedCacheEntries();

    ImagePtr getFirstFramePreview() const;

    ImagePtr getPickerAnimation() const;

    ImagePtr getEmotePickerImage(bool still = false, bool smooth = false) const;

    void setPickerUrl(Url url);

    void releasePickerFrames();

    const Url &url() const;
    bool loaded() const;

    bool isLoading() const;
    std::optional<QPixmap> pixmapOrLoad() const;

    std::optional<QPixmap> firstPixmapOrLoad() const;
    void load() const;

    void retryLoad() const;
    qreal scale() const;
    bool isEmpty() const;
    const QString &failureReason() const;
    int width() const;
    int height() const;
    QSizeF size() const;
    bool animated() const;
    void setFrameCacheLifetime(std::chrono::milliseconds lifetime);

    bool usesOwnAnimationTimer() const;
    bool operator==(const Image &image) = delete;
    bool operator!=(const Image &image) = delete;

private:
    static ImagePtr fromUrlCached(const Url &url, qreal scale,
                                  QSize expectedSize,
                                  std::optional<uint16_t> autoScale,
                                  bool firstFrameOnly = false,
                                  int maxFrameDimension = 0,
                                  bool streamingGif = false);

    Image();
    Image(const Url &url, qreal scale, QSize expectedSize,
          bool firstFrameOnly = false, bool pickerCopy = false,
          int maxFrameDimension = 0, bool streamingGif = false);
    Image(qreal scale);

    void setPixmap(const QPixmap &pixmap);
    void actuallyLoad(bool gifFallback = false);
    void expireFrames();
    void initializeStreamingGif(QByteArray data, std::uint64_t loadGeneration,
                                bool gifFallback = false);
    QString streamingGifFallbackUrl() const;
    void clearStreamingGif();
    void updateStreamingGifAccounting();
    void advanceStreamingGif(unsigned long position);
    void failStreamingGif(QString reason, std::uint64_t loadGeneration);
    bool shouldPlayStreamingGif() const;
    bool hasResidentData() const;
    int64_t residentMemoryUsage() const;

    const Url url_{};

    Url pickerUrl_{};
    qreal scale_{1};

    const qreal cacheScale_{1};
    const QSize cacheExpectedSize_{16, 16};

    QSize expectedSize_{16, 16};
    std::atomic_bool empty_{false};

    bool shouldLoad_{false};
    bool animated_{false};

    std::optional<uint16_t> autoScale_;
    const bool firstFrameOnly_{false};
    const bool pickerCopy_{false};
    const int maxFrameDimension_{0};
    const bool streamingGif_{false};
    bool emotePicker_ = false;
    bool smoothPicker_ = false;
    std::shared_ptr<detail::PickerImageReservation> pickerReservation_;
    std::stop_source pickerRequest_{std::nostopstate};

    bool useGifFallback_{false};
    std::atomic<int64_t> frameCacheLifetimeMs_{0};
    std::atomic<std::uint64_t> loadGeneration_{0};
    std::atomic<int> retryCount_{0};
    mutable std::chrono::time_point<std::chrono::steady_clock> lastUsed_;

    QString failureReason_;
    std::unique_ptr<detail::Frames> frames_;
    std::unique_ptr<detail::StreamingGif> streamingGifPlayer_;
    int64_t streamingGifResidentBytesEstimate_ = 0;

    friend class ImageExpirationPool;
    friend void detail::assignFrames(
        std::weak_ptr<Image>, QList<detail::Frame>, std::uint64_t,
        std::shared_ptr<detail::PickerImageReservation>);
};

ImagePtr getEmptyImagePtr();

#ifndef DISABLE_IMAGE_EXPIRATION_POOL

class ImageExpirationPool
{
public:
    struct ProviderUsage {
        QString provider;
        int64_t bytes = 0;
        size_t images = 0;
        size_t animatedImages = 0;
    };

    ImageExpirationPool();
    static ImageExpirationPool &instance();

    void addImagePtr(ImagePtr imgPtr);
    void removeImagePtr(Image *rawPtr);

    void freeOld();

    void requestBudgetCheck(bool force = false);

    void trackStreamingGif(const ImagePtr &image);
    void untrackStreamingGif(Image *image);
    void freeInactiveStreamingGifs();

    std::vector<ProviderUsage> getProviderUsageSnapshot();

    void freeAll();

    QTimer *freeTimer_;
    std::map<Image *, std::weak_ptr<Image>> allImages_;
    std::mutex mutex_;

private:
    void runBudgetCheck();

    bool enforceBudget(int64_t maxBytes, int64_t targetBytes);

    bool budgetCheckQueued_ = false;
    QTimer *streamingGifTimer_ = nullptr;
    std::map<Image *, std::weak_ptr<Image>> streamingGifs_;
};

#endif
}
