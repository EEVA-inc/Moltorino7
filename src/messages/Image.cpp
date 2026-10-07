// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/Image.hpp"

#include "messages/StreamingGif.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "debug/Benchmark.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/PostToThread.hpp"

#include <boost/functional/hash.hpp>
#include <QBuffer>
#include <QApplication>
#include <QtEndian>
#include <QFile>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>

const auto IMAGE_POOL_CLEANUP_INTERVAL = std::chrono::minutes(1);

const auto IMAGE_POOL_IMAGE_LIFETIME = std::chrono::minutes(10);

constexpr int64_t IMAGE_POOL_MAX_BYTES = 256LL * 1024 * 1024;
constexpr int64_t IMAGE_POOL_TARGET_BYTES = 192LL * 1024 * 1024;
constexpr auto IMAGE_POOL_ACTIVE_IMAGE_GRACE = std::chrono::seconds(5);
constexpr auto STREAMING_GIF_RECENT_PAINT = std::chrono::seconds(2);

namespace chatterino::detail {

class PickerImageReservation
{
public:
    static constexpr int64_t LIMIT = 48LL * 1024 * 1024;
    static constexpr int64_t PER_IMAGE = 4LL * 1024 * 1024;

    static std::shared_ptr<PickerImageReservation> acquire()
    {
        auto used = total_.load();
        while (used + PER_IMAGE <= LIMIT)
        {
            if (total_.compare_exchange_weak(used, used + PER_IMAGE))
            {
                return std::shared_ptr<PickerImageReservation>(
                    new PickerImageReservation);
            }
        }
        return {};
    }
    ~PickerImageReservation()
    {
        total_.fetch_sub(this->bytes_);
    }
    void shrink(int64_t bytes)
    {
        bytes = std::clamp<int64_t>(bytes, 0, this->bytes_);
        total_.fetch_sub(this->bytes_ - bytes);
        this->bytes_ = bytes;
    }

private:
    inline static std::atomic<int64_t> total_{0};
    int64_t bytes_ = PER_IMAGE;
};

}

namespace chatterino {
namespace {

std::atomic<int64_t> &streamingGifResidentBytes()
{
    static std::atomic<int64_t> bytes{0};
    return bytes;
}

struct PendingGifRepaint {
    std::weak_ptr<Image> image;
    unsigned long lastPaint;
};

std::map<Image *, PendingGifRepaint> &streamingGifRepaints()
{
    static std::map<Image *, PendingGifRepaint> pending;
    return pending;
}

void queueImageLayoutInvalidation()
{
    static bool queued = false;
    if (queued || qApp == nullptr)
    {
        return;
    }

    queued = true;
    QMetaObject::invokeMethod(
        qApp,
        [] {
            queued = false;
            if (auto *app = tryGetApp(); app && app->getWindows())
            {
                app->getWindows()->forceLayoutChannelViews();
            }
        },
        Qt::QueuedConnection);
}

}
}

namespace chatterino::detail {

Frames::Frames()
{
    DebugCount::increase(DebugObject::Image);
}

Frames::Frames(QList<Frame> &&frames)
    : items_(std::move(frames))
{
    assertInGuiThread();
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        qCDebug(chatterinoImage)
            << "Frames constructor called while app is shutting down";
        return;
    }

    DebugCount::increase(DebugObject::Image);
    if (!this->empty())
    {
        DebugCount::increase(DebugObject::LoadedImage);
    }

    if (this->animated())
    {
        DebugCount::increase(DebugObject::AnimatedImage);

        this->totalDuration_ =
            std::accumulate(this->items_.begin(), this->items_.end(), 0UL,
                            [](auto init, auto &&frame) {
                                return init + frame.duration;
                            });
        this->lastTimerPosition_ =
            app->getEmotes()->getGIFTimer()->position();

        if (this->totalDuration_ == 0)
        {
            this->durationOffset_ = 0;
        }
        else
        {
            this->durationOffset_ = std::min<uint64_t>(
                this->lastTimerPosition_ % this->totalDuration_, 60000);
        }
        this->processOffset();
    }

    DebugCount::increase(DebugObject::BytesImageCurrent, this->memoryUsage());
    DebugCount::increase(DebugObject::BytesImageLoaded, this->memoryUsage());
}

Frames::~Frames()
{
    assertInGuiThread();
    DebugCount::decrease(DebugObject::Image);
    if (!this->empty())
    {
        DebugCount::decrease(DebugObject::LoadedImage);
    }

    if (this->animated())
    {
        DebugCount::decrease(DebugObject::AnimatedImage);
    }
    DebugCount::decrease(DebugObject::BytesImageCurrent, this->memoryUsage());
    DebugCount::increase(DebugObject::BytesImageUnloaded, this->memoryUsage());
}

int64_t Frames::memoryUsage() const
{
    int64_t usage = 0;
    for (const auto &frame : this->items_)
    {
        auto sz = frame.image.size();
        auto area = sz.width() * sz.height();
        auto memory = area * frame.image.depth() / 8;

        usage += memory;
    }
    return usage;
}

void Frames::synchronizeAnimation()
{
    if (!this->animated())
    {
        return;
    }

    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return;
    }

    const auto currentPosition =
        app->getEmotes()->getGIFTimer()->position();
    this->synchronizeAnimation(currentPosition);
}

void Frames::synchronizeAnimation(unsigned long currentPosition)
{
    const auto elapsed = currentPosition - this->lastTimerPosition_;
    this->lastTimerPosition_ = currentPosition;
    if (elapsed == 0 || this->totalDuration_ == 0)
    {
        return;
    }

    auto elapsedWithinCycle = elapsed % this->totalDuration_;
    if (elapsedWithinCycle == 0)
    {
        elapsedWithinCycle = this->totalDuration_;
    }
    this->durationOffset_ += elapsedWithinCycle;
    this->processOffset();
}

void Frames::processOffset()
{
    if (this->items_.isEmpty())
    {
        return;
    }

    while (true)
    {
        this->index_ %= this->items_.size();

        if (this->durationOffset_ > this->items_[this->index_].duration)
        {
            this->durationOffset_ -= this->items_[this->index_].duration;
            this->index_ = (this->index_ + 1) % this->items_.size();
        }
        else
        {
            break;
        }
    }
}

void Frames::clear()
{
    assertInGuiThread();
    const auto wasAnimated = this->animated();
    const auto memory = this->memoryUsage();
    if (!this->empty())
    {
        DebugCount::decrease(DebugObject::LoadedImage);
    }
    if (wasAnimated)
    {
        DebugCount::decrease(DebugObject::AnimatedImage);
    }
    DebugCount::decrease(DebugObject::BytesImageCurrent, memory);
    DebugCount::increase(DebugObject::BytesImageUnloaded, memory);

    this->items_.clear();
    this->index_ = 0;
    this->durationOffset_ = 0;
    this->totalDuration_ = 0;
    this->lastTimerPosition_ = 0;
}

bool Frames::empty() const
{
    return this->items_.empty();
}

bool Frames::animated() const
{
    return this->items_.size() > 1;
}

std::optional<QPixmap> Frames::current()
{
    if (this->items_.empty())
    {
        return std::nullopt;
    }

    this->synchronizeAnimation();
    return this->items_[this->index_].image;
}

std::optional<QPixmap> Frames::first() const
{
    if (this->items_.empty())
    {
        return std::nullopt;
    }

    return this->items_.front().image;
}

QList<Frame> readFrames(QImageReader &reader, const Url &url,
                        bool firstFrameOnly, int maxFrameDimension,
                        int minimumFrameDuration)
{
    QList<Frame> frames;
    const auto imageCount = firstFrameOnly ? 1 : reader.imageCount();
    frames.reserve(imageCount);

    for (int index = 0; index < imageCount; ++index)
    {
        auto pixmap = QPixmap::fromImageReader(&reader);
        if (!pixmap.isNull())
        {
            if (maxFrameDimension > 0 &&
                (pixmap.width() > maxFrameDimension ||
                 pixmap.height() > maxFrameDimension))
            {
                pixmap = pixmap.scaled(maxFrameDimension, maxFrameDimension,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
            }

            int duration = reader.nextImageDelay();
            if (duration <= 10)
            {
                duration = 100;
            }
            duration = std::max(20, duration);
            if (!frames.empty() && duration < minimumFrameDuration &&
                frames.back().duration < minimumFrameDuration &&
                !(index == imageCount - 1 && frames.size() == 1))
            {
                frames.back().duration += duration;
            }
            else
            {
                frames.append(Frame{
                    .image = std::move(pixmap),
                    .duration = duration,
                });
            }
        }
    }

    if (frames.empty())
    {
        qCDebug(chatterinoImage) << "Error while reading image" << url.string
                                 << ": '" << reader.errorString() << "'";
    }

    return frames;
}

void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed,
                  std::uint64_t loadGeneration,
                  std::shared_ptr<PickerImageReservation> reservation)
{
    auto cb = [parsed = std::move(parsed), weak = std::move(weak),
               loadGeneration, reservation = std::move(reservation)]() mutable {
        auto shared = weak.lock();
        if (!shared)
        {
            return;
        }
        if (shared->loadGeneration_.load(std::memory_order_relaxed) !=
            loadGeneration)
        {
            return;
        }
        if (!parsed.empty())
        {
            shared->empty_ = false;
            shared->shouldLoad_ = false;
            shared->retryCount_.store(0, std::memory_order_relaxed);
            shared->failureReason_.clear();
        }
        shared->frames_ = std::make_unique<detail::Frames>(std::move(parsed));
        shared->animated_ = shared->frames_->animated();
        if (shared->pickerReservation_)
        {
            shared->pickerReservation_->shrink(shared->residentMemoryUsage());
        }
        if (shared->autoScale_)
        {

            auto firstFrame = shared->frames_->first();
            if (firstFrame)
            {
                auto actualSize = firstFrame->size();
                shared->scale_ =
                    static_cast<qreal>(*shared->autoScale_) /
                    std::max({actualSize.width(), actualSize.height(), 1});
            }
        }
        if (shared->emotePicker_)
        {
            if (auto first = shared->frames_->first())
            {
                shared->expectedSize_ = first->size();
            }
        }

#ifndef DISABLE_IMAGE_EXPIRATION_POOL
        ImageExpirationPool::instance().requestBudgetCheck();
#endif

        queueImageLayoutInvalidation();
    };

    runInGuiThread(std::move(cb));
}

}

namespace chatterino {

namespace {

QSize defaultExpectedPhysicalSize(qreal scale)
{
    constexpr qreal DEFAULT_LOGICAL_SIZE = 16.0;
    if (!std::isfinite(scale) || scale <= 0.0)
    {
        return {int(DEFAULT_LOGICAL_SIZE), int(DEFAULT_LOGICAL_SIZE)};
    }

    const auto physicalSize =
        std::max(1, int(std::lround(DEFAULT_LOGICAL_SIZE / scale)));
    return {physicalSize, physicalSize};
}

template <typename Cache>
void sweepExpiredWeakEntries(Cache &cache)
{
    std::erase_if(cache, [](const auto &entry) {
        return entry.second.expired();
    });
}

struct ImageCacheKey {
    Url url;
    qreal scale;
    QSize expectedSize;
    std::optional<uint16_t> autoScale;
    bool firstFrameOnly;
    int maxFrameDimension;
    bool streamingGif;

    bool operator==(const ImageCacheKey &) const = default;
};

struct ImageCacheKeyHash {
    size_t operator()(const ImageCacheKey &key) const
    {
        size_t seed = qHash(key.url.string);
        boost::hash_combine(seed, key.scale);
        boost::hash_combine(seed, key.expectedSize.width());
        boost::hash_combine(seed, key.expectedSize.height());
        boost::hash_combine(seed, key.autoScale.has_value());
        if (key.autoScale)
        {
            boost::hash_combine(seed, *key.autoScale);
        }
        boost::hash_combine(seed, key.firstFrameOnly);
        boost::hash_combine(seed, key.maxFrameDimension);
        boost::hash_combine(seed, key.streamingGif);
        return seed;
    }
};

using UrlImageCache = std::unordered_multimap<size_t, std::weak_ptr<Image>>;

UrlImageCache &urlImageCache()
{
    static UrlImageCache cache;
    return cache;
}

std::mutex &urlImageCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

size_t &urlImageCacheInsertionsSinceSweep()
{
    static size_t count = 0;
    return count;
}

bool shouldRetryImageLoad(const NetworkResult &result)
{
    if (const auto status = result.status())
    {
        return *status == 408 || *status == 425 || *status == 429 ||
               *status >= 500;
    }

    switch (result.error())
    {
        case QNetworkReply::OperationCanceledError:
        case QNetworkReply::ProtocolUnknownError:
        case QNetworkReply::ProtocolInvalidOperationError:
            return false;
        default:
            return true;
    }
}

}

Image::~Image()
{
    this->pickerRequest_.request_stop();
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
    ImageExpirationPool::instance().removeImagePtr(this);
#endif

    if (this->streamingGifResidentBytesEstimate_ > 0)
    {
        streamingGifResidentBytes().fetch_sub(
            this->streamingGifResidentBytesEstimate_,
            std::memory_order_relaxed);
        this->streamingGifResidentBytesEstimate_ = 0;
    }

    if (this->empty_ && !this->frames_ && !this->streamingGifPlayer_)
    {
        return;
    }

    if (isAppAboutToQuit())
    {
        if (this->frames_)
        {
            std::ignore = this->frames_.release();
        }
        std::ignore = this->streamingGifPlayer_.release();
        return;
    }

    if (!isGuiThread())
    {
        postToThread([frames = this->frames_.release(),
                      player = this->streamingGifPlayer_.release(),
                      reservation = std::move(this->pickerReservation_)]() {
            delete frames;
            delete player;
        });
    }
}

ImagePtr Image::fromUrl(const Url &url, qreal scale, QSize expectedSize)
{
    return Image::fromUrlCached(url, scale, expectedSize, std::nullopt, false,
                                0, false);
}

ImagePtr Image::fromUrlWithMaxFrameDimension(const Url &url,
                                             int maxFrameDimension, qreal scale,
                                             QSize expectedSize)
{
    return Image::fromUrlCached(url, scale, expectedSize, std::nullopt, false,
                                std::max(0, maxFrameDimension), false);
}

ImagePtr Image::fromStreamingGifUrl(const Url &url, int maxFrameDimension,
                                    qreal scale, QSize expectedSize)
{
    return Image::fromUrlCached(url, scale, expectedSize, std::nullopt, false,
                                std::max(0, maxFrameDimension), true);
}

ImagePtr Image::fromUrlCached(const Url &url, qreal scale, QSize expectedSize,
                              std::optional<uint16_t> autoScale,
                              bool firstFrameOnly, int maxFrameDimension,
                              bool streamingGif)
{
    auto &cache = urlImageCache();
    auto &newEntriesSinceSweep = urlImageCacheInsertionsSinceSweep();

    const auto effectiveExpectedSize = expectedSize.isValid()
                                           ? expectedSize
                                           : defaultExpectedPhysicalSize(scale);
    const ImageCacheKey key{url, scale, effectiveExpectedSize, autoScale,
                            firstFrameOnly, maxFrameDimension, streamingGif};
    const auto fingerprint = ImageCacheKeyHash{}(key);

    std::vector<ImagePtr> collisionOwners;
    std::lock_guard<std::mutex> lock(urlImageCacheMutex());

    const auto [begin, end] = cache.equal_range(fingerprint);
    for (auto it = begin; it != end;)
    {
        if (auto shared = it->second.lock())
        {
            const ImageCacheKey candidate{shared->url_,
                                          shared->cacheScale_,
                                          shared->cacheExpectedSize_,
                                          shared->autoScale_,
                                          shared->firstFrameOnly_,
                                          shared->maxFrameDimension_,
                                          shared->streamingGif_};
            if (candidate == key)
            {
                return shared;
            }
            collisionOwners.push_back(std::move(shared));
            ++it;
        }
        else
        {
            it = cache.erase(it);
        }
    }

    constexpr size_t CACHE_SWEEP_INTERVAL = 256;
    constexpr size_t CACHE_SWEEP_MIN_SIZE = 1024;
    if (++newEntriesSinceSweep >= CACHE_SWEEP_INTERVAL)
    {
        newEntriesSinceSweep = 0;
        if (cache.size() >= CACHE_SWEEP_MIN_SIZE)
        {
            sweepExpiredWeakEntries(cache);
        }
    }

    auto shared = ImagePtr(
        new Image(url, scale, effectiveExpectedSize, firstFrameOnly, false,
                  maxFrameDimension, streamingGif));
    shared->autoScale_ = autoScale;
    cache.emplace(fingerprint, shared);

    return shared;
}

void Image::releaseUnusedCacheEntries()
{
    std::lock_guard<std::mutex> lock(urlImageCacheMutex());
    auto &cache = urlImageCache();
    sweepExpiredWeakEntries(cache);
    cache.rehash(0);
    urlImageCacheInsertionsSinceSweep() = 0;
}

ImagePtr Image::fromAutoscaledUrl(const Url &url, uint16_t autoScale)
{
    return Image::fromUrlCached(url, 1.0, {autoScale, autoScale}, autoScale,
                                false, 0, false);
}

ImagePtr Image::fromResourcePixmap(const QPixmap &pixmap, qreal scale)
{
    using key_t = std::pair<const QPixmap *, qreal>;
    static std::unordered_map<key_t, std::weak_ptr<Image>, boost::hash<key_t>>
        cache;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);

    auto it = cache.find({&pixmap, scale});
    if (it != cache.end())
    {
        auto shared = it->second.lock();
        if (shared)
        {
            return shared;
        }

        cache.erase(it);
    }

    auto newImage = ImagePtr(new Image(scale));

    newImage->setPixmap(pixmap);

    cache.insert({{&pixmap, scale}, std::weak_ptr<Image>(newImage)});

    return newImage;
}

ImagePtr Image::getEmpty()
{
    static auto empty = ImagePtr(new Image);
    return empty;
}

ImagePtr getEmptyImagePtr()
{
    return Image::getEmpty();
}

Image::Image()
    : empty_(true)
{
}

Image::Image(const Url &url, qreal scale, QSize expectedSize,
             bool firstFrameOnly, bool pickerCopy, int maxFrameDimension,
             bool streamingGif)
    : url_(url)
    , scale_(scale)
    , cacheScale_(scale)
    , cacheExpectedSize_(expectedSize.isValid()
                             ? expectedSize
                             : defaultExpectedPhysicalSize(scale))
    , expectedSize_(expectedSize.isValid() ? expectedSize
                                           : defaultExpectedPhysicalSize(scale))
    , shouldLoad_(true)
    , firstFrameOnly_(firstFrameOnly)
    , pickerCopy_(pickerCopy)
    , maxFrameDimension_(std::max(0, maxFrameDimension))
    , streamingGif_(streamingGif)
{
    if (this->streamingGif_)
    {
        this->animated_ = !this->firstFrameOnly_;
        this->frameCacheLifetimeMs_ = 5000;
    }
}

Image::Image(qreal scale)
    : scale_(scale)
    , frames_(std::make_unique<detail::Frames>())
{
}

void Image::setPixmap(const QPixmap &pixmap)
{
    auto setFrames = [shared = this->shared_from_this(), pixmap]() {
        shared->frames_ = std::make_unique<detail::Frames>(
            QList<detail::Frame>{detail::Frame{pixmap, 1}});
        shared->animated_ = false;
    };

    if (isGuiThread())
    {
        setFrames();
    }
    else
    {
        postToThread(setFrames);
    }
}

const Url &Image::url() const
{
    return this->url_;
}

bool Image::loaded() const
{
    assertInGuiThread();

    if (this->streamingGifPlayer_)
    {
        return !this->streamingGifPlayer_->current().isNull();
    }

    if (!this->frames_)
    {
        return false;
    }

    return this->frames_->current().has_value();
}

std::optional<QPixmap> Image::pixmapOrLoad() const
{
    assertInGuiThread();

    this->lastUsed_ = std::chrono::steady_clock::now();

    this->load();

    if (this->streamingGifPlayer_)
    {
        auto *self = const_cast<Image *>(this);
        if (this->shouldPlayStreamingGif())
        {
            const auto position =
                getApp()->getEmotes()->getGIFTimer()->position();
            self->advanceStreamingGif(position);
        }

        const auto pixmap = this->streamingGifPlayer_->current();
        return pixmap.isNull() ? std::nullopt
                               : std::optional<QPixmap>{pixmap};
    }

    if (!this->frames_)
    {
        return std::nullopt;
    }

    return this->frames_->current();
}

bool Image::isLoading() const
{
    assertInGuiThread();
    return !this->shouldLoad_ && !this->empty_ && !this->loaded();
}

ImagePtr Image::getFirstFramePreview() const
{
    assertInGuiThread();

    auto self = const_cast<Image *>(this)->shared_from_this();
    if (this->firstFrameOnly_ || this->url_.string.isEmpty() ||
        (this->loaded() && !this->animated_))
    {
        return self;
    }

    auto preview = Image::fromUrlCached(
        this->url_, this->scale_, this->expectedSize_, this->autoScale_, true,
        this->maxFrameDimension_, this->streamingGif_);
    preview->useGifFallback_ |= this->useGifFallback_;
    preview->setFrameCacheLifetime(std::chrono::milliseconds{
        this->frameCacheLifetimeMs_.load(std::memory_order_relaxed)});
    return preview;
}

ImagePtr Image::getPickerAnimation() const
{
    assertInGuiThread();

    if (this->url_.string.isEmpty())
    {
        return const_cast<Image *>(this)->shared_from_this();
    }

    auto picker = ImagePtr(
        new Image(this->url_, this->scale_, this->expectedSize_, false, true,
                  this->maxFrameDimension_, this->streamingGif_));
    picker->autoScale_ = this->autoScale_;
    picker->useGifFallback_ = this->useGifFallback_;
    return picker;
}

void Image::setPickerUrl(Url url)
{
    std::lock_guard lock(urlImageCacheMutex());
    this->pickerUrl_ = std::move(url);
}

ImagePtr Image::getEmotePickerImage(bool still, bool smooth) const
{
    assertInGuiThread();
    if (this->url_.string.isEmpty())
    {
        return const_cast<Image *>(this)->shared_from_this();
    }
    Url pickerUrl;
    {
        std::lock_guard lock(urlImageCacheMutex());
        pickerUrl =
            this->pickerUrl_.string.isEmpty() ? this->url_ : this->pickerUrl_;
    }
    auto picker = ImagePtr(new Image(pickerUrl, this->scale_,
                                     this->expectedSize_, still, true, 256));
    picker->emotePicker_ = true;
    picker->smoothPicker_ = smooth;
    picker->autoScale_ = this->autoScale_;
    picker->setFrameCacheLifetime(std::chrono::seconds(5));
    return picker;
}

void Image::releasePickerFrames()
{
    assertInGuiThread();
    if (this->pickerCopy_)
    {
        if (this->shouldLoad_ && !this->hasResidentData() && !this->empty_)
        {
            return;
        }
        this->loadGeneration_.fetch_add(1, std::memory_order_relaxed);
        this->pickerRequest_.request_stop();
        this->frames_.reset();
        this->clearStreamingGif();
        this->pickerReservation_.reset();
        this->shouldLoad_ = true;
        this->empty_ = false;
        this->animated_ = this->streamingGif_;
        this->retryCount_.store(0, std::memory_order_relaxed);
        this->failureReason_.clear();
#ifndef DISABLE_IMAGE_EXPIRATION_POOL

        ImageExpirationPool::instance().removeImagePtr(this);
#endif
    }
}

void Image::setFrameCacheLifetime(std::chrono::milliseconds lifetime)
{
    this->frameCacheLifetimeMs_.store(
        std::max<int64_t>(0, lifetime.count()), std::memory_order_relaxed);
}

std::optional<QPixmap> Image::firstPixmapOrLoad() const
{
    assertInGuiThread();

    this->lastUsed_ = std::chrono::steady_clock::now();
    this->load();

    if (this->streamingGifPlayer_)
    {
        const auto pixmap = this->streamingGifPlayer_->current();
        return pixmap.isNull() ? std::nullopt
                               : std::optional<QPixmap>{pixmap};
    }

    if (!this->frames_)
    {
        return std::nullopt;
    }

    return this->frames_->first();
}

void Image::load() const
{
    assertInGuiThread();

    if (this->shouldLoad_)
    {
        Image *this2 = const_cast<Image *>(this);
        if (this->emotePicker_ && !this->firstFrameOnly_ &&
            !this->pickerReservation_)
        {
            this2->pickerReservation_ =
                detail::PickerImageReservation::acquire();
            if (!this2->pickerReservation_)
            {
                return;
            }
        }
        this2->shouldLoad_ = false;
        if (this->pickerCopy_)
        {
            this2->pickerRequest_ = std::stop_source{};
        }
        this2->actuallyLoad();
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
        ImageExpirationPool::instance().addImagePtr(this2->shared_from_this());
#endif
    }
}

void Image::retryLoad() const
{
    assertInGuiThread();

    if (this->loaded())
    {
        return;
    }

    if (this->empty_)
    {
        auto *self = const_cast<Image *>(this);
        self->empty_ = false;
        self->shouldLoad_ = true;
        self->animated_ = self->streamingGif_;
        self->useGifFallback_ = false;
        self->retryCount_.store(0, std::memory_order_relaxed);
        self->failureReason_.clear();
    }
    this->load();
}

qreal Image::scale() const
{
    return this->scale_;
}

bool Image::isEmpty() const
{
    return this->empty_;
}

const QString &Image::failureReason() const
{
    assertInGuiThread();
    return this->failureReason_;
}

bool Image::animated() const
{
    assertInGuiThread();
    return this->animated_;
}

bool Image::usesOwnAnimationTimer() const
{
    return (this->streamingGif_ || this->streamingGifPlayer_ != nullptr) &&
           !this->firstFrameOnly_;
}

int Image::width() const
{
    assertInGuiThread();

    if (this->streamingGifPlayer_)
    {
        const auto pixmap = this->streamingGifPlayer_->current();
        if (!pixmap.isNull())
        {
            return static_cast<int>(pixmap.width() * this->scale_);
        }
    }

    if (!this->frames_)
    {
        return this->empty_ ? 0
                            : static_cast<int>(this->expectedSize_.width() *
                                               this->scale_);
    }

    if (auto pixmap = this->frames_->first())
    {
        return static_cast<int>(pixmap->width() * this->scale_);
    }

    return static_cast<int>(this->expectedSize_.width() * this->scale_);
}

int Image::height() const
{
    assertInGuiThread();

    if (this->streamingGifPlayer_)
    {
        const auto pixmap = this->streamingGifPlayer_->current();
        if (!pixmap.isNull())
        {
            return static_cast<int>(pixmap.height() * this->scale_);
        }
    }

    if (!this->frames_)
    {
        return this->empty_ ? 0
                            : static_cast<int>(this->expectedSize_.height() *
                                               this->scale_);
    }

    if (auto pixmap = this->frames_->first())
    {
        return static_cast<int>(pixmap->height() * this->scale_);
    }

    return static_cast<int>(this->expectedSize_.height() * this->scale_);
}

QSizeF Image::size() const
{
    assertInGuiThread();

    if (this->streamingGifPlayer_)
    {
        const auto pixmap = this->streamingGifPlayer_->current();
        if (!pixmap.isNull())
        {
            return pixmap.size().toSizeF() * this->scale_;
        }
    }

    if (!this->frames_)
    {
        return this->empty_ ? QSizeF{0, 0}
                            : this->expectedSize_.toSizeF() * this->scale_;
    }

    if (auto pixmap = this->frames_->first())
    {
        return pixmap->size().toSizeF() * this->scale_;
    }

    return this->expectedSize_.toSizeF() * this->scale_;
}

bool Image::shouldPlayStreamingGif() const
{
    auto *app = tryGetApp();
    return this->streamingGifPlayer_ &&
           !this->streamingGifPlayer_->finished() && app != nullptr &&
           (!this->streamingGif_ || this->emotePicker_ ||
            getSettings()->enableTwitchGifs) &&
           app->getEmotes()->getGIFTimer()->shouldAnimate();
}

void Image::advanceStreamingGif(unsigned long position)
{
    this->streamingGifPlayer_->advance(position);
    if (!this->streamingGifPlayer_->finished())
    {
        streamingGifRepaints().insert_or_assign(
            this, PendingGifRepaint{weakOf(this), position});
    }
}

bool Image::takeStreamingGifRepaint(unsigned long position)
{
    assertInGuiThread();
    bool due = false;
    auto &pending = streamingGifRepaints();
    for (auto it = pending.begin(); it != pending.end();)
    {
        auto image = it->second.image.lock();
        if (!image || !image->streamingGifPlayer_ ||
            image->streamingGifPlayer_->finished())
        {
            it = pending.erase(it);
        }
        else if (image->streamingGifPlayer_->frameDue(position) ||
                 position - it->second.lastPaint >= 1000)
        {
            due = true;
            it = pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return due;
}

bool Image::hasResidentData() const
{
    return (this->frames_ && !this->frames_->empty()) ||
           this->streamingGifPlayer_ != nullptr;
}

int64_t Image::residentMemoryUsage() const
{
    return (this->frames_ ? this->frames_->memoryUsage() : 0) +
           this->streamingGifResidentBytesEstimate_;
}

void Image::updateStreamingGifAccounting()
{
    const auto bytes = this->streamingGifPlayer_
                           ? this->streamingGifPlayer_->residentBytes()
                           : 0;
    streamingGifResidentBytes().fetch_add(
        bytes - this->streamingGifResidentBytesEstimate_,
        std::memory_order_relaxed);
    this->streamingGifResidentBytesEstimate_ = bytes;
}

void Image::clearStreamingGif()
{
    assertInGuiThread();
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
    ImageExpirationPool::instance().untrackStreamingGif(this);
#endif
    streamingGifRepaints().erase(this);
    this->streamingGifPlayer_.reset();
    this->updateStreamingGifAccounting();
}

void Image::failStreamingGif(QString reason, std::uint64_t loadGeneration)
{
    assertInGuiThread();
    if (this->loadGeneration_.load(std::memory_order_relaxed) != loadGeneration)
    {
        return;
    }
    this->loadGeneration_.fetch_add(1, std::memory_order_relaxed);
    this->clearStreamingGif();
    this->pickerReservation_.reset();
    this->failureReason_ = std::move(reason);
    this->empty_ = true;
    this->shouldLoad_ = false;
    this->animated_ = false;
    queueImageLayoutInvalidation();
}

void Image::initializeStreamingGif(QByteArray data,
                                   std::uint64_t loadGeneration,
                                   bool gifFallback)
{
    assertInGuiThread();
    if (this->loadGeneration_.load(std::memory_order_relaxed) != loadGeneration)
    {
        return;
    }
    this->clearStreamingGif();
    this->streamingGifPlayer_ = std::make_unique<detail::StreamingGif>(
        std::move(data), this->maxFrameDimension_,
        this->emotePicker_ ? detail::PickerImageReservation::PER_IMAGE
                           : Image::maxBytesRam,
        this->firstFrameOnly_,
        [weak = weakOf(this), loadGeneration, gifFallback](bool firstFrame) {
            const auto image = weak.lock();
            if (!image ||
                image->loadGeneration_.load(std::memory_order_relaxed) !=
                    loadGeneration ||
                !image->streamingGifPlayer_)
            {
                return;
            }
            auto &player = *image->streamingGifPlayer_;
            if (player.current().isNull())
            {
                const auto error = player.error();
                image->clearStreamingGif();
                if (!gifFallback && !image->streamingGifFallbackUrl().isEmpty())
                {
                    image->actuallyLoad(true);
                }
                else
                {
                    image->failStreamingGif(error, loadGeneration);
                }
                return;
            }
            const bool animationChanged = image->animated_ == player.finished();
            image->expectedSize_ = player.current().size();
            if (image->autoScale_)
            {
                image->scale_ = qreal(*image->autoScale_) /
                                std::max(player.current().width(),
                                         player.current().height());
            }
            image->animated_ = !player.finished();
            image->failureReason_ = player.error();
            image->retryCount_.store(0, std::memory_order_relaxed);
            image->updateStreamingGifAccounting();
            if (image->pickerReservation_)
            {
                image->pickerReservation_->shrink(player.residentBytes());
            }
            if (player.finished())
            {
                streamingGifRepaints().erase(image.get());
            }
            if (firstFrame || animationChanged)
            {
                queueImageLayoutInvalidation();
            }
            else if (auto *app = tryGetApp(); app && app->getWindows())
            {
                app->getWindows()->repaintTwitchGifs();
            }
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
            ImageExpirationPool::instance().requestBudgetCheck();
#endif
        },
        this->pickerReservation_);
    this->updateStreamingGifAccounting();
    this->empty_ = false;
    this->shouldLoad_ = false;
    this->animated_ = !this->firstFrameOnly_;
    this->failureReason_.clear();

#ifndef DISABLE_IMAGE_EXPIRATION_POOL
    ImageExpirationPool::instance().trackStreamingGif(this->shared_from_this());
    ImageExpirationPool::instance().requestBudgetCheck();
#endif
}

QString Image::streamingGifFallbackUrl() const
{
    if (!this->streamingGif_)
    {
        return {};
    }
    const QUrl url(this->url_.string);
    const auto parts = url.path().split(u'/');
    if (url.scheme() != u"https" || url.host() != u"media.giphy.com" ||
        !url.userInfo().isEmpty() || url.port(443) != 443 || url.hasQuery() ||
        url.hasFragment() || parts.size() != 4 || parts[1] != u"media" ||
        parts[2].isEmpty() ||
        (parts[3] != u"200w.webp" && parts[3] != u"giphy.webp") ||
        !std::ranges::all_of(parts[2], [](QChar c) {
            return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
                   (c >= u'0' && c <= u'9');
        }))
    {
        return {};
    }
    auto fallback = this->url_.string;
    fallback.chop(4);
    return fallback + QStringLiteral("gif");
}

void Image::actuallyLoad(bool gifFallback)
{
    gifFallback = gifFallback || this->useGifFallback_;
    this->useGifFallback_ = gifFallback;
    auto weak = weakOf(this);
    const auto reservation = this->pickerReservation_;
    const auto loadGeneration =
        this->loadGeneration_.load(std::memory_order_relaxed);
    auto markEmpty = [loadGeneration](const std::shared_ptr<Image> &shared,
                                      QString reason = {}) {
        postToThread([weak = std::weak_ptr<Image>(shared), loadGeneration,
                      reason = std::move(reason)]() mutable {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }
            if (shared->loadGeneration_.load(std::memory_order_relaxed) !=
                loadGeneration)
            {
                return;
            }
            shared->failureReason_ = std::move(reason);
            shared->empty_ = true;
            shared->shouldLoad_ = false;
            shared->pickerReservation_.reset();

            if (shared->streamingGif_)
            {
                shared->failStreamingGif(shared->failureReason_,
                                         loadGeneration);
                return;
            }

            detail::assignFrames(shared, {}, loadGeneration);
        });
    };
    auto onSuccess = [weak, markEmpty, loadGeneration, reservation,
                      gifFallback](const auto &result) {
        auto shared = weak.lock();
        if (!shared)
        {
            return;
        }
        if (shared->loadGeneration_.load(std::memory_order_relaxed) !=
            loadGeneration)
        {
            return;
        }

        assert(!isAppAboutToQuit());

        shared->retryCount_.store(0, std::memory_order_relaxed);

        if (shared->emotePicker_ &&
            result.getData().size() >
                detail::PickerImageReservation::PER_IMAGE / 2)
        {
            markEmpty(shared, "Emote preview exceeded the download limit");
            return;
        }

        if (shared->streamingGif_)
        {
            auto data = result.getData();
            postToGuiThread([weak, loadGeneration, gifFallback,
                             data = std::move(data)]() mutable {
                if (auto image = weak.lock())
                {
                    image->initializeStreamingGif(std::move(data),
                                                  loadGeneration, gifFallback);
                }
            });
            return;
        }

        QBuffer buffer;
        buffer.setData(result.getData());
        QImageReader reader(&buffer);

        if (!reader.canRead())
        {
            qCDebug(chatterinoImage)
                << "Error: image cant be read " << shared->url().string;
            markEmpty(shared, "Cannot read image format (" +
                                  reader.errorString() + ")");
            return;
        }

        auto size = reader.size();
        const auto &encoded = buffer.data();
        if (encoded.size() >= 10 &&
            (encoded.startsWith("GIF87a") || encoded.startsWith("GIF89a")))
        {
            size = size.expandedTo(
                QSize(qFromLittleEndian<quint16>(encoded.constData() + 6),
                      qFromLittleEndian<quint16>(encoded.constData() + 8)));
        }
        if (size.isEmpty())
        {
            markEmpty(shared, "Image has empty dimensions (" +
                                  reader.errorString() + ")");
            return;
        }

        auto decodedFrameSize = size;
        if (shared->maxFrameDimension_ > 0 &&
            (size.width() > shared->maxFrameDimension_ ||
             size.height() > shared->maxFrameDimension_))
        {
            decodedFrameSize.scale(shared->maxFrameDimension_,
                                   shared->maxFrameDimension_,
                                   Qt::KeepAspectRatio);
            decodedFrameSize.setWidth(std::max(1, decodedFrameSize.width()));
            decodedFrameSize.setHeight(std::max(1, decodedFrameSize.height()));
            reader.setScaledSize(decodedFrameSize);
        }

        const auto frameCount =
            shared->firstFrameOnly_ ? 1 : reader.imageCount();
        if (frameCount <= 0 || (shared->emotePicker_ && frameCount > 10000))
        {
            qCDebug(chatterinoImage)
                << "Error: image has less than 1 frame " << shared->url().string
                << ": " << reader.errorString();
            markEmpty(shared,
                      "Image has 0 frames (" + reader.errorString() + ")");
            return;
        }

        const double originalFrameBytes =
            double(size.width()) * double(size.height()) * 4.0;
        const double decodedFramesBytes =
            double(decodedFrameSize.width()) *
            double(decodedFrameSize.height()) * double(frameCount) * 4.0;

        const double estimatedBytes =
            std::max(originalFrameBytes, decodedFramesBytes);
        const bool canStream =
            encoded.startsWith("GIF87a") || encoded.startsWith("GIF89a") ||
            (encoded.startsWith("RIFF") && encoded.mid(8, 4) == "WEBP");
        const double streamEstimate =
            encoded.size() * (encoded.startsWith("RIFF") ? 1.0 : 2.0) +
            64 * 1024 + originalFrameBytes * 3 +
            decodedFramesBytes / frameCount * 2 + (frameCount + 5.0) * 256;
        if ((shared->emotePicker_ || decodedFramesBytes > Image::maxBytesRam) &&
            !shared->firstFrameOnly_ && canStream &&
            decodedFramesBytes > 512 * 1024 &&
            streamEstimate < decodedFramesBytes)
        {
            const auto scale =
                shared->cacheScale_ * std::max(size.width(), size.height()) /
                std::max(decodedFrameSize.width(), decodedFrameSize.height());
            postToGuiThread([weak, loadGeneration, reservation, scale,
                             data = result.getData()]() mutable {
                if (auto image = weak.lock();
                    image && image->loadGeneration_.load() == loadGeneration)
                {
                    image->scale_ = scale;
                    image->initializeStreamingGif(std::move(data),
                                                  loadGeneration);
                }
            });
            return;
        }
        const auto maxBytes =
            shared->emotePicker_ ? detail::PickerImageReservation::PER_IMAGE / 2
                                 : Image::maxBytesRam;
        const auto pickerPeak =
            originalFrameBytes * 3 + decodedFramesBytes + encoded.size() * 2.0;
        if (estimatedBytes > double(maxBytes) ||
            (shared->emotePicker_ &&
             pickerPeak > detail::PickerImageReservation::PER_IMAGE))
        {
            qCDebug(chatterinoImage) << "image too large in RAM";
            markEmpty(shared, QString("Image exceeded memory limit (%1 MB)")
                                  .arg(estimatedBytes / (1024.0 * 1024.0),
                                       0, 'f', 0));
            return;
        }

        auto parsed = detail::readFrames(
            reader, shared->url(), shared->firstFrameOnly_,
            shared->maxFrameDimension_,
            shared->emotePicker_ && !shared->smoothPicker_ ? 67 : 0);
        if (parsed.empty())
        {
            markEmpty(shared, "Failed to decode frames (" +
                                  reader.errorString() + ")");
            return;
        }

        if (shared->emotePicker_)
        {
            const auto scale =
                shared->cacheScale_ * std::max(size.width(), size.height()) /
                std::max(decodedFrameSize.width(), decodedFrameSize.height());
            postToGuiThread([weak, loadGeneration, reservation, scale,
                             parsed = std::move(parsed)]() mutable {
                if (auto image = weak.lock();
                    image && image->loadGeneration_.load() == loadGeneration)
                {
                    image->scale_ = scale;
                    assignFrames(image, std::move(parsed), loadGeneration,
                                 reservation);
                }
            });
        }
        else
        {
            assignFrames(shared, std::move(parsed), loadGeneration);
        }
    };
    auto onError = [weak, markEmpty, gifFallback,
                    loadGeneration](const NetworkResult &result) {
        auto shared = weak.lock();
        if (!shared || shared->loadGeneration_.load(
                           std::memory_order_relaxed) != loadGeneration)
        {
            return false;
        }

        if (!gifFallback && !shared->streamingGifFallbackUrl().isEmpty())
        {
            postToGuiThread([weak, loadGeneration] {
                if (auto image = weak.lock();
                    image && image->loadGeneration_.load(
                                 std::memory_order_relaxed) == loadGeneration)
                {
                    image->actuallyLoad(true);
                }
            });
            return true;
        }

        if (shouldRetryImageLoad(result))
        {
            const auto currentRetry =
                shared->retryCount_.fetch_add(1, std::memory_order_relaxed);
            if (currentRetry < 2)
            {
                const auto delayMs = (currentRetry + 1) * 1000;
                postToThread([weak, delayMs, loadGeneration, gifFallback]() {
                    QTimer::singleShot(
                        delayMs, [weak, loadGeneration, gifFallback]() {
                            if (auto img = weak.lock())
                            {
                                if (img->loadGeneration_.load(
                                        std::memory_order_relaxed) ==
                                    loadGeneration)
                                {
                                    img->shouldLoad_ = false;
                                    img->actuallyLoad(gifFallback);
                                }
                            }
                        });
                });
                return true;
            }
        }

        markEmpty(shared,
                  QString("Network error (%1)").arg(result.formatError()));
        return true;
    };

    if (this->url_.string.startsWith(u"data:image/"))
    {
        QThreadPool::globalInstance()->start([onSuccess = std::move(onSuccess),
                                              onError = std::move(onError),
                                              url = this->url_.string] {
            const auto comma = url.indexOf(u',');
            if (comma <= 0 ||
                !url.first(comma).endsWith(u";base64",
                                           Qt::CaseInsensitive))
            {
                onError(NetworkResult{
                    NetworkResult::NetworkError::ProtocolInvalidOperationError,
                    400,
                    {},
                });
                return;
            }

            auto data = QByteArray::fromBase64(
                url.sliced(comma + 1).toLatin1());
            if (data.isEmpty())
            {
                onError(NetworkResult{
                    NetworkResult::NetworkError::ProtocolFailure,
                    400,
                    {},
                });
                return;
            }
            onSuccess(NetworkResult{
                NetworkResult::NetworkError::NoError,
                200,
                std::move(data),
            });
        });
    }
    else if (this->url_.string.startsWith(u":/"))
    {
        QThreadPool::globalInstance()->start([onSuccess = std::move(onSuccess),
                                              onError = std::move(onError),
                                              url = this->url_.string] {
            QByteArray data;
            {
                QFile file(url);
                if (!file.open(QFile::ReadOnly))
                {
                    onError(NetworkResult{
                        NetworkResult::NetworkError::ContentNotFoundError,
                        404,
                        {},
                    });
                    return;
                }
                data = file.readAll();
            }
            onSuccess(NetworkResult{
                NetworkResult::NetworkError::NoError,
                200,
                std::move(data),
            });
        });
    }
    else
    {
        const auto url =
            gifFallback ? this->streamingGifFallbackUrl() : this->url().string;
        auto request =
            NetworkRequest(url)
                .concurrent()
                .cache([](const QByteArray &data) {
                    QBuffer buffer;
                    buffer.setData(data);
                    QImageReader reader(&buffer);
                    return reader.canRead() && !reader.size().isEmpty();
                })
                .timeout(15000)
                .maximumResponseSize(
                    this->emotePicker_
                        ? detail::PickerImageReservation::PER_IMAGE / 2
                        : Image::maxBytesRam);
        if (this->streamingGif_ || this->pickerCopy_)
        {
            request = std::move(request).maximumRedirectsAllowed(3).attribute(
                QNetworkRequest::RedirectPolicyAttribute,
                QNetworkRequest::SameOriginRedirectPolicy);
        }
        if (this->pickerCopy_)
        {
            request =
                std::move(request).cancelWith(this->pickerRequest_.get_token());
        }
        std::move(request)
            .onSuccess(std::move(onSuccess))
            .onError(std::move(onError))
            .execute();
    }
}

void Image::expireFrames()
{
    assertInGuiThread();
    if (!this->hasResidentData())
    {
        return;
    }

    this->frames_.reset();
    if (this->streamingGifPlayer_)
    {
        this->loadGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
    this->clearStreamingGif();
    this->pickerReservation_.reset();
    if (this->streamingGif_)
    {
        this->animated_ = !this->firstFrameOnly_;
    }
    this->shouldLoad_ = true;
    this->empty_ = false;
}

#ifndef DISABLE_IMAGE_EXPIRATION_POOL

ImageExpirationPool::ImageExpirationPool()
    : freeTimer_(new QTimer)
{
    QObject::connect(this->freeTimer_, &QTimer::timeout, [this] {
        if (isGuiThread())
        {
            this->freeOld();
        }
        else
        {
            postToThread([this] {
                this->freeOld();
            });
        }
    });

    this->freeTimer_->start(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            IMAGE_POOL_CLEANUP_INTERVAL));
}

ImageExpirationPool &ImageExpirationPool::instance()
{
    static auto *instance = new ImageExpirationPool;
    return *instance;
}

void ImageExpirationPool::trackStreamingGif(const ImagePtr &image)
{
    assertInGuiThread();
    this->streamingGifs_.insert_or_assign(image.get(), image);
    if (this->streamingGifTimer_ == nullptr)
    {
        this->streamingGifTimer_ = new QTimer(qApp);
        this->streamingGifTimer_->setInterval(std::chrono::seconds(2));
        QObject::connect(this->streamingGifTimer_, &QTimer::timeout, qApp,
                         [this] {
                             this->freeInactiveStreamingGifs();
                         });
    }
    if (!this->streamingGifTimer_->isActive())
    {
        this->streamingGifTimer_->start();
    }
}

void ImageExpirationPool::untrackStreamingGif(Image *image)
{
    assertInGuiThread();
    this->streamingGifs_.erase(image);
    if (this->streamingGifs_.empty() && this->streamingGifTimer_ != nullptr)
    {
        this->streamingGifTimer_->stop();
    }
}

void ImageExpirationPool::freeInactiveStreamingGifs()
{
    assertInGuiThread();
    const auto now = std::chrono::steady_clock::now();
    int64_t released = 0;
    for (auto it = this->streamingGifs_.begin();
         it != this->streamingGifs_.end();)
    {
        auto image = it->second.lock();
        if (!image || !image->streamingGifPlayer_)
        {
            it = this->streamingGifs_.erase(it);
            continue;
        }
        const auto lifetime = std::chrono::milliseconds{
            image->frameCacheLifetimeMs_.load(std::memory_order_relaxed)};
        if (now - image->lastUsed_ >
            std::max(lifetime, std::chrono::milliseconds{5000}))
        {
            ++it;
            released += image->residentMemoryUsage();
            image->expireFrames();
            this->removeImagePtr(image.get());
        }
        else
        {
            ++it;
        }
    }
    if (this->streamingGifs_.empty() && this->streamingGifTimer_ != nullptr)
    {
        this->streamingGifTimer_->stop();
    }
    if (released >= 8 * 1024 * 1024)
    {
        requestMemoryPressureRelief();
    }
}

void ImageExpirationPool::addImagePtr(ImagePtr imgPtr)
{
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->allImages_.emplace(imgPtr.get(), std::weak_ptr<Image>(imgPtr));
}

void ImageExpirationPool::removeImagePtr(Image *rawPtr)
{
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->allImages_.erase(rawPtr);
}

namespace {

std::vector<ImagePtr> snapshotPoolImages(ImageExpirationPool &pool)
{
    std::vector<ImagePtr> images;
    std::lock_guard lock(pool.mutex_);
    images.reserve(pool.allImages_.size());
    for (auto it = pool.allImages_.begin(); it != pool.allImages_.end();)
    {
        if (auto image = it->second.lock())
        {
            images.push_back(std::move(image));
            ++it;
        }
        else
        {
            it = pool.allImages_.erase(it);
        }
    }
    return images;
}

}

void ImageExpirationPool::freeAll()
{
    assertInGuiThread();
    for (const auto &image : snapshotPoolImages(*this))
    {
        image->expireFrames();
        this->removeImagePtr(image.get());
    }
    this->freeOld();
}

void ImageExpirationPool::freeOld()
{
    assertInGuiThread();

    const auto images = snapshotPoolImages(*this);
    size_t numExpired = 0;
    size_t eligible = 0;
    int64_t releasedBytes = 0;
    auto now = std::chrono::steady_clock::now();
    for (const auto &img : images)
    {
        if (!img->hasResidentData())
        {
            continue;
        }
        ++eligible;
        const auto diff =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - img->lastUsed_);
        const auto customLifetimeMs =
            img->frameCacheLifetimeMs_.load(std::memory_order_relaxed);
        const auto lifetime =
            customLifetimeMs > 0
                ? std::chrono::milliseconds{customLifetimeMs}
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      IMAGE_POOL_IMAGE_LIFETIME);
        if (diff > lifetime)
        {
            ++numExpired;
            releasedBytes += img->residentMemoryUsage();
            img->expireFrames();
            this->removeImagePtr(img.get());
        }
    }

#    ifndef NDEBUG
    qCDebug(chatterinoImage) << "freed frame data for" << numExpired << "/"
                             << eligible << "eligible images";
#    endif
    DebugCount::set(DebugObject::LastImageGcExpired, numExpired);
    DebugCount::set(DebugObject::LastImageGcEligible, eligible);
    {
        std::lock_guard lock(this->mutex_);
        DebugCount::set(DebugObject::LastImageGcLeft, this->allImages_.size());
    }
    if (releasedBytes >= 8 * 1024 * 1024)
    {
        requestMemoryPressureRelief();
    }
}

void ImageExpirationPool::requestBudgetCheck(bool force)
{
    assertInGuiThread();

    const auto trackedResidentBytes =
        DebugCount::get(DebugObject::BytesImageCurrent) +
        streamingGifResidentBytes().load(std::memory_order_relaxed);
    if (this->budgetCheckQueued_ ||
        (!force && trackedResidentBytes <= IMAGE_POOL_MAX_BYTES))
    {
        return;
    }

    this->budgetCheckQueued_ = true;
    QTimer::singleShot(0, [this] {
        this->runBudgetCheck();
    });
}

void ImageExpirationPool::runBudgetCheck()
{
    assertInGuiThread();
    if (this->enforceBudget(IMAGE_POOL_MAX_BYTES, IMAGE_POOL_TARGET_BYTES))
    {
        QTimer::singleShot(IMAGE_POOL_ACTIVE_IMAGE_GRACE, [this] {
            this->runBudgetCheck();
        });
        return;
    }

    this->budgetCheckQueued_ = false;
}

bool ImageExpirationPool::enforceBudget(int64_t maxBytes, int64_t targetBytes)
{
    assertInGuiThread();
    assert(targetBytes <= maxBytes);

    struct Candidate {
        ImagePtr image;
        int64_t bytes;
    };

    std::vector<Candidate> candidates;
    int64_t totalBytes = 0;
    const auto now = std::chrono::steady_clock::now();

    const auto images = snapshotPoolImages(*this);
    candidates.reserve(images.size());
    for (const auto &image : images)
    {
        if (!image->hasResidentData())
        {
            continue;
        }
        const auto bytes = image->residentMemoryUsage();
        if (bytes > 0)
        {
            totalBytes += bytes;
            if (now - image->lastUsed_ >= IMAGE_POOL_ACTIVE_IMAGE_GRACE)
            {
                candidates.push_back({image, bytes});
            }
        }
    }

    if (totalBytes <= maxBytes)
    {
        return false;
    }

    std::ranges::sort(candidates, [](const auto &left, const auto &right) {
        return left.image->lastUsed_ < right.image->lastUsed_;
    });

    size_t evicted = 0;
    int64_t evictedBytes = 0;
    for (auto &candidate : candidates)
    {
        if (totalBytes <= targetBytes)
        {
            break;
        }

        candidate.image->expireFrames();
        this->removeImagePtr(candidate.image.get());
        totalBytes -= candidate.bytes;
        evictedBytes += candidate.bytes;
        ++evicted;
    }

#ifndef NDEBUG
    qCDebug(chatterinoImage)
        << "decoded image budget evicted" << evicted << "images, leaving"
        << totalBytes << "bytes";
#endif

    if (evictedBytes >= 8 * 1024 * 1024)
    {
        requestMemoryPressureRelief();
    }

    return totalBytes > maxBytes;
}

std::vector<ImageExpirationPool::ProviderUsage>
ImageExpirationPool::getProviderUsageSnapshot()
{
    const auto providerForUrl = [](const QString &url) {
        if (url.isEmpty())
        {
            return QStringLiteral("internal");
        }
        if (url.startsWith(u":/"))
        {
            return QStringLiteral("resources");
        }

        const QUrl parsed(url);
        const auto host = parsed.host().toLower();
        const auto path = parsed.path().toLower();
        if (host.isEmpty())
        {
            return QStringLiteral("unknown");
        }

        if (host.contains(u"chatterinohomies.com") ||
            host == u"itzalex.github.io")
        {
            return QStringLiteral("Homies");
        }
        if (host.contains(u"7tv"))
        {
            if (path.contains(u"/emote/"))
            {
                return QStringLiteral("7TV emotes");
            }
            if (path.contains(u"/paint/"))
            {
                return QStringLiteral("7TV paints");
            }
            if (path.contains(u"/badge/"))
            {
                return QStringLiteral("7TV badges");
            }
            if (path.contains(u"/cosmetic/"))
            {
                return QStringLiteral("7TV cosmetics");
            }
            return QStringLiteral("7TV");
        }
        if (host.contains(u"jtvnw.net") || host.contains(u"ttvnw.net") ||
            host.contains(u"twitch.tv"))
        {
            return QStringLiteral("Twitch");
        }
        if (host.contains(u"betterttv") || host.contains(u"bttv"))
        {
            return QStringLiteral("BTTV");
        }
        if (host.contains(u"frankerfacez") || host.contains(u"ffzap"))
        {
            return QStringLiteral("FFZ");
        }
        if (host.contains(u"chatterino"))
        {
            return QStringLiteral("Chatterino");
        }

        return host;
    };

    const auto images = snapshotPoolImages(*this);

    std::map<QString, ProviderUsage> providers;
    for (const auto &img : images)
    {
        if (!img->hasResidentData())
        {
            continue;
        }
        const auto bytes = img->residentMemoryUsage();
        if (bytes <= 0)
        {
            continue;
        }

        const auto provider = providerForUrl(img->url_.string);
        auto &usage = providers[provider];
        usage.provider = provider;
        usage.bytes += bytes;
        usage.images += 1;
        if (img->animated_)
        {
            usage.animatedImages += 1;
        }
    }

    std::vector<ProviderUsage> result;
    result.reserve(providers.size());
    for (auto &[_, usage] : providers)
    {
        result.push_back(std::move(usage));
    }

    std::ranges::sort(result, [](const auto &a, const auto &b) {
        return a.bytes > b.bytes;
    });

    return result;
}

#endif

}
